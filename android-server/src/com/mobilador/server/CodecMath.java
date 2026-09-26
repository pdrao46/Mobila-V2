/*
 * ============================================================================
 *  MOBILADOR - android-server  src/com/mobilador/server/CodecMath.java
 *
 *  Annex-B / AVCC helpers and the codec-info packet builder.
 *
 *  WHY THIS CLASS EXISTS
 *  MediaCodec emits "csd-0/csd-1" buffers holding SPS/PPS (H.264) or
 *  VPS/SPS/PPS (H.265).  A decoder on the PC can be configured directly from
 *  them, which is what removes the need for an SDP negotiation or an RTSP
 *  session (both add a round trip and a buffer).  This class slices Annex-B
 *  start codes, parses the NAL length prefix mode and rebuilds the decoder
 *  specific data blob the Windows client expects.
 *
 *  Threading: stateless, called only from the capture thread.
 * ============================================================================
 */
package com.mobilador.server;

import java.nio.ByteBuffer;

final class CodecMath {

    static final int PACKET_NONE         = 0;
    static final int PACKET_VIDEO_CONFIG = 1;
    static final int PACKET_VIDEO_FRAME  = 2;
    static final int PACKET_FRAME_META   = 3;
    static final int PACKET_PING         = 4;
    static final int PACKET_PONG         = 5;
    static final int PACKET_KEYFRAME_REQ = 6;
    static final int PACKET_AUDIO_CONFIG = 7;
    static final int PACKET_AUDIO_FRAME  = 8;
    static final int PACKET_STATS        = 9;
    static final int PACKET_DEVICE_INFO  = 10;
    static final int PACKET_TOUCH        = 20;
    static final int PACKET_KEY          = 21;
    static final int PACKET_MOUSE_MOVE   = 22;
    static final int PACKET_MOUSE_BUTTON = 23;
    static final int PACKET_SCROLL       = 24;
    static final int PACKET_GAMEPAD      = 25;
    static final int PACKET_EVENT_BASE   = 20;
    static final int PACKET_EVENT_END    = 29;
    static final int PACKET_QUIT         = 30;
    static final int PACKET_KEYFRAME     = 31;

    /* frame flags (10th byte of a video frame header) */
    static final int FLAG_KEYFRAME = 1;
    static final int FLAG_CONFIG   = 2;
    static final int FLAG_CODEC_H264 = 0;
    static final int FLAG_CODEC_H265 = 4;

    private CodecMath() {}

    /* ---------------------------------------------------------------- framing */

    static void putU16(byte[] b, int off, int v) {
        b[off]     = (byte) ((v >>> 8) & 0xFF);
        b[off + 1] = (byte) (v & 0xFF);
    }

    static void putU32(byte[] b, int off, long v) {
        b[off]     = (byte) ((v >>> 24) & 0xFF);
        b[off + 1] = (byte) ((v >>> 16) & 0xFF);
        b[off + 2] = (byte) ((v >>> 8) & 0xFF);
        b[off + 3] = (byte) (v & 0xFF);
    }

    static void putI32(byte[] b, int off, int v) {
        b[off]     = (byte) ((v >>> 24) & 0xFF);
        b[off + 1] = (byte) ((v >>> 16) & 0xFF);
        b[off + 2] = (byte) ((v >>> 8) & 0xFF);
        b[off + 3] = (byte) (v & 0xFF);
    }

    static void putU64(byte[] b, int off, long v) {
        for (int i = 0; i < 8; i++) {
            b[off + i] = (byte) ((v >>> (56 - 8 * i)) & 0xFF);
        }
    }

    static long getU32(ByteBuffer b, int off) {
        return ((long) (b.get(off) & 0xFF) << 24) | ((b.get(off + 1) & 0xFF) << 16)
             | ((b.get(off + 2) & 0xFF) << 8) | (b.get(off + 3) & 0xFF);
    }

    static int getU16(ByteBuffer b, int off) {
        return ((b.get(off) & 0xFF) << 8) | (b.get(off + 1) & 0xFF);
    }

    /**
     * Every packet carries the same 18 byte header:
     *
     *   offset  size  field
     *   0       8     magic  "MOBILADO"
     *   8       1     type
     *   9       1     flags
     *   10      4     sequence number
     *   14      4     payload length (bytes that follow the header)
     *
     * A single header shape keeps both ends trivially bufferable: the reader
     * always consumes exactly 18 bytes, then exactly payload_len bytes.
     */
    static final int HEADER_SIZE = 18;

    static int headerSize(int type) {
        return HEADER_SIZE;
    }

    /* ------------------------------------------------------------ NAL parsing */

    /** Finds the first Annex-B start code and returns its length (3 or 4) or 0. */
    static int findStartCode(ByteBuffer buf, int offset, int end) {
        for (int i = offset; i + 3 <= end; i++) {
            if (buf.get(i) == 0 && buf.get(i + 1) == 0) {
                if (buf.get(i + 2) == 1) return 3;
                if (i + 4 <= end && buf.get(i + 2) == 0 && buf.get(i + 3) == 1) return 4;
            }
        }
        return 0;
    }

    /**
     * Builds decoder specific data as plain Annex-B (start codes preserved),
     * which is what Media Foundation expects when we hand it the extradata.
     * Also returns the SPS/PPS NAL units concatenated for MediaCodec's
     * "csd" format on older devices.
     */
    static byte[] concatCsd(ByteBuffer csd0, ByteBuffer csd1) {
        int n0 = csd0 == null ? 0 : csd0.remaining();
        int n1 = csd1 == null ? 0 : csd1.remaining();
        byte[] out = new byte[n0 + n1];
        if (n0 > 0) csd0.get(out, 0, n0);
        if (n1 > 0) csd1.get(out, n0, n1);
        return out;
    }

    /** Strips a 4-byte length prefix when the codec produces AVCC (rare on
     *  Android, but MediaCodec on a few vendor builds does it). */
    static boolean looksAnnexB(ByteBuffer buf, int offset, int length) {
        if (length < 4) return false;
        if (buf.get(offset) == 0 && buf.get(offset + 1) == 0) {
            if (buf.get(offset + 2) == 1) return true;
            if (length >= 5 && buf.get(offset + 2) == 0 && buf.get(offset + 3) == 1) return true;
        }
        return false;
    }

    /* ---------------------------------------------------------- display utils */

    /** Largest size with the requested aspect that fits both limits. */
    static int[] fitSize(int maxW, int maxH, int limitW, int limitH) {
        int w = maxW, h = maxH;
        if (w <= 0 || h <= 0) { w = limitW; h = limitH; }
        if (w > limitW || h > limitH) {
            double s = Math.min(limitW / (double) w, limitH / (double) h);
            w = (int) (w * s);
            h = (int) (h * s);
        }
        // encoders want even dimensions
        w &= ~1;
        h &= ~1;
        if (w < 2) w = 2;
        if (h < 2) h = 2;
        return new int[] { w, h };
    }

    /** MediaCodec quality/bitrate rates must be integers per second. */
    static int fpsToInt(int fps) {
        if (fps <= 0) return 60;
        if (fps > 240) return 240;
        return fps;
    }
}
