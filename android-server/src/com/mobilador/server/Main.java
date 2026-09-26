/*
 * ============================================================================
 *  MOBILADOR - android-server  src/com/mobilador/server/Main.java
 *
 *  Entry point.  Runs inside the phone as the shell user, started by the PC
 *  through the standard platform mechanism:
 *
 *      adb shell CLASSPATH=/data/local/tmp/mobilador.dex \
 *                app_process /system/bin com.mobilador.server.Main --port 27183
 *
 *  There is no APK, no UI and no permission prompt: the server exists only to
 *  move pixels and events, and it is removed again when the session ends.
 *
 *  PROCESS DESIGN
 *  --------------
 *   * main thread      : argument parsing, device info, watchdog ticks (10 Hz)
 *   * "mobilador-encoder": MediaCodec callback thread, writes video to the socket
 *   * "mobilador-control": reads control packets from the same socket
 *   * "mobilador-input"  : reads input packets and injects them
 *   * "mobilador-audio"  : optional audio encode + forward
 *  Nothing else runs.  No wake locks beyond the ones the platform already holds
 *  while adb is connected, no polling loops, no timers that fire per frame.
 * ============================================================================
 */
package com.mobilador.server;

import android.content.Context;
import android.hardware.display.DisplayManager;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.os.Build;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.view.Display;

import java.lang.reflect.Method;

public final class Main {

    /* ------------------------------------------------------------- arguments */
    private int mPort = 27183;
    private int mInputPort = 27184;
    private int mAudioPort = 27185;
    private int mWidth = 1920, mHeight = 1080;
    private int mFps = 60;
    private int mBitrateKbps = 12000;
    private String mCodec = "h264";
    private int mIFrame = 2;
    private int mQuality = 1;
    private boolean mAudioEnabled = false;
    private boolean mPreferKernelInput = true;
    private int mMaxSeconds = 0;         // 0 = run until stopped

    private Context mContext;
    private CaptureSocket mCapture;
    private ScreenServer mScreen;
    private InputServer mInput;
    private AudioServer mAudio;
    private volatile boolean mRunning = true;
    private long mStartedAt;

    public static void main(String[] args) {
        // Hidden API access is required (media projection, InputManager).  The
        // process is started by the platform and is not subject to the
        // restrictions that apply to an installed app, but newer releases still
        // consult the exempt list, so it is set explicitly.
        allowHiddenApis();
        if (Looper.myLooper() == null) Looper.prepare();

        Main m = new Main();
        int rc = m.run(args);
        System.exit(rc);
    }

    private static void allowHiddenApis() {
        try {
            Class<?> vmRuntime = Class.forName("dalvik.system.VMRuntime");
            Method getRuntime = vmRuntime.getMethod("getRuntime");
            Object runtime = getRuntime.invoke(null);
            Method setExemptions = vmRuntime.getMethod("setHiddenApiExemptions", String[].class);
            setExemptions.invoke(runtime, (Object) new String[] { "L" });
            Log.debug("hidden API exemptions applied");
        } catch (Throwable t) {
            Log.debug("hidden API exemptions unavailable: " + t);
        }
    }

    private int run(String[] args) {
        parse(args);
        mStartedAt = SystemClock.elapsedRealtime();

        // The server is a bridge; it must never compete with the game.  A
        // priority *below* normal keeps the game's render thread ahead of us,
        // while the encoder and input threads are raised individually.
        try { Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND); } catch (Throwable ignored) {}

        mContext = systemContext();
        if (mContext == null) {
            Log.error("no system context - cannot continue");
            return 2;
        }

        Log.info("Mobilador server " + BuildConfig.VERSION + " on " + Build.MODEL
                 + " (Android " + Build.VERSION.RELEASE + ", API " + Build.VERSION.SDK_INT + ")");

        /* ---------------------------------------------------------- video */
        mCapture = new CaptureSocket();
        if (!mCapture.listen(mPort)) return 3;

        mScreen = new ScreenServer(mCapture, new ScreenServer.Stats() {
            @Override public void onEncoderStarted(int w, int h, int fps, String codec, boolean hw) {
                sendDeviceInfoOnce();
            }
            @Override public void onFrameEncoded(long captureUs, int encodeUs, int bytes, boolean key) {}
            @Override public void onError(String message) {
                Log.error("screen: " + message);
            }
        });

        /* ---------------------------------------------------------- input */
        mInput = new InputServer();
        mInput.listen(mInputPort);

        /* ---------------------------------------------------------- audio */
        if (mAudioEnabled) {
            mAudio = new AudioServer();
            mAudio.listen(mAudioPort);
        }

        Log.info("waiting for the PC client on port " + mPort + "...");
        if (!mCapture.accept(20000, new CaptureSocket.ControlHandler() {
            @Override public void onKeyframeRequest() { if (mScreen != null) mScreen.requestKeyframe(); }
            @Override public void onPing(long clientUs, long phoneUs) { /* pong already sent */ }
            @Override public void onBitrateChange(int kbps) { if (mScreen != null) mScreen.setBitrate(kbps); }
            @Override public void onQuit() { mRunning = false; }
            @Override public void onStreamEnded() { mRunning = false; }
        })) {
            Log.error("the PC never connected - exiting");
            mCapture.close();
            return 4;
        }

        if (!mScreen.start(mContext, mWidth, mHeight, mFps, mBitrateKbps, mCodec, mIFrame, mQuality)) {
            Log.error("capture could not start");
            mCapture.close();
            return 5;
        }

        DisplayManager dm = (DisplayManager) mContext.getSystemService(Context.DISPLAY_SERVICE);
        Display d = dm.getDisplay(Display.DEFAULT_DISPLAY);
        android.graphics.Point p = new android.graphics.Point();
        d.getRealSize(p);
        mInput.setScreenSize(mScreen.width(), mScreen.height());
        mInput.start(new InputServer.Listener() {
            @Override public void onInputError(String message) { Log.warn("input: " + message); }
            @Override public void onInjectionBackend(String backend) { sendBackendInfo(backend); }
        }, mPreferKernelInput);

        if (mAudio != null) mAudio.start(mContext, mBitrateKbps);

        /* ------------------------------------------------------- watchdog */
        // 10 Hz is enough to notice rotation and to nudge a stalled encoder, and
        // it costs nothing measurable.
        long lastTick = 0;
        while (mRunning) {
            long now = SystemClock.elapsedRealtime();
            if (now - lastTick >= 100) {
                lastTick = now;
                mScreen.tick(mContext);
                if (!mCapture.connected()) {
                    Log.warn("PC disconnected");
                    break;
                }
                if (mMaxSeconds > 0 && (now - mStartedAt) > mMaxSeconds * 1000L) {
                    Log.info("maximum session time reached");
                    break;
                }
            }
            try { Thread.sleep(20); } catch (InterruptedException ignored) {}
        }

        Log.info("shutting down (sent " + mScreen.framesSent() + " frames, "
                 + (mScreen.bytesSent() / 1048576L) + " MB, " + mInput.eventsInjected() + " input events)");
        if (mAudio != null) mAudio.stop();
        mInput.stop();
        mScreen.stop();
        mCapture.close();
        return 0;
    }

    private void parse(String[] args) {
        for (int i = 0; i < args.length; i++) {
            String a = args[i];
            try {
                if (a.equals("--port") && i + 1 < args.length) mPort = Integer.parseInt(args[++i]);
                else if (a.equals("--input-port") && i + 1 < args.length) mInputPort = Integer.parseInt(args[++i]);
                else if (a.equals("--audio-port") && i + 1 < args.length) mAudioPort = Integer.parseInt(args[++i]);
                else if (a.equals("--width") && i + 1 < args.length) mWidth = Integer.parseInt(args[++i]);
                else if (a.equals("--height") && i + 1 < args.length) mHeight = Integer.parseInt(args[++i]);
                else if (a.equals("--fps") && i + 1 < args.length) mFps = Integer.parseInt(args[++i]);
                else if (a.equals("--bitrate") && i + 1 < args.length) mBitrateKbps = Integer.parseInt(args[++i]);
                else if (a.equals("--codec") && i + 1 < args.length) mCodec = args[++i];
                else if (a.equals("--iframe") && i + 1 < args.length) mIFrame = Integer.parseInt(args[++i]);
                else if (a.equals("--quality") && i + 1 < args.length) mQuality = Integer.parseInt(args[++i]);
                else if (a.equals("--audio")) mAudioEnabled = true;
                else if (a.equals("--no-audio")) mAudioEnabled = false;
                else if (a.equals("--framework-input")) mPreferKernelInput = false;
                else if (a.equals("--max-seconds") && i + 1 < args.length) mMaxSeconds = Integer.parseInt(args[++i]);
            } catch (Throwable t) {
                Log.warn("bad argument " + a + ": " + t.getMessage());
            }
        }
    }

    /**
     * app_process has no ActivityThread, so ActivityThread.systemMain() is used
     * to obtain a real Context.  This is the same trick the platform's own
     * command line tools use.
     */
    private Context systemContext() {
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Method systemMain = at.getMethod("systemMain");
            Object thread = systemMain.invoke(null);
            Method getSystemContext = at.getMethod("getSystemContext");
            return (Context) getSystemContext.invoke(thread);
        } catch (Throwable t) {
            Log.error("systemMain failed: " + t);
            return null;
        }
    }

    /* --------------------------------------------------------- device info */

    private volatile boolean mInfoSent = false;

    private void sendDeviceInfoOnce() {
        if (mInfoSent) return;
        mInfoSent = true;
        StringBuilder sb = new StringBuilder();
        sb.append('{');
        sb.append("\"model\":\"").append(esc(Build.MANUFACTURER)).append(' ').append(esc(Build.MODEL)).append("\",");
        sb.append("\"android\":\"").append(esc(Build.VERSION.RELEASE)).append("\",");
        sb.append("\"sdk\":").append(Build.VERSION.SDK_INT).append(',');
        sb.append("\"device\":\"").append(esc(Build.DEVICE)).append("\",");
        sb.append("\"abi\":\"").append(esc(Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?")).append("\",");
        try {
            DisplayManager dm = (DisplayManager) mContext.getSystemService(Context.DISPLAY_SERVICE);
            Display d = dm.getDisplay(Display.DEFAULT_DISPLAY);
            android.graphics.Point p = new android.graphics.Point();
            d.getRealSize(p);
            sb.append("\"display_w\":").append(p.x).append(',');
            sb.append("\"display_h\":").append(p.y).append(',');
            sb.append("\"refresh_hz\":").append(String.format("%.2f", d.getRefreshRate())).append(',');
            if (Build.VERSION.SDK_INT >= 23) {
                float best = d.getRefreshRate();
                for (Display.Mode mode : d.getSupportedModes()) {
                    if (mode.getPhysicalWidth() == p.x && mode.getPhysicalHeight() == p.y) {
                        if (mode.getRefreshRate() > best) best = mode.getRefreshRate();
                    }
                }
                sb.append("\"max_hz\":").append(String.format("%.2f", best)).append(',');
            } else {
                sb.append("\"max_hz\":").append(String.format("%.2f", d.getRefreshRate())).append(',');
            }
        } catch (Throwable t) {
            sb.append("\"display_w\":1080,\"display_h\":1920,\"refresh_hz\":60,\"max_hz\":60,");
        }
        sb.append("\"encoders\":[").append(encoderList()).append("],");
        sb.append("\"decoders\":[").append(decoderList()).append("]");
        sb.append('}');
        sendTextPacket(CodecMath.PACKET_DEVICE_INFO, sb.toString());
    }

    private void sendBackendInfo(String backend) {
        sendTextPacket(CodecMath.PACKET_STATS, "{\"input_backend\":\"" + esc(backend) + "\"}");
    }

    private void sendTextPacket(int type, String text) {
        try {
            byte[] payload = text.getBytes("UTF-8");
            byte[] packet = new byte[CodecMath.HEADER_SIZE + payload.length];
            CodecMath.putU64(packet, 0, CaptureSocket.MAGIC);
            packet[8] = (byte) type;
            CodecMath.putU32(packet, 10, 0);
            CodecMath.putU32(packet, 14, payload.length);
            System.arraycopy(payload, 0, packet, CodecMath.HEADER_SIZE, payload.length);
            mCapture.send(packet, null);
        } catch (Throwable t) {
            Log.warn("send " + type + " failed: " + t);
        }
    }

    private static String esc(String s) {
        if (s == null) return "";
        return s.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    private static String encoderList() {
        StringBuilder sb = new StringBuilder();
        try {
            MediaCodecList list = new MediaCodecList(MediaCodecList.ALL_CODECS);
            for (MediaCodecInfo info : list.getCodecInfos()) {
                if (!info.isEncoder()) continue;
                for (String t : info.getSupportedTypes()) {
                    String type = t.toLowerCase();
                    if (!type.startsWith("video/")) continue;
                    if (type.contains("avc") || type.contains("hevc") || type.contains("av01")) {
                        if (sb.length() > 0) sb.append(',');
                        sb.append("\"").append(info.getName()).append(':').append(t).append('"');
                    }
                }
            }
        } catch (Throwable ignored) {}
        return sb.toString();
    }

    private static String decoderList() {
        StringBuilder sb = new StringBuilder();
        try {
            MediaCodecList list = new MediaCodecList(MediaCodecList.ALL_CODECS);
            for (MediaCodecInfo info : list.getCodecInfos()) {
                if (info.isEncoder()) continue;
                boolean hw = true;
                try { hw = info.isHardwareAccelerated(); } catch (Throwable ignored) {}
                for (String t : info.getSupportedTypes()) {
                    String type = t.toLowerCase();
                    if (!type.startsWith("video/")) continue;
                    if (type.contains("avc") || type.contains("hevc") || type.contains("av01")) {
                        if (sb.length() > 0) sb.append(',');
                        sb.append("{\"name\":\"").append(info.getName())
                          .append("\",\"type\":\"").append(t)
                          .append("\",\"hw\":").append(hw).append('}');
                    }
                }
            }
        } catch (Throwable ignored) {}
        return sb.toString();
    }
}

/** Version string kept in one place so the PC can compare it after an update. */
final class BuildConfig {
    static final String VERSION = "1.0.0";
    private BuildConfig() {}
}
