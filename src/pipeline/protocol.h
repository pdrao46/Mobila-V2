// ============================================================================
//  MOBILADOR - src/pipeline/protocol.h
//  Wire format shared by the Windows client and the on-device server module.
//  Keep in sync with android-server/src/com/mobilador/server/CodecMath.java.
//
//  Every packet, in both directions, carries the same 18 byte header:
//
//    offset  size  field
//    0       8     magic "MOBILADO" (0x4D4F42494C41444F)
//    8       1     type
//    9       1     flags (codec id / key frame for video)
//    10      4     sequence number
//    14      4     payload length
//
//  A fixed header means the reader never has to search for a delimiter, never
//  reallocates, and can be implemented as "read 18 bytes, then read length".
//  Video payloads are written straight into a preallocated frame buffer, so a
//  frame costs exactly one heap-free copy from the socket.
// ============================================================================
#pragma once

#include "../core/base.h"

namespace mob {

#define MOB_PROTO_MAGIC 0x4D4F42494C41444FULL
#define MOB_PROTO_VERSION 1

enum PacketType : u8 {
    PKT_NONE          = 0,
    PKT_VIDEO_CONFIG  = 1,
    PKT_VIDEO_FRAME   = 2,
    PKT_FRAME_META    = 3,
    PKT_PING          = 4,
    PKT_PONG          = 5,
    PKT_KEYFRAME_REQ  = 6,
    PKT_AUDIO_CONFIG  = 7,
    PKT_AUDIO_FRAME   = 8,
    PKT_STATS         = 9,
    PKT_DEVICE_INFO   = 10,
    PKT_TOUCH         = 20,
    PKT_KEY           = 21,
    PKT_MOUSE_MOVE    = 22,
    PKT_MOUSE_BUTTON  = 23,
    PKT_SCROLL        = 24,
    PKT_GAMEPAD       = 25,
    PKT_QUIT          = 30,
};

enum PacketFlags : u8 {
    PKT_FLAG_KEYFRAME = 1,
    PKT_FLAG_CONFIG   = 2,
    PKT_FLAG_CODEC_H264 = 0,
    PKT_FLAG_CODEC_H265 = 4,
};

#define MOB_PROTO_HEADER 18

enum VideoCodecId : u8 { VCODEC_H264 = 0, VCODEC_H265 = 1, VCODEC_AV1 = 2 };

// Payload prefix sizes (they follow the common header).
#define MOB_VIDEO_FRAME_PREFIX 20
#define MOB_VIDEO_CONFIG_PREFIX 8
#define MOB_AUDIO_FRAME_PREFIX 12

MOB_INLINE void pkt_put_u64(u8* b, u32 off, u64 v) {
    for (u32 i = 0; i < 8; ++i) b[off + i] = (u8)((v >> (56 - 8 * i)) & 0xFF);
}
MOB_INLINE void pkt_put_u32(u8* b, u32 off, u32 v) {
    b[off] = (u8)(v >> 24); b[off+1] = (u8)(v >> 16); b[off+2] = (u8)(v >> 8); b[off+3] = (u8)v;
}
MOB_INLINE void pkt_put_u16(u8* b, u32 off, u16 v) {
    b[off] = (u8)(v >> 8); b[off + 1] = (u8)v;
}
MOB_INLINE u64 pkt_get_u64(const u8* b, u32 off) {
    u64 v = 0;
    for (u32 i = 0; i < 8; ++i) v = (v << 8) | b[off + i];
    return v;
}
MOB_INLINE u32 pkt_get_u32(const u8* b, u32 off) {
    return ((u32)b[off] << 24) | ((u32)b[off+1] << 16) | ((u32)b[off+2] << 8) | (u32)b[off+3];
}
MOB_INLINE u16 pkt_get_u16(const u8* b, u32 off) {
    return (u16)(((u32)b[off] << 8) | (u32)b[off + 1]);
}
MOB_INLINE i32 pkt_get_i32(const u8* b, u32 off) { return (i32)pkt_get_u32(b, off); }

MOB_INLINE void pkt_write_header(u8* b, u8 type, u8 flags, u32 seq, u32 payload_len) {
    pkt_put_u64(b, 0, MOB_PROTO_MAGIC);
    b[8] = type;
    b[9] = flags;
    pkt_put_u32(b, 10, seq);
    pkt_put_u32(b, 14, payload_len);
}

} // namespace mob
