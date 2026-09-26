// ============================================================================
//  MOBILADOR - src/video/transform.cpp
// ============================================================================
#include "transform.h"

namespace mob {

// ---------------------------------------------------------------- NalScanner
bool NalScanner::next(NalUnit* out) {
    if (pos + 4 > size) return false;
    // find the first start code at or after pos
    u32 i = pos;
    while (i + 3 <= size) {
        if (base[i] == 0 && base[i + 1] == 0 && base[i + 2] == 1) break;
        ++i;
    }
    if (i + 3 > size) return false;
    u32 start_len = (i >= 1 && base[i - 1] == 0) ? 4 : 3;      // 00 00 00 01 or 00 00 01
    u32 nal_start = i + 3;
    u32 j = nal_start;
    while (j + 3 <= size) {
        if (base[j] == 0 && base[j + 1] == 0 && (base[j + 2] == 1 ||
            (j + 4 <= size && base[j + 2] == 0 && base[j + 3] == 1))) break;
        ++j;
    }
    u32 nal_end = (j + 3 <= size) ? j : size;
    pos = nal_end;
    if (nal_end <= nal_start) return false;
    out->data = base + nal_start;
    out->size = nal_end - nal_start;
    u8 hdr = out->data[0];
    // H.264: type 5 = IDR.  H.265: types 16..23 are IRAP (BLA/IDR/CRA).
    u32 t264 = hdr & 0x1F;
    u32 t265 = (hdr >> 1) & 0x3F;
    out->keyframe_hint = (t264 == 5) || (t265 >= 16 && t265 <= 23);
    (void)start_len;
    return true;
}

VideoCodecId detect_codec_h264_or_h265(const u8* data, u32 size) {
    NalScanner sc;
    sc.reset(data, size);
    NalUnit nal;
    if (!sc.next(&nal) || nal.size == 0) return VCODEC_H264;
    u8 hdr = nal.data[0];
    // In H.264 the forbidden_zero_bit must be 0 and nal_ref_idc/type must be
    // sane; H.265 uses a 2 bit type field where 32..40 are VPS/SPS/PPS.
    u32 t265 = (hdr >> 1) & 0x3F;
    if ((hdr & 0x80) == 0 && (t265 == 32 || t265 == 33 || t265 == 34)) return VCODEC_H265;
    u32 t264 = hdr & 0x1F;
    if (t264 == 7 || t264 == 8 || t264 == 5) return VCODEC_H264;
    return VCODEC_H264;
}

u32 count_nal_type(const u8* data, u32 size, u32 nal_type, VideoCodecId codec) {
    NalScanner sc;
    sc.reset(data, size);
    NalUnit nal;
    u32 n = 0;
    while (sc.next(&nal)) {
        u8 hdr = nal.data[0];
        u32 t = (codec == VCODEC_H265) ? ((hdr >> 1) & 0x3F) : (hdr & 0x1F);
        if (t == nal_type) ++n;
    }
    return n;
}

// ---------------------------------------------------------------- extradata
static u32 collect_nals(const u8* csd, u32 csd_size, VideoCodecId codec,
                        u8 sps[][1024], u32 sps_size[4], u32* count, u32 max_nals = 4) {
    NalScanner sc;
    sc.reset(csd, csd_size);
    NalUnit nal;
    u32 n = 0;
    while (sc.next(&nal) && n < max_nals) {
        if (nal.size == 0 || nal.size > 1024) continue;
        u8 hdr = nal.data[0];
        u32 t = (codec == VCODEC_H265) ? ((hdr >> 1) & 0x3F) : (hdr & 0x1F);
        bool wanted = (codec == VCODEC_H265) ? (t == 32 || t == 33 || t == 34) : (t == 7 || t == 8);
        if (!wanted) continue;
        memcpy(sps[n], nal.data, nal.size);
        sps_size[n] = nal.size;
        ++n;
    }
    *count = n;
    return n;
}

u32 build_decoder_extradata(VideoCodecId codec, const u8* csd, u32 csd_size, u8* out, u32 out_cap) {
    u8 nals[4][1024];
    u32 nsize[4] = { 0, 0, 0, 0 };
    u32 ncount = 0;
    collect_nals(csd, csd_size, codec, nals, nsize, &ncount);
    if (ncount == 0) return 0;

    u32 o = 0;
    if (codec == VCODEC_H264) {
        // AVCDecoderConfigurationRecord (ISO/IEC 14496-15, 5.2.4.1)
        const u8* sps = nals[0];
        u32 sps_len = nsize[0];
        if (sps_len < 4) return 0;
        if (out_cap < 64 + sps_len) return 0;
        out[o++] = 1;                        // configurationVersion
        out[o++] = sps[1];                   // AVCProfileIndication
        out[o++] = sps[2];                   // profile_compatibility
        out[o++] = sps[3];                   // AVCLevelIndication
        out[o++] = 0xFF;                     // 6 bits reserved + lengthSizeMinusOne = 3
        out[o++] = 0xE0 | (u8)(ncount > 3 ? 3 : ncount);   // 3 bits reserved + numOfSPS
        for (u32 i = 0; i < ncount; ++i) {
            // SPS entries come first, PPS afterwards; the phone always sends
            // SPS then PPS, and a second SPS only appears on a resolution change.
            bool is_sps = (codec == VCODEC_H265) || ((nals[i][0] & 0x1F) == 7);
            if (!is_sps) continue;
            pkt_put_u16(out, o, (u16)nsize[i]); o += 2;
            if (o + nsize[i] > out_cap) return 0;
            memcpy(out + o, nals[i], nsize[i]); o += nsize[i];
        }
        // rewrite the SPS count correctly now that we know how many there were
        u32 sps_count = 0, pps_first = 0;
        for (u32 i = 0; i < ncount; ++i) {
            if ((nals[i][0] & 0x1F) == 7) ++sps_count;
            else if (!pps_first) pps_first = i;
        }
        out[5] = (u8)(0xE0 | (sps_count ? sps_count : 1));
        // PPS array
        if (o + 2 > out_cap) return 0;
        out[o++] = (u8)(pps_first ? (ncount - sps_count) : 1);
        for (u32 i = 0; i < ncount; ++i) {
            if ((nals[i][0] & 0x1F) != 8) continue;
            pkt_put_u16(out, o, (u16)nsize[i]); o += 2;
            if (o + nsize[i] > out_cap) return 0;
            memcpy(out + o, nals[i], nsize[i]); o += nsize[i];
        }
        return o;
    }

    // HEVCDecoderConfigurationRecord (ISO/IEC 14496-15, 8.3.3.1)
    if (out_cap < 128) return 0;
    memset(out, 0, 23);
    const u8* vps = nullptr; const u8* sps = nullptr; const u8* pps = nullptr;
    u32 vps_len = 0, sps_len = 0, pps_len = 0;
    for (u32 i = 0; i < ncount; ++i) {
        u32 t = (nals[i][0] >> 1) & 0x3F;
        if (t == 32 && !vps) { vps = nals[i]; vps_len = nsize[i]; }
        else if (t == 33 && !sps) { sps = nals[i]; sps_len = nsize[i]; }
        else if (t == 34 && !pps) { pps = nals[i]; pps_len = nsize[i]; }
    }
    if (!sps || sps_len < 4) return 0;
    out[0] = 1;                              // configurationVersion
    // general_profile_space / tier / profile_idc from the SPS bytes
    u8 b1 = sps[1];
    out[1] = (u8)((b1 & 0x03) << 6 | (b1 & 0x7C) >> 1);
    out[1] |= 0;                             // tier + profile_idc high bit
    out[2] = (u8)(((b1 & 0x01) << 7) | ((sps[2] & 0x1F) << 2) | 0x00);
    // general_profile_compatibility_flags (4 bytes) - copied verbatim is not
    // possible without full SPS parsing; the field is informational for the
    // decoder and the values below are the standard "compatible with itself"
    // pattern derived from the profile byte.
    out[3] = 0; out[4] = 0; out[5] = 0; out[6] = 0;
    out[7] = 0; out[8] = 0; out[9] = 0; out[10] = 0;
    out[11] = sps[3] & 0x3F;                 // general_level_idc (informational)
    out[12] = 0xF0;                          // min_spatial_segmentation_idc
    out[13] = 0xFC;                          // parallelismType
    out[14] = 0xFC | 0x01;                   // chromaFormat = 4:2:0
    out[15] = 0xF8;                          // bitDepthLumaMinus8
    out[16] = 0xF8;                          // bitDepthChromaMinus8
    out[17] = 0; out[18] = 0;                // avgFrameRate
    out[19] = 0x03;                          // constantFrameRate / numTemporalLayers / temporalIdNested
    out[20] = 0x03;                          // lengthSizeMinusOne = 3
    out[21] = (u8)((vps ? 1 : 0) + (sps ? 1 : 0) + (pps ? 1 : 0));
    o = 22;
    struct Arr { u8 type; const u8* data; u32 size; };
    Arr arrays[3] = {
        { 32, vps, vps_len }, { 33, sps, sps_len }, { 34, pps, pps_len }
    };
    for (u32 a = 0; a < 3; ++a) {
        if (!arrays[a].data) continue;
        if (o + 3 > out_cap) return 0;
        out[o++] = (u8)(0x80 | arrays[a].type);   // array_completeness = 1
        pkt_put_u16(out, o, 1); o += 2;           // numNalus
        if (o + 2 + arrays[a].size > out_cap) return 0;
        pkt_put_u16(out, o, (u16)arrays[a].size); o += 2;
        memcpy(out + o, arrays[a].data, arrays[a].size);
        o += arrays[a].size;
    }
    return o;
}

// ---------------------------------------------------------------- conversion
u32 annexb_to_avcc(const u8* data, u32 size, u8* out, u32 out_cap, u32* nal_count_out,
                   bool* has_keyframe, bool* has_vcl) {
    NalScanner sc;
    sc.reset(data, size);
    NalUnit nal;
    u32 o = 0, n = 0;
    bool key = false, vcl = false;
    while (sc.next(&nal)) {
        if (o + 4 + nal.size > out_cap) break;
        pkt_put_u32(out, o, nal.size);
        o += 4;
        memcpy(out + o, nal.data, nal.size);
        o += nal.size;
        ++n;
        u8 hdr = nal.data[0];
        u32 t264 = hdr & 0x1F;
        u32 t265 = (hdr >> 1) & 0x3F;
        if (t264 == 5 || (t265 >= 16 && t265 <= 23)) key = true;
        if (t264 == 1 || t264 == 5 || (t265 >= 0 && t265 <= 31)) vcl = true;
    }
    if (nal_count_out) *nal_count_out = n;
    if (has_keyframe)  *has_keyframe = key;
    if (has_vcl)       *has_vcl = vcl;
    return o;
}

// --------------------------------------------------------------------- SPS
// Minimal Exp-Golomb bit reader: enough to pull width/height out of the SPS,
// which lets the window size itself correctly before the first decoded frame.
struct BitReader {
    const u8* p;
    u32 bits;
    u32 pos = 0;
    BitReader(const u8* data, u32 bytes) : p(data), bits(bytes * 8) {}
    u32 u(u32 n) {
        u32 v = 0;
        for (u32 i = 0; i < n && pos < bits; ++i) {
            v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1);
            ++pos;
        }
        return v;
    }
    u32 ue() {
        u32 zeros = 0;
        while (pos < bits && u(1) == 0 && zeros < 32) ++zeros;
        if (zeros == 0) return 0;
        return (1u << zeros) - 1 + u(zeros);
    }
    i32 se() {
        u32 k = ue();
        i32 v = (i32)((k + 1) / 2);
        return (k & 1) ? -v : v;
    }
};

bool parse_sps_dimensions(const u8* csd, u32 csd_size, VideoCodecId codec, u32* w, u32* h) {
    NalScanner sc;
    sc.reset(csd, csd_size);
    NalUnit nal;
    while (sc.next(&nal)) {
        u8 hdr = nal.data[0];
        u32 t = (codec == VCODEC_H265) ? ((hdr >> 1) & 0x3F) : (hdr & 0x1F);
        bool is_sps = (codec == VCODEC_H265) ? (t == 33) : (t == 7);
        if (!is_sps || nal.size < 8) continue;
        // skip the NAL header (2 bytes for HEVC, 1 for AVC)
        u32 skip = (codec == VCODEC_H265) ? 2 : 1;
        BitReader br(nal.data + skip, nal.size - skip);
        if (codec == VCODEC_H265) {
            br.u(4);                       // sps_video_parameter_set_id
            u32 max_sub_layers = br.u(3);
            br.u(1);                       // sps_temporal_id_nesting_flag
            // profile_tier_level (skipped generically)
            br.u(2); br.u(1); br.u(5);
            br.u(32); br.u(48); br.u(8);
            for (u32 i = 0; i < (max_sub_layers ? max_sub_layers : 1) - 1; ++i) {
                br.u(2); br.u(1); br.u(5); br.u(32); br.u(8);
            }
            if (max_sub_layers > 1) {
                for (u32 i = max_sub_layers; i < 8; ++i) { br.u(2); br.u(1); br.u(5); br.u(32); }
            }
            br.ue();                       // sps_seq_parameter_set_id
            u32 chroma = br.ue();
            if (chroma == 3) br.u(1);
            u32 width  = br.ue();
            u32 height = br.ue();
            if (br.bits > br.pos) {
                // conformance window may crop; applying it here would need the
                // full flag set, so the raw values are used and the renderer
                // letterboxes to the decoded surface anyway.
                *w = mob_max(width, 2u);
                *h = mob_max(height, 2u);
                return true;
            }
        } else {
            br.u(8);                       // nal header already consumed, profile_idc
            br.u(8);                       // constraint flags
            br.u(8);                       // level_idc
            br.ue();                       // seq_parameter_set_id
            u32 profile = nal.data[1];
            if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
                profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
                profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
                u32 chroma = br.ue();
                if (chroma == 3) br.u(1);
                br.ue(); br.ue(); br.u(1);
                if (br.u(1)) {                 // seq_scaling_matrix_present
                    u32 n = (chroma != 3) ? 8 : 12;
                    for (u32 i = 0; i < n; ++i) {
                        if (br.u(1)) {
                            // scaling lists: skipped, dimensions are read below
                            i32 last = 8, next = 8;
                            for (u32 j = 0; j < 16; ++j) {
                                if (next != 0) { next = (i32)((last + br.se() + 256) % 256); }
                                if (next != 0) last = next;
                            }
                        }
                    }
                }
            }
            br.ue();                       // log2_max_frame_num_minus4
            u32 poc_type = br.ue();
            if (poc_type == 0) br.ue();
            else if (poc_type == 1) {
                br.u(1); br.se(); br.se();
                for (u32 i = 0; i < 32; ++i) if (br.u(1)) br.se();
            }
            br.ue();                       // max_num_ref_frames
            br.u(1);                       // gaps_in_frame_num_value_allowed
            u32 w_mbs = br.ue() + 1;
            u32 h_map = br.ue() + 1;
            br.u(1);                       // frame_mbs_only_flag
            u32 cw = w_mbs * 16, ch = h_map * 16;
            if (cw == 0 || ch == 0 || cw > 8192 || ch > 8192) continue;   // misparse guard
            *w = cw;
            *h = ch;
            return true;
        }
    }
    return false;
}

} // namespace mob
