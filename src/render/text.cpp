// ============================================================================
//  MOBILADOR - src/render/text.cpp
// ============================================================================
#include "text.h"
#include "../core/log.h"
#include <math.h>
#include <new>

namespace mob {

// ------------------------------------------------------------------ GDI blob
static bool create_font_face(Font& f, const wchar_t* family, u32 px, bool bold, f32 ui_scale) {
    // Reuse the DC across rebuilds. Creating one here on every font rebuild
    // leaked nine device contexts per UI-scale change, because nothing ever
    // deleted the previous one.
    HDC dc = (HDC)f.dc;
    if (!dc) {
        HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!dc) return false;
        f.dc = dc;
    }

    u32 size = (u32)((f32)px * ui_scale + 0.5f);
    if (size < 8) size = 8;

    LOGFONTW lf{};
    lf.lfHeight = -(LONG)size;
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    // Grayscale AA (not ClearType): subpixel AA cannot be alpha blended over
    // arbitrary backgrounds, and our UI blends text over video.
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    u32 n = (u32)wcslen(family);
    if (n > 31) n = 31;
    memcpy(lf.lfFaceName, family, n * sizeof(wchar_t));

    HFONT font = CreateFontIndirectW(&lf);
    if (!font) return false;
    f.face = font;
    f.px   = size;
    f.bold = bold;
    return true;
}

static bool create_atlas(Font& f, Gfx* gfx, u32 w, u32 h) {
    f.atlas_w = w; f.atlas_h = h;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;              // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = (HDC)f.dc;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) return false;
    f.dib = dib;
    f.pixels = (u8*)bits;
    f.old_dib = SelectObject(dc, dib);
    SelectObject(dc, (HFONT)f.face);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    memset(bits, 0, (usize)w * h * 4);

    // GPU-visible copy of the same atlas (R8: coverage only).
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    // DEFAULT, not DYNAMIC: the glyphs are written with UpdateSubresource, and
    // D3D11 does not perform that copy on a DYNAMIC resource (DYNAMIC requires
    // Map/Unmap). With DYNAMIC the upload silently did nothing, the atlas
    // stayed zeroed, every glyph sampled as coverage 0 and the whole UI drew as
    // opaque black blocks - with no error anywhere. The atlas is written once
    // per glyph and only ever read by the GPU, so DEFAULT costs nothing here.
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    if (FAILED(gfx->dev->CreateTexture2D(&td, nullptr, &f.tex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R8_UNORM;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(gfx->dev->CreateShaderResourceView(f.tex, &srvd, &f.srv))) return false;
    return true;
}

static bool upload_atlas_row(Font& f, Gfx* gfx, u32 x, u32 y, u32 w, u32 h) {
    if (!f.tex || !f.pixels) return false;
    D3D11_BOX box{};
    box.left = x; box.right = x + w;
    box.top = y;  box.bottom = y + h;
    box.front = 0; box.back = 1;
    // Pack the coverage bytes into a small contiguous staging buffer.
    u8* tmp = (u8*)malloc((usize)w * h);
    if (!tmp) return false;
    for (u32 r = 0; r < h; ++r) {
        const u8* src = f.pixels + ((usize)(y + r) * f.atlas_w + x) * 4;
        u8* dst = tmp + (usize)r * w;
        for (u32 c = 0; c < w; ++c) dst[c] = src[c * 4];
    }
    gfx->ctx->UpdateSubresource(f.tex, 0, &box, tmp, w, 0);
    free(tmp);
    return true;
}

// ------------------------------------------------------------------- glyphs
static bool rasterize_glyph(Font& f, Gfx* gfx, u32 cp) {
    HDC dc = (HDC)f.dc;
    MAT2 mat{};
    mat.eM11.value = 1; mat.eM22.value = 1;
    GLYPHMETRICS gm{};
    DWORD size = GetGlyphOutlineW(dc, cp, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &mat);
    if (size == GDI_ERROR) return false;

    Glyph g{};
    g.adv = (f32)gm.gmCellIncX;
    g.bx  = (f32)gm.gmptGlyphOrigin.x;
    g.by  = (f32)gm.gmptGlyphOrigin.y;
    g.w   = (f32)gm.gmBlackBoxX;
    g.h   = (f32)gm.gmBlackBoxY;

    if (size > 0 && gm.gmBlackBoxX > 0 && gm.gmBlackBoxY > 0) {
        u8* buf = (u8*)malloc(size);
        if (!buf) return false;
        if (GetGlyphOutlineW(dc, cp, GGO_GRAY8_BITMAP, &gm, size, buf, &mat) == GDI_ERROR) {
            free(buf);
            return false;
        }
        u32 pitch = (gm.gmBlackBoxX + 3) & ~3u;

        // shelf pack
        if (f.shelf_x + gm.gmBlackBoxX + 2 > f.atlas_w) {
            f.shelf_x = 2;
            f.shelf_y += f.shelf_h + 2;
            f.shelf_h = 0;
        }
        if (f.shelf_y + gm.gmBlackBoxY + 2 > f.atlas_h) {
            free(buf);
            return false;                       // atlas full: caller falls back to .notdef
        }
        u32 ax = f.shelf_x, ay = f.shelf_y;
        if (gm.gmBlackBoxY > f.shelf_h) f.shelf_h = gm.gmBlackBoxY;

        for (u32 r = 0; r < gm.gmBlackBoxY; ++r) {
            u8* dst = f.pixels + ((usize)(ay + r) * f.atlas_w + ax) * 4;
            const u8* src = buf + (usize)r * pitch;
            for (u32 c = 0; c < gm.gmBlackBoxX; ++c) {
                // GGO_GRAY8 gives 0..64 coverage; 0 is "no coverage" and 64 is
                // full coverage. Values are scaled to 0..255 alpha.
                u32 cov = src[c];
                u32 a = cov >= 64 ? 255 : cov * 4;
                dst[c * 4 + 0] = (u8)a;
                dst[c * 4 + 1] = (u8)a;
                dst[c * 4 + 2] = (u8)a;
                dst[c * 4 + 3] = 255;
            }
        }
        upload_atlas_row(f, gfx, ax, ay, gm.gmBlackBoxX, gm.gmBlackBoxY);

        g.u0 = (f32)ax / (f32)f.atlas_w;
        g.v0 = (f32)ay / (f32)f.atlas_h;
        g.u1 = (f32)(ax + gm.gmBlackBoxX) / (f32)f.atlas_w;
        g.v1 = (f32)(ay + gm.gmBlackBoxY) / (f32)f.atlas_h;
        f.shelf_x = ax + gm.gmBlackBoxX + 2;
        free(buf);
    } else {
        // whitespace: keep advance, no bitmap
        g.u0 = g.v0 = g.u1 = g.v1 = 0;
        g.w = g.h = 0;
    }
    g.valid = true;
    f.glyphs->set(Str((const char*)&cp, 4), g);   // 4-byte key = codepoint
    return true;
}

u32 TextRenderer::next_codepoint(Str s, u32* i) {
    if (*i >= s.n) return 0;
    u8 c0 = (u8)s[(*i)++];
    if (c0 < 0x80) return c0;
    u32 cp = 0; u32 extra = 0;
    if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; extra = 1; }
    else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; extra = 2; }
    else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; extra = 3; }
    else return 0xFFFD;
    for (u32 k = 0; k < extra; ++k) {
        if (*i >= s.n) return 0xFFFD;
        u8 cx = (u8)s[(*i)++];
        if ((cx & 0xC0) != 0x80) { return 0xFFFD; }
        cp = (cp << 6) | (cx & 0x3F);
    }
    return cp;
}

static void font_metrics(Font& f, const wchar_t* family, u32 px, bool bold, f32 scale, Gfx* gfx) {
    HDC dc = (HDC)f.dc;
    TEXTMETRICW tm{};
    SelectObject(dc, (HFONT)f.face);
    GetTextMetricsW(dc, &tm);
    f.ascent  = (f32)tm.tmAscent;
    f.descent = (f32)tm.tmDescent;
    f.line_height = (f32)(tm.tmHeight + tm.tmExternalLeading);
    ABC abc{};
    if (GetCharABCWidthsW(dc, ' ', ' ', &abc)) f.space_adv = (f32)(abc.abcA + (i32)abc.abcB + abc.abcC);
    else f.space_adv = (f32)(px / 3);
    (void)family; (void)px; (void)scale; (void)gfx;
}

struct FontSpec { const wchar_t* family; u32 px; bool bold; };
static const FontSpec kFontSpecs[FONT_COUNT] = {
    { L"Segoe UI", 10, false },   // TINY
    { L"Segoe UI", 12, false },   // SMALL
    { L"Segoe UI", 14, false },   // BODY
    { L"Segoe UI", 13, true  },   // LABEL (semibold)
    { L"Segoe UI", 18, true  },   // TITLE
    { L"Segoe UI", 26, true  },   // H1
    { L"Segoe UI", 40, true  },   // DISPLAY
    { L"Consolas",  13, false },  // MONO
    { L"Consolas",  13, true  },  // MONO_BOLD
};

static bool build_fonts(TextRenderer* tr, f32 ui_scale) {
    for (int i = 0; i < FONT_COUNT; ++i) {
        Font& f = tr->fonts[i];
        const FontSpec& sp = kFontSpecs[i];
        if (!create_font_face(f, sp.family, sp.px, sp.bold, ui_scale)) {
            if (!create_font_face(f, L"Arial", sp.px, sp.bold, ui_scale)) return false;
        }
        if (!create_atlas(f, tr->gfx, 1024, 1024)) return false;
        font_metrics(tr->fonts[i], sp.family, sp.px, sp.bold, ui_scale, tr->gfx);
    }
    for (int i = 0; i < FONT_COUNT; ++i) {
        for (u32 c = 32; c < 127; ++c) tr->glyph((FontId)i, c);
        Font& f = tr->fonts[i];
        Glyph g{};
        g.adv = f.px * 0.55f;
        g.valid = true;
        f.missing = g;
    }
    return true;
}

// Reads one atlas back through a staging texture and reports whether any
// coverage arrived. The 1.0.1 failure mode (UpdateSubresource on a DYNAMIC
// texture) produced no error, no warning and no visual clue beyond the black
// blocks; this check turns it into one precise line in the log.
static bool atlas_has_content(Font& f, Gfx* gfx) {
    if (!f.tex) return false;
    D3D11_TEXTURE2D_DESC td{};
    f.tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(gfx->dev->CreateTexture2D(&td, nullptr, &staging))) return false;
    gfx->ctx->CopyResource(staging, f.tex);
    bool any = false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(gfx->ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (u32 y = 0; y < td.Height && !any; ++y) {
            const u8* row = (const u8*)m.pData + (usize)y * m.RowPitch;
            for (u32 x = 0; x < td.Width; ++x) if (row[x]) { any = true; break; }
        }
        gfx->ctx->Unmap(staging, 0);
    }
    staging->Release();
    return any;
}

bool TextRenderer::init(Gfx* gfx, f32 ui_scale) {
    arena.init(1 << 20);
    scale = ui_scale;
    this->gfx = gfx;
    for (int i = 0; i < FONT_COUNT; ++i) {
        Font& f = fonts[i];
        f.glyphs = (StrMap<Glyph>*)arena.alloc(sizeof(StrMap<Glyph>), alignof(StrMap<Glyph>));
        new (f.glyphs) StrMap<Glyph>();
        f.glyphs->init(&arena, 512);
    }
    if (!build_fonts(this, ui_scale)) {
        MOB_ERROR("text: font initialisation failed");
        return false;
    }
    // One-off check of the upload path: an empty atlas means the window will
    // open with unreadable text, which is not something the user can diagnose.
    {
        u32 checked = 0, empty = 0;
        for (int i = 0; i < FONT_COUNT; ++i) {
            if (!fonts[i].tex) continue;
            ++checked;
            if (!atlas_has_content(fonts[i], gfx)) ++empty;
        }
        if (empty)
            MOB_ERROR("text: %u of %u glyph atlases read back EMPTY - text will draw as solid blocks",
                      empty, checked);
        else
            MOB_DEBUG("text: %u glyph atlases verified (coverage uploaded)", checked);
    }

    ok = true;
    MOB_DEBUG("text: atlases ready (scale %.2f, %d faces)", ui_scale, (int)FONT_COUNT);
    return true;
}

void TextRenderer::shutdown() {
    for (int i = 0; i < FONT_COUNT; ++i) {
        Font& f = fonts[i];
        if (f.srv) f.srv->Release();
        if (f.tex) f.tex->Release();
        if (f.dc && f.old_dib) SelectObject((HDC)f.dc, (HGDIOBJ)f.old_dib);
        if (f.dib) DeleteObject((HGDIOBJ)f.dib);
        if (f.face) DeleteObject((HGDIOBJ)f.face);
        if (f.dc) DeleteDC((HDC)f.dc);
        f.srv = nullptr; f.tex = nullptr; f.dib = nullptr; f.face = nullptr; f.dc = nullptr;
    }
    arena.shutdown();
}

bool TextRenderer::set_scale(f32 ui_scale) {
    if (ui_scale <= 0.1f) ui_scale = 1.0f;
    for (int i = 0; i < FONT_COUNT; ++i) {
        Font& f = fonts[i];
        if (f.srv) { f.srv->Release(); f.srv = nullptr; }
        if (f.tex) { f.tex->Release(); f.tex = nullptr; }
        if (f.dc && f.old_dib) SelectObject((HDC)f.dc, (HGDIOBJ)f.old_dib);
        if (f.dib) DeleteObject((HGDIOBJ)f.dib);
        if (f.face) DeleteObject((HGDIOBJ)f.face);
        f.dib = nullptr; f.pixels = nullptr; f.old_dib = nullptr; f.face = nullptr;
        f.glyphs->clear();
        f.shelf_x = 2; f.shelf_y = 2; f.shelf_h = 0;
    }
    scale = ui_scale;
    if (!build_fonts(this, ui_scale)) return false;
    MOB_INFO("text: atlases rebuilt at %.2f scale", ui_scale);
    return true;
}

Glyph* TextRenderer::glyph(FontId fid, u32 cp) {
    Font& f = fonts[fid];
    Str key((const char*)&cp, 4);
    Glyph* g = f.glyphs->find(key);
    if (g) return g;
    if (!rasterize_glyph(f, this->gfx, cp)) {
        f.glyphs->set(key, f.missing);
        return &f.missing;
    }
    return f.glyphs->find(key);
}

f32 TextRenderer::advance(FontId fid, u32 cp) {
    Glyph* g = glyph(fid, cp);
    return g && g->valid ? g->adv : 0;
}

f32 TextRenderer::measure(FontId fid, Str utf8) {
    f32 w = 0;
    u32 i = 0;
    while (i < utf8.n) {
        u32 cp = next_codepoint(utf8, &i);
        if (!cp) break;
        if (cp == '\n') { w = 0; continue; }
        Glyph* g = glyph(fid, cp);
        w += (g && g->valid) ? g->adv : 0;
    }
    return w;
}

f32 TextRenderer::line_height(FontId fid) { return fonts[fid].line_height; }
f32 TextRenderer::ascent(FontId fid) { return fonts[fid].ascent; }
const char* TextRenderer::family_name(FontId fid) const { return fonts[fid].bold ? "Segoe UI Semibold" : "Segoe UI"; }

} // namespace mob
