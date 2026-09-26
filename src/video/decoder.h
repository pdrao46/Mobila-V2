// ============================================================================
//  MOBILADOR - src/video/decoder.h
//  Hardware (Media Foundation / D3D11) video decoder with a software fallback.
//
//  LATENCY DESIGN
//  --------------
//   * The MFT is created with MFT_ENUM_FLAG_HARDWARE and asked for DXGI output
//     buffers, so decode output lands directly in a D3D11 texture that the
//     renderer can bind as a shader resource.  There is no staging copy and no
//     CPU touch of the pixels on the fast path (NV12 planes are sampled with
//     two shader resource views, one per plane).
//   * CODECAPI_AVLowLatencyMode is enabled: hardware decoders that support it
//     stop holding frames for reordering and emit each picture as soon as it is
//     decoded, which removes one to three frames of buffering.
//   * The decoder is driven by the network thread and hands the newest frame to
//     a single-slot mailbox.  If the render thread has not consumed the
//     previous frame, the old sample is released before the new one is stored -
//     a stale frame can never be shown.
//   * Stream changes / parameter set updates renegotiate the output type in
//     place; the decoder never tears down the device.
// ============================================================================
#pragma once

#include "../render/gfx.h"
#include "transform.h"
#include "../pipeline/protocol.h"
#include "../core/threads.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

namespace mob {

enum DecodeBackend : int {
    DECODE_NONE = 0,
    DECODE_HW_D3D11,       // zero copy: decoder writes into D3D11 textures
    DECODE_HW_SYSTEM,      // hardware decoder, system memory output
    DECODE_SOFTWARE,       // software decoder (fallback)
};

struct VideoFrame {
    IMFSample*              sample = nullptr;   // released by the consumer
    ID3D11Texture2D*        texture = nullptr;
    ID3D11ShaderResourceView* srv_y  = nullptr;
    ID3D11ShaderResourceView* srv_uv = nullptr;
    u32   width = 0, height = 0;
    u32   subresource = 0;
    // telemetry carried with the frame
    u64   capture_us = 0;      // phone clock, as stamped by the encoder
    u64   arrived_us = 0;      // client clock when the last byte arrived
    u64   decode_start_us = 0;
    u64   decode_done_us = 0;
    u32   encode_us = 0;
    u32   payload_bytes = 0;
    bool  keyframe = false;
    bool  software = false;
};

struct DecoderStats {
    u32  frames_decoded = 0;
    u32  frames_dropped = 0;     // superseded before being drawn
    u32  stream_changes = 0;
    f32  last_decode_ms = 0;
    f32  avg_decode_ms = 0;
    f64  decode_sum_ms = 0;
    u32  decode_count = 0;
};

struct Decoder {
    Gfx*  gfx = nullptr;
    Arena arena;
    IMFTransform* transform = nullptr;
    IMFActivate*  activate = nullptr;
    IMFSample*    csd_sample = nullptr;
    IMFSample*    input_sample = nullptr;    // reused: no per-frame MFCreateSample
    IMFMediaBuffer* input_buffer = nullptr;
    u32  input_capacity = 0;

    DecodeBackend backend = DECODE_NONE;
    VideoCodecId  codec = VCODEC_H264;
    u8   extradata[1024];
    u32  extradata_size = 0;
    bool extradata_ready = false;
    bool inband_parameter_sets = true;
    u32  width = 0, height = 0;
    i64  frame_duration_100ns = 166666;   // 60 fps
    u64  input_timestamp = 0;
    bool output_type_set = false;
    bool has_d3d_manager = false;
    bool low_latency_enabled = false;

    // scratch buffers (preallocated: the decode path never allocates)
    u8*  scratch = nullptr;
    u32  scratch_cap = 0;
    u8*  csd_concat = nullptr;
    u32  csd_concat_cap = 0;
    u32  csd_concat_size = 0;

    DecoderStats stats{};
    char backend_name[64] = "none";
    char decoder_name[128] = "none";
    char last_error[192] = "";

    // D3D11 device manager handed to the MFT (this is what makes the decoder
    // allocate its output in GPU memory instead of system memory).
    IMFDXGIDeviceManager* dxgi_manager = nullptr;
    UINT dxgi_reset_token = 0;
    // Software fallback: the NV12 system-memory output is uploaded into two
    // reusable dynamic textures (luma R8 + chroma RG8) so the video shader
    // keeps exactly one code path for both the GPU and the CPU decoder.
    ID3D11Texture2D* staging_tex = nullptr;      // Y  (width x height)
    ID3D11ShaderResourceView* srv_staging = nullptr;
    ID3D11Texture2D* staging_tex_uv = nullptr;   // UV (width/2 x height/2)
    ID3D11ShaderResourceView* srv_staging_uv = nullptr;
    bool texture_from_system = false;
    // Uploads the two planes of a system-memory NV12 buffer.
    bool upload_system_frame(const u8* scan0, i32 pitch);
    // Async MFT support: hardware decoders on most GPUs are asynchronous and
    // report readiness through events instead of return codes.
    IMFMediaEventGenerator* event_gen = nullptr;
    bool async_mode = false;

    // newest frame handoff
    Mailbox<VideoFrame> output;
    u64  submitted = 0;
    u64  produced = 0;

    bool init(Gfx* gfx, VideoCodecId codec, u32 width, u32 height, u32 fps,
              bool hardware, bool prefer_d3d11);
    void shutdown();
    void set_extradata(const u8* csd, u32 size);
    // Parses the Annex-B access unit and runs it through the decoder.
    bool submit(const u8* data, u32 size, u64 capture_us, u64 arrived_us, u32 encode_us,
                bool keyframe, VideoFrame* out_frame);
    void flush();
    void release_frame(VideoFrame* f);
    bool ok() const { return transform != nullptr; }
    bool negotiate_output_type();
    void set_target_fps(u32 fps) { frame_duration_100ns = (i64)(10000000ll / mob_max(fps, 1u)); }
    void request_keyframe();

private:
    bool create_transform(bool hardware, bool prefer_d3d11);
    bool configure_input_type();
    bool configure_output_type(bool dxgi);
    bool drain_output(VideoFrame* out_frame);
    // Waits for an event of the requested type on an async MFT (bounded wait:
    // the render thread must never block longer than a frame).
    bool wait_for_event(long want_type, u32 timeout_ms);
    ID3D11ShaderResourceView* view_for(ID3D11Texture2D* tex, u32 subresource, u32 plane, bool uv);
    // Cached shader-resource views. The decoder's output textures come from a
    // fixed pool owned by the MFT, so the same texture pointer comes back every
    // frame: creating a view per frame would leak GPU resources and cost time.
    // Each entry holds its own reference to the texture, and the table is used
    // round-robin so it can never overflow.
    struct ViewKey { ID3D11Texture2D* tex; u32 subresource; u32 plane; ID3D11ShaderResourceView* srv; };
    ViewKey views[64]{};
    u32 view_count = 0;
    u32 view_next = 0;
};

// Resolves the GUIDs that are not covered by the mingw import libraries.
struct MfGuids {
    static const GUID* media_type_video();
    static const GUID* video_format_h264();
    static const GUID* video_format_hevc();
    static const GUID* video_format_nv12();
    static const GUID* mft_category_video_decoder();
};
// Provides the Media Foundation GUID symbols the linker would normally take
// from mfuuid.lib (mingw's import library does not export all of them).
void mf_guids_anchor();

} // namespace mob
