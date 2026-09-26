// ============================================================================
//  MOBILADOR - src/render/ui2d.cpp
// ============================================================================
#include "ui2d.h"
#include "../core/log.h"
#include <math.h>

namespace mob {

f32 g_ui_scale = 1.0f;

static f32 snap(f32 v) { return (f32)(i32)(v + 0.5f); }

bool Ui2D::init(Gfx* g, TextRenderer* t) {
    gfx = g;
    txt = t;
    arena.init(2 << 20);
    cmds.init(&arena, 512);

    // 1x1 white texture: keeps a valid SRV bound even for pure-shape batches.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    u32 white = 0xFFFFFFFF;
    D3D11_SUBRESOURCE_DATA sd{ &white, 4, 4 };
    if (FAILED(gfx->dev->CreateTexture2D(&td, &sd, &white_tex_obj))) return false;
    if (FAILED(gfx->dev->CreateShaderResourceView(white_tex_obj, nullptr, &white_tex))) return false;
    return true;
}

void Ui2D::shutdown() {
    if (white_tex) white_tex->Release();
    if (white_tex_obj) white_tex_obj->Release();
    arena.shutdown();
}

void Ui2D::begin(u32 w, u32 h) {
    width = w; height = h;
    g_ui_scale = dpi;
    vcount = icount = 0;
    cmds.reset();
    triangles = 0; draw_calls = 0;
    verts_quads = verts_used = 0;
    indices_ok = true;
    clip_depth = 0;
    clip_rect = Rect{ 0, 0, (f32)w, (f32)h };
    cmd_tex = nullptr;
    cmd_shape = true;
    cmd_coverage = false;
    cmd_start_index = 0;

    D3D11_MAPPED_SUBRESOURCE mv{}, mi{};
    if (FAILED(gfx->ctx->Map(gfx->vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mv))) { vbase = nullptr; return; }
    if (FAILED(gfx->ctx->Map(gfx->ib, 0, D3D11_MAP_WRITE_DISCARD, 0, &mi))) { gfx->ctx->Unmap(gfx->vb, 0); vbase = nullptr; return; }
    vbase = (UiVert*)mv.pData;  vcap = gfx->vb_capacity;
    ibase = (u16*)mi.pData;     icap = gfx->ib_capacity;

    gfx->begin_ui_pass(true);
}

void Ui2D::flush_cmd() {
    if (icount > cmd_start_index) {
        DrawCmd& c = cmds.add();
        c.tex = cmd_tex ? cmd_tex : white_tex;
        c.clip[0] = (i32)clip_rect.x; c.clip[1] = (i32)clip_rect.y;
        c.clip[2] = (i32)clip_rect.w; c.clip[3] = (i32)clip_rect.h;
        c.first_index = cmd_start_index;
        c.index_count = icount - cmd_start_index;
        c.shape = cmd_shape;
        c.linear_filter = cmd_linear;
        c.coverage = cmd_coverage;
    }
    cmd_start_index = icount;
}

void Ui2D::set_texture(ID3D11ShaderResourceView* srv, bool shape, bool linear, bool coverage) {
    if (srv == cmd_tex && shape == cmd_shape && linear == cmd_linear && coverage == cmd_coverage) return;
    flush_cmd();
    cmd_tex = srv;
    cmd_shape = shape;
    cmd_linear = linear;
    cmd_coverage = coverage;
}

void Ui2D::push_clip(f32 x, f32 y, f32 w, f32 h) {
    if (clip_depth < 8) clip_stack[clip_depth++] = clip_rect;
    Rect n{ x, y, w, h };
    // intersect with the current clip
    f32 x0 = mob_max(clip_rect.x, n.x), y0 = mob_max(clip_rect.y, n.y);
    f32 x1 = mob_min(clip_rect.r(), n.r()); f32 y1 = mob_min(clip_rect.b(), n.b());
    clip_rect = Rect{ x0, y0, mob_max(x1 - x0, 0.0f), mob_max(y1 - y0, 0.0f) };
    flush_cmd();
}

void Ui2D::pop_clip() {
    if (clip_depth > 0) clip_rect = clip_stack[--clip_depth];
    flush_cmd();
}

void Ui2D::reset_clip() {
    clip_depth = 0;
    clip_rect = Rect{ 0, 0, (f32)width, (f32)height };
    flush_cmd();
}

UiVert* Ui2D::verts(u32 n, u16** idx, u32* first_index) {
    if (!vbase || vcount + n > vcap || icount + n > icap) {
        // Extremely dense frames (a long graph plus a full screen of text) can
        // overflow; flush and continue rather than dropping the frame.
        flush_cmd();
        set_texture(nullptr, cmd_shape, cmd_linear, cmd_coverage);
        return nullptr;
    }
    *idx = ibase + icount;
    *first_index = vcount;
    UiVert* base = vbase + vcount;
    vcount += n;
    icount += n;
    triangles += n / 3;
    verts_quads += n / 6;
    verts_used += n;
    // One index per vertex, in order. A quad is 6 vertices drawn as
    // triangle list 0,1,2 + 3,4,5, so the indices must be exactly 0..n-1.
    //
    // This used to advance icount by n*3 and write first+i*3+{0,1,2}, i.e. 18
    // indices per quad reaching up to first+17. Only the first six belong to
    // the quad: the rest drew six triangles per element, pulled the next
    // element's vertices (so glyphs came out doubled, dark and bold) and,
    // when the next element sat in another vertex range, fetched whatever
    // geometry was there - solid dark blocks over parts of the screen, with
    // no D3D error because every index stayed inside the buffer.
    for (u32 i = 0; i < n; ++i) (*idx)[i] = (u16)(*first_index + i);
    return base;
}

static void set_geo(UiVert* v, f32 hw, f32 hh, f32 radius, f32 border) {
    for (int i = 0; i < 3; ++i) { v[i].hw = hw; v[i].hh = hh; v[i].radius = radius; v[i].border = border; }
}

static void quad(UiVert* v, f32 x0, f32 y0, f32 x1, f32 y1,
                 f32 u0, f32 v0, f32 u1, f32 v1, Col c0, Col c1) {
    v[0] = { x0, y0, u0, v0, c0.r, c0.g, c0.b, c0.a, 0, 0, 0, 0 };
    v[1] = { x1, y0, u1, v0, c0.r, c0.g, c0.b, c0.a, 0, 0, 0, 0 };
    v[2] = { x1, y1, u1, v1, c1.r, c1.g, c1.b, c1.a, 0, 0, 0, 0 };
    // second triangle
    v[3] = v[0]; v[4] = v[2];
    v[5] = { x0, y1, u0, v1, c1.r, c1.g, c1.b, c1.a, 0, 0, 0, 0 };
}

// ------------------------------------------------------------------ shapes
void Ui2D::rect(f32 x, f32 y, f32 w, f32 h, Col c) {
    if (w <= 0 || h <= 0) return;
    set_texture(white_tex, true, true);
    u16* idx; u32 fi;
    UiVert* v = verts(6, &idx, &fi);
    if (!v) return;
    x = snap(x); y = snap(y); w = snap(w); h = snap(h);
    quad(v, x, y, x + w, y + h, 0, 0, 1, 1, c, c);
    set_geo(v, w * 0.5f, h * 0.5f, 0.0f, 0.0f);
    // reorder: quad() writes triangles as (0,1,2) and (3,4,5) which for the
    // shape shader both are correct because uv encodes the local position.
    v[3] = v[0]; v[4] = v[2];
    v[5] = { x, y + h, 0, 1, c.r, c.g, c.b, c.a, 0, 0, 0, 0 };
    for (int i = 0; i < 6; ++i) { v[i].hw = w * 0.5f; v[i].hh = h * 0.5f; v[i].radius = 0; v[i].border = 0; }
}

void Ui2D::rect_grad(f32 x, f32 y, f32 w, f32 h, Col top, Col bottom) {
    if (w <= 0 || h <= 0) return;
    set_texture(white_tex, true, true);
    u16* idx; u32 fi;
    UiVert* v = verts(6, &idx, &fi);
    if (!v) return;
    x = snap(x); y = snap(y); w = snap(w); h = snap(h);
    quad(v, x, y, x + w, y + h, 0, 0, 1, 1, top, bottom);
    for (int i = 0; i < 6; ++i) { v[i].hw = w * 0.5f; v[i].hh = h * 0.5f; v[i].radius = 0; v[i].border = 0; }
}

void Ui2D::rect_grad_h(f32 x, f32 y, f32 w, f32 h, Col left, Col right) {
    if (w <= 0 || h <= 0) return;
    set_texture(white_tex, true, true);
    u16* idx; u32 fi;
    UiVert* v = verts(6, &idx, &fi);
    if (!v) return;
    x = snap(x); y = snap(y); w = snap(w); h = snap(h);
    quad(v, x, y, x + w, y + h, 0, 0, 1, 1, left, left);
    v[1].r = right.r; v[1].g = right.g; v[1].b = right.b; v[1].a = right.a;
    v[2].r = right.r; v[2].g = right.g; v[2].b = right.b; v[2].a = right.a;
    v[4].r = right.r; v[4].g = right.g; v[4].b = right.b; v[4].a = right.a;
    for (int i = 0; i < 6; ++i) { v[i].hw = w * 0.5f; v[i].hh = h * 0.5f; v[i].radius = 0; v[i].border = 0; }
}

void Ui2D::rrect(f32 x, f32 y, f32 w, f32 h, f32 radius, Col c) {
    if (w <= 0 || h <= 0) return;
    if (radius <= 0.5f) { rect(x, y, w, h, c); return; }
    set_texture(white_tex, true, true);
    u16* idx; u32 fi;
    UiVert* v = verts(6, &idx, &fi);
    if (!v) return;
    f32 px = snap(x), py = snap(y), pw = snap(w), ph = snap(h);
    radius = mob_min(radius, mob_min(pw, ph) * 0.5f);
    quad(v, px, py, px + pw, py + ph, 0, 0, 1, 1, c, c);
    for (int i = 0; i < 6; ++i) { v[i].hw = pw * 0.5f; v[i].hh = ph * 0.5f; v[i].radius = radius; v[i].border = 0; }
}

void Ui2D::rrect_grad(f32 x, f32 y, f32 w, f32 h, f32 radius, Col top, Col bottom) {
    if (w <= 0 || h <= 0) return;
    set_texture(white_tex, true, true);
    u16* idx; u32 fi;
    UiVert* v = verts(6, &idx, &fi);
    if (!v) return;
    f32 px = snap(x), py = snap(y), pw = snap(w), ph = snap(h);
    radius = mob_min(radius, mob_min(pw, ph) * 0.5f);
    quad(v, px, py, px + pw, py + ph, 0, 0, 1, 1, top, bottom);
    for (int i = 0; i < 6; ++i) { v[i].hw = pw * 0.5f; v[i].hh = ph * 0.5f; v[i].radius = radius; v[i].border = 0; }
}

void Ui2D::rrect_border(f32 x, f32 y, f32 w, f32 h, f32 radius, f32 thickness, Col c) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    set_texture(white_tex, true, true);
    u16* idx; u32 fi;
    UiVert* v = verts(6, &idx, &fi);
    if (!v) return;
    f32 px = snap(x), py = snap(y), pw = snap(w), ph = snap(h);
    radius = mob_min(radius, mob_min(pw, ph) * 0.5f);
    quad(v, px, py, px + pw, py + ph, 0, 0, 1, 1, c, c);
    for (int i = 0; i < 6; ++i) {
        v[i].hw = pw * 0.5f; v[i].hh = ph * 0.5f; v[i].radius = radius; v[i].border = thickness;
    }
}

void Ui2D::circle(f32 cx, f32 cy, f32 r, Col c) {
    rrect(cx - r, cy - r, r * 2, r * 2, r, c);
}

void Ui2D::ring(f32 cx, f32 cy, f32 r, f32 thickness, Col c) {
    rrect_border(cx - r, cy - r, r * 2, r * 2, r, thickness, c);
}

// Stroke with butt caps; joins are covered by the caller adding small discs.
static void stroke_segment(Ui2D* ui, f32 x0, f32 y0, f32 x1, f32 y1, f32 t, Col c, u16* idx, u32 fi, UiVert* v) {
    (void)ui; (void)idx; (void)fi; (void)v;
    if (x0 == x1 && y0 == y1) return;
    f32 dx = x1 - x0, dy = y1 - y0;
    f32 len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;
    f32 nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    v[0] = { x0 + nx, y0 + ny, 0, 0, c.r, c.g, c.b, c.a, 0, 0, 0, 0 };
    v[1] = { x1 + nx, y1 + ny, 0, 0, c.r, c.g, c.b, c.a, 0, 0, 0, 0 };
    v[2] = { x1 - nx, y1 - ny, 0, 0, c.r, c.g, c.b, c.a, 0, 0, 0, 0 };
    v[3] = { x0 - nx, y0 - ny, 0, 0, c.r, c.g, c.b, c.a, 0, 0, 0, 0 };
    // UV encodes normalised position inside the quad so ps_shape can apply AA
    // to the long edges while keeping the caps crisp.
    f32 hw = t * 0.5f;
    v[0].u = 0; v[0].v = 0.0f;  v[1].u = 1; v[1].v = 0.0f;
    v[2].u = 1; v[2].v = 1.0f;  v[3].u = 0; v[3].v = 1.0f;
    for (int i = 0; i < 4; ++i) { v[i].hw = hw; v[i].hh = len * 0.5f; v[i].radius = 0; v[i].border = 0; }
    // Quad indices were pre-generated as (0,1,2) and (3,4,5); for a 4-vertex
    // stroke we need (0,1,2) and (0,2,3), so rewrite them here.
    idx[3] = (u16)(fi + 0); idx[4] = (u16)(fi + 2); idx[5] = (u16)(fi + 3);
}

void Ui2D::line(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, Col c) {
    set_texture(white_tex, true, true);
    u16* idx; u32 fi;
    UiVert* v = verts(6, &idx, &fi);
    if (!v) return;
    stroke_segment(this, x0, y0, x1, y1, thickness, c, idx, fi, v);
}

void Ui2D::triangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, Col c) {
    set_texture(white_tex, true, true);
    u16* idx; u32 fi;
    UiVert* v = verts(6, &idx, &fi);
    if (!v) return;
    // Emit as two degenerate halves so the batch stays uniform in size.
    for (int i = 0; i < 6; ++i) {
        v[i].r = c.r; v[i].g = c.g; v[i].b = c.b; v[i].a = c.a;
        v[i].hw = 0; v[i].hh = 0; v[i].radius = 0; v[i].border = 0;
        v[i].u = 0; v[i].v = 0;
    }
    v[0].x = x0; v[0].y = y0;
    v[1].x = x1; v[1].y = y1;
    v[2].x = x2; v[2].y = y2;
    v[3] = v[0]; v[4] = v[1]; v[5] = v[2];
    idx[3] = (u16)fi; idx[4] = (u16)(fi + 1); idx[5] = (u16)(fi + 2);
}

void Ui2D::polyline(const f32* xy, u32 count, f32 thickness, Col c) {
    if (count < 2) return;
    for (u32 i = 0; i + 1 < count; ++i) {
        line(xy[i * 2], xy[i * 2 + 1], xy[i * 2 + 2], xy[i * 2 + 3], thickness, c);
        if (i + 2 < count) {
            // join: small disc hides the notch between segments
            circle(xy[i * 2 + 2], xy[i * 2 + 3], thickness * 0.5f, c);
        }
    }
}

void Ui2D::meter(f32 x, f32 y, f32 w, f32 h, f32 t, Col bg, Col fill) {
    rrect(x, y, w, h, h * 0.5f, bg);
    f32 fw = mob_clamp(t, 0.0f, 1.0f) * (w - 2);
    if (fw > 1.0f) rrect(x + 1, y + 1, fw, h - 2, mob_max((h - 2) * 0.5f, 1.0f), fill);
}

// ------------------------------------------------------------------- icons
void Ui2D::icon(IconId id, f32 x, f32 y, f32 size, Col c, f32 thickness) {
    const IconDef* def = icon_def(id);
    if (!def) return;
    f32 s = size / 24.0f;
    f32 tk = mob_max(thickness * (size / 24.0f) * (size / 24.0f) * (24.0f / size), thickness * 0.75f);
    tk = mob_max(thickness * mob_clamp(size / 24.0f, 0.7f, 1.6f), 1.0f);
    for (u16 si = 0; si < def->seg_count; ++si) {
        const IconSeg& seg = icon_segs(def)[si];
        const IconPoint* pts = icon_pts(def) + seg.first_point;
        if (seg.kind == ICON_SEG_STROKE) {
            for (u16 i = 0; i + 1 < seg.point_count; ++i) {
                f32 x0 = x + pts[i].x * s, y0 = y + pts[i].y * s;
                f32 x1 = x + pts[i + 1].x * s, y1 = y + pts[i + 1].y * s;
                if (seg.closed) {
                    // ensure the closing segment is drawn by the loop below
                }
                line(x0, y0, x1, y1, tk, c);
                if (i + 2 < seg.point_count) circle(x1, y1, tk * 0.5f, c);
            }
            if (seg.closed && seg.point_count > 2) {
                f32 x0 = x + pts[seg.point_count - 1].x * s, y0 = y + pts[seg.point_count - 1].y * s;
                f32 x1 = x + pts[0].x * s, y1 = y + pts[0].y * s;
                line(x0, y0, x1, y1, tk, c);
            }
        } else if (seg.kind == ICON_SEG_FILL) {
            for (u16 i = 1; i + 1 < seg.point_count; ++i) {
                triangle(x + pts[0].x * s, y + pts[0].y * s,
                         x + pts[i].x * s, y + pts[i].y * s,
                         x + pts[i + 1].x * s, y + pts[i + 1].y * s, c);
            }
        } else if (seg.kind == 2) {
            for (u16 i = 0; i < seg.point_count; ++i)
                circle(x + pts[i].x * s, y + pts[i].y * s, mob_max(pts[i].x * 0 + tk * 0.9f, 1.0f), c);
        }
    }
}

void Ui2D::icon_centered(IconId id, f32 cx, f32 cy, f32 size, Col c, f32 thickness) {
    icon(id, cx - size * 0.5f, cy - size * 0.5f, size, c, thickness);
}

// -------------------------------------------------------------------- text
void Ui2D::text(Str s, f32 x, f32 y, FontId font, Col c, TextAlign align) {
    if (!txt || !txt->ok || s.empty()) return;
    Font& f = txt->fonts[font];
    f32 w = txt->measure(font, s);
    if (align == ALIGN_CENTER) x -= w * 0.5f;
    else if (align == ALIGN_RIGHT) x -= w;
    x = snap(x);
    f32 pen = x;
    f32 top = snap(y);
    u32 i = 0;
    while (i < s.n) {
        u32 cp = TextRenderer::next_codepoint(s, &i);
        if (!cp) break;
        if (cp == '\n') { pen = x; top += f.line_height; continue; }
        if (cp == '\r') continue;
        Glyph* g = txt->glyph(font, cp);
        if (!g || !g->valid) continue;
        if (g->w > 0.5f && g->h > 0.5f) {
            set_texture(f.srv, false, true, /*coverage*/ true);
            u16* idx; u32 fi;
            // glyph quads are 4 verts / 6 indices; allocate 6 slots
            UiVert* v = verts(6, &idx, &fi);
            if (v) {
                f32 gx = snap(pen + g->bx);
                f32 gy = snap(top + f.ascent - g->by);
                v[0] = { gx,          gy,          g->u0, g->v0, c.r, c.g, c.b, c.a, 0, 0, 0, 0 };
                v[1] = { gx + g->w,   gy,          g->u1, g->v0, c.r, c.g, c.b, c.a, 0, 0, 0, 0 };
                v[2] = { gx + g->w,   gy + g->h,   g->u1, g->v1, c.r, c.g, c.b, c.a, 0, 0, 0, 0 };
                v[3] = { gx,          gy,          g->u0, g->v0, c.r, c.g, c.b, c.a, 0, 0, 0, 0 };
                v[4] = { gx + g->w,   gy + g->h,   g->u1, g->v1, c.r, c.g, c.b, c.a, 0, 0, 0, 0 };
                v[5] = { gx,          gy + g->h,   g->u0, g->v1, c.r, c.g, c.b, c.a, 0, 0, 0, 0 };
            }
        }
        pen += g->adv;
    }
}

void Ui2D::text_bg(Str s, f32 x, f32 y, FontId font, Col fg, Col bg, f32 pad_x, f32 pad_y, f32 radius) {
    f32 w = txt->measure(font, s);
    f32 h = txt->line_height(font);
    rrect(x, y, w + pad_x * 2, h + pad_y * 2, radius, bg);
    text(s, x + pad_x, y + pad_y, font, fg);
}

void Ui2D::text_ellipsis(Str s, f32 x, f32 y, f32 max_w, FontId font, Col c, TextAlign align) {
    if (!txt || !txt->ok) return;
    if (txt->measure(font, s) <= max_w) { text(s, x, y, font, c, align); return; }
    f32 dots = txt->measure(font, Str("..."));
    f32 acc = 0;
    u32 i = 0, end = 0;
    while (i < s.n) {
        u32 start = i;
        u32 cp = TextRenderer::next_codepoint(s, &i);
        f32 adv = txt->advance(font, cp);
        if (acc + adv + dots > max_w) { end = start; break; }
        acc += adv;
        end = i;
    }
    text(s.sub(0, end), x, y, font, c, align);
    f32 px = (align == ALIGN_CENTER) ? x + max_w * 0.5f : (align == ALIGN_RIGHT ? x : x + acc);
    text(Str("..."), px, y, font, c, align == ALIGN_LEFT ? ALIGN_LEFT : align);
}

void Ui2D::text_wrapped(Str s, f32 x, f32 y, f32 max_w, f32 line_h, FontId font, Col c, u32 max_lines) {
    if (!txt || !txt->ok) return;
    u32 line_start = 0;
    u32 i = 0;
    u32 lines = 0;
    f32 acc = 0;
    f32 cur_x = x;
    f32 cur_y = y;
    u32 last_space = 0xFFFFFFFFu;
    while (i < s.n && lines < max_lines) {
        u32 start = i;
        u32 cp = TextRenderer::next_codepoint(s, &i);
        if (cp == '\n') {
            text(s.sub(line_start, start - line_start), cur_x, cur_y, font, c);
            cur_y += line_h; lines++; cur_x = x; line_start = i; acc = 0;
            last_space = 0xFFFFFFFFu;
            continue;
        }
        f32 adv = txt->advance(font, cp);
        if (cp == ' ') last_space = start;
        if (acc + adv > max_w && start > line_start) {
            u32 brk = (last_space != 0xFFFFFFFFu && last_space > line_start) ? last_space : start;
            text(s.sub(line_start, brk - line_start), cur_x, cur_y, font, c);
            cur_y += line_h; lines++;
            if (lines >= max_lines) {
                text(Str("..."), cur_x + txt->measure(font, s.sub(line_start, brk - line_start)), cur_y - line_h, font, c);
                return;
            }
            cur_x = x; acc = 0;
            line_start = (brk == last_space) ? brk + 1 : brk;
            last_space = 0xFFFFFFFFu;
            continue;
        }
        acc += adv;
    }
    if (line_start < s.n && lines < max_lines)
        text(s.sub(line_start), cur_x, cur_y, font, c);
}

// ------------------------------------------------------------------ images
void Ui2D::image(ID3D11ShaderResourceView* srv, f32 x, f32 y, f32 w, f32 h, Col tint, bool linear) {
    if (!srv || w <= 0 || h <= 0) return;
    set_texture(srv, false, linear);
    u16* idx; u32 fi;
    UiVert* v = verts(6, &idx, &fi);
    if (!v) return;
    quad(v, x, y, x + w, y + h, 0, 0, 1, 1, tint, tint);
    set_geo(v, w * 0.5f, h * 0.5f, 0, 0);
}

bool Ui2D::verts_consistent() const {
    // One index per vertex, the counters in agreement, and the pattern that
    // Ui2D::end() verified while the buffer was mapped. The 1.0.3 code advanced
    // the index cursor by n*3 and wrote first+i*3+k: this catches that and any
    // other drift, from the log rather than from the user's screen.
    return indices_ok && icount == verts_used && verts_used == verts_quads * 6;
}

// -------------------------------------------------------------------- flush
void Ui2D::end() {
    flush_cmd();
    if (!vbase) { indices_ok = false; return; }
    // The index pattern can only be inspected while the buffer is mapped, so it
    // is verified here and cached. A quad is 6 vertices drawn as 0,1,2 + 3,4,5,
    // so the index buffer must hold exactly 0,1,2,3,...
    indices_ok = true;
    for (u32 i = 0; i < icount; ++i) {
        if (ibase[i] != (u16)i) { indices_ok = false; break; }
    }
    gfx->ctx->Unmap(gfx->vb, 0);
    gfx->ctx->Unmap(gfx->ib, 0);
    vbase = nullptr; ibase = nullptr;

    for (u32 i = 0; i < cmds.count; ++i) {
        DrawCmd& c = cmds[i];
        gfx->set_scissor(c.clip[0], c.clip[1], c.clip[2], c.clip[3]);
        ID3D11ShaderResourceView* srv = c.tex ? c.tex : white_tex;
        gfx->ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* s = c.linear_filter ? gfx->samp_linear : gfx->samp_point;
        gfx->ctx->PSSetSamplers(0, 1, &s);
        // Shapes use the analytic shader. Everything else is textured: the
        // glyph atlas carries coverage in .r (R8) while images carry colour and
        // alpha (ARGB), and the two need different maths.
        ID3D11PixelShader* ps = c.shape ? gfx->ps_shape : (c.coverage ? gfx->ps_text : gfx->ps_ui);
        gfx->ctx->PSSetShader(ps, nullptr, 0);
        gfx->ctx->DrawIndexed(c.index_count, c.first_index, 0);
        draw_calls++;
    }
    gfx->set_scissor(0, 0, (i32)width, (i32)height);
}


// ---------------------------------------------------------------------------
// Video frame draw. Deliberately "wide": one quad, two texture bindings, one
// constant buffer, one draw call, no intermediate render target and no copy of
// anything. The pixel shader converts NV12 to RGB in the same pass that scales
// the image, which is why this path costs one draw call per frame.
// ---------------------------------------------------------------------------
void Ui2D::draw_video(ID3D11ShaderResourceView* srv_y, ID3D11ShaderResourceView* srv_uv,
                      f32 x, f32 y, f32 w, f32 h,
                      f32 u0, f32 v0, f32 u1, f32 v1, f32 sharpness) {
    if (!gfx || !gfx->ctx || !srv_y) return;
    // Whatever was batched so far sits behind the video; emit it now.
    flush_cmd();
    if (!vbase) return;

    u16* idx = nullptr;
    u32 first = 0;
    UiVert* v = verts(6, &idx, &first);
    if (!v) return;

    const f32 x1 = x + w, y1 = y + h;
    const f32 px[6] = { x,  x1, x1, x,  x1, x  };
    const f32 py[6] = { y,  y,  y1, y,  y1, y1 };
    const f32 pu[6] = { u0, u1, u1, u0, u1, u0 };
    const f32 pv[6] = { v0, v0, v1, v0, v1, v1 };
    for (u32 i = 0; i < 6; ++i) {
        UiVert& o = v[i];
        o.x = px[i]; o.y = py[i];
        o.u = pu[i]; o.v = pv[i];
        o.r = 1; o.g = 1; o.b = 1; o.a = 1;
        o.hw = 0; o.hh = 0; o.radius = 0; o.border = 0;
    }

    ID3D11DeviceContext* ctx = gfx->ctx;
    gfx->set_scissor((i32)x, (i32)y, (i32)w, (i32)h);
    ID3D11ShaderResourceView* srvs[2] = { srv_y, srv_uv };
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &gfx->samp_linear);
    ctx->PSSetConstantBuffers(0, 1, &gfx->cb_video);
    ctx->PSSetShader(gfx->ps_video, nullptr, 0);
    ctx->OMSetBlendState(gfx->blend_none, nullptr, 0xFFFFFFFF);
    ctx->RSSetState(gfx->rast_video);
    ctx->DrawIndexed(6, first, 0);
    draw_calls++;

    // Restore the UI state for everything that follows (overlay, HUD, menus).
    gfx->begin_ui_pass(true);
    ctx->PSSetConstantBuffers(0, 1, &gfx->cb_frame);
    gfx->set_scissor((i32)clip_rect.x, (i32)clip_rect.y, (i32)clip_rect.w, (i32)clip_rect.h);
    cmd_start_index = icount;      // the video quad is already submitted
    cmd_tex = nullptr;
    cmd_shape = true;
}

} // namespace mob
