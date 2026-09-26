// ============================================================================
//  MOBILADOR - src/render/gfx.h
//  D3D11 device + swapchain + present path.
//
//  LATENCY DESIGN (the important part of this file)
//  ------------------------------------------------
//  * Flip model swapchain (DXGI_SWAP_EFFECT_FLIP_DISCARD). The legacy bitblt
//    model copies through a staging surface and costs a full extra frame.
//  * DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT +
//    SetMaximumFrameLatency(1): the swap chain keeps exactly one frame in
//    flight. Waiting on the handle is a kernel wait - no spin, no CPU burn -
//    and it removes the multi-frame queue that makes a game feel "floaty".
//  * DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING + Present(0, ALLOW_TEARING) for the
//    ULTRA LOW LATENCY profile: no vsync wait at all, the newest frame appears
//    as soon as the GPU has it.
//  * Borderless fullscreen (WS_POPUP over the whole monitor) is used instead of
//    exclusive fullscreen: with flip model this becomes an *independent flip*
//    surface, so DWM is bypassed and windowed mode costs no extra composition.
//  * The render target is redrawn entirely every frame; there is no
//    accumulate-based UI path that could delay a present.
// ============================================================================
#pragma once

#include "../platform/win.h"
#include <d3d11.h>
#include <dxgi1_6.h>

namespace mob {

struct GpuInfo {
    char name[128];
    u64  dedicated_vram = 0;
    u64  shared_ram = 0;
    u32  vendor_id = 0;
    u32  device_id = 0;
    bool is_software = false;
    bool is_discrete = false;
    u32  max_feature_level = 0;      // D3D_FEATURE_LEVEL value
};

enum PresentMode : int {
    PRESENT_ULTRA_LOW_LATENCY = 0,   // no vsync, tearing allowed, 1 frame in flight
    PRESENT_VSYNC             = 1,   // Present(1,0)
    PRESENT_FRAME_PACED       = 2,   // frame limiter (target fps), tearing off
};

enum ScalingMode : int {
    SCALE_ASPECT_FIT = 0,     // keep aspect, letterbox (default: no distortion)
    SCALE_FILL        = 1,    // fill window, may distort
    SCALE_STRETCH     = 2,    // alias of fill, kept for UI clarity
    SCALE_INTEGER     = 3,    // integer scaling, pixel perfect
    SCALE_NATIVE_1TO1 = 4,    // unsighted 1:1 pixels, centered
};

struct GfxStats {
    u64 presents = 0;
    u64 presents_dropped = 0;
    u64 frame_latency_waits_us = 0;
    u64 gpu_present_to_sync_us = 0;
    u32 refresh_hz = 0;
};

struct Gfx {
    // device
    ID3D11Device*        dev  = nullptr;
    ID3D11DeviceContext* ctx  = nullptr;
    IDXGISwapChain1*     swap = nullptr;
    IDXGISwapChain2*     swap2 = nullptr;      // frame-latency waitable object
    IDXGIAdapter1*       adapter = nullptr;
    IDXGIFactory2*       factory = nullptr;
    IDXGIFactory5*       factory5 = nullptr;   // EnumWarpAdapter / CheckFeatureSupport
    GpuInfo              gpu{};
    u32                  feature_level = 0;

    // backbuffer
    ID3D11RenderTargetView* rtv = nullptr;
    u32  width = 0, height = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    u32  buffer_count = 2;
    bool tearing_supported = false;
    bool waitable_supported = false;
    HANDLE frame_latency_handle = nullptr;

    // pipeline objects
    ID3D11VertexShader*  vs_ui = nullptr;
    ID3D11PixelShader*   ps_ui = nullptr;
    ID3D11PixelShader*   ps_shape = nullptr;
    ID3D11PixelShader*   ps_video = nullptr;
    ID3D11InputLayout*   layout_ui = nullptr;
    ID3D11Buffer*        cb_frame = nullptr;      // per-frame constants (screen size, time, gamma)
    ID3D11Buffer*        cb_video = nullptr;      // video sampling/conversion params (b0 of ps_video)
    ID3D11SamplerState*  samp_linear = nullptr;
    ID3D11SamplerState*  samp_point = nullptr;
    ID3D11BlendState*    blend_alpha = nullptr;
    ID3D11BlendState*    blend_none = nullptr;
    ID3D11RasterizerState* rast_ui = nullptr;
    ID3D11RasterizerState* rast_video = nullptr;
    ID3D11DepthStencilState* depth_none = nullptr;
    ID3D11Buffer*        vb = nullptr;            // dynamic vertex buffer (batcher)
    ID3D11Buffer*        ib = nullptr;
    u32                  vb_capacity = 0, ib_capacity = 0;

    // present control
    PresentMode present_mode = PRESENT_ULTRA_LOW_LATENCY;
    u32  target_fps = 0;                          // 0 = uncapped
    u64  last_present_us = 0;
    GfxStats stats{};

    bool init(HWND hwnd, u32 width, u32 height, bool allow_tearing, i32 adapter_index);
    void shutdown();
    bool resize(u32 width, u32 height);
    void set_present_mode(PresentMode m) { present_mode = m; }
    void set_target_fps(u32 fps) { target_fps = fps; }

    // Waits for a free backbuffer slot. Never spins: blocks on the swapchain's
    // frame-latency waitable object (or a short timeout when waitable objects
    // are unavailable).
    void wait_for_frame_slot(u32 timeout_ms = 8);
    // Frame limiter used by PRESENT_FRAME_PACED.
    void pace_to_target_fps();
    // Returns false when the frame was not presented (device lost / occluded).
    bool present();
    void begin_frame(const f32 clear[4]);
    u64  last_present_delta_us() const;

    void set_viewport(f32 x, f32 y, f32 w, f32 h);
    void set_scissor(i32 x, i32 y, i32 w, i32 h);
    void begin_ui_pass(bool alpha_blend);
    // Video parameters for the NV12 -> RGB pass. Kept in a dedicated constant
    // buffer so the UI constant buffer is never rewritten mid-frame.
    void set_video_params(f32 uv_scale_x, f32 uv_scale_y, f32 uv_off_x, f32 uv_off_y,
                          f32 contrast, f32 saturation, f32 brightness, f32 sharpness);
    void draw_batch(const void* verts, u32 vcount, const u16* indices, u32 icount);

    bool create_video_shader_objects();
    void refresh_stats();
    const char* mode_name() const;
    // dxgi frame statistics -> real display latency estimate (present -> vblank)
    u64  display_latency_estimate_us() const;
};

// shader helpers (d3dcompiler_47)
bool gfx_compile(const char* src, const char* entry, const char* profile, ID3DBlob** out, Str* err, Arena* a);

} // namespace mob
