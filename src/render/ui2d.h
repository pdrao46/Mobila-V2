// ============================================================================
//  MOBILADOR - src/render/ui2d.h
//  Batched 2D drawing for the custom UI.
//
//  Everything the interface needs (cards, buttons, sliders, sparklines,
//  vector icons, glyphs) is emitted into ONE vertex buffer and ONE index
//  buffer per frame and executed as a handful of draw calls.  No per-widget
//  draw call, no render target switches, no CPU-side passes: the whole window
//  is one pass, which keeps driver overhead - and therefore present latency -
//  negligible even at 240 Hz.
// ============================================================================
#pragma once

#include "color.h"
#include "gfx.h"
#include "text.h"
#include "../ui/Icons.generated.h"

namespace mob {

struct UiVert {
    f32 x, y;
    f32 u, v;
    f32 r, g, b, a;
    f32 hw, hh, radius, border;
};

struct DrawCmd {
    ID3D11ShaderResourceView* tex;
    i32 clip[4];
    u32 first_index;
    u32 index_count;
    bool shape;
    bool linear_filter;
    bool coverage;      // texture carries coverage in .r (glyph atlas), not colour
};

// Current UI scale, published for the layout macro used by the screens.
extern f32 g_ui_scale;

struct Rect {
    f32 x = 0, y = 0, w = 0, h = 0;
    bool contains(f32 px, f32 py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    f32 cx() const { return x + w * 0.5f; }
    f32 cy() const { return y + h * 0.5f; }
    f32 r()  const { return x + w; }
    f32 b()  const { return y + h; }
};

struct Ui2D {
    Gfx* gfx = nullptr;
    TextRenderer* txt = nullptr;      // glyph atlas renderer
    Arena arena;

    UiVert*  vbase = nullptr;
    u16*     ibase = nullptr;
    u32      vcap = 0, icap = 0;
    u32      vcount = 0, icount = 0;
    u32      cmd_start_index = 0;
    ID3D11ShaderResourceView* cmd_tex = nullptr;
    bool     cmd_shape = true;
    bool     cmd_linear = true;
    bool     cmd_coverage = false;   // bound texture is a coverage (R8) atlas
    bool     indices_ok = true;      // index buffer verified as 0,1,2,... on submit
    Rect     clip_rect{ 0, 0, 0, 0 };
    Rect     clip_stack[8];
    u32      clip_depth = 0;
    Vec<DrawCmd> cmds;
    ID3D11ShaderResourceView* white_tex = nullptr;
    ID3D11Texture2D* white_tex_obj = nullptr;
    u32 width = 0, height = 0;
    f32 dpi = 1.0f;
    u32 triangles = 0, draw_calls = 0;

    bool init(Gfx* gfx, TextRenderer* text);
    void shutdown();
    void set_dpi(f32 dpi_scale) { dpi = dpi_scale; g_ui_scale = dpi_scale; }
    f32  scale() const { return dpi; }
    i32  px(f32 logical) const { return (i32)(logical * dpi + 0.5f); }
    f32  sp(f32 logical) const { return logical * dpi; }

    void begin(u32 width, u32 height);
    void end();

    // ---------------------------------------------------------------- state
    void push_clip(f32 x, f32 y, f32 w, f32 h);
    void pop_clip();
    void reset_clip();

    // ----------------------------------------------------------- primitives
    void rect(f32 x, f32 y, f32 w, f32 h, Col c);
    void rect_grad(f32 x, f32 y, f32 w, f32 h, Col top, Col bottom);
    void rect_grad_h(f32 x, f32 y, f32 w, f32 h, Col left, Col right);
    void rrect(f32 x, f32 y, f32 w, f32 h, f32 radius, Col c);
    void rrect_grad(f32 x, f32 y, f32 w, f32 h, f32 radius, Col top, Col bottom);
    void rrect_border(f32 x, f32 y, f32 w, f32 h, f32 radius, f32 thickness, Col c);
    void line(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, Col c);
    void circle(f32 cx, f32 cy, f32 radius, Col c);
    void ring(f32 cx, f32 cy, f32 radius, f32 thickness, Col c);
    void polyline(const f32* xy, u32 count, f32 thickness, Col c);
    void triangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, Col c);
    // Vertical "meter" bar with rounded caps - used for every gauge.
    void meter(f32 x, f32 y, f32 w, f32 h, f32 t, Col bg, Col fill);

    // ---------------------------------------------------------------- icons
    void icon(IconId id, f32 x, f32 y, f32 size, Col c, f32 thickness = 1.75f);
    void icon_centered(IconId id, f32 cx, f32 cy, f32 size, Col c, f32 thickness = 1.75f);
    void icon_filled(IconId id, f32 x, f32 y, f32 size, Col c);

    // ----------------------------------------------------------------- text
    void text(Str s, f32 x, f32 y, FontId font, Col c, TextAlign align = ALIGN_LEFT);
    void text_bg(Str s, f32 x, f32 y, FontId font, Col fg, Col bg, f32 pad_x = 4, f32 pad_y = 2, f32 radius = 3);
    void text_ellipsis(Str s, f32 x, f32 y, f32 max_w, FontId font, Col c, TextAlign align = ALIGN_LEFT);
    void text_wrapped(Str s, f32 x, f32 y, f32 max_w, f32 line_h, FontId font, Col c, u32 max_lines = 8);
    f32  measure(Str s, FontId font) { return txt ? txt->measure(font, s) : 0; }
    f32  line_h(FontId font) { return txt ? txt->line_height(font) : 14; }

    // ---------------------------------------------------------------- images
    void image(ID3D11ShaderResourceView* srv, f32 x, f32 y, f32 w, f32 h, Col tint = Col(1, 1, 1, 1), bool linear = true);

    // Draws one decoded video frame: two NV12 planes converted to RGB by the
    // pixel shader. The quad is submitted immediately with its own state (its
    // own constant buffer, both textures, video rasteriser), so the scene graph
    // stays a single vertex buffer with zero extra passes and zero copies.
    // Must be called before the UI of the same frame is drawn.
    void draw_video(ID3D11ShaderResourceView* srv_y, ID3D11ShaderResourceView* srv_uv,
                    f32 x, f32 y, f32 w, f32 h,
                    f32 u0, f32 v0, f32 u1, f32 v1, f32 sharpness = 0.0f);

    // True when the vertex/index invariant holds. Every quad is 6 vertices
    // indexed 0..5, and the index cursor advances one slot per vertex, so the
    // two counters must match and the index buffer must read 0,1,2,3,...
    // The 1.0.3 code advanced it by n*3 and emitted first+i*3+k, which drew
    // six triangles per element and pulled in geometry from the next elements.
    // Checked by the app on the first frame and reported in the log.
    u32  verts_quads = 0;        // quads emitted through verts()
    u32  verts_used  = 0;        // vertex slots they reserved
    // True when the index buffer of the frame just submitted holds exactly
    // 0,1,2,3,... and the counters agree. Verified while the buffer is still
    // mapped (see Ui2D::end) and cached for the caller.
    // True when the frame just submitted used one index per vertex (index
    // buffer reads 0,1,2,3,...) and the counters agree. The pattern is checked
    // while the buffer is still mapped, in Ui2D::end(), and cached here.
    bool verts_consistent() const;

private:
    void flush_cmd();
    void set_texture(ID3D11ShaderResourceView* srv, bool shape, bool linear, bool coverage = false);
    UiVert* verts(u32 n, u16** idx, u32* first_index);
};

} // namespace mob
