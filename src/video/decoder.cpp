// ============================================================================
//  MOBILADOR - src/video/decoder.cpp
// ============================================================================
#include "decoder.h"
#include "../core/log.h"
#include <codecapi.h>
#include <mferror.h>
#include <icodecapi.h>      // ICodecAPI: low-latency decode mode

namespace mob {

// ---------------------------------------------------------------------------
// GUIDs. The mingw import library (mfuuid) only exports a subset of the Media
// Foundation symbols, so the ones we use are defined here with their documented
// values. This keeps the build independent of the SDK version.
// ---------------------------------------------------------------------------
#define MOB_DEF_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }

MOB_DEF_GUID(G_MFMediaType_Video,          0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
MOB_DEF_GUID(G_MFVideoFormat_H264,         0x34363248, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
MOB_DEF_GUID(G_MFVideoFormat_HEVC,         0x43564548, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
MOB_DEF_GUID(G_MFVideoFormat_NV12,         0x3231564E, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
MOB_DEF_GUID(G_MFVideoFormat_YV12,         0x32315659, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
MOB_DEF_GUID(G_MFT_CATEGORY_VIDEO_DECODER, 0xD6C02D4B, 0x6833, 0x45B4, 0x97, 0x1A, 0x05, 0xA4, 0xB0, 0x4B, 0xAB, 0x91);
MOB_DEF_GUID(G_MF_MT_MAJOR_TYPE,           0x48EBA18E, 0xF8C9, 0x4687, 0xBF, 0x11, 0x0A, 0x74, 0xC9, 0xF9, 0x6A, 0x8F);
MOB_DEF_GUID(G_MF_MT_SUBTYPE,              0xF7E34C9A, 0x42E8, 0x4714, 0xB7, 0x4B, 0xCB, 0x29, 0xD7, 0x2C, 0x35, 0xE5);
MOB_DEF_GUID(G_MF_MT_FRAME_SIZE,           0x1652C33D, 0xD6B2, 0x4012, 0xB8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xA3, 0x7D);
MOB_DEF_GUID(G_MF_MT_FRAME_RATE,           0xC459A2E8, 0x3D2C, 0x4E44, 0xB1, 0x32, 0xFE, 0xE5, 0x15, 0x6C, 0x7B, 0xB0);
MOB_DEF_GUID(G_MF_MT_PIXEL_ASPECT_RATIO,   0xC6376A1E, 0x8D0A, 0x4027, 0xBE, 0x45, 0x6D, 0x9A, 0x0A, 0xD3, 0x9B, 0xB6);
MOB_DEF_GUID(G_MF_MT_INTERLACE_MODE,       0xE2724BB8, 0xE676, 0x4806, 0xB4, 0xB2, 0xA8, 0xD6, 0xEF, 0xB4, 0x4C, 0xCD);
MOB_DEF_GUID(G_MF_MT_ALL_SAMPLES_INDEPENDENT, 0xC9173739, 0x5E56, 0x461C, 0xB7, 0x13, 0x46, 0xFB, 0x99, 0x5C, 0xB9, 0x5F);
MOB_DEF_GUID(G_MF_MT_USER_DATA,            0xB6BC765F, 0x4C3B, 0x40A4, 0xBD, 0x51, 0x25, 0x35, 0xB6, 0x6F, 0xE0, 0x9D);
MOB_DEF_GUID(G_MF_MT_AVG_BITRATE,          0x20332624, 0xFB0D, 0x4D9E, 0xBD, 0x0D, 0xCB, 0xF6, 0x78, 0x6C, 0x10, 0x2E);
MOB_DEF_GUID(G_CODECAPI_AVLowLatencyMode,  0x9C27891A, 0xED7A, 0x40E1, 0x88, 0xE8, 0xB2, 0x27, 0x27, 0xA0, 0x24, 0xEE);
MOB_DEF_GUID(G_CODECAPI_AVDecNumWorkerThreads, 0x814C1B19, 0xDF99, 0x4562, 0x83, 0x24, 0xDB, 0x6C, 0x5B, 0x8F, 0xB4, 0xB5);
MOB_DEF_GUID(G_CODECAPI_AVDecVideoAcceleration_H264, 0xF7DB8D10, 0x6FD1, 0x4CA9, 0xBF, 0x74, 0x1D, 0x48, 0x55, 0x48, 0x16, 0x2E);

// Local accessors keep the call sites short while the definitions stay in one
// place. Every GUID below is a documented Media Foundation attribute value.
#define MF_INTERLACE_PROGRESSIVE 2
static const GUID* mt_major()        { return &G_MF_MT_MAJOR_TYPE; }
static const GUID* mt_subtype()      { return &G_MF_MT_SUBTYPE; }
static const GUID* mt_frame_size()   { return &G_MF_MT_FRAME_SIZE; }
static const GUID* mt_frame_rate()   { return &G_MF_MT_FRAME_RATE; }
static const GUID* mt_par()          { return &G_MF_MT_PIXEL_ASPECT_RATIO; }
static const GUID* mt_interlace()    { return &G_MF_MT_INTERLACE_MODE; }
static const GUID* mt_independent()  { return &G_MF_MT_ALL_SAMPLES_INDEPENDENT; }
static const GUID* mt_user_data()    { return &G_MF_MT_USER_DATA; }
static const GUID* mt_bitrate()      { return &G_MF_MT_AVG_BITRATE; }

const GUID* MfGuids::media_type_video()           { return &G_MFMediaType_Video; }
const GUID* MfGuids::video_format_h264()          { return &G_MFVideoFormat_H264; }
const GUID* MfGuids::video_format_hevc()          { return &G_MFVideoFormat_HEVC; }
const GUID* MfGuids::video_format_nv12()          { return &G_MFVideoFormat_NV12; }
const GUID* MfGuids::mft_category_video_decoder() { return &G_MFT_CATEGORY_VIDEO_DECODER; }
void mf_guids_anchor() {}

static const GUID* subtype_for(VideoCodecId c) {
    return c == VCODEC_H265 ? MfGuids::video_format_hevc() : MfGuids::video_format_h264();
}

// ---------------------------------------------------------------------------
// MFT enumeration. Hardware first, then (only if allowed) the software
// Microsoft decoder, so a machine without a GPU decoder still works.
// ---------------------------------------------------------------------------
bool Decoder::create_transform(bool hardware, bool prefer_d3d11) {
    MFT_REGISTER_TYPE_INFO in_type{ *MfGuids::media_type_video(), *subtype_for(codec) };
    MFT_REGISTER_TYPE_INFO out_type{ *MfGuids::media_type_video(), *MfGuids::video_format_nv12() };

    // HARDWARE + SORTANDFILTER is the combination the GPU decoders advertise
    // themselves under. MFT_ENUM_FLAG_ASYNCMFT must be present or the Intel and
    // NVIDIA decoders (which are asynchronous) are filtered out.
    UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER;
    if (hardware) flags |= MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT;

    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(*MfGuids::mft_category_video_decoder(), flags, &in_type, &out_type, &acts, &count);
    if (FAILED(hr) || count == 0) {
        // Some drivers only expose NV12 through a decoder that also advertises
        // other output formats; retry without an output restriction.
        hr = MFTEnumEx(*MfGuids::mft_category_video_decoder(), flags, &in_type, nullptr, &acts, &count);
    }
    if (FAILED(hr) || count == 0) {
        MOB_WARN("no %s decoder found (codec=%d, hardware=%d)", hardware ? "hardware" : "software",
                 (int)codec, (int)hardware);
        return false;
    }

    bool created = false;
    for (UINT32 i = 0; i < count && !created; ++i) {
        IMFTransform* t = nullptr;
        if (FAILED(acts[i]->ActivateObject(__uuidof(IMFTransform), (void**)&t)) || !t) {
            continue;
        }
        // Name for the diagnostics page.
        WCHAR wname[128] = { 0 };
        UINT32 nlen = 0;
        if (SUCCEEDED(acts[i]->GetString(MFT_FRIENDLY_NAME_Attribute, wname, 128, &nlen))) {
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, decoder_name, sizeof(decoder_name) - 1, nullptr, nullptr);
        } else {
            snprintf(decoder_name, sizeof(decoder_name), "Media Foundation decoder #%u", i);
        }

        // ---- D3D11 aware output (zero copy)
        // The MFT is handed a *device manager*, not a device: that is the only
        // way it can allocate its own DXGI textures, which is what removes the
        // copy through system memory.
        if (prefer_d3d11 && !dxgi_manager) {
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&dxgi_reset_token, &dxgi_manager))) {
                if (FAILED(dxgi_manager->ResetDevice(gfx->dev, dxgi_reset_token))) {
                    dxgi_manager->Release();
                    dxgi_manager = nullptr;
                }
            }
        }
        if (dxgi_manager) {
            HRESULT mhr = t->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)dxgi_manager);
            has_d3d_manager = SUCCEEDED(mhr);
            if (!has_d3d_manager) MOB_DEBUG("MFT refused the D3D11 device manager (0x%08X)", (u32)mhr);
        }

        // ---- asynchronous MFTs advertise an event generator. Those are the
        //      common case for Intel/NVIDIA/AMD hardware decoders, and driving
        //      them with the event model is what keeps their latency minimal
        //      (a sample is supplied the instant the decoder asks for it).
        IMFMediaEventGenerator* gen = nullptr;
        if (SUCCEEDED(t->QueryInterface(__uuidof(IMFMediaEventGenerator), (void**)&gen)) && gen) {
            event_gen = gen;
            async_mode = true;
        }

        // ---- low latency mode
        ICodecAPI* capi = nullptr;
        if (SUCCEEDED(t->QueryInterface(__uuidof(ICodecAPI), (void**)&capi)) && capi) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
            if (SUCCEEDED(capi->SetValue(&G_CODECAPI_AVLowLatencyMode, &v))) low_latency_enabled = true;
            // Two worker threads is the point where a 1080p hardware decoder is
            // fast enough without adding the cross-thread handoff that extra
            // slices would introduce.
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = 2;
            capi->SetValue(&G_CODECAPI_AVDecNumWorkerThreads, &v);
            VariantInit(&v);
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
            capi->SetValue(&G_CODECAPI_AVDecVideoAcceleration_H264, &v);
            capi->Release();
        }

        transform = t;
        if (activate) activate->Release();
        activate = acts[i];
        activate->AddRef();
        created = true;
    }

    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (!created) {
        snprintf(last_error, sizeof(last_error), "decoder could not be activated");
        return false;
    }
    return true;
}

bool Decoder::configure_input_type() {
    IMFMediaType* type = nullptr;
    if (FAILED(MFCreateMediaType(&type))) return false;
    type->SetGUID(*mt_major(), *MfGuids::media_type_video());
    type->SetGUID(*mt_subtype(), *subtype_for(codec));
    type->SetUINT32(*mt_interlace(), MF_INTERLACE_PROGRESSIVE);
    type->SetUINT64(*mt_frame_size(), ((u64)width << 32) | height);
    type->SetUINT64(*mt_frame_rate(), ((u64)60 << 32) | 1);
    type->SetUINT64(*mt_par(), ((u64)1 << 32) | 1);
    type->SetUINT32(*mt_bitrate(), 12000000);
    type->SetUINT32(*mt_independent(), TRUE);
    if (extradata_size > 0) {
        // Decoder specific data (AVCDecoderConfigurationRecord / HEVC equivalent)
        type->SetBlob(*mt_user_data(), extradata, extradata_size);
    }
    HRESULT hr = transform->SetInputType(0, type, 0);
    if (FAILED(hr)) {
        // Retry without the extradata blob: hardware decoders often prefer to
        // discover parameter sets from the stream itself, and the caller also
        // prepends them to every key frame.
        hr = transform->SetInputType(0, type, 0);
        if (extradata_size > 0) {
            MOB_DEBUG("decoder rejected extradata (0x%08X), falling back to in-band parameter sets", (u32)hr);
            inband_parameter_sets = true;
            IMFMediaType* t2 = nullptr;
            if (SUCCEEDED(MFCreateMediaType(&t2))) {
                t2->SetGUID(*mt_major(), *MfGuids::media_type_video());
                t2->SetGUID(*mt_subtype(), *subtype_for(codec));
                t2->SetUINT32(*mt_interlace(), MF_INTERLACE_PROGRESSIVE);
                t2->SetUINT64(*mt_frame_size(), ((u64)width << 32) | height);
                t2->SetUINT64(*mt_frame_rate(), ((u64)60 << 32) | 1);
                t2->SetUINT32(*mt_independent(), TRUE);
                hr = transform->SetInputType(0, t2, 0);
                t2->Release();
            }
        }
    }
    type->Release();
    if (FAILED(hr)) {
        snprintf(last_error, sizeof(last_error), "SetInputType failed (0x%08X) for %s",
                 (u32)hr, codec == VCODEC_H265 ? "HEVC" : "H.264");
        MOB_ERROR("%s", last_error);
        return false;
    }
    return true;
}

bool Decoder::configure_output_type(bool dxgi) {
    // Ask for NV12 first; some decoders only advertise YV12 in software mode.
    const GUID* out_subtypes[3] = { MfGuids::video_format_nv12(), &G_MFVideoFormat_YV12, nullptr };
    for (int s = 0; out_subtypes[s] != nullptr; ++s) {
        for (int i = 0; i < 32; ++i) {
            IMFMediaType* type = nullptr;
            if (FAILED(transform->GetOutputAvailableType(0, i, &type))) break;
            GUID sub = {};
            type->GetGUID(*mt_subtype(), &sub);
            if (IsEqualGUID(sub, *out_subtypes[s])) {
                // The MFT has already been told about the device manager, so
                // accepting its type as-is keeps the DXGI path.
                HRESULT hr = transform->SetOutputType(0, type, 0);
                if (SUCCEEDED(hr)) {
                    UINT64 size = 0;
                    type->GetUINT64(*mt_frame_size(), &size);
                    u32 w = (u32)(size >> 32), h = (u32)size;
                    if (w && h) { width = w; height = h; }
                    type->Release();
                    output_type_set = true;
                    return true;
                }
            }
            type->Release();
        }
    }
    snprintf(last_error, sizeof(last_error), "no acceptable output type (NV12/YV12) offered");
    return false;
}

bool Decoder::init(Gfx* g, VideoCodecId c, u32 w, u32 h, u32 fps, bool hardware, bool prefer_d3d11) {
    gfx = g;
    codec = c;
    width = w; height = h;
    set_target_fps(fps);
    arena.init(1 << 18);

    if (!create_transform(hardware, prefer_d3d11)) {
        if (hardware) {
            MOB_WARN("hardware decode unavailable, retrying in software");
            shutdown();
            if (!create_transform(false, false)) return false;
            backend = DECODE_SOFTWARE;
            snprintf(backend_name, sizeof(backend_name), "Software");
        } else {
            return false;
        }
    } else {
        backend = has_d3d_manager ? DECODE_HW_D3D11 : DECODE_HW_SYSTEM;
        snprintf(backend_name, sizeof(backend_name), "%s",
                 has_d3d_manager ? "Hardware decode (D3D11 zero-copy)"
                                 : (hardware ? "Hardware decode (system memory)" : "Software decode (system memory)"));
    }

    if (!configure_input_type()) return false;
    transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // Output type: with a D3D manager attached, negotiating it lazily (after the
    // first input) is what lets the decoder report its true DXGI capability, but
    // most MFTs accept it right away, which saves one frame of latency at start.
    if (!configure_output_type(has_d3d_manager)) {
        MOB_DEBUG("output type negotiated after first sample");
    } else if (has_d3d_manager) {
        backend = DECODE_HW_D3D11;
    }

    scratch_cap = mob_max((u32)(4 << 20), w * h);
    scratch = (u8*)arena.alloc(scratch_cap + 1024, 32);
    csd_concat_cap = scratch_cap;
    csd_concat = (u8*)arena.alloc(csd_concat_cap, 32);

    MOB_INFO("decoder: %s [%s] %ux%u, low-latency=%s", decoder_name, backend_name, width, height,
             low_latency_enabled ? "on" : "not advertised");
    return true;
}

// ---------------------------------------------------------------------------
// Uploads one system-memory NV12 frame into the two reusable staging textures.
// Row-by-row copies are used (never a full-surface memcpy) because the source
// and destination pitches differ in general; the destination RowPitch is the
// GPU's preferred alignment, so writing past the row would corrupt the image.
// ---------------------------------------------------------------------------
bool Decoder::upload_system_frame(const u8* scan0, i32 pitch) {
    if (!gfx || !gfx->dev || !gfx->ctx || !scan0 || width == 0 || height == 0) return false;
    if (pitch <= 0) pitch = (i32)width;
    if ((u32)pitch < width) pitch = (i32)width;

    const u32 cw = mob_max(width / 2, 1u);
    const u32 ch = mob_max(height / 2, 1u);

    if (!staging_tex) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width; td.Height = height;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(gfx->dev->CreateTexture2D(&td, nullptr, &staging_tex))) return false;
    }
    if (!staging_tex_uv) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = cw; td.Height = ch;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(gfx->dev->CreateTexture2D(&td, nullptr, &staging_tex_uv))) return false;
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(gfx->ctx->Map(staging_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (u32 y = 0; y < height; ++y) {
            memcpy((u8*)m.pData + (usize)y * m.RowPitch, scan0 + (usize)y * (usize)pitch, width);
        }
        gfx->ctx->Unmap(staging_tex, 0);
    } else {
        return false;
    }

    // NV12: the chroma plane follows the luma plane, half height, interleaved UV.
    const u8* uv = scan0 + (usize)pitch * (usize)height;
    if (SUCCEEDED(gfx->ctx->Map(staging_tex_uv, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (u32 y = 0; y < ch; ++y) {
            memcpy((u8*)m.pData + (usize)y * m.RowPitch, uv + (usize)y * (usize)pitch, cw * 2);
        }
        gfx->ctx->Unmap(staging_tex_uv, 0);
    } else {
        return false;
    }
    return true;
}

void Decoder::shutdown() {
    if (csd_sample) { csd_sample->Release(); csd_sample = nullptr; }
    if (input_sample) { input_sample->Release(); input_sample = nullptr; }
    if (input_buffer) { input_buffer->Release(); input_buffer = nullptr; }
    if (event_gen) { event_gen->Release(); event_gen = nullptr; }
    if (transform) { transform->Release(); transform = nullptr; }
    if (activate) { activate->Release(); activate = nullptr; }
    if (dxgi_manager) { dxgi_manager->Release(); dxgi_manager = nullptr; }
    if (staging_tex) { staging_tex->Release(); staging_tex = nullptr; }
    if (srv_staging) { srv_staging->Release(); srv_staging = nullptr; }
    if (staging_tex_uv) { staging_tex_uv->Release(); staging_tex_uv = nullptr; }
    if (srv_staging_uv) { srv_staging_uv->Release(); srv_staging_uv = nullptr; }
    for (u32 i = 0; i < view_count; ++i) {
        if (views[i].srv) views[i].srv->Release();
        if (views[i].tex) views[i].tex->Release();
        views[i].srv = nullptr;
        views[i].tex = nullptr;
    }
    view_count = 0;
    view_next = 0;
    arena.shutdown();
}

void Decoder::set_extradata(const u8* csd, u32 size) {
    if (!csd || size == 0) return;
    extradata_size = build_decoder_extradata(codec, csd, size, extradata, sizeof(extradata));
    // Keep the raw Annex-B parameter sets: they are prepended to key frames, so
    // a decoder that cannot take extradata at all still gets them in band.
    if (size <= csd_concat_cap) {
        memcpy(csd_concat, csd, size);
        csd_concat_size = size;
    }
    extradata_ready = extradata_size > 0;
    u32 w = 0, h = 0;
    if (parse_sps_dimensions(csd, size, codec, &w, &h) && w > 0 && h > 0) {
        if (w != width || h != height) {
            MOB_INFO("stream resolution %ux%u (was %ux%u)", w, h, width, height);
            width = w; height = h;
            transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            output_type_set = false;
        }
    }
    if (!output_type_set) {
        if (transform->SetInputType(0, nullptr, 0) == S_OK) {
            configure_input_type();
        }
        configure_output_type(has_d3d_manager);
    }
    MOB_DEBUG("codec config: %u bytes CSD, %u bytes extradata", size, extradata_size);
}

// ---------------------------------------------------------------------------
// Input sample construction. One IMFSample + one IMFMediaBuffer are allocated
// once and reused: MFCreateSample per frame would cost an allocation and a
// COM round trip on the hot path for no benefit.
// ---------------------------------------------------------------------------
static bool ensure_input(Decoder* d, u32 needed) {
    if (d->input_sample && d->input_capacity >= needed) return true;
    if (d->input_sample) { d->input_sample->Release(); d->input_sample = nullptr; }
    if (d->input_buffer) { d->input_buffer->Release(); d->input_buffer = nullptr; }
    u32 cap = mob_align_up32(mob_max(needed, 512u * 1024u), 64 * 1024);
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(MFCreateMemoryBuffer(cap, &buf))) return false;
    IMFSample* sample = nullptr;
    if (FAILED(MFCreateSample(&sample))) { buf->Release(); return false; }
    if (FAILED(sample->AddBuffer(buf))) { sample->Release(); buf->Release(); return false; }
    d->input_sample = sample;
    d->input_buffer = buf;
    d->input_capacity = cap;
    return true;
}

bool Decoder::submit(const u8* data, u32 size, u64 capture_us, u64 arrived_us, u32 encode_us,
                     bool keyframe, VideoFrame* out_frame) {
    if (!transform || size == 0) return false;
    u64 t_start = now_us();

    // ---- build the sample. AVCC (length prefixed) is the format the input
    //      type declares, so the Annex-B stream is normalised here.
    u32 needed = size + 4 * (size / 3 + 4) + (keyframe ? csd_concat_size : 0) + 64;
    if (needed > scratch_cap) {
        // A single frame larger than the buffer: grow once, then reuse.
        u32 cap = mob_align_up32(needed, 256 * 1024);
        scratch = (u8*)arena.alloc(cap, 32);
        scratch_cap = cap;
    }
    if (!ensure_input(this, needed)) {
        snprintf(last_error, sizeof(last_error), "input sample allocation failed");
        return false;
    }

    u32 written = 0;
    if (keyframe && csd_concat_size > 0) {
        // parameter sets first, in AVCC form
        memcpy(scratch, data, 0);   // no-op, keeps the intent explicit
        u32 csd_avcc = annexb_to_avcc(csd_concat, csd_concat_size, scratch, scratch_cap, nullptr, nullptr, nullptr);
        written = csd_avcc;
    }
    u32 nals = 0; bool key = false;
    u32 frame_bytes = annexb_to_avcc(data, size, scratch + written, scratch_cap - written, &nals,
                                     &key, nullptr);
    if (frame_bytes == 0) return false;
    written += frame_bytes;

    BYTE* dst = nullptr;
    if (FAILED(input_buffer->Lock(&dst, nullptr, nullptr))) return false;
    memcpy(dst, scratch, written);
    input_buffer->Unlock();
    input_buffer->SetCurrentLength(written);
    input_sample->SetSampleTime((LONGLONG)input_timestamp);
    input_timestamp += frame_duration_100ns;
    input_sample->SetSampleDuration((LONGLONG)frame_duration_100ns);

    if (async_mode) {
        // Wait (briefly) until the decoder says it wants input, then hand the
        // sample over. The wait is bounded: a decoder that is already saturated
        // must not stall the network thread, because dropping a frame is always
        // better than delaying the next one.
        if (!wait_for_event(METransformNeedInput, 4)) return false;
    }
    HRESULT hr = transform->ProcessInput(0, input_sample, 0);
    if (hr == MF_E_NOTACCEPTING) {
        // The decoder is still busy: force the output out and retry once.
        if (!drain_output(out_frame)) return false;
        hr = transform->ProcessInput(0, input_sample, 0);
    }
    if (FAILED(hr)) {
        if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            configure_output_type(has_d3d_manager);
            hr = transform->ProcessInput(0, input_sample, 0);
        }
        if (FAILED(hr)) {
            snprintf(last_error, sizeof(last_error), "ProcessInput failed (0x%08X)", (u32)hr);
            return false;
        }
    }
    submitted++;

    bool got = drain_output(out_frame);
    if (got && out_frame) {
        out_frame->capture_us = capture_us;
        out_frame->arrived_us = arrived_us;
        out_frame->encode_us = encode_us;
        out_frame->keyframe = key || keyframe;
        out_frame->payload_bytes = size;
        out_frame->decode_start_us = t_start;
        out_frame->decode_done_us = now_us();
        out_frame->software = (backend == DECODE_SOFTWARE);
        f32 ms = (f32)(out_frame->decode_done_us - t_start) / 1000.0f;
        stats.last_decode_ms = ms;
        stats.decode_sum_ms += ms;
        stats.decode_count++;
        stats.avg_decode_ms = (f32)(stats.decode_sum_ms / mob_max(stats.decode_count, 1u));
        stats.frames_decoded++;
    }
    return got;
}

bool Decoder::drain_output(VideoFrame* out_frame) {
    // One output per input: hardware decoders in low latency mode emit the
    // frame they were just given. Asking twice would only ever return a later
    // frame, which is by definition not the one being timed.
    if (async_mode && !wait_for_event(METransformHaveOutput, 4)) return false;
    MFT_OUTPUT_STREAM_INFO si{};
    if (FAILED(transform->GetOutputStreamInfo(0, &si))) return false;

    MFT_OUTPUT_DATA_BUFFER out{};
    out.dwStreamID = 0;
    out.pSample = nullptr;
    out.pEvents = nullptr;
    bool we_allocate = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0;

    if (we_allocate) {
        // Software decoders want a caller supplied sample.
        u32 cap = mob_max((u32)si.cbSize, width * height * 2);
        IMFMediaBuffer* buf = nullptr;
        if (FAILED(MFCreateMemoryBuffer(cap, &buf))) return false;
        IMFSample* sample = nullptr;
        if (FAILED(MFCreateSample(&sample))) { buf->Release(); return false; }
        sample->AddBuffer(buf);
        buf->Release();
        out.pSample = sample;
    }

    DWORD status = 0;
    HRESULT hr = transform->ProcessOutput(0, 1, &out, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        stats.stream_changes++;
        MOB_DEBUG("decoder stream change - renegotiating output type");
        output_type_set = false;
        if (out.pSample && we_allocate) out.pSample->Release();
        configure_output_type(has_d3d_manager);
        return false;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) {
        if (out.pSample && we_allocate) out.pSample->Release();
        if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
            snprintf(last_error, sizeof(last_error), "ProcessOutput failed (0x%08X)", (u32)hr);
        }
        return false;
    }
    if (!out.pSample) return false;

    // ---- extract the texture (or copy from system memory)
    IMFSample* sample = out.pSample;
    ID3D11Texture2D* tex = nullptr;
    u32 subresource = 0;
    IMFMediaBuffer* buffer = nullptr;
    if (SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && buffer) {
        IMFDXGIBuffer* dxgi_buf = nullptr;
        if (SUCCEEDED(buffer->QueryInterface(__uuidof(IMFDXGIBuffer), (void**)&dxgi_buf)) && dxgi_buf) {
            if (FAILED(dxgi_buf->GetResource(__uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) { tex = nullptr; }
            dxgi_buf->GetSubresourceIndex(&subresource);
            dxgi_buf->Release();
        }
        if (!tex) {
            // System memory output: two uploads (luma + chroma) into decoder
            // owned dynamic textures. This path is only used when no hardware
            // decoder could be created, so a CPU-side copy is unavoidable; it is
            // still a single copy per plane, straight into the shader resource.
            IMF2DBuffer* b2d = nullptr;
            bool have2d = SUCCEEDED(buffer->QueryInterface(__uuidof(IMF2DBuffer), (void**)&b2d)) && b2d;
            if (have2d) {
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(b2d->Lock2D(&scan0, &pitch)) && scan0) {
                    if (upload_system_frame(scan0, pitch)) texture_from_system = true;
                    b2d->Unlock2D();
                }
                b2d->Release();
            } else {
                BYTE* src = nullptr;
                DWORD cur = 0;
                if (SUCCEEDED(buffer->Lock(&src, nullptr, &cur)) && src) {
                    if (upload_system_frame(src, (i32)width)) texture_from_system = true;
                    buffer->Unlock();
                }
            }
        }
        buffer->Release();
    }

    VideoFrame frame{};
    if (tex) {
        frame.texture = tex;              // held through the sample
        frame.subresource = subresource;
        frame.srv_y  = view_for(tex, subresource, 0, false);
        frame.srv_uv = view_for(tex, subresource, 1, true);
        if (!frame.srv_y) { sample->Release(); return false; }
        frame.sample = sample;            // keep the sample alive until drawn
        if (we_allocate) { /* ownership transferred to frame */ }
    } else if (staging_tex && staging_tex_uv) {
        frame.texture = staging_tex;
        if (!srv_staging) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R8_UNORM;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            gfx->dev->CreateShaderResourceView(staging_tex, &sd, &srv_staging);
        }
        if (!srv_staging_uv) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R8G8_UNORM;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            gfx->dev->CreateShaderResourceView(staging_tex_uv, &sd, &srv_staging_uv);
        }
        frame.srv_y = srv_staging;
        frame.srv_uv = nullptr;           // software path uses a planar shader variant
        frame.sample = sample;
        frame.software = true;
    } else {
        if (we_allocate) sample->Release();
        return false;
    }
    frame.width = width;
    frame.height = height;
    produced++;
    if (out_frame) *out_frame = frame;
    else {
        // Caller does not want the frame (e.g. the mailbox is full): release.
        release_frame(&frame);
    }
    return true;
}

void Decoder::release_frame(VideoFrame* f) {
    if (!f) return;
    if (f->sample) { f->sample->Release(); f->sample = nullptr; }
    f->srv_y = nullptr;
    f->srv_uv = nullptr;
    f->texture = nullptr;
}

// ---------------------------------------------------------------------------
// Shader resource views are cached per (texture, subresource, plane): creating
// them per frame would allocate a COM object at 60-240 Hz for nothing.
// ---------------------------------------------------------------------------
ID3D11ShaderResourceView* Decoder::view_for(ID3D11Texture2D* tex, u32 subresource, u32 plane, bool uv) {
    for (u32 i = 0; i < view_count; ++i) {
        if (views[i].tex == tex && views[i].subresource == subresource && views[i].plane == plane) {
            return views[i].srv;
        }
    }
    // Planar views: the luma plane is exposed as R8_UNORM and the interleaved
    // chroma plane as R8G8_UNORM, which is what lets one draw call sample both
    // planes of an NV12 frame with no conversion pass and no copy.
    //
    // mingw's d3d11.h predates DXGI 1.1 planar views, so D3D11_TEX2D_SRV and
    // D3D11_TEX2D_ARRAY_SRV there have no PlaneSlice member. The structs below
    // reproduce the Windows SDK layout exactly and are handed to the runtime by
    // pointer; the runtime only reads the fields its ViewDimension defines.
    struct SrvTex2DPlane { UINT MostDetailedMip; UINT MipLevels; UINT PlaneSlice; };
    struct SrvTex2DArrayPlane {
        UINT MostDetailedMip; UINT MipLevels; UINT FirstArraySlice; UINT ArraySize; UINT PlaneSlice;
    };
    struct SrvDescPlane {
        DXGI_FORMAT          Format;
        D3D11_SRV_DIMENSION  ViewDimension;
        union {
            SrvTex2DPlane      tex2d;
            SrvTex2DArrayPlane tex2d_array;
            UINT               raw[10];      // never smaller than the SDK descriptor
        };
    };

    ID3D11ShaderResourceView* srv = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    const bool planar = (td.Format == DXGI_FORMAT_NV12);
    const bool array  = (td.ArraySize > 1);
    SrvDescPlane sd{};
    sd.Format = uv ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
    if (planar) {
        sd.ViewDimension = array ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D;
        if (array) {
            sd.tex2d_array.MostDetailedMip  = 0;
            sd.tex2d_array.MipLevels        = 1;
            sd.tex2d_array.FirstArraySlice  = subresource % td.ArraySize;
            sd.tex2d_array.ArraySize        = 1;
            sd.tex2d_array.PlaneSlice       = uv ? 1u : 0u;
        } else {
            sd.tex2d.MostDetailedMip = 0;
            sd.tex2d.MipLevels       = 1;
            sd.tex2d.PlaneSlice      = uv ? 1u : 0u;
        }
    } else {
        // Non-planar staging textures (the software decoder path) are plain
        // single-plane resources, so no plane slice is involved.
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.tex2d.MostDetailedMip = 0;
        sd.tex2d.MipLevels       = 1;
        sd.tex2d.PlaneSlice      = 0;
    }
    if (FAILED(gfx->dev->CreateShaderResourceView(
                   tex, (const D3D11_SHADER_RESOURCE_VIEW_DESC*)&sd, &srv)) || !srv) {
        // The driver refused the planar view (rare, and only worth handling
        // once): fall back to the whole resource so the frame still displays.
        MOB_DEBUG("planar SRV rejected (fmt %d, plane %u) - using a plain view", (int)td.Format, uv ? 1u : 0u);
        if (FAILED(gfx->dev->CreateShaderResourceView(tex, nullptr, &srv)) || !srv) return nullptr;
    }
    // Cache the view (with its own texture reference) so the next frame reuses
    // it. Round-robin eviction: the table never grows, never overflows and the
    // released view leaves no dangling entry.
    u32 slot;
    if (view_count < 64) {
        slot = view_count++;
    } else {
        slot = view_next++ & 63u;
        if (views[slot].srv) views[slot].srv->Release();
        if (views[slot].tex) views[slot].tex->Release();
    }
    tex->AddRef();
    views[slot].tex = tex;
    views[slot].subresource = subresource;
    views[slot].plane = plane;
    views[slot].srv = srv;
    return srv;
}

bool Decoder::wait_for_event(long want_type, u32 timeout_ms) {
    if (!event_gen) return true;
    u64 deadline = now_us() + (u64)timeout_ms * 1000ull;
    for (;;) {
        IMFMediaEvent* ev = nullptr;
        HRESULT hr = event_gen->GetEvent(0, &ev);
        if (hr == E_NOINTERFACE || hr == MF_E_NO_EVENTS_AVAILABLE) {
            if (now_us() >= deadline) return false;
            sleep_ms(0);
            continue;
        }
        if (FAILED(hr)) return false;
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        HRESULT status = S_OK;
        ev->GetStatus(&status);
        ev->Release();
        if (FAILED(status)) {
            snprintf(last_error, sizeof(last_error), "decoder event 0x%X failed (0x%08X)", (u32)type, (u32)status);
            return false;
        }
        if (type == want_type) return true;
        // Not what we waited for: keep draining until the deadline. Extra
        // events are always cheap bookkeeping, never a frame.
        if (now_us() >= deadline) return false;
    }
}

void Decoder::flush() {
    if (!transform) return;
    transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    input_timestamp = 0;
    MOB_DEBUG("decoder flushed (resync)");
}

void Decoder::request_keyframe() {
    // Handled by the client: it asks the phone for a sync frame and flushes.
}

} // namespace mob
