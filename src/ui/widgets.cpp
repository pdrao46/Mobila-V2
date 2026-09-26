// ============================================================================
//  MOBILADOR - src/ui/widgets.cpp
// ============================================================================
#include "widgets.h"
#include <new>      // placement new for the animation map
#include <stdio.h>
#include <math.h>

namespace mob {

u32 w_id(Str label) { return label.hash() ? label.hash() : 1; }
u32 w_id2(Str label, int index) {
    u32 h = label.hash();
    h ^= (u32)(index + 1) * 2654435761u;
    h *= 2246822519u;
    return h ? h : 1;
}
u32 w_idptr(const void* p) {
    u64 v = (u64)(uintptr_t)p;
    u32 h = (u32)(v ^ (v >> 32));
    h *= 2654435761u;
    return h ? h : 1;
}

void WidgetCtx::init(Arena* a, Ui2D* u, Theme* t) {
    arena = a;
    ui = u;
    theme = t;
    anim_map = (StrMap<f32>*)a->alloc(sizeof(StrMap<f32>), alignof(StrMap<f32>));
    new (anim_map) StrMap<f32>();
    anim_map->init(a, 256);
}

void WidgetCtx::begin_frame(f32 dt, u64 t_us) {
    in.dt = dt;
    in.time_us = t_us;
    now_ms = (f32)(t_us / 1000ull);
    hot = hot_next;
    hot_next = 0;
    if (!in.down[MOB_MB_LEFT]) active = 0;
    if (game_mode) {
        // Game Mode: no transitions, no pulsing. Every animation resolves
        // immediately so nothing keeps re-drawing on the CPU.
        for (u32 i = 0; i < anim_count; ++i) anims[i].value = anims[i].from;
    }
}

f32 WidgetCtx::anim(u32 id, f32 target, f32 duration) {
    char keybuf[8];
    memcpy(keybuf, &id, 4);
    keybuf[4] = (char)((int)duration & 0x7F);
    keybuf[5] = (char)(((int)duration >> 7) & 0x7F);
    keybuf[6] = 0; keybuf[7] = 0;
    Str key(keybuf, 7);
    f32* slot = anim_map->find(key);
    if (!slot) {
        f32 v = target;
        anim_map->set(key, v);
        return v;
    }
    if (game_mode || duration <= 0.0f) { *slot = target; return target; }
    f32 dt_s = in.dt > 0.0f ? in.dt : (1.0f / 60.0f);
    f32 k = 1.0f - powf(0.001f, dt_s / (duration / 1000.0f));   // exponential e-out
    if (k > 1.0f) k = 1.0f;
    *slot += (target - *slot) * k;
    if (fabsf(*slot - target) < 0.0015f) *slot = target;
    return *slot;
}

void WidgetCtx::toast(Str text, Col color, IconId icon, f32 seconds) {
    Toast* t;
    if (toast_count < 6) t = &toasts[toast_count++];
    else { memmove(toasts, toasts + 1, sizeof(Toast) * 5); t = &toasts[5]; }
    memset(t, 0, sizeof(*t));
    u32 n = mob_min(text.n, (u32)sizeof(t->text) - 1);
    memcpy(t->text, text.p, n);
    t->text[n] = 0;
    t->color = color;
    t->icon = icon;
    t->ttl = seconds;
    t->life = 0;
}

void WidgetCtx::draw_toasts() {
    if (!toast_count) return;
    f32 w = ui->width;
    f32 y = (f32)ui->height - ui->sp(64);
    for (u32 i = toast_count; i-- > 0;) {
        Toast& t = toasts[i];
        f32 in_k = mob_clamp(t.life / 0.18f, 0.0f, 1.0f);
        f32 out_k = mob_clamp((t.ttl - t.life) / 0.25f, 0.0f, 1.0f);
        f32 a = mob_min(in_k, out_k);
        if (a <= 0.01f) continue;
        f32 tw = ui->measure(Str(t.text), FONT_SMALL) + ui->sp(64);
        f32 th = ui->sp(38);
        f32 x = w - tw - ui->sp(20) + (1.0f - in_k) * ui->sp(24);
        Rect r{ x, y, tw, th };
        ui->rrect(r.x, r.y, r.w, r.h, ui->sp(7), theme->surface.with_a(0.97f * a));
        ui->rrect_border(r.x, r.y, r.w, r.h, ui->sp(7), 1.0f, theme->border.with_a(a));
        ui->rect(r.x, r.y + ui->sp(7), ui->sp(2.0f), r.h - ui->sp(14), theme->text_faint.with_a(0.25f * a));
        ui->icon(t.icon, r.x + ui->sp(14), r.y + r.h * 0.5f - ui->sp(7), ui->sp(14), t.color.with_a(a));
        ui->text(Str(t.text), r.x + ui->sp(36), r.y + (r.h - ui->line_h(FONT_SMALL)) * 0.5f,
                 FONT_SMALL, theme->text.with_a(a));
        y -= th + ui->sp(8);
    }
}

// ================================================================ buttons
static Col btn_fill(WidgetCtx* c, BtnKind kind, f32 hover, bool pressed) {
    Theme* th = c->theme;
    Col base;
    switch (kind) {
        case BTN_PRIMARY:   base = th->accent; break;
        case BTN_DANGER:    base = th->err; break;
        case BTN_SUCCESS:   base = th->ok; break;
        case BTN_GHOST:     base = th->surface_3; break;
        default:            base = th->surface_2; break;
    }
    f32 lift = hover * 0.10f - (pressed ? 0.06f : 0.0f);
    Col out = base.mix(base.lighten(0.18f), mob_max(lift, 0.0f));
    if (lift < 0.0f) out = base.darken(-lift);
    return out;
}

static Col btn_text(WidgetCtx* c, BtnKind kind) {
    switch (kind) {
        case BTN_PRIMARY: return c->theme->accent.readable_on();
        case BTN_DANGER:  return Col(1, 1, 1);
        case BTN_SUCCESS: return Col(1, 1, 1);
        case BTN_GHOST:   return c->theme->text_dim;
        default:          return c->theme->text;
    }
}

BtnResult button(WidgetCtx* c, Str label, Rect r, BtnKind kind, IconId icon, bool enabled) {
    BtnResult res;
    u32 id = w_id(label);
    bool hovered = enabled && r.contains(c->in.mouse_x, c->in.mouse_y) && !c->modal_id;
    if (hovered) c->hot_next = id;
    f32 hover = c->anim(w_id2(Str("hov"), (int)(id & 0x7FFFFFFF)), hovered ? 1.0f : 0.0f, 110.0f);
    bool pressed_now = false;
    if (hovered && c->in.pressed[MOB_MB_LEFT]) c->active = id;
    if (c->active == id) {
        if (c->in.released[MOB_MB_LEFT]) { res.clicked = true; c->active = 0; }
        else if (c->in.down[MOB_MB_LEFT]) pressed_now = true;
    }
    res.hovered = hovered;
    res.held = pressed_now;

    Theme* th = c->theme;
    f32 radius = c->ui->sp(6);
    Col fill = btn_fill(c, kind, hover, pressed_now);
    if (!enabled) fill = fill.mul_a(1.0f).mix(th->surface_2, 0.6f).with_a(kind == BTN_PRIMARY ? 0.35f : 1.0f);

    if (kind == BTN_GHOST) {
        if (hover > 0.01f) c->ui->rrect(r.x, r.y, r.w, r.h, radius, th->surface_3.with_a(0.55f * hover));
        if (pressed_now) c->ui->rrect(r.x, r.y, r.w, r.h, radius, th->surface_3.with_a(0.9f));
    } else {
        // subtle press offset communicates the click without a shadow
        f32 oy = pressed_now ? c->ui->sp(1) : 0.0f;
        c->ui->rrect(r.x, r.y + oy, r.w, r.h, radius, fill);
        if (kind == BTN_SECONDARY) c->ui->rrect_border(r.x, r.y + oy, r.w, r.h, radius, 1.0f,
                                                       th->border.mix(th->border_strong, hover));
        if (kind == BTN_PRIMARY && hover > 0.01f)
            c->ui->rrect_border(r.x, r.y + oy, r.w, r.h, radius, 1.0f, fill.lighten(0.25f).with_a(0.8f * hover));
    }

    Col fg = btn_text(c, kind);
    if (!enabled) fg = fg.with_a(0.45f);
    f32 text_w = icon == ICON_NONE ? 0.0f : c->ui->sp(20);
    f32 tw = c->ui->measure(label, FONT_LABEL);
    f32 start = r.x + (r.w - (tw + text_w)) * 0.5f;
    if (icon != ICON_NONE) {
        c->ui->icon(icon, start, r.y + r.h * 0.5f - c->ui->sp(8), c->ui->sp(16), fg, 1.6f);
        start += text_w;
    }
    c->ui->text(label, start, r.y + (r.h - c->ui->line_h(FONT_LABEL)) * 0.5f - (kind == BTN_PRIMARY ? 0.0f : 0.0f),
                FONT_LABEL, fg);
    return res;
}

BtnResult button_icon(WidgetCtx* c, IconId icon, Rect r, const char* tip, bool enabled, f32 icon_size) {
    BtnResult res;
    u32 id = w_id2(Str("ibtn"), (int)icon * 7919 + (int)(r.x * 13.0f) + (int)(r.y * 7.0f));
    bool hovered = enabled && r.contains(c->in.mouse_x, c->in.mouse_y) && !c->modal_id;
    if (hovered) c->hot_next = id;
    f32 hover = c->anim(id, hovered ? 1.0f : 0.0f, 110.0f);
    bool pressed_now = false;
    if (hovered && c->in.pressed[MOB_MB_LEFT]) c->active = id;
    if (c->active == id) {
        if (c->in.released[MOB_MB_LEFT]) { res.clicked = true; c->active = 0; }
        else if (c->in.down[MOB_MB_LEFT]) pressed_now = true;
    }
    Theme* th = c->theme;
    if (hover > 0.01f || pressed_now) {
        c->ui->rrect(r.x, r.y, r.w, r.h, c->ui->sp(5),
                     (pressed_now ? th->surface_3 : th->surface_3.with_a(0.55f * hover)));
    }
    Col fg = enabled ? th->text_dim.mix(th->text, hover) : th->text_faint.with_a(0.5f);
    c->ui->icon_centered(icon, r.cx(), r.cy(), c->ui->sp(icon_size), fg, 1.6f);
    if (tip && hovered) tooltip(c, id, Str(tip));
    res.hovered = hovered;
    return res;
}

BtnResult button_big(WidgetCtx* c, Str label, IconId icon, Rect r, BtnKind kind, bool enabled, Str sub) {
    BtnResult res;
    u32 id = w_id(label);
    bool hovered = enabled && r.contains(c->in.mouse_x, c->in.mouse_y) && !c->modal_id;
    if (hovered) c->hot_next = id;
    f32 hover = c->anim(id, hovered ? 1.0f : 0.0f, 130.0f);
    bool pressed_now = false;
    if (hovered && c->in.pressed[MOB_MB_LEFT]) c->active = id;
    if (c->active == id) {
        if (c->in.released[MOB_MB_LEFT]) { res.clicked = true; c->active = 0; }
        else if (c->in.down[MOB_MB_LEFT]) pressed_now = true;
    }
    res.hovered = hovered;
    Theme* th = c->theme;
    f32 radius = c->ui->sp(8);
    Col base = kind == BTN_PRIMARY ? th->accent : (kind == BTN_DANGER ? th->err : th->surface_2);
    if (!enabled) base = base.mix(th->surface_2, 0.65f);
    Col fill = base.mix(base.lighten(0.16f), hover * (pressed_now ? 0.35f : 1.0f));
    f32 oy = pressed_now ? c->ui->sp(1) : 0.0f;

    // Primary CTA gets a soft halo drawn as two translucent rounded rects
    // instead of a blur (zero extra sampling cost).
    if (kind == BTN_PRIMARY && hover > 0.02f) {
        c->ui->rrect(r.x - c->ui->sp(3), r.y - c->ui->sp(3) + oy, r.w + c->ui->sp(6), r.h + c->ui->sp(6),
                     radius + c->ui->sp(3), th->accent.with_a(0.10f * hover));
    }
    c->ui->rrect(r.x, r.y + oy, r.w, r.h, radius, fill);
    c->ui->rrect_border(r.x, r.y + oy, r.w, r.h, radius, 1.0f,
                        kind == BTN_PRIMARY ? fill.lighten(0.22f).with_a(0.55f) : th->border);

    Col fg = kind == BTN_PRIMARY ? th->accent.readable_on() : (kind == BTN_DANGER ? Col(1, 1, 1) : th->text);
    if (!enabled) fg = fg.with_a(0.5f);
    f32 ih = c->ui->sp(22);
    f32 text_w = c->ui->measure(label, FONT_TITLE);
    f32 sub_w = sub.empty() ? 0.0f : c->ui->measure(sub, FONT_SMALL);
    f32 total = ih + c->ui->sp(12) + mob_max(text_w, sub_w);
    f32 x = r.x + (r.w - total) * 0.5f;
    c->ui->icon(icon, x, r.cy() - ih * 0.5f, ih, fg, 1.7f);
    f32 tx = x + ih + c->ui->sp(12);
    if (sub.empty()) {
        c->ui->text(label, tx, r.cy() - c->ui->line_h(FONT_TITLE) * 0.5f, FONT_TITLE, fg);
    } else {
        c->ui->text(label, tx, r.cy() - c->ui->sp(19), FONT_TITLE, fg);
        c->ui->text(sub, tx, r.cy() + c->ui->sp(2), FONT_SMALL, fg.with_a(0.72f));
    }
    return res;
}

// =============================================================== controls
bool toggle(WidgetCtx* c, Str label, Rect r, bool* value, bool enabled) {
    Theme* th = c->theme;
    u32 id = w_id(label);
    bool hovered = enabled && r.contains(c->in.mouse_x, c->in.mouse_y);
    if (hovered) c->hot_next = id;
    bool changed = false;
    if (hovered && c->in.pressed[MOB_MB_LEFT] && enabled) { *value = !*value; changed = true; }
    f32 k = c->anim(id, *value ? 1.0f : 0.0f, 130.0f);
    f32 hover = c->anim(w_id2(Str("tgh"), (int)(id & 0x7FFFFFFF)), hovered ? 1.0f : 0.0f, 110.0f);

    f32 sw = c->ui->sp(36), sh = c->ui->sp(20);
    f32 tx = r.x + r.w - sw;
    Col track = (th->surface_3).mix(enabled ? th->accent : th->border_strong, k);
    c->ui->rrect(tx, r.y + (r.h - sh) * 0.5f, sw, sh, sh * 0.5f, track.mix(track.lighten(0.1f), hover * 0.6f));
    if (k > 0.05f) c->ui->rrect_border(tx, r.y + (r.h - sh) * 0.5f, sw, sh, sh * 0.5f, 1.0f,
                                       th->accent.lighten(0.3f).with_a(0.35f * k));
    f32 knob = sh - c->ui->sp(6);
    f32 kx = tx + c->ui->sp(3) + k * (sw - knob - c->ui->sp(6));
    Col knob_col = enabled ? Col(1, 1, 1) : Col(0.75f, 0.77f, 0.8f);
    c->ui->rrect(kx, r.y + (r.h - knob) * 0.5f, knob, knob, knob * 0.5f, knob_col.with_a(enabled ? 1.0f : 0.6f));
    Col lc = enabled ? th->text : th->text_faint;
    c->ui->text_ellipsis(label, r.x, r.y + (r.h - c->ui->line_h(FONT_BODY)) * 0.5f, r.w - sw - c->ui->sp(12),
                         FONT_BODY, lc);
    return changed;
}

bool segmented(WidgetCtx* c, Str label, Rect r, const char* const* items, u32 count, u32* index) {
    Theme* th = c->theme;
    f32 pad = c->ui->sp(3);
    c->ui->rrect(r.x, r.y, r.w, r.h, c->ui->sp(6), th->surface_2);
    c->ui->rrect_border(r.x, r.y, r.w, r.h, c->ui->sp(6), 1.0f, th->border);
    f32 seg_w = (r.w - pad * 2) / (f32)count;
    bool changed = false;
    // sliding indicator
    f32 target = pad + (f32)(*index) * seg_w;
    f32 ind = c->anim(w_id2(label, 7717), target, 160.0f);
    c->ui->rrect(r.x + ind, r.y + pad, seg_w, r.h - pad * 2, c->ui->sp(4), th->accent.with_a(0.9f));

    for (u32 i = 0; i < count; ++i) {
        Rect sr{ r.x + pad + seg_w * i, r.y + pad, seg_w, r.h - pad * 2 };
        u32 id = w_id2(label, (int)i);
        bool hovered = sr.contains(c->in.mouse_x, c->in.mouse_y);
        if (hovered) c->hot_next = id;
        if (hovered && c->in.pressed[MOB_MB_LEFT]) { if (*index != i) changed = true; *index = i; }
        bool sel = (*index == i);
        Col fg = sel ? th->accent.readable_on() : th->text_dim;
        c->ui->text(Str(items[i]), sr.cx(), sr.y + (sr.h - c->ui->line_h(FONT_LABEL)) * 0.5f,
                    FONT_LABEL, fg, ALIGN_CENTER);
    }
    return changed;
}

bool dropdown(WidgetCtx* c, Str label, Rect r, const char* const* items, u32 count, u32* index,
              const char* const* hints) {
    Theme* th = c->theme;
    u32 id = w_id(label);
    bool hovered = r.contains(c->in.mouse_x, c->in.mouse_y);
    if (hovered) c->hot_next = id;
    bool open = (c->open_dropdown_id == id);
    bool changed = false;
    f32 hover = c->anim(w_id2(Str("ddh"), (int)(id & 0x7FFFFFFF)), hovered ? 1.0f : 0.0f, 110.0f);

    c->ui->rrect(r.x, r.y, r.w, r.h, c->ui->sp(6), th->surface_2.mix(th->surface_3, hover * 0.7f));
    c->ui->rrect_border(r.x, r.y, r.w, r.h, c->ui->sp(6), 1.0f,
                        open ? th->accent.with_a(0.85f) : th->border.mix(th->border_strong, hover));
    c->ui->text(Str(items[*index]), r.x + c->ui->sp(10),
                r.y + (r.h - c->ui->line_h(FONT_BODY)) * 0.5f, FONT_BODY, th->text);
    c->ui->icon(ICON_CHEVRON_DOWN, r.r() - c->ui->sp(24), r.cy() - c->ui->sp(7), c->ui->sp(14),
                open ? th->accent : th->text_faint, open ? 0.0f : 1.7f);

    if (hovered && c->in.pressed[MOB_MB_LEFT]) {
        c->open_dropdown_id = open ? 0 : id;
        open = !open;
    }
    if (open) {
        f32 item_h = c->ui->sp(hints ? 44 : 30);
        f32 list_h = item_h * (f32)count + c->ui->sp(8);
        f32 ly = r.b() + c->ui->sp(4);
        if (ly + list_h > (f32)c->ui->height - c->ui->sp(8)) ly = r.y - list_h - c->ui->sp(4);
        Rect lr{ r.x, ly, mob_max(r.w, c->ui->sp(180)), list_h };
        c->ui->rrect(lr.x, lr.y, lr.w, lr.h, c->ui->sp(6), th->surface_2.with_a(0.99f));
        c->ui->rrect_border(lr.x, lr.y, lr.w, lr.h, c->ui->sp(6), 1.0f, th->border_strong);
        for (u32 i = 0; i < count; ++i) {
            Rect ir{ lr.x + c->ui->sp(4), lr.y + c->ui->sp(4) + item_h * i, lr.w - c->ui->sp(8), item_h };
            bool ih = ir.contains(c->in.mouse_x, c->in.mouse_y);
            if (ih) {
                c->ui->rrect(ir.x, ir.y, ir.w, ir.h, c->ui->sp(4), th->surface_3);
                if (c->in.pressed[MOB_MB_LEFT]) {
                    if (*index != i) changed = true;
                    *index = i;
                    c->open_dropdown_id = 0;
                }
            }
            bool sel = (*index == i);
            f32 ty = hints ? ir.y + c->ui->sp(5) : ir.y + (ir.h - c->ui->line_h(FONT_BODY)) * 0.5f;
            c->ui->text(Str(items[i]), ir.x + c->ui->sp(10), ty, FONT_BODY,
                        sel ? th->accent : th->text);
            if (sel) c->ui->icon(ICON_CHECK, ir.r() - c->ui->sp(24), ir.cy() - c->ui->sp(7), c->ui->sp(14), th->accent, 1.8f);
            if (hints && hints[i] && *hints[i]) {
                c->ui->text_ellipsis(Str(hints[i]), ir.x + c->ui->sp(10), ir.y + c->ui->sp(23),
                                     ir.w - c->ui->sp(40), FONT_TINY, th->text_faint);
            }
        }
        // click outside closes
        if (c->in.pressed[MOB_MB_LEFT] && !lr.contains(c->in.mouse_x, c->in.mouse_y) && !hovered)
            c->open_dropdown_id = 0;
    }
    return changed;
}

bool slider(WidgetCtx* c, Str label, Rect r, f32* value, f32 mn, f32 mx, f32 step, const char* fmt, bool enabled) {
    Theme* th = c->theme;
    u32 id = w_id(label);
    bool hovered = enabled && r.contains(c->in.mouse_x, c->in.mouse_y);
    if (hovered) c->hot_next = id;
    bool changed = false;
    if (hovered && c->in.pressed[MOB_MB_LEFT]) c->active = id;
    f32 track_y = r.cy() - c->ui->sp(2);
    f32 track_h = c->ui->sp(4);
    f32 knob_r = c->ui->sp(7);
    f32 x0 = r.x + knob_r, x1 = r.r() - knob_r;
    f32 t = (mx > mn) ? mob_clamp((*value - mn) / (mx - mn), 0.0f, 1.0f) : 0.0f;
    if (c->active == id && enabled) {
        f32 nt = mob_clamp((c->in.mouse_x - x0) / mob_max(x1 - x0, 1.0f), 0.0f, 1.0f);
        f32 raw = mn + nt * (mx - mn);
        if (step > 0.0001f) raw = mn + floorf((raw - mn) / step + 0.5f) * step;
        raw = mob_clamp(raw, mn, mx);
        if (fabsf(raw - *value) > 0.0001f) { *value = raw; changed = true; }
        t = (mx > mn) ? mob_clamp((*value - mn) / (mx - mn), 0.0f, 1.0f) : 0.0f;
    }
    f32 hover = c->anim(w_id2(Str("slh"), (int)(id & 0x7FFFFFFF)), hovered ? 1.0f : 0.0f, 110.0f);
    if (c->in.key_left && c->focus == id && enabled) {}
    c->ui->rrect(x0, track_y, x1 - x0, track_h, track_h * 0.5f, th->surface_3);
    f32 fx = (x1 - x0) * t;
    if (fx > 0.5f) c->ui->rrect(x0, track_y, mob_max(fx, track_h), track_h, track_h * 0.5f,
                               enabled ? th->accent : th->text_faint);
    f32 kx = x0 + fx;
    f32 kr = knob_r * (1.0f + 0.12f * hover + (c->active == id ? 0.08f : 0.0f));
    c->ui->circle(kx, r.cy(), kr, th->surface);
    c->ui->ring(kx, r.cy(), kr, 2.0f, enabled ? th->accent : th->text_faint);
    if (c->active == id) c->ui->circle(kx, r.cy(), kr + c->ui->sp(3), th->accent.with_a(0.18f));

    char val[64];
    snprintf(val, sizeof(val), fmt, *value);
    if (!label.empty()) {
        c->ui->text(label, r.x, r.y - c->ui->sp(19), FONT_SMALL, enabled ? th->text_dim : th->text_faint);
    }
    c->ui->text(Str(val), r.r(), r.y - c->ui->sp(19), FONT_MONO, enabled ? th->text : th->text_faint, ALIGN_RIGHT);
    return changed;
}

bool checkbox(WidgetCtx* c, Str label, Rect r, bool* value) {
    Theme* th = c->theme;
    u32 id = w_id(label);
    bool hovered = r.contains(c->in.mouse_x, c->in.mouse_y);
    if (hovered) c->hot_next = id;
    bool changed = false;
    if (hovered && c->in.pressed[MOB_MB_LEFT]) { *value = !*value; changed = true; }
    f32 k = c->anim(id, *value ? 1.0f : 0.0f, 130.0f);
    f32 box = c->ui->sp(16);
    Rect br{ r.x, r.y + (r.h - box) * 0.5f, box, box };
    c->ui->rrect(br.x, br.y, box, box, c->ui->sp(4), th->surface_2.mix(th->accent, k));
    c->ui->rrect_border(br.x, br.y, box, box, c->ui->sp(4), 1.0f,
                        k > 0.5f ? th->accent : th->border_strong);
    if (k > 0.05f) {
        f32 s = c->ui->sp(10);
        c->ui->icon(ICON_CHECK, br.cx() - s * 0.5f, br.cy() - s * 0.5f, s,
                    th->accent.readable_on(), 2.0f);
    }
    c->ui->text(label, br.r() + c->ui->sp(10), r.y + (r.h - c->ui->line_h(FONT_BODY)) * 0.5f, FONT_BODY, th->text);
    return changed;
}

bool text_field(WidgetCtx* c, Str label, Rect r, TextFieldState* st, Str placeholder) {
    Theme* th = c->theme;
    u32 id = w_id(label);
    bool hovered = r.contains(c->in.mouse_x, c->in.mouse_y);
    if (hovered) c->hot_next = id;
    bool focused = (c->focus == id);
    if (c->in.pressed[MOB_MB_LEFT]) {
        if (hovered) c->focus = id;
        else if (focused) c->focus = 0;
    }
    focused = (c->focus == id);
    bool changed = false;
    if (focused) {
        st->caret_blink += c->in.dt;
        for (u32 i = 0; i < c->in.text_input_count; ++i) {
            u32 cp = c->in.text_input[i];
            if (cp < 32) continue;
            if (st->len + 4 >= sizeof(st->buf)) break;
            // encode UTF-8
            u8 tmp[4]; u32 n = 0;
            if (cp < 0x80) tmp[n++] = (u8)cp;
            else if (cp < 0x800) { tmp[n++] = (u8)(0xC0 | (cp >> 6)); tmp[n++] = (u8)(0x80 | (cp & 0x3F)); }
            else { tmp[n++] = (u8)(0xE0 | (cp >> 12)); tmp[n++] = (u8)(0x80 | ((cp >> 6) & 0x3F)); tmp[n++] = (u8)(0x80 | (cp & 0x3F)); }
            if (st->caret > st->len) st->caret = st->len;
            memmove(st->buf + st->caret + n, st->buf + st->caret, st->len - st->caret);
            memcpy(st->buf + st->caret, tmp, n);
            st->caret += n; st->len += n; changed = true;
        }
        if (c->in.key_backspace && st->caret > 0) {
            --st->caret;
            memmove(st->buf + st->caret, st->buf + st->caret + 1, st->len - st->caret - 1);
            --st->len; changed = true;
        }
        if (c->in.key_delete && st->caret < st->len) {
            memmove(st->buf + st->caret, st->buf + st->caret + 1, st->len - st->caret - 1);
            --st->len; changed = true;
        }
        if (c->in.key_left && st->caret > 0) --st->caret;
        if (c->in.key_right && st->caret < st->len) ++st->caret;
        if (c->in.key_home) st->caret = 0;
        if (c->in.key_end) st->caret = st->len;
    }
    c->ui->rrect(r.x, r.y, r.w, r.h, c->ui->sp(6), th->surface_2);
    c->ui->rrect_border(r.x, r.y, r.w, r.h, c->ui->sp(6), focused ? 1.5f : 1.0f,
                        focused ? th->accent : (hovered ? th->border_strong : th->border));
    Str val(st->buf, st->len);
    f32 ty = r.y + (r.h - c->ui->line_h(FONT_BODY)) * 0.5f;
    if (val.empty() && !placeholder.empty() && !focused) {
        c->ui->text(placeholder, r.x + c->ui->sp(10), ty, FONT_BODY, th->text_faint);
    } else {
        // keep the caret visible by scrolling the text
        f32 caret_x = c->ui->measure(val.sub(0, st->caret), FONT_BODY);
        f32 total = c->ui->measure(val, FONT_BODY);
        f32 avail = r.w - c->ui->sp(20);
        if (caret_x - st->scroll > avail) st->scroll = caret_x - avail;
        if (caret_x < st->scroll) st->scroll = caret_x;
        if (total < avail) st->scroll = 0;
        c->ui->push_clip(r.x + c->ui->sp(8), r.y, r.w - c->ui->sp(16), r.h);
        c->ui->text(val, r.x + c->ui->sp(10) - st->scroll, ty, FONT_BODY, th->text);
        if (focused && fmodf(st->caret_blink, 1.06f) < 0.55f) {
            f32 cx = r.x + c->ui->sp(10) - st->scroll + caret_x;
            c->ui->rect(cx, r.y + c->ui->sp(7), 1.5f, r.h - c->ui->sp(14), th->accent);
        }
        c->ui->pop_clip();
    }
    return changed;
}

bool keybind_field(WidgetCtx* c, Str label, Rect r, u32* vk, bool* waiting) {
    Theme* th = c->theme;
    u32 id = w_id(label);
    bool hovered = r.contains(c->in.mouse_x, c->in.mouse_y);
    if (hovered) c->hot_next = id;
    if (hovered && c->in.pressed[MOB_MB_LEFT]) *waiting = true;
    bool changed = false;
    if (*waiting) {
        // One-shot: the App feeds the pressed key through input.text_input and
        // the virtual key through key_vk.
        extern u32 g_last_vk;
        if (g_last_vk) {
            *vk = g_last_vk;
            *waiting = false;
            changed = true;
            g_last_vk = 0;
        }
    }
    c->ui->rrect(r.x, r.y, r.w, r.h, c->ui->sp(6), th->surface_2);
    c->ui->rrect_border(r.x, r.y, r.w, r.h, c->ui->sp(6), 1.0f,
                        *waiting ? th->accent : (hovered ? th->border_strong : th->border));
    char buf[32];
    if (*waiting) snprintf(buf, sizeof(buf), "press a key...");
    else {
        extern const char* vk_name(u32 vk);
        snprintf(buf, sizeof(buf), "%s", vk_name(*vk));
    }
    c->ui->text(Str(buf), r.cx(), r.y + (r.h - c->ui->line_h(FONT_MONO)) * 0.5f, FONT_MONO,
                *waiting ? th->accent : th->text, ALIGN_CENTER);
    return changed;
}

// ================================================================ display
void section_header(WidgetCtx* c, Str title, Rect r, IconId icon) {
    Theme* th = c->theme;
    f32 x = r.x;
    if (icon != ICON_NONE) {
        c->ui->icon(icon, x, r.cy() - c->ui->sp(8), c->ui->sp(16), th->accent, 1.7f);
        x += c->ui->sp(24);
    }
    c->ui->text(title, x, r.cy() - c->ui->line_h(FONT_TITLE) * 0.5f, FONT_TITLE, th->text);
    f32 tw = c->ui->measure(title, FONT_TITLE);
    f32 line_x = x + tw + c->ui->sp(14);
    if (line_x < r.r()) {
        c->ui->rect(line_x, r.cy(), r.r() - line_x, 1.0f, th->border);
    }
}

void card(WidgetCtx* c, Rect r, bool elevated) {
    Theme* th = c->theme;
    c->ui->rrect(r.x, r.y, r.w, r.h, c->ui->sp(8), elevated ? th->surface_2 : th->surface);
    c->ui->rrect_border(r.x, r.y, r.w, r.h, c->ui->sp(8), 1.0f, th->border);
}

void divider(WidgetCtx* c, f32 x, f32 y, f32 w) {
    c->ui->rect(x, y, w, 1.0f, c->theme->border);
}

void label(WidgetCtx* c, Str text, f32 x, f32 y, FontId font, Col color) {
    c->ui->text(text, x, y, font, color);
}
void label_dim(WidgetCtx* c, Str text, f32 x, f32 y) {
    c->ui->text(text, x, y, FONT_SMALL, c->theme->text_dim);
}

void kv_row(WidgetCtx* c, Str key, Str value, Rect r, Col value_color) {
    Theme* th = c->theme;
    if (value_color.r < 0) value_color = th->text;
    c->ui->text(key, r.x, r.y + (r.h - c->ui->line_h(FONT_SMALL)) * 0.5f, FONT_SMALL, th->text_dim);
    c->ui->text_ellipsis(value, r.r(), r.y + (r.h - c->ui->line_h(FONT_MONO)) * 0.5f,
                         r.w * 0.62f, FONT_MONO, value_color, ALIGN_RIGHT);
}

void status_dot(WidgetCtx* c, f32 cx, f32 cy, Col color, bool pulse) {
    if (pulse && !c->game_mode) {
        // Single expanding ring, 1.4 s period: enough to read as "live" without
        // a per-frame effect chain.
        f32 t = fmodf(c->now_ms / 1400.0f, 1.0f);
        c->ui->circle(cx, cy, c->ui->sp(5) + t * c->ui->sp(9), color.with_a(0.22f * (1.0f - t)));
    }
    c->ui->circle(cx, cy, c->ui->sp(5), color);
    c->ui->ring(cx, cy, c->ui->sp(5), 1.0f, color.lighten(0.4f).with_a(0.5f));
}

void badge(WidgetCtx* c, Str text, f32 x, f32 y, Col color) {
    f32 w = c->ui->measure(text, FONT_TINY) + c->ui->sp(14);
    f32 h = c->ui->sp(18);
    c->ui->rrect(x, y, w, h, h * 0.5f, color.with_a(0.15f));
    c->ui->rrect_border(x, y, w, h, h * 0.5f, 1.0f, color.with_a(0.35f));
    c->ui->text(text, x + w * 0.5f, y + (h - c->ui->line_h(FONT_TINY)) * 0.5f, FONT_TINY, color, ALIGN_CENTER);
}

void meter_row(WidgetCtx* c, Str lb, f32 t, Str value_text, Rect r, Col fill) {
    Theme* th = c->theme;
    f32 label_h = c->ui->sp(16);
    c->ui->text(lb, r.x, r.y, FONT_SMALL, th->text_dim);
    c->ui->text(value_text, r.r(), r.y, FONT_MONO, th->text, ALIGN_RIGHT);
    c->ui->meter(r.x, r.y + label_h, r.w, c->ui->sp(6), t, th->surface_3, fill);
}

void stat_tile(WidgetCtx* c, Str lb, Str value, Str unit, Rect r, IconId icon, Col value_color) {
    Theme* th = c->theme;
    card(c, r, true);
    f32 pad = c->ui->sp(14);
    if (icon != ICON_NONE)
        c->ui->icon(icon, r.x + pad, r.y + pad, c->ui->sp(15), th->text_faint, 1.6f);
    c->ui->text(lb, r.x + pad + (icon != ICON_NONE ? c->ui->sp(22) : 0), r.y + pad + c->ui->sp(1),
                FONT_TINY, th->text_faint);
    f32 vy = r.y + pad + c->ui->sp(20);
    f32 vw = c->ui->measure(value, FONT_DISPLAY);
    c->ui->text(value, r.x + pad, vy, FONT_DISPLAY, value_color);
    if (!unit.empty())
        c->ui->text(unit, r.x + pad + vw + c->ui->sp(6), vy + c->ui->sp(22), FONT_SMALL, th->text_faint);
}

void graph(WidgetCtx* c, const f32* samples, u32 count, u32 head, Rect r, Col line, f32 mn, f32 mx_,
           const char* y_label, bool fill_area) {
    Theme* th = c->theme;
    c->ui->rrect(r.x, r.y, r.w, r.h, c->ui->sp(6), th->surface_2.with_a(0.55f));
    // grid: 4 horizontal rules, no vertical clutter
    for (u32 i = 0; i <= 4; ++i) {
        f32 y = r.y + r.h * (f32)i / 4.0f;
        c->ui->rect(r.x, y, r.w, 1.0f, th->chart_grid.with_a(i == 4 ? 0.9f : 0.55f));
    }
    if (y_label && *y_label) {
        c->ui->text(Str(y_label), r.x + c->ui->sp(6), r.y + c->ui->sp(3), FONT_TINY, th->text_faint);
    }
    if (count < 2) return;
    f32 span = mob_max(mx_ - mn, 0.0001f);
    // Build a polyline of at most `count` points; the newest sample is at `head`.
    const u32 MAXP = 512;
    f32 pts[MAXP * 2];
    u32 n = mob_min(count, MAXP);
    u32 stride = mob_max(count / MAXP, 1u);
    u32 pi = 0;
    for (u32 i = 0; i < n; ++i) {
        u32 idx = (head + MOB_GRAPH_RING - 1 - (n - 1 - i) * stride) % MOB_GRAPH_RING;
        f32 v = mob_clamp((samples[idx] - mn) / span, 0.0f, 1.0f);
        pts[pi * 2 + 0] = r.x + r.w * (f32)i / (f32)(n - 1);
        pts[pi * 2 + 1] = r.b() - v * (r.h - c->ui->sp(4)) - c->ui->sp(2);
        ++pi;
        if (pi >= MAXP) break;
    }
    if (fill_area && pi > 1) {
        // fill under the curve as a triangle strip (cheap, no extra pass)
        for (u32 i = 0; i + 1 < pi; ++i) {
            f32 y0 = pts[i * 2 + 1], y1 = pts[(i + 1) * 2 + 1];
            f32 x0 = pts[i * 2], x1 = pts[(i + 1) * 2];
            c->ui->triangle(x0, y0, x1, y1, x0, r.b(), th->accent.with_a(0.13f));
            c->ui->triangle(x1, y1, x1, r.b(), x0, r.b(), th->accent.with_a(0.13f));
        }
    }
    c->ui->polyline(pts, pi, c->ui->sp(1.6f), line);
    // newest sample marker
    if (pi > 0) c->ui->circle(pts[(pi - 1) * 2], pts[(pi - 1) * 2 + 1], c->ui->sp(2.4f), line);
}

void progress_ring(WidgetCtx* c, f32 cx, f32 cy, f32 radius, f32 t, Col color, f32 thickness) {
    u32 seg = 40;
    f32 prev = -1.0f;
    for (u32 i = 0; i <= seg; ++i) {
        f32 a = -1.5707963f + (f32)i / (f32)seg * 6.2831853f * mob_clamp(t, 0.0f, 1.0f);
        f32 x = cx + cosf(a) * radius, y = cy + sinf(a) * radius;
        if (prev >= 0.0f) {
            f32 pa = -1.5707963f + (f32)(i - 1) / (f32)seg * 6.2831853f * mob_clamp(t, 0.0f, 1.0f);
            f32 px = cx + cosf(pa) * radius, py = cy + sinf(pa) * radius;
            c->ui->line(px, py, x, y, c->ui->sp(thickness), color);
        }
        prev = a;
    }
}

bool list_row(WidgetCtx* c, Str text, Str sub, Rect r, bool selected, IconId icon) {
    Theme* th = c->theme;
    u32 id = w_id(text);
    bool hovered = r.contains(c->in.mouse_x, c->in.mouse_y);
    if (hovered) c->hot_next = id;
    bool clicked = hovered && c->in.pressed[MOB_MB_LEFT];
    f32 hv = c->anim(id, (hovered || selected) ? 1.0f : 0.0f, 120.0f);
    if (hv > 0.01f) c->ui->rrect(r.x, r.y, r.w, r.h, c->ui->sp(5),
                                 (selected ? th->accent.with_a(0.14f) : th->surface_3.with_a(0.7f * hv)));
    if (selected) c->ui->rect(r.x, r.y + c->ui->sp(6), c->ui->sp(2), r.h - c->ui->sp(12), th->accent);
    f32 tx = r.x + c->ui->sp(12);
    if (icon != ICON_NONE) {
        c->ui->icon(icon, tx, r.cy() - c->ui->sp(8), c->ui->sp(16), selected ? th->accent : th->text_dim, 1.6f);
        tx += c->ui->sp(24);
    }
    if (sub.empty()) {
        c->ui->text(text, tx, r.cy() - c->ui->line_h(FONT_BODY) * 0.5f, FONT_BODY,
                    selected ? th->text : th->text_dim.mix(th->text, hv));
    } else {
        c->ui->text(text, tx, r.cy() - c->ui->sp(16), FONT_BODY, selected ? th->text : th->text.mix(th->text_dim, 1.0f - hv));
        c->ui->text_ellipsis(sub, tx, r.cy() + c->ui->sp(3), r.r() - tx - c->ui->sp(10), FONT_SMALL, th->text_faint);
    }
    return clicked;
}

void tooltip(WidgetCtx* c, u32 id, Str text, bool condition) {
    if (!condition || text.empty()) return;
    if (c->tip.id != id) {
        c->tip.id = id;
        c->tip.text = text;
        c->tip.delay = 0.0f;
    }
    c->tip.x = c->in.mouse_x;
    c->tip.y = c->in.mouse_y;
    c->tip.delay += c->in.dt;
}

void empty_state(WidgetCtx* c, Rect r, IconId icon, Str title, Str text) {
    Theme* th = c->theme;
    f32 cy = r.cy() - c->ui->sp(20);
    c->ui->icon_centered(icon, r.cx(), cy, c->ui->sp(40), th->text_faint.with_a(0.55f), 1.4f);
    c->ui->text(title, r.cx(), cy + c->ui->sp(34), FONT_TITLE, th->text_dim, ALIGN_CENTER);
    c->ui->text_wrapped(text, r.cx() - r.w * 0.35f, cy + c->ui->sp(60), r.w * 0.7f,
                        c->ui->line_h(FONT_SMALL) + c->ui->sp(2), FONT_SMALL, th->text_faint, 3);
}

void scrollbar(WidgetCtx* c, Rect track, f32 offset, f32 content_h, f32 view_h) {
    if (content_h <= view_h + 1.0f) return;
    Theme* th = c->theme;
    f32 frac = mob_clamp(view_h / content_h, 0.06f, 1.0f);
    f32 bar_h = mob_max(track.h * frac, c->ui->sp(28));
    f32 max_off = mob_max(content_h - view_h, 1.0f);
    f32 t = mob_clamp(offset / max_off, 0.0f, 1.0f);
    f32 y = track.y + (track.h - bar_h) * t;
    c->ui->rrect(track.r() - c->ui->sp(6), y, c->ui->sp(4), bar_h, c->ui->sp(2), th->border_strong.with_a(0.85f));
}

// ------------------------------------------------------------------- modal
static Rect g_modal_rect{};
static bool g_modal_active = false;

bool modal_begin(WidgetCtx* c, Str title, f32 w, f32 h, Rect* content) {
    if (!g_modal_active) g_modal_active = true;
    Theme* th = c->theme;
    f32 sc_w = w * c->ui->scale(), sc_h = h * c->ui->scale();
    Rect r{ ((f32)c->ui->width - sc_w) * 0.5f, ((f32)c->ui->height - sc_h) * 0.5f, sc_w, sc_h };
    g_modal_rect = r;
    c->ui->rect(0, 0, (f32)c->ui->width, (f32)c->ui->height, Col(0, 0, 0, th->is_light() ? 0.35f : 0.55f));
    c->ui->rrect(r.x, r.y, r.w, r.h, c->ui->sp(10), th->surface);
    c->ui->rrect_border(r.x, r.y, r.w, r.h, c->ui->sp(10), 1.0f, th->border_strong);
    f32 head = c->ui->sp(48);
    c->ui->text(title, r.x + c->ui->sp(20), r.y + (head - c->ui->line_h(FONT_TITLE)) * 0.5f, FONT_TITLE, th->text);
    divider(c, r.x, r.y + head, r.w);
    *content = Rect{ r.x + c->ui->sp(20), r.y + head + c->ui->sp(16), r.w - c->ui->sp(40), r.h - head - c->ui->sp(72) };
    return true;
}

void modal_end(WidgetCtx* c) {
    (void)c;
    g_modal_active = false;
}

// ------------------------------------------------------------------ helpers
u32 g_last_vk = 0;

const char* vk_name(u32 vk) {
    static char buf[24];
    switch (vk) {
        case 0x70: return "F1";  case 0x71: return "F2";  case 0x72: return "F3";
        case 0x73: return "F4";  case 0x74: return "F5";  case 0x75: return "F6";
        case 0x76: return "F7";  case 0x77: return "F8";  case 0x78: return "F9";
        case 0x79: return "F10"; case 0x7A: return "F11"; case 0x7B: return "F12";
        case 0x20: return "Space"; case 0x1B: return "Esc"; case 0x09: return "Tab";
        case 0x0D: return "Enter"; case 0x08: return "Backspace";
        case 0x2D: return "Insert"; case 0x2E: return "Delete";
        case 0xC0: return "`"; case 0xBD: return "-"; case 0xBB: return "=";
        default: break;
    }
    if (vk >= 0x30 && vk <= 0x39) { snprintf(buf, sizeof(buf), "%c", (char)vk); return buf; }
    if (vk >= 0x41 && vk <= 0x5A) { snprintf(buf, sizeof(buf), "%c", (char)vk); return buf; }
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

} // namespace mob
