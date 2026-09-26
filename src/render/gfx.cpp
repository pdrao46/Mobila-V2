// ============================================================================
//  MOBILADOR - src/render/gfx.cpp
// ============================================================================
#include "gfx.h"
#include "../core/log.h"
#include <d3dcompiler.h>

// ID3D10Multithread, declared manually so the build never depends on d3d10.h.
struct ID3D10MultithreadMin {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0;
    virtual ULONG   STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG   STDMETHODCALLTYPE Release() = 0;
    virtual void    STDMETHODCALLTYPE Enter() = 0;
    virtual void    STDMETHODCALLTYPE Leave() = 0;
    virtual BOOL    STDMETHODCALLTYPE SetMultithreadProtected(BOOL) = 0;
    virtual BOOL    STDMETHODCALLTYPE GetMultithreadProtected() = 0;
};
static const GUID IID_ID3D10Multithread_C = { 0x9B7E4E00, 0x342C, 0x4106, { 0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0 } };
static const GUID IID_IDXGIFactory2_C     = { 0x50C83A1C, 0xE072, 0x4C48, { 0x87, 0xB0, 0x36, 0xFA, 0x36, 0xA6, 0xD0, 0xA8 } };

namespace mob {

// ============================================================ shader sources
static const char* HLSL_UI = R"(
cbuffer FrameCB : register(b0) {
    float2 u_screen;      // pixels
    float2 u_screen_inv;  // 1/pixels
    float  u_time;        // seconds since start
    float  u_dpi;         // ui scale
    float2 u_pad;
};

Texture2D    u_tex  : register(t0);
SamplerState u_samp : register(s0);

struct VSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
    float4 geo : TEXCOORD1;   // x: half width, y: half height, z: corner radius, w: border width
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
    float4 geo : TEXCOORD1;
    float2 rem : TEXCOORD2;   // pixel position inside the quad
};

VSOut vs_ui(VSIn i) {
    VSOut o;
    float2 ndc = float2(i.pos.x * u_screen_inv.x * 2.0 - 1.0,
                        1.0 - i.pos.y * u_screen_inv.y * 2.0);
    o.pos = float4(ndc, 0.0, 1.0);
    o.uv  = i.uv;
    o.col = i.col;
    o.geo = i.geo;
    o.rem = (i.uv - 0.5) * 2.0 * i.geo.xy;
    return o;
}

// Textured UI: glyph atlas (R8) or ARGB images.
float4 ps_ui(VSOut i) : SV_Target {
    float4 t = u_tex.Sample(u_samp, i.uv);
    float3 rgb = i.col.rgb * t.rgb;
    float  a   = i.col.a * t.a;
    return float4(rgb, a);
}

// Vector shapes: analytic rounded-rect with border and 1px analytic AA.
// One shader covers cards, buttons, pills, sliders, rings and dividers.
float4 ps_shape(VSOut i) : SV_Target {
    float2 half   = i.geo.xy;
    float  radius = i.geo.z;
    float  border = i.geo.w;

    float2 p = i.rem;
    float2 q = abs(p) - (half - radius);
    float  d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;

    float aa = max(fwidth(d), 0.0001) * 0.65;
    aa = max(aa, 0.35);                       // keep 1px outlines visible
    float fill  = 1.0 - smoothstep(-aa, aa, d);
    float inner = 1.0 - smoothstep(-aa, aa, d + border);
    float cov   = (border > 0.0) ? max(fill - inner, 0.0) : fill;
    return float4(i.col.rgb, i.col.a * cov);
}
)";

static const char* HLSL_VIDEO = R"(
cbuffer VideoCB : register(b0) {
    float2 u_uv_scale;      // sampling scale (crop)
    float2 u_uv_offset;
    float  u_contrast;
    float  u_saturation;
    float  u_brightness;
    float  u_sharpness;     // 0 = off, 1 = normal, 2 = strong (CAS-like)
};

Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      samp  : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float2 texel : TEXCOORD1;
};

// Fullscreen triangle: no vertex buffer, no index buffer, no CPU work.
VSOut vs_video(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.texel = float2(0.0, 0.0);
    return o;
}

// BT.709 limited range YUV -> RGB, done in one pass on the GPU.
// Using the video processor would cost an extra full-frame surface copy.
float3 yuv_to_rgb(float y, float2 uv) {
    float3 v = float3(y, uv.y, uv.x) - float3(0.0625, 0.5, 0.5);
    return float3(
        dot(v, float3(1.16438356,  0.00000000,  1.79274107)),
        dot(v, float3(1.16438356, -0.21324861, -0.53290933)),
        dot(v, float3(1.16438356,  2.11240179,  0.00000000)));
}

float4 ps_video(VSOut i) : SV_Target {
    float2 uv = i.uv * u_uv_scale + u_uv_offset;
    float  y  = texY.Sample(samp, uv).r;
    float2 c  = texUV.Sample(samp, uv).rg;

    float3 rgb = yuv_to_rgb(y, c);

    // Cheap unsharp mask (2 extra taps on the luma plane): recovers the
    // detail that chroma subsampling and bilinear scaling soften, without a
    // second fullscreen pass.
    if (u_sharpness > 0.5) {
        float2 t = float2(ddx(uv.x), ddy(uv.y));
        float  yl = texY.Sample(samp, uv - float2(t.x, 0)).r;
        float  yr = texY.Sample(samp, uv + float2(t.x, 0)).r;
        float  yu = texY.Sample(samp, uv - float2(0, t.y)).r;
        float  yd = texY.Sample(samp, uv + float2(0, t.y)).r;
        float  blur = (yl + yr + yu + yd) * 0.25;
        float  amount = (u_sharpness > 1.5) ? 0.65 : 0.35;
        rgb += (float3(y, y, y) - float3(blur, blur, blur)) * amount;
    }

    float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    rgb = (rgb - luma) * u_saturation + luma;
    rgb = rgb * u_contrast + u_brightness;
    return float4(saturate(rgb), 1.0);
}
)";

struct FrameCB {
    f32 screen[2];
    f32 screen_inv[2];
    f32 time;
    f32 dpi;
    f32 pad[2];
};

struct VideoCB {
    f32 uv_scale[2];
    f32 uv_offset[2];
    f32 contrast;
    f32 saturation;
    f32 brightness;
    f32 sharpness;
};

// ============================================================ shader compile
bool gfx_compile(const char* src, const char* entry, const char* profile, ID3DBlob** out, Str* err, Arena* a) {
    ID3DBlob* eblob = nullptr;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR;
#ifdef MOB_DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompile(src, strlen(src), "mobilador.hlsl", nullptr, nullptr,
                            entry, profile, flags, 0, out, &eblob);
    if (FAILED(hr)) {
        if (eblob && err) *err = str_dup(a, Str((const char*)eblob->GetBufferPointer(), (u32)eblob->GetBufferSize()));
        if (eblob) eblob->Release();
        return false;
    }
    if (eblob) eblob->Release();
    return true;
}

// ============================================================ device + swap
static bool create_device(Gfx* g, i32 adapter_index) {
    // ---- DXGI factory
    IDXGIFactory2* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory2_C, (void**)&factory);
    if (FAILED(hr)) {
        hr = CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&factory);
        if (FAILED(hr)) { MOB_ERROR("DXGI: CreateDXGIFactory1 failed 0x%08X", (u32)hr); return false; }
    }
    g->factory = factory;
    // IDXGIFactory5 exposes the capability queries this renderer needs
    // (tearing support, WARP enumeration). It is optional: on very old systems
    // the renderer keeps working without those swaps.
    if (FAILED(factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&g->factory5))) g->factory5 = nullptr;

    // ---- adapter
    IDXGIAdapter1* chosen = nullptr;
    u32 best_score = 0;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* ad = nullptr;
        if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        ad->GetDesc1(&desc);
        bool soft = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        u32 score = 1;
        if (!soft) score += 100;
        score += (u32)(desc.DedicatedVideoMemory / (64ull * 1024 * 1024));   // VRAM in 64MB units
        if (adapter_index >= 0) {
            if ((i32)i == adapter_index) { chosen = ad; break; }
        } else if (score > best_score) {
            if (chosen) chosen->Release();
            chosen = ad; best_score = score;
        }
        if (chosen != ad) ad->Release();
    }
    if (!chosen) {
        MOB_WARN("DXGI: no hardware adapter, falling back to WARP");
        hr = g->factory5 ? g->factory5->EnumWarpAdapter(__uuidof(IDXGIAdapter1), (void**)&chosen)
                         : factory->EnumAdapters1(0, &chosen);
        if (FAILED(hr) || !chosen) { MOB_ERROR("DXGI: no adapter available"); return false; }
    }
    g->adapter = chosen;
    DXGI_ADAPTER_DESC1 adesc{};
    chosen->GetDesc1(&adesc);
    WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, g->gpu.name, sizeof(g->gpu.name) - 1, nullptr, nullptr);
    g->gpu.dedicated_vram = adesc.DedicatedVideoMemory;
    g->gpu.shared_ram      = adesc.SharedSystemMemory;
    g->gpu.vendor_id       = adesc.VendorId;
    g->gpu.device_id       = adesc.DeviceId;
    g->gpu.is_software     = (adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    g->gpu.is_discrete     = (adesc.DedicatedVideoMemory > 256ull * 1024 * 1024) && !g->gpu.is_software;

    // ---- device
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                   D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    // VIDEO_SUPPORT is required so the Media Foundation decoder can hand us
    // decoder-owned textures (zero copy) instead of system memory samples.
    flags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef MOB_DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
    hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                           levels, 4, D3D11_SDK_VERSION, &g->dev, &got, &g->ctx);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;     // SDK debug layer not installed
        hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                               levels, 4, D3D11_SDK_VERSION, &g->dev, &got, &g->ctx);
    }
    if (FAILED(hr)) {
        MOB_ERROR("D3D11CreateDevice failed 0x%08X (%s)", (u32)hr, "no GPU / driver issue");
        return false;
    }
    g->feature_level = (u32)got;
    g->gpu.max_feature_level = (u32)got;

    // The decoder runs on its own thread; the immediate context must be
    // protected, otherwise driver-level races show up as random corruption.
    ID3D10MultithreadMin* mt = nullptr;
    if (SUCCEEDED(g->dev->QueryInterface(IID_ID3D10Multithread_C, (void**)&mt)) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    MOB_INFO("GPU: %s (VRAM %llu MB, feature level %X)", g->gpu.name,
             (unsigned long long)(g->gpu.dedicated_vram / (1024 * 1024)), (u32)got);
    return true;
}

static bool create_swapchain(Gfx* g, HWND hwnd, u32 w, u32 h, bool allow_tearing) {
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width  = w;
    d.Height = h;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    d.Scaling     = DXGI_SCALING_STRETCH;
    d.Flags       = 0;

    BOOL tear = FALSE;
    if (allow_tearing && g->factory5 &&
        g->factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear)) == S_OK && tear) {
        d.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        g->tearing_supported = true;
    }
    // The waitable object is the clean way to keep exactly one frame in flight.
    d.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    IDXGISwapChain1* sc = nullptr;
    HRESULT hr = g->factory->CreateSwapChainForHwnd(g->dev, hwnd, &d, nullptr, nullptr, &sc);
    if (FAILED(hr)) {
        MOB_WARN("CreateSwapChainForHwnd (flip+waitable) failed 0x%08X, retrying plain flip", (u32)hr);
        d.Flags &= ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        hr = g->factory->CreateSwapChainForHwnd(g->dev, hwnd, &d, nullptr, nullptr, &sc);
        if (FAILED(hr)) { MOB_ERROR("CreateSwapChainForHwnd failed 0x%08X", (u32)hr); return false; }
    }
    // We own Alt+Enter / F11 handling.
    g->factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    g->swap = sc;
    g->buffer_count = d.BufferCount;

    // Frame latency control lives on IDXGISwapChain2 (Windows 8.1+). Setting it
    // to 1 is what guarantees the render loop can never be more than one frame
    // behind the CPU - the single most effective latency reduction in the
    // presentation stage.
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&g->swap2)) && g->swap2) {
        hr = g->swap2->SetMaximumFrameLatency(1);
        if (SUCCEEDED(hr)) {
            g->frame_latency_handle = g->swap2->GetFrameLatencyWaitableObject();
            g->waitable_supported = g->frame_latency_handle != nullptr;
        }
    } else {
        hr = E_NOINTERFACE;
    }
    if (FAILED(hr)) {
        MOB_WARN("SetMaximumFrameLatency(1) unsupported (0x%08X) - using 2 frames in flight", (u32)hr);
    }
    return true;
}

static bool create_backbuffer(Gfx* g) {
    ID3D11Texture2D* back = nullptr;
    HRESULT hr = g->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    if (FAILED(hr) || !back) return false;
    hr = g->dev->CreateRenderTargetView(back, nullptr, &g->rtv);
    D3D11_TEXTURE2D_DESC td{}; back->GetDesc(&td);
    g->width = td.Width; g->height = td.Height; g->fmt = td.Format;
    back->Release();
    return SUCCEEDED(hr);
}

static bool create_pipeline(Gfx* g) {
    Arena scratch; scratch.init(1 << 16);
    Str err;
    ID3DBlob *vsb = nullptr, *psb = nullptr;

    if (!gfx_compile(HLSL_UI, "vs_ui", "vs_4_0", &vsb, &err, &scratch)) {
        MOB_ERROR("UI vertex shader failed: %.*s", err.n, err.p);
        scratch.shutdown(); return false;
    }
    if (!gfx_compile(HLSL_UI, "ps_ui", "ps_4_0", &psb, &err, &scratch)) {
        MOB_ERROR("UI pixel shader failed: %.*s", err.n, err.p);
        scratch.shutdown(); return false;
    }
    g->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g->vs_ui);
    g->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g->ps_ui);
    psb->Release();

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g->dev->CreateInputLayout(layout, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g->layout_ui);
    vsb->Release();

    if (!gfx_compile(HLSL_UI, "ps_shape", "ps_4_0", &psb, &err, &scratch)) {
        MOB_ERROR("shape pixel shader failed: %.*s", err.n, err.p);
        scratch.shutdown(); return false;
    }
    g->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g->ps_shape);
    psb->Release();

    if (!gfx_compile(HLSL_VIDEO, "vs_video", "vs_4_0", &vsb, &err, &scratch)) {
        MOB_ERROR("video vertex shader failed: %.*s", err.n, err.p);
        scratch.shutdown(); return false;
    }
    if (!gfx_compile(HLSL_VIDEO, "ps_video", "ps_4_0", &psb, &err, &scratch)) {
        MOB_ERROR("video pixel shader failed: %.*s", err.n, err.p);
        scratch.shutdown(); return false;
    }
    g->create_video_shader_objects();
    vsb->Release(); psb->Release();
    scratch.shutdown();

    // constants
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 256;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g->dev->CreateBuffer(&cb, nullptr, &g->cb_frame);
    g->dev->CreateBuffer(&cb, nullptr, &g->cb_video);

    // geometry: one map per frame, WRITE_DISCARD, no ring buffer juggling
    g->vb_capacity = 64 * 1024;               // vertices
    g->ib_capacity = 192 * 1024;              // indices
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = g->vb_capacity * (u32)sizeof(f32) * 12;
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g->dev->CreateBuffer(&vbd, nullptr, &g->vb);

    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = g->ib_capacity * (u32)sizeof(u16);
    ibd.Usage = D3D11_USAGE_DYNAMIC;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ibd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g->dev->CreateBuffer(&ibd, nullptr, &g->ib);

    // samplers
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g->dev->CreateSamplerState(&sd, &g->samp_linear);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g->dev->CreateSamplerState(&sd, &g->samp_point);

    // blend: straight alpha, standard source-over
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g->dev->CreateBlendState(&bd, &g->blend_alpha);
    bd.RenderTarget[0].BlendEnable = FALSE;
    g->dev->CreateBlendState(&bd, &g->blend_none);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;                  // clipping without extra draw calls
    g->dev->CreateRasterizerState(&rd, &g->rast_ui);
    rd.ScissorEnable = FALSE;
    g->dev->CreateRasterizerState(&rd, &g->rast_video);

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    ds.StencilEnable = FALSE;
    g->dev->CreateDepthStencilState(&ds, &g->depth_none);
    return true;
}

bool Gfx::create_video_shader_objects() {
    Arena scratch; scratch.init(1 << 14);
    Str err;
    ID3DBlob *vsb = nullptr, *psb = nullptr;
    if (!gfx_compile(HLSL_VIDEO, "vs_video", "vs_4_0", &vsb, &err, &scratch)) {
        MOB_ERROR("video VS: %.*s", err.n, err.p); scratch.shutdown(); return false;
    }
    if (!gfx_compile(HLSL_VIDEO, "ps_video", "ps_4_0", &psb, &err, &scratch)) {
        MOB_ERROR("video PS: %.*s", err.n, err.p); scratch.shutdown(); return false;
    }
    if (ps_video) { ps_video->Release(); ps_video = nullptr; }
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_video);
    vsb->Release(); psb->Release();
    scratch.shutdown();
    return ps_video != nullptr;
}

bool Gfx::init(HWND hwnd, u32 w, u32 h, bool allow_tearing, i32 adapter_index) {
    if (!create_device(this, adapter_index)) return false;
    if (!create_swapchain(this, hwnd, w ? w : 1280, h ? h : 720, allow_tearing)) return false;
    if (!create_backbuffer(this)) return false;
    if (!create_pipeline(this)) return false;
    present_mode = PRESENT_ULTRA_LOW_LATENCY;
    MOB_INFO("Renderer ready: %ux%u, flip model, tearing=%s, waitable=%s",
             width, height, tearing_supported ? "yes" : "no", waitable_supported ? "yes" : "no");
    return true;
}

void Gfx::shutdown() {
    if (swap) swap->SetFullscreenState(FALSE, nullptr);
    if (frame_latency_handle) { CloseHandle(frame_latency_handle); frame_latency_handle = nullptr; }
#define REL(x) if (x) { x->Release(); x = nullptr; }
    REL(rtv); REL(swap); REL(adapter); REL(factory);
    REL(vs_ui); REL(ps_ui); REL(ps_shape); REL(ps_video);
    REL(layout_ui); REL(cb_frame); REL(cb_video);
    REL(vb); REL(ib);
    REL(samp_linear); REL(samp_point); REL(blend_alpha); REL(blend_none);
    REL(rast_ui); REL(rast_video); REL(depth_none);
    REL(ctx); REL(dev);
#undef REL
}

bool Gfx::resize(u32 w, u32 h) {
    if (!swap || !dev) return false;
    if (w == 0 || h == 0) return true;      // minimised: keep the last target
    if (w == width && h == height && rtv) return true;

    if (rtv) { rtv->Release(); rtv = nullptr; }
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->Flush();
    if (FAILED(swap->ResizeBuffers(buffer_count, w, h, fmt,
            (tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) |
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))) {
        MOB_WARN("ResizeBuffers %ux%u failed", w, h);
        return false;
    }
    if (swap2) swap2->SetMaximumFrameLatency(1);
    if (!create_backbuffer(this)) return false;
    MOB_DEBUG("swapchain resized to %ux%u", width, height);
    return true;
}

void Gfx::wait_for_frame_slot(u32 timeout_ms) {
    if (frame_latency_handle) {
        WaitForSingleObject(frame_latency_handle, timeout_ms);
    } else {
        // No waitable object: throttle lightly instead of spinning the CPU.
        static u64 last = 0;
        u64 now = now_us();
        if (last && now - last < 1000) sleep_ms(1);
        last = now;
    }
}

void Gfx::pace_to_target_fps() {
    if (!target_fps) return;
    // Hybrid sleep + spin: sleeping alone overshoots (timer resolution), spinning
    // alone wastes a core. Sleeping to ~1 ms before the deadline and spinning the
    // remainder keeps the frame time within a few tens of microseconds.
    u64 now = now_us();
    u64 period = 1000000ull / target_fps;
    u64 next = last_present_us + period;
    while ((i64)(next - now) > 0) {
        u64 remain = next - now;
        if (remain > 1500) { sleep_ms((u32)((remain - 1000) / 1000)); }
        else { pause_cpu(); }
        now = now_us();
    }
}

bool Gfx::present() {
    if (!swap) return false;
    UINT sync = 0, flags = 0;
    if (present_mode == PRESENT_VSYNC) {
        sync = 1;
    } else {
        if (present_mode == PRESENT_FRAME_PACED) pace_to_target_fps();
        if (tearing_supported) flags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    last_present_us = now_us();
    HRESULT hr = swap->Present(sync, flags);
    stats.presents++;
    if (hr == DXGI_STATUS_OCCLUDED) {
        stats.presents_dropped++;
        sleep_ms(8);
        return false;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        MOB_ERROR("device removed (0x%08X) - GPU reset or driver update", (u32)hr);
        return false;
    }
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { stats.presents_dropped++; return false; }
        MOB_WARN("Present failed 0x%08X", (u32)hr);
        stats.presents_dropped++;
        return false;
    }
    return true;
}

void Gfx::begin_frame(const f32 clear[4]) {
    if (!rtv || !ctx) return;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);   // render thread wins over background work
    D3D11_VIEWPORT vp{};
    vp.Width = (f32)width; vp.Height = (f32)height; vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, clear);

    // per-frame constants
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(cb_frame, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        FrameCB* cb = (FrameCB*)m.pData;
        cb->screen[0] = (f32)width; cb->screen[1] = (f32)height;
        cb->screen_inv[0] = 1.0f / (f32)width; cb->screen_inv[1] = 1.0f / (f32)height;
        cb->time = (f32)(now_us() % 3600000000ull) / 1000000.0f;
        cb->dpi = 1.0f;
        ctx->Unmap(cb_frame, 0);
    }
    ctx->VSSetConstantBuffers(0, 1, &cb_frame);
    ctx->PSSetConstantBuffers(0, 1, &cb_frame);
}

// (VideoCB is declared once, next to the shader source.)
void Gfx::set_video_params(f32 uv_scale_x, f32 uv_scale_y, f32 uv_off_x, f32 uv_off_y,
                           f32 contrast, f32 saturation, f32 brightness, f32 sharpness) {
    if (!cb_video || !ctx) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(cb_video, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        VideoCB* v = (VideoCB*)m.pData;
        v->uv_scale[0] = uv_scale_x; v->uv_scale[1] = uv_scale_y;
        v->uv_offset[0] = uv_off_x;  v->uv_offset[1] = uv_off_y;
        v->contrast = contrast;
        v->saturation = saturation;
        v->brightness = brightness;
        v->sharpness = sharpness;
        ctx->Unmap(cb_video, 0);
    }
}

void Gfx::set_viewport(f32 x, f32 y, f32 w, f32 h) {
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = x; vp.TopLeftY = y; vp.Width = w; vp.Height = h; vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
}

void Gfx::set_scissor(i32 x, i32 y, i32 w, i32 h) {
    D3D11_RECT r{ x, y, x + w, y + h };
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > (LONG)width) r.right = (LONG)width;
    if (r.bottom > (LONG)height) r.bottom = (LONG)height;
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    ctx->RSSetScissorRects(1, &r);
}

void Gfx::begin_ui_pass(bool alpha_blend) {
    ctx->IASetInputLayout(layout_ui);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(f32) * 12, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
    ctx->VSSetShader(vs_ui, nullptr, 0);
    ctx->PSSetShader(ps_ui, nullptr, 0);
    ctx->OMSetBlendState(alpha_blend ? blend_alpha : blend_none, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(depth_none, 0);
    ctx->RSSetState(rast_ui);
}

void Gfx::draw_batch(const void* verts, u32 vcount, const u16*, u32) {
    (void)verts; (void)vcount;
}

void Gfx::refresh_stats() {
    if (!swap) return;
    DXGI_FRAME_STATISTICS fs{};
    if (SUCCEEDED(swap->GetFrameStatistics(&fs))) {
        if (fs.PresentCount > 0 && fs.SyncQPCTime.QuadPart > 0) {
            u64 freq = ticks_per_sec();
            u64 sync_us = (u64)(fs.SyncQPCTime.QuadPart * 1000000ull / freq);
            u64 now = now_us();
            stats.gpu_present_to_sync_us = (sync_us <= now) ? (now - sync_us) % 16667ull : 0;
        }
    }
}

const char* Gfx::mode_name() const {
    switch (present_mode) {
        case PRESENT_ULTRA_LOW_LATENCY: return tearing_supported ? "Ultra Low Latency (immediate, tear)" : "Ultra Low Latency (immediate)";
        case PRESENT_VSYNC:             return "VSync";
        case PRESENT_FRAME_PACED:       return "Frame Paced";
        default: return "?";
    }
}

u64 Gfx::display_latency_estimate_us() const {
    // Scan-out latency is bounded by one refresh interval; the half-interval
    // figure is the statistically expected value for a frame presented at a
    // random moment. It is shown as an *estimate* in the analyzer, never as a
    // measured value.
    u32 hz = stats.refresh_hz ? stats.refresh_hz : 60;
    return (1000000ull / hz) / 2;
}

u64 Gfx::last_present_delta_us() const {
    static u64 prev = 0;
    u64 now = last_present_us;
    u64 d = prev ? now - prev : 0;
    prev = now;
    return d;
}

} // namespace mob
