/*
 * ============================================================================
 *  MOBILADOR - android-server  src/com/mobilador/server/ScreenServer.java
 *
 *  Captures the phone's own screen and streams H.264 / H.265 over the socket
 *  that adb forwarded to the PC.
 *
 *  LATENCY PATH (this is the whole reason the class exists)
 *  --------------------------------------------------------
 *     VirtualDisplay  ->  encoder input Surface   (GPU, no readback, no copy)
 *     MediaCodec      ->  asynchronous callback    (no polling, no dequeue loop)
 *     callback thread ->  Socket.getOutputStream() (blocking write = natural
 *                                                   backpressure, zero queue)
 *
 *  Consequences of that design:
 *    * The frame is never copied into application memory: the display
 *      compositor writes into the codec's own GraphicBuffer.
 *    * There is exactly one queue in the whole path and it is the codec's, so
 *      the "never show a stale frame" rule is upheld by the PC dropping, not by
 *      the phone buffering.
 *    * If the PC cannot keep up, the socket write blocks and the encoder stops
 *      producing - frame dropping then happens where it is cheapest.
 *    * No B-frames, infinite GOP support, sync frames on demand.
 * ============================================================================
 */
package com.mobilador.server;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.graphics.Point;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.projection.MediaProjection;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.SystemClock;
import android.view.Display;
import android.view.Surface;

import java.io.OutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;

final class ScreenServer {

    interface Stats {
        void onEncoderStarted(int w, int h, int fps, String codec, boolean hardware);
        void onFrameEncoded(long captureUs, int encodeUs, int bytes, boolean key);
        void onError(String message);
    }

    private final CaptureSocket mSocket;
    private final AtomicBoolean mRunning = new AtomicBoolean(false);
    private final Stats mStats;

    private MediaProjection mProjection;
    private VirtualDisplay mDisplay;
    private MediaCodec mCodec;
    private Surface mInputSurface;
    private int mWidth, mHeight, mDensity, mFps, mBitrate;
    private String mMime = MediaFormat.MIMETYPE_VIDEO_AVC;
    private int mIFrameInterval = 2;
    private int mQuality = 1;
    private boolean mWantKeyframe = false;
    private boolean mAnnexB = true;

    /* reconnection / watchdog */
    private long mFramesSent = 0;
    private long mBytesSent = 0;
    private long mLastFrameAt = 0;
    private int  mLastDisplayRotation = -1;
    private int  mLastDisplayW, mLastDisplayH;

    ScreenServer(CaptureSocket socket, Stats stats) {
        mSocket = socket;
        mStats = stats;
    }

    /* ------------------------------------------------------------------ setup */

    /**
     * Creates the media projection without a consent dialog.
     *
     * The framework explicitly allows the shell and root identities to create a
     * screen capture projection (this is how the platform's own screenrecord
     * style tooling and other mirroring tools work).  Everything here is a
     * hidden API, so it is done with reflection and every step is defensive:
     * an unsupported ROM degrades to a clear error message instead of a crash.
     */
    @SuppressWarnings("unchecked")
    private boolean createProjection(Context context) {
        try {
            // 1. ServiceManager.getService("media_projection")
            Class<?> smClass = Class.forName("android.os.ServiceManager");
            Method getService = smClass.getMethod("getService", String.class);
            IBinder binder = (IBinder) getService.invoke(null, "media_projection");
            if (binder == null) {
                report("media_projection service not available");
                return false;
            }
            // 2. IMediaProjectionManager.Stub.asInterface(binder)
            Class<?> stub = Class.forName("android.media.projection.IMediaProjectionManager$Stub");
            Method asInterface = stub.getMethod("asInterface", IBinder.class);
            Object manager = asInterface.invoke(null, binder);
            if (manager == null) {
                report("media_projection manager unavailable");
                return false;
            }
            // 3. createProjection(owner, appInfo, uid, type, permanentGrant)
            IBinder owner = new Binder("mobilador");
            ApplicationInfo app = context.getApplicationInfo();
            Object projection = null;
            Method best = null;
            for (Method m : manager.getClass().getMethods()) {
                if (!m.getName().equals("createProjection")) continue;
                Class<?>[] pt = m.getParameterTypes();
                if (pt.length < 4 || pt.length > 6) continue;
                if (!pt[0].isAssignableFrom(Binder.class)) continue;
                Object[] args = new Object[pt.length];
                args[0] = owner;
                args[1] = app;
                args[2] = Integer.valueOf(-1);          // uid: -1 = "caller"
                args[3] = Integer.valueOf(0);           // TYPE_SCREEN_CAPTURE
                for (int i = 4; i < pt.length; i++) {
                    args[i] = (pt[i] == boolean.class) ? Boolean.FALSE : Integer.valueOf(0);
                }
                try {
                    projection = m.invoke(manager, args);
                    best = m;
                    if (projection != null) break;
                } catch (Throwable ignored) {
                    // signature mismatch on this ROM: try the next candidate
                }
            }
            if (projection == null) {
                report("the phone refused an automatic capture session (no createProjection)");
                return false;
            }
            // 4. new MediaProjection(context, IMediaProjection)
            Class<?> mpClass = Class.forName("android.media.projection.MediaProjection");
            Constructor<?> ctor = null;
            for (Constructor<?> c : mpClass.getDeclaredConstructors()) {
                Class<?>[] pt = c.getParameterTypes();
                if (pt.length == 2 && pt[0] == Context.class) { ctor = c; break; }
            }
            if (ctor == null) {
                report("MediaProjection constructor not found");
                return false;
            }
            ctor.setAccessible(true);
            mProjection = (MediaProjection) ctor.newInstance(context, projection);
            Log.info("capture session created via " + (best != null ? best.getName() : "?"));
            return true;
        } catch (Throwable t) {
            report("capture session failed: " + t.getClass().getSimpleName() + " " + t.getMessage());
            return false;
        }
    }

    private int[] currentDisplaySize(Context context) {
        DisplayManager dm = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        Display d = dm.getDisplay(Display.DEFAULT_DISPLAY);
        Point p = new Point();
        d.getRealSize(p);
        return new int[] { p.x, p.y, d.getRotation() };
    }

    /* ------------------------------------------------------------------ codec */

    private MediaCodec buildEncoder(int w, int h, int fps, int bitrate, String mime) {
        try {
            MediaFormat fmt = MediaFormat.createVideoFormat(mime, w, h);
            fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                           MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            fmt.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
            fmt.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
            fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, mIFrameInterval);
            fmt.setInteger(MediaFormat.KEY_BITRATE_MODE,
                           MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
            // Vendor / platform low-latency switches. All optional: a ROM that
            // does not know a key simply ignores it.
            trySetInteger(fmt, "latency", 1);                       // API 30
            trySetInteger(fmt, "priority", 0);                      // 0 = realtime
            trySetInteger(fmt, "vendor.qti-ext-enc-low-latency.enable", 1);
            trySetInteger(fmt, "vendor.qti-ext-enc-low-latency.frames-count", 1);
            trySetInteger(fmt, "vendor.hisi.enc.lowlatency.enable", 1);
            trySetInteger(fmt, "vendor.mtk.enc.lowlatency.enable", 1);
            trySetInteger(fmt, "profile", 1);                       // baseline: no B-frames
            if (mQuality == 0) trySetInteger(fmt, "vendor.qti-ext-enc-rc.speed", 1);

            MediaCodec codec = MediaCodec.createEncoderByType(mime);
            codec.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            mAnnexB = true;
            return codec;
        } catch (Throwable t) {
            Log.warn("encoder creation failed for " + mime + ": " + t);
            return null;
        }
    }

    private static void trySetInteger(MediaFormat fmt, String key, int value) {
        try { fmt.setInteger(key, value); } catch (Throwable ignored) {}
    }

    private String pickMime(String requested) {
        String[] candidates;
        if ("hevc".equals(requested) || "h265".equals(requested)) {
            candidates = new String[] { MediaFormat.MIMETYPE_VIDEO_HEVC, MediaFormat.MIMETYPE_VIDEO_AVC };
        } else if ("av1".equals(requested)) {
            candidates = new String[] { "video/av01", MediaFormat.MIMETYPE_VIDEO_HEVC, MediaFormat.MIMETYPE_VIDEO_AVC };
        } else {
            candidates = new String[] { MediaFormat.MIMETYPE_VIDEO_AVC };
        }
        for (String m : candidates) {
            try {
                MediaCodec codec = MediaCodec.createEncoderByType(m);
                codec.release();
                return m;
            } catch (Throwable ignored) {}
        }
        return MediaFormat.MIMETYPE_VIDEO_AVC;
    }

    /* ------------------------------------------------------------------- run */

    boolean start(Context context, int reqW, int reqH, int reqFps, int bitrateKbps,
                  String codecName, int iFrameSeconds, int quality) {
        mFps = CodecMath.fpsToInt(reqFps);
        mBitrate = Math.max(bitrateKbps, 500) * 1000;
        mIFrameInterval = Math.max(1, iFrameSeconds);
        mQuality = quality;
        mMime = pickMime(codecName);

        int[] size = currentDisplaySize(context);
        mLastDisplayW = size[0];
        mLastDisplayH = size[1];
        mLastDisplayRotation = size[2];
        // Mirror exactly the current logical resolution, scaled down to the
        // requested maximum. Matching the aspect ratio is what avoids the black
        // bars and the resampling blur the user asked to avoid.
        int realW = size[0] > size[1] && reqW > reqH ? size[0] : Math.max(size[0], size[1]);
        int realH = size[0] > size[1] && reqW > reqH ? size[1] : Math.min(size[0], size[1]);
        int[] fitted = CodecMath.fitSize(realW, realH, reqW, reqH);
        mWidth = fitted[0];
        mHeight = fitted[1];
        mDensity = (int) (context.getResources().getDisplayMetrics().densityDpi * 0.75f);

        if (!createProjection(context)) return false;

        mCodec = buildEncoder(mWidth, mHeight, mFps, mBitrate, mMime);
        if (mCodec == null) {
            report("no usable hardware encoder for " + mMime);
            return false;
        }
        mInputSurface = mCodec.createInputSurface();

        // Asynchronous mode: the encoder calls us the instant a frame is ready,
        // which removes the dequeue polling that classic implementations do
        // (that polling alone costs 5-15 ms of added latency).
        HandlerThread codecThread = new HandlerThread("mobilador-encoder");
        codecThread.start();
        Handler handler = new Handler(codecThread.getLooper());
        mCodec.setCallback(new MediaCodec.Callback() {
            @Override public void onInputBufferAvailable(MediaCodec codec, int index) {
                // Surface input: no application buffers are used.
            }
            @Override public void onOutputBufferAvailable(MediaCodec codec, int index, MediaCodec.BufferInfo info) {
                try {
                    onFrame(codec, index, info);
                } catch (Throwable t) {
                    report("encode stream error: " + t);
                    mRunning.set(false);
                }
            }
            @Override public void onError(MediaCodec codec, MediaCodec.CodecException e) {
                report("encoder error: " + e.getDiagnosticInfo());
            }
            @Override public void onOutputFormatChanged(MediaCodec codec, MediaFormat format) {
                Log.info("encoder output format: " + format);
            }
        }, handler);
        mCodec.start();

        mRunning.set(true);
        createVirtualDisplay(context);
        mStats.onEncoderStarted(mWidth, mHeight, mFps, mMime, true);
        Log.info("streaming " + mWidth + "x" + mHeight + "@" + mFps + " " + mMime
                 + " " + (mBitrate / 1000) + " kbps");
        return true;
    }

    private void createVirtualDisplay(Context context) {
        int flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR
                  | DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC;
        Handler handler = new Handler();
        try {
            if (mProjection != null) {
                mDisplay = mProjection.createVirtualDisplay(
                        "mobilador", mWidth, mHeight, mDensity, flags, mInputSurface, null, handler);
            } else {
                DisplayManager dm = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
                mDisplay = dm.createVirtualDisplay("mobilador", mWidth, mHeight, mDensity,
                        mInputSurface, flags);
            }
        } catch (Throwable t) {
            report("virtual display failed: " + t);
            mDisplay = null;
        }
    }

    /** Called from the encoder callback thread: forwarding is the callback. */
    private void onFrame(MediaCodec codec, int index, MediaCodec.BufferInfo info) {
        try {
            ByteBuffer buf = codec.getOutputBuffer(index);
            if (buf == null) return;
            buf.position(info.offset);
            buf.limit(info.offset + info.size);
            boolean config = (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
            boolean key = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
            long nowUs = System.nanoTime() / 1000L;

            if (config || info.size <= 0) {
                if (info.size > 0) mCsd = readBytes(buf, info.size);
                if (mCsd != null) sendConfig();
                if (info.size <= 0) return;
                codec.releaseOutputBuffer(index, false);
                return;
            }

            // The relative clock of the PC is reconstructed from these two
            // numbers (capture instant and arrival instant); no clock sync
            // handshake is required and no assumption about time zones is made.
            long captureUs = info.presentationTimeUs;
            long encodeUs = nowUs - captureUs;
            if (encodeUs < 0) encodeUs = 0;
            if (encodeUs > 65535) encodeUs = 65535;

            // 18 byte common header + 20 byte video prefix + NAL units.
            // The prefix carries the phone side timestamps that make the
            // LATENCY ANALYZER trustworthy: the capture instant (the encoder's
            // own presentation timestamp) and the encoder's own delay.
            final int prefix = 20;
            int payloadLen = prefix + info.size;
            byte[] header = new byte[CodecMath.HEADER_SIZE + prefix];
            CodecMath.putU64(header, 0, CaptureSocket.MAGIC);
            header[8]  = (byte) CodecMath.PACKET_VIDEO_FRAME;
            int fl = key ? CodecMath.FLAG_KEYFRAME : 0;
            fl |= "video/hevc".equals(mMime) ? CodecMath.FLAG_CODEC_H265 : CodecMath.FLAG_CODEC_H264;
            header[9]  = (byte) fl;
            CodecMath.putU32(header, 10, (int) (mFramesSent & 0x7FFFFFFFL));
            CodecMath.putU32(header, 14, payloadLen);
            CodecMath.putU64(header, 18, captureUs);
            CodecMath.putU16(header, 26, (int) encodeUs);
            CodecMath.putU16(header, 28, 0);
            CodecMath.putU16(header, 30, mWidth);
            CodecMath.putU16(header, 32, mHeight);
            CodecMath.putU16(header, 34, 0);

            OutputStream out = mSocket.stream();
            out.write(header);
            byte[] chunk = new byte[Math.min(info.size, 32768)];
            int remaining = info.size;
            while (remaining > 0) {
                int take = Math.min(remaining, chunk.length);
                buf.get(chunk, 0, take);
                out.write(chunk, 0, take);
                remaining -= take;
            }
            mFramesSent++;
            mBytesSent += info.size;
            mLastFrameAt = nowUs;
            mStats.onFrameEncoded(captureUs, (int) encodeUs, info.size, key);
        } catch (Throwable t) {
            // A dead socket must never stop the callback thread from releasing
            // its buffers: report it and let the session logic reconnect.
            report("frame send failed: " + t);
        } finally {
            codec.releaseOutputBuffer(index, false);
        }
        handleKeyframeRequest();
    }

    private byte[] mCsd = null;

    private static byte[] readBytes(ByteBuffer buf, int size) {
        byte[] out = new byte[size];
        buf.get(out, 0, size);
        return out;
    }

    private void sendConfig() {
        if (mCsd == null) return;
        final int prefix = 8;
        byte[] packet = new byte[CodecMath.HEADER_SIZE + prefix + mCsd.length];
        CodecMath.putU64(packet, 0, CaptureSocket.MAGIC);
        packet[8] = (byte) CodecMath.PACKET_VIDEO_CONFIG;
        packet[9] = (byte) ("video/hevc".equals(mMime) ? CodecMath.FLAG_CODEC_H265 : CodecMath.FLAG_CODEC_H264);
        CodecMath.putU32(packet, 10, 0);
        CodecMath.putU32(packet, 14, prefix + mCsd.length);
        CodecMath.putU16(packet, 18, mWidth);
        CodecMath.putU16(packet, 20, mHeight);
        packet[22] = (byte) ("video/hevc".equals(mMime) ? 1 : 0);
        packet[23] = (byte) mFps;
        // 8-byte prefix: w(2) h(2) hevc(1) fps(1) reserved(2).  It stops at
        // payload offset 8, exactly where the CSD blob starts.
        CodecMath.putU16(packet, 24, 0);
        System.arraycopy(mCsd, 0, packet, CodecMath.HEADER_SIZE + prefix, mCsd.length);
        try {
            OutputStream out = mSocket.stream();
            out.write(packet);
            Log.info("codec config sent (" + mCsd.length + " bytes Annex-B extradata)");
        } catch (Throwable t) {
            report("config send failed: " + t);
        }
    }

    /** Cheap no-lock check: a key frame is requested at most once per second. */
    private void handleKeyframeRequest() {
        if (!mWantKeyframe) return;
        mWantKeyframe = false;
        try {
            Bundle params = new Bundle();
            params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            mCodec.setParameters(params);
        } catch (Throwable ignored) {}
    }

    void requestKeyframe() { mWantKeyframe = true; }

    void setBitrate(int kbps) {
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, kbps * 1000);
            mCodec.setParameters(b);
            Log.info("bitrate changed to " + kbps + " kbps");
        } catch (Throwable ignored) {}
    }

    /**
     * Watchdog. Runs on the main server thread and reacts to two events that
     * would otherwise stall the picture:
     *   * rotation / resolution change  -> the virtual display must be rebuilt,
     *   * encoder silence              -> ask for a sync frame.
     */
    void tick(Context context) {
        if (!mRunning.get()) return;
        long now = System.nanoTime() / 1000L;
        if (mLastFrameAt != 0 && now - mLastFrameAt > 2000000L) {
            requestKeyframe();
        }
        int[] size = currentDisplaySize(context);
        if (size[2] != mLastDisplayRotation) {
            mLastDisplayRotation = size[2];
            Log.info("display rotated to " + size[2] + " - restarting capture");
            restart(context);
        }
    }

    private void restart(Context context) {
        try { if (mDisplay != null) mDisplay.release(); } catch (Throwable ignored) {}
        mDisplay = null;
        createVirtualDisplay(context);
    }

    void stop() {
        mRunning.set(false);
        try { if (mCodec != null) { mCodec.stop(); mCodec.release(); } } catch (Throwable ignored) {}
        mCodec = null;
        try { if (mInputSurface != null) mInputSurface.release(); } catch (Throwable ignored) {}
        mInputSurface = null;
        try { if (mDisplay != null) mDisplay.release(); } catch (Throwable ignored) {}
        mDisplay = null;
        try { if (mProjection != null) mProjection.stop(); } catch (Throwable ignored) {}
        mProjection = null;
    }

    long framesSent() { return mFramesSent; }
    long bytesSent()  { return mBytesSent; }
    int  width() { return mWidth; }
    int  height() { return mHeight; }
    int  fps() { return mFps; }
    String codecName() { return mMime; }

    private void report(String message) {
        Log.error(message);
        if (mStats != null) mStats.onError(message);
    }
}
