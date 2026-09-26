/*
 * ============================================================================
 *  MOBILADOR - android-server  src/com/mobilador/server/AudioServer.java
 *
 *  Optional: forwards the phone's audio output to the PC as AAC.
 *
 *  It is optional on purpose.  Audio needs its own capture buffer, its own
 *  encoder and its own socket, and every one of those adds 20-60 ms of jitter
 *  to the *video* socket if it were multiplexed into it.  Keeping it on a
 *  separate port means turning it on never affects the latency of the picture.
 * ============================================================================
 */
package com.mobilador.server;

import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Build;
import android.os.SystemClock;

import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;

final class AudioServer {

    private static final int SAMPLE_RATE = 44100;
    private static final int CHANNELS = 2;
    private static final int BITRATE = 128000;

    private ServerSocket mServer;
    private Socket mClient;
    private Thread mThread;
    private volatile boolean mRunning;
    private AudioRecord mRecord;
    private MediaCodec mCodec;
    private int mPort;

    boolean listen(int port) {
        mPort = port;
        try {
            mServer = new ServerSocket();
            mServer.setReuseAddress(true);
            mServer.bind(new InetSocketAddress("127.0.0.1", port), 1);
            Log.info("audio channel on 127.0.0.1:" + port);
            return true;
        } catch (Throwable t) {
            Log.warn("audio bind failed: " + t);
            return false;
        }
    }

    void start(final Context context, final int bitrateKbps) {
        mRunning = true;
        mThread = new Thread(new Runnable() {
            @Override public void run() { runLoop(context, bitrateKbps); }
        }, "mobilador-audio");
        mThread.setDaemon(true);
        mThread.start();
    }

    void stop() {
        mRunning = false;
        try { if (mRecord != null) mRecord.stop(); } catch (Throwable ignored) {}
        try { if (mCodec != null) { mCodec.stop(); mCodec.release(); } } catch (Throwable ignored) {}
        try { if (mClient != null) mClient.close(); } catch (Throwable ignored) {}
        try { if (mServer != null) mServer.close(); } catch (Throwable ignored) {}
    }

    private void runLoop(Context context, int bitrateKbps) {
        try {
            mServer.setSoTimeout(5000);
            mClient = mServer.accept();
            mClient.setTcpNoDelay(true);
            OutputStream out = mClient.getOutputStream();

            int minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_STEREO,
                                                      AudioFormat.ENCODING_PCM_16BIT);
            int bufferSize = Math.max(minBuf, SAMPLE_RATE / 10 * 4);
            mRecord = createRecord(context, bufferSize);
            if (mRecord == null) { Log.error("audio capture unavailable on this device"); return; }
            mRecord.startRecording();

            MediaFormat fmt = MediaFormat.createVideoFormat("audio/mp4a-latm", SAMPLE_RATE, CHANNELS);
            fmt.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC);
            fmt.setInteger(MediaFormat.KEY_BIT_RATE, Math.max(bitrateKbps, 32) * 1000);
            fmt.setInteger(MediaFormat.KEY_CHANNEL_COUNT, CHANNELS);
            fmt.setInteger(MediaFormat.KEY_SAMPLE_RATE, SAMPLE_RATE);
            fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, bufferSize);
            mCodec = MediaCodec.createEncoderByType("audio/mp4a-latm");
            mCodec.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);

            // Audio uses the classic API on purpose: the asynchronous callback is
            // unnecessary here because a 5 ms poll is far below the audio frame
            // duration and keeps the code compact.
            mCodec.start();
            byte[] pcm = new byte[bufferSize];
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            Log.info("audio forwarding active (" + (bitrateKbps) + " kbps AAC)");
            while (mRunning) {
                int read = mRecord.read(pcm, 0, pcm.length);
                if (read <= 0) continue;
                int index = mCodec.dequeueInputBuffer(2000);
                if (index < 0) continue;
                ByteBuffer in = mCodec.getInputBuffer(index);
                in.clear();
                in.put(pcm, 0, read);
                mCodec.queueInputBuffer(index, 0, read, System.nanoTime() / 1000L, 0);
                for (;;) {
                    int oi = mCodec.dequeueOutputBuffer(info, 0);
                    if (oi < 0) break;
                    ByteBuffer ob = mCodec.getOutputBuffer(oi);
                    if (info.size > 0 && ob != null) {
                        final int prefix = 12;
                        byte[] header = new byte[CodecMath.HEADER_SIZE + prefix];
                        CodecMath.putU64(header, 0, CaptureSocket.MAGIC);
                        header[8] = (byte) CodecMath.PACKET_AUDIO_FRAME;
                        CodecMath.putU32(header, 10, 0);
                        CodecMath.putU32(header, 14, prefix + info.size);
                        CodecMath.putU64(header, 18, info.presentationTimeUs);
                        CodecMath.putU32(header, 26, info.size);
                        out.write(header);
                        byte[] chunk = new byte[info.size];
                        ob.position(info.offset);
                        ob.get(chunk);
                        out.write(chunk);
                    }
                    mCodec.releaseOutputBuffer(oi, false);
                }
            }
        } catch (Throwable t) {
            if (mRunning) Log.warn("audio ended: " + t);
        }
    }

    /** REMOTE_SUBMIX (8) is the mixer output; available to the shell identity. */
    private AudioRecord createRecord(Context context, int bufferSize) {
        try {
            return new AudioRecord(8 /* REMOTE_SUBMIX */, SAMPLE_RATE, AudioFormat.CHANNEL_IN_STEREO,
                                   AudioFormat.ENCODING_PCM_16BIT, bufferSize);
        } catch (Throwable t) {
            Log.warn("REMOTE_SUBMIX not permitted: " + t.getMessage());
        }
        if (Build.VERSION.SDK_INT >= 29) {
            // Fall back to the documented capture API. It requires the same
            // projection the screen capture already owns.
            Log.warn("audio needs the playback capture configuration on this ROM");
        }
        return null;
    }

    int port() { return mPort; }
}
