/*
 * ============================================================================
 *  MOBILADOR - android-server  src/com/mobilador/server/CaptureSocket.java
 *
 *  Owns the video socket and the small control channel that shares it.
 *
 *  WHY ONE SOCKET FOR VIDEO AND CONTROL
 *  ------------------------------------
 *  'adb reverse tcp:PORT tcp:PORT' gives exactly one tunnel.  Multiplexing the
 *  rare control packets (key frame request, ping, quit) into the same stream
 *  keeps that single tunnel at full MTU efficiency for video, and it removes an
 *  entire extra round trip that a control connection would need.  Control
 *  packets are only ever sent from the PC to the phone, they are tiny, and they
 *  are read off the stream by a dedicated thread that never blocks the encoder.
 *
 *  A dedicated Receiver thread also means the socket is always drained, so a
 *  request from the PC is acted upon within microseconds even while frames are
 *  flowing.
 * ============================================================================
 */
package com.mobilador.server;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;

final class CaptureSocket {

    static final long MAGIC = 0x4D4F42494C41444FL;   // "MOBILADO"

    interface ControlHandler {
        void onKeyframeRequest();
        void onPing(long clientUs, long phoneUs);
        void onBitrateChange(int kbps);
        void onQuit();
        void onStreamEnded();
    }

    private ServerSocket mServer;

       private Socket mClient;
    private OutputStream mOut;
    private InputStream mIn;
    private Thread mReader;
    private volatile boolean mRunning;
    private ControlHandler mHandler;
    private int mPort;
    private long mLastPacketAt;
    private volatile long mBytesOut;

    boolean listen(int port) {
        try {
            mServer = new ServerSocket();
            mServer.setReuseAddress(true);
            // Bound to loopback only: adb reverse does the rest, so the stream
            // is never exposed on the network.
            mServer.bind(new java.net.InetSocketAddress("127.0.0.1", port), 1);
            mPort = port;
            Log.info("listening on 127.0.0.1:" + port);
            return true;
        } catch (IOException e) {
            Log.error("cannot bind port " + port + ": " + e);
            return false;
        }
    }

    /** Blocks until the PC connects. Returns false on timeout. */
    boolean accept(int timeoutMs, ControlHandler handler) {
        mHandler = handler;
        try {
            if (mServer == null) return false;
            mServer.setSoTimeout(timeoutMs);
            mClient = mServer.accept();
            mClient.setTcpNoDelay(true);
            mClient.setReceiveBufferSize(256 * 1024);
            mClient.setSendBufferSize(4 * 1024 * 1024);   // deep enough for 4K key frames
            mOut = mClient.getOutputStream();
            mIn  = mClient.getInputStream();
            mRunning = true;
            mReader = new Thread(new Runnable() {
                @Override public void run() { readLoop(); }
            }, "mobilador-control");
            mReader.setDaemon(true);
            mReader.start();
            Log.info("client connected from " + mClient.getInetAddress());
            return true;
        } catch (IOException e) {
            Log.warn("no client: " + e.getMessage());
            return false;
        }
    }

    OutputStream stream() { return mOut; }
    boolean connected()   { return mRunning && mClient != null && mClient.isConnected(); }
    long lastPacketAt()   { return mLastPacketAt; }
    long bytesOut()       { return mBytesOut; }
    int  port()           { return mPort; }

    /**
     * Reads control packets. The format mirrors the PC side: 8 byte magic,
     * 1 byte type, 1 byte flags, 4 byte sequence, 8 byte payload length.
     */
    private void readLoop() {
        byte[] header = new byte[64];
        try {
            ByteBuffer hb = ByteBuffer.wrap(header);
            while (mRunning) {
                if (!readFully(mIn, header, CodecMath.HEADER_SIZE)) break;
                long magic = 0;
                for (int i = 0; i < 8; i++) magic = (magic << 8) | (header[i] & 0xFF);
                if (magic != MAGIC) {
                    Log.error("control stream desynchronised - closing");
                    break;
                }
                int type = header[8] & 0xFF;
                long len = CodecMath.getU32(hb, 14);
                if (len > 65536) { Log.error("oversized control packet"); break; }
                byte[] payload = new byte[(int) len];
                if (len > 0 && !readFully(mIn, payload, (int) len)) break;
                ByteBuffer pb = ByteBuffer.wrap(payload);
                mLastPacketAt = System.nanoTime() / 1000L;
                switch (type) {
                    case CodecMath.PACKET_KEYFRAME_REQ:
                        if (mHandler != null) mHandler.onKeyframeRequest();
                        break;
                    case CodecMath.PACKET_PING: {
                        int probeId = (int) CodecMath.getU32(pb, 0);
                        long clientUs = len >= 16 ? pb.getLong(8) : 0;
                        long phoneUs = System.nanoTime() / 1000L;
                        sendPong(probeId, phoneUs);
                        if (mHandler != null) mHandler.onPing(clientUs, phoneUs);
                        break;
                    }
                    case CodecMath.PACKET_QUIT:
                        if (mHandler != null) mHandler.onQuit();
                        mRunning = false;
                        break;
                    default:
                        break;   // input packets arrive on their own socket
                }
            }
        } catch (Throwable t) {
            if (mRunning) Log.warn("control channel closed: " + t.getMessage());
        }
        mRunning = false;
        if (mHandler != null) mHandler.onStreamEnded();
    }

    private void sendPong(int probeId, long phoneUs) {
        try {
            byte[] p = new byte[CodecMath.HEADER_SIZE + 16];
            CodecMath.putU64(p, 0, MAGIC);
            p[8] = (byte) CodecMath.PACKET_PONG;
            CodecMath.putU32(p, 10, probeId);
            CodecMath.putU32(p, 14, 16);
            CodecMath.putU32(p, 18, probeId);
            CodecMath.putU32(p, 22, 0);
            CodecMath.putU64(p, 26, phoneUs);
            synchronized (this) {
                if (mOut != null) { mOut.write(p); mOut.flush(); }
            }
            mBytesOut += p.length;
        } catch (Throwable ignored) {}
    }

    /** Sends a control packet (or any header+payload pair) atomically. */
    boolean send(byte[] header, byte[] payload) {
        try {
            synchronized (this) {
                if (mOut == null) return false;
                mOut.write(header);
                if (payload != null && payload.length > 0) mOut.write(payload);
                mBytesOut += header.length + (payload == null ? 0 : payload.length);
            }
            return true;
        } catch (Throwable t) {
            mRunning = false;
            return false;
        }
    }

    static boolean readFully(InputStream in, byte[] buf, int len) throws IOException {
        int off = 0;
        while (off < len) {
            int n = in.read(buf, off, len - off);
            if (n < 0) return false;
            off += n;
        }
        return true;
    }

    void close() {
        mRunning = false;
        try { if (mClient != null) mClient.close(); } catch (Throwable ignored) {}
        try { if (mServer != null) mServer.close(); } catch (Throwable ignored) {}
        mClient = null;
        mOut = null;
        mIn = null;
    }
}
