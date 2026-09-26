// ============================================================================
//  MOBILADOR - src/render/color.h
//  Linear-ish colour type used by the whole UI.  All theme colours are stored
//  as f32 RGBA so that accent blending, hover transitions and chart gradients
//  can be computed without repeated integer conversions.
// ============================================================================
#pragma once

#include "../core/base.h"

namespace mob {

struct Col {
    f32 r = 0, g = 0, b = 0, a = 1;

    Col() = default;
    Col(f32 rr, f32 gg, f32 bb, f32 aa = 1.0f) : r(rr), g(gg), b(bb), a(aa) {}

    static Col from_u32(u32 rgba) {
        return Col(((rgba >> 24) & 0xFF) / 255.0f,
                   ((rgba >> 16) & 0xFF) / 255.0f,
                   ((rgba >> 8)  & 0xFF) / 255.0f,
                   ((rgba)       & 0xFF) / 255.0f);
    }
    static Col from_rgb(u32 rgb, f32 alpha = 1.0f) {
        return Col(((rgb >> 16) & 0xFF) / 255.0f,
                   ((rgb >> 8)  & 0xFF) / 255.0f,
                   ((rgb)       & 0xFF) / 255.0f,
                   alpha);
    }
    static Col hsv(f32 h, f32 s, f32 v, f32 a = 1.0f) {
        h = h - (f32)((int)h); if (h < 0) h += 1.0f;
        f32 i = (f32)(int)(h * 6.0f);
        f32 f = h * 6.0f - i;
        f32 p = v * (1.0f - s), q = v * (1.0f - f * s), t = v * (1.0f - (1.0f - f) * s);
        switch ((int)i % 6) {
            case 0: return Col(v, t, p, a);
            case 1: return Col(q, v, p, a);
            case 2: return Col(p, v, t, a);
            case 3: return Col(p, q, v, a);
            case 4: return Col(t, p, v, a);
            default: return Col(v, p, q, a);
        }
    }
    u32 to_u32() const {
        u32 rr = (u32)(mob_clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
        u32 gg = (u32)(mob_clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
        u32 bb = (u32)(mob_clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
        u32 aa = (u32)(mob_clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        return (rr << 24) | (gg << 16) | (bb << 8) | aa;
    }
    Col with_a(f32 alpha) const { return Col(r, g, b, alpha); }
    Col mul_a(f32 k) const { return Col(r, g, b, a * k); }

    Col mix(const Col& o, f32 t) const {
        return Col(mob_lerp(r, o.r, t), mob_lerp(g, o.g, t), mob_lerp(b, o.b, t), mob_lerp(a, o.a, t));
    }
    Col lighten(f32 k) const { return mix(Col(1, 1, 1, a), k); }
    Col darken(f32 k) const { return mix(Col(0, 0, 0, a), k); }
    // Relative luminance (sRGB coefficients) - used to pick readable text on
    // top of an user-chosen accent colour.
    f32 luma() const { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
    Col readable_on() const { return luma() > 0.55f ? Col(0.06f, 0.07f, 0.09f, a) : Col(1, 1, 1, a); }
};

MOB_INLINE Col operator*(const Col& c, f32 k) { return Col(c.r * k, c.g * k, c.b * k, c.a); }
MOB_INLINE Col operator+(const Col& a, const Col& b) { return Col(a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a); }

} // namespace mob
