// ============================================================================
//  MOBILADOR - src/render/text.h
//  Glyph atlas + text shaping for the custom UI.
//
//  Implementation note: glyphs are rasterised *once* with GDI anti-aliasing
//  (GGO_GRAY8_BITMAP) into a shelf-packed atlas texture per size, then drawn as
//  textured quads.  There is no DirectWrite layout engine in the frame loop, so
//  laying out a label costs a few dozen adds to a vertex buffer and nothing else.
//  Glyphs are generated lazily, so non-ASCII device names render correctly too.
// ============================================================================
#pragma once

#include "color.h"
#include "gfx.h"

namespace mob {

enum FontId : int {
    FONT_TINY = 0,     // 10 px  - axis labels, tags
    FONT_SMALL = 1,    // 12 px  - secondary text, table cells
    FONT_BODY = 2,     // 14 px  - default body text
    FONT_LABEL = 3,    // 13 px  - control labels (medium weight)
    FONT_TITLE = 4,    // 18 px  - section titles
    FONT_H1 = 5,       // 26 px  - page title
    FONT_DISPLAY = 6,  // 40 px  - big numbers on the dashboard
    FONT_MONO = 7,     // 13 px  - telemetry values (tabular numbers)
    FONT_MONO_BOLD = 8,
    FONT_COUNT = 9,
};

enum TextAlign : int {
    ALIGN_LEFT = 0,
    ALIGN_CENTER,
    ALIGN_RIGHT,
};

struct Glyph {
    f32 u0, v0, u1, v1;   // atlas uv
    f32 w, h;             // bitmap size in px
    f32 bx, by;           // bearing (offset from pen to bitmap top-left)
    f32 adv;              // advance width
    bool valid = false;
};

struct Font {
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D*          tex = nullptr;
    u8*                       pixels = nullptr;   // CPU staging (RGBA, written through GDI)
    u32  atlas_w = 1024, atlas_h = 1024;
    u32  shelf_x = 2, shelf_y = 2, shelf_h = 0;
    u32  px = 14;
    bool bold = false;
    f32  ascent = 0, descent = 0, line_height = 0;
    f32  space_adv = 4;
    void* face = nullptr;         // HFONT
    void* dc = nullptr;           // HDC (memory DC owning the DIB)
    void* dib = nullptr;          // HBITMAP
    void* old_dib = nullptr;
    StrMap<Glyph>* glyphs = nullptr;
    Glyph missing{};
    // hash of codepoints the atlas already contains
    bool dirty = false;
};

struct TextRenderer {
    Font fonts[FONT_COUNT];
    Arena arena;
    Gfx*  gfx = nullptr;
    bool  ok = false;

    bool init(Gfx* gfx, f32 ui_scale);
    void shutdown();
    // Rebuilds the atlas when the DPI scale changes (glyphs are hinted per size).
    bool set_scale(f32 ui_scale);
    Glyph* glyph(FontId f, u32 codepoint);
    f32   advance(FontId f, u32 codepoint);
    f32   measure(FontId f, Str utf8);
    f32   line_height(FontId f);
    f32   ascent(FontId f);
    // Splits UTF-8 into codepoints.
    static u32 next_codepoint(Str s, u32* i);
    const char* family_name(FontId f) const;
    f32  scale = 1.0f;
};

} // namespace mob
