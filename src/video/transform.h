// ============================================================================
//  MOBILADOR - src/video/transform.h
//  Bitstream conversion between what MediaCodec produces (Annex-B, with the
//  SPS/PPS carried out of band in the codec config packet) and what a Windows
//  Media Foundation decoder wants.
//
//  WHY CONVERT AT ALL
//  ------------------
//  Media Foundation's H.264/H.265 decoder accepts two forms:
//    * raw Annex-B access units, and
//    * AVCC (4 byte length prefixed) samples plus an extradata blob.
//  The framework decoders are documented for the AVCC form, and - more
//  importantly - the hardware decoder opens faster and does not have to sniff
//  for parameter sets on every sample when it is given extradata up front.
//  Converting is a single pass over a buffer we already own: no allocation, no
//  extra threading, and no measurable cost against the ~1 ms decode budget.
// ============================================================================
#pragma once

#include "../core/base.h"
#include "../pipeline/protocol.h"   // VideoCodecId, packet constants

namespace mob {

struct NalUnit {
    const u8* data;   // points at the NAL header byte (start code stripped)
    u32       size;
    bool      keyframe_hint;   // IDR (H.264) / IRAP (H.265)
};

struct NalScanner {
    const u8* base = nullptr;
    u32       size = 0;
    u32       pos  = 0;

    void reset(const u8* data, u32 len) { base = data; size = len; pos = 0; }
    // Returns false when no further NAL unit exists.
    bool next(NalUnit* out);
};

// ---------------------------------------------------------------------------
// Detects the codec from the first NAL header (used when the phone connects
// before the config packet arrives).
VideoCodecId detect_codec_h264_or_h265(const u8* data, u32 size);

// ---------------------------------------------------------------------------
// Scans the Annex-B parameter sets in `csd` and builds the decoder specific
// data blob (AVCDecoderConfigurationRecord / HEVCDecoderConfigurationRecord).
// `out` must be at least 512 bytes. Returns the written size, 0 on failure.
u32 build_decoder_extradata(VideoCodecId codec, const u8* csd, u32 csd_size, u8* out, u32 out_cap);

// ---------------------------------------------------------------------------
// Rewrites one Annex-B access unit as AVCC (4 byte big endian lengths) into
// `out`. Returns the number of bytes written. `out` must have room for
// `size + 4 * nal_count` bytes; nal_count is bounded by size/3.
u32 annexb_to_avcc(const u8* data, u32 size, u8* out, u32 out_cap, u32* nal_count_out,
                   bool* has_keyframe, bool* has_vcl);

// ---------------------------------------------------------------------------
// Counts NAL units of a given type without copying (used to answer "is this a
// key frame" and to detect parameter set changes inside the stream).
u32 count_nal_type(const u8* data, u32 size, u32 nal_type, VideoCodecId codec);

// Reads the width/height from the SPS inside an Annex-B buffer, so the renderer
// can letterbox correctly even before the first frame is decoded.
bool parse_sps_dimensions(const u8* csd, u32 csd_size, VideoCodecId codec, u32* w, u32* h);

} // namespace mob
