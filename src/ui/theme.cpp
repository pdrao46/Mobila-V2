// ============================================================================
//  MOBILADOR - src/ui/theme.cpp
// ============================================================================
#include "theme.h"

namespace mob {

const AccentPreset kAccentPresets[8] = {
    // Green leads: it is the product identity (and the reference palette).
    { "Green",    0x22C55E },
    { "Blue",     0x4C8DFF },
    { "Purple",   0x8B5CF6 },
    { "Red",      0xEF4444 },
    { "Orange",   0xF59E0B },
    { "Cyan",     0x06B6D4 },
    { "White",    0xE8ECF3 },
    { "Magenta",  0xEC4899 },
};
const int kAccentPresetCount = 8;

void Theme::apply(ThemeMode m, Col ac, const char* name) {
    mode   = m;
    accent = ac;
    if (name) {
        u32 i = 0;
        for (; name[i] && i < sizeof(accent_name) - 1; ++i) accent_name[i] = name[i];
        accent_name[i] = 0;
    }

    switch (mode) {
        case THEME_LIGHT:
            bg            = Col::from_rgb(0xF5F7FA);
            surface       = Col::from_rgb(0xFFFFFF);
            surface_2     = Col::from_rgb(0xF0F3F8);
            surface_3     = Col::from_rgb(0xE4EAF2);
            sidebar       = Col::from_rgb(0xFFFFFF);
            titlebar      = Col::from_rgb(0xFFFFFF);
            border        = Col::from_rgb(0xDBE2EC);
            border_strong = Col::from_rgb(0xC0CAD8);
            text          = Col::from_rgb(0x10151D);
            text_dim      = Col::from_rgb(0x4B5566);
            text_faint    = Col::from_rgb(0x8A94A6);
            on_accent     = Col(1, 1, 1);
            ok            = Col::from_rgb(0x16A34A);
            warn          = Col::from_rgb(0xD97706);
            err           = Col::from_rgb(0xDC2626);
            info          = Col::from_rgb(0x2563EB);
            chart_grid    = Col::from_rgb(0xE4E9F1);
            break;
        case THEME_AMOLED:
            bg            = Col::from_rgb(0x000000);
            surface       = Col::from_rgb(0x07090C);
            surface_2     = Col::from_rgb(0x0E1218);
            surface_3     = Col::from_rgb(0x18202A);
            sidebar       = Col::from_rgb(0x040608);
            titlebar      = Col::from_rgb(0x040608);
            border        = Col::from_rgb(0x18222C);
            border_strong = Col::from_rgb(0x2A3A48);
            text          = Col::from_rgb(0xF1F5F9);
            text_dim      = Col::from_rgb(0x94A3B8);
            text_faint    = Col::from_rgb(0x5B6B7C);
            on_accent     = accent.readable_on();
            ok            = Col::from_rgb(0x2DD46B);
            warn          = Col::from_rgb(0xF5A524);
            err           = Col::from_rgb(0xF2544B);
            info          = Col::from_rgb(0x5C9BFF);
            chart_grid    = Col::from_rgb(0x181C23);
            break;
        case THEME_DARK:
        default:
            // Palette tuned against the product reference: near-black cool
            // background, cards one step lighter, one saturated accent (green by
            // default) carrying every interactive surface.
            bg            = Col::from_rgb(0x0A0E14);
            surface       = Col::from_rgb(0x111823);
            surface_2     = Col::from_rgb(0x172030);
            surface_3     = Col::from_rgb(0x1F2A3A);
            sidebar       = Col::from_rgb(0x0C121B);
            titlebar      = Col::from_rgb(0x0C121B);
            border        = Col::from_rgb(0x1E2A38);
            border_strong = Col::from_rgb(0x2E3F52);
            text          = Col::from_rgb(0xEDF2F7);
            text_dim      = Col::from_rgb(0x9DACBE);
            text_faint    = Col::from_rgb(0x64748B);
            on_accent     = accent.readable_on();
            ok            = Col::from_rgb(0x22C55E);
            warn          = Col::from_rgb(0xF59E0B);
            err           = Col::from_rgb(0xEF4444);
            info          = Col::from_rgb(0x38BDF8);
            chart_grid    = Col::from_rgb(0x1B2534);
            break;
    }

    // The accent must remain readable as a stroke on the current surface.
    if (mode == THEME_LIGHT && accent.luma() > 0.82f) accent = accent.darken(0.35f);
    if (mode != THEME_LIGHT && accent.luma() < 0.14f) accent = accent.lighten(0.45f);

    ok_dim   = ok.mul_a(0.18f).with_a(0.16f);
    warn_dim = warn.mul_a(0.18f).with_a(0.16f);
    err_dim  = err.mul_a(0.18f).with_a(0.16f);

    chart_line        = accent;
    chart_fill_top    = accent.with_a(is_light() ? 0.22f : 0.28f);
    chart_fill_bottom = accent.with_a(0.0f);
}

void theme_init(Theme* t, ThemeMode mode, u32 accent_rgb, const char* accent_name) {
    const char* name = accent_name ? accent_name : "Custom";
    t->apply(mode, Col::from_rgb(accent_rgb), name);
}

} // namespace mob
