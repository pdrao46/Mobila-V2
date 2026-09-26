// ============================================================================
//  MOBILADOR - src/ui/theme.h
//  Design tokens of the product's visual identity.
//
//  DESIGN RULES (kept deliberately small so the UI never drifts)
//    * One accent colour drives every interactive surface. Nothing else is
//      allowed to be saturated.
//    * Neutral ramp is cool-grey (never pure black except in AMOLED mode).
//    * Elevation is expressed with a 1 px border and a slightly lighter fill,
//      NOT with drop shadows (shadows add overdraw and blur cost every frame).
//    * Motion: 120-180 ms, ease-out, and only where it communicates state.
//      In Game Mode every transition duration collapses to 0.
//    * Spacing scale: 4, 8, 12, 16, 20, 24, 32. Radii: 999 (pills: navigation
//      items, switches), 10 (buttons, inputs, tiles), 14 (cards). Stroke: 1 px
//      hairlines, 1.75 px icons in lists.
// ============================================================================
#pragma once

#include "../render/color.h"
#include "../core/base.h"

namespace mob {

enum ThemeMode : int {
    THEME_DARK = 0,
    THEME_LIGHT = 1,
    THEME_AMOLED = 2,
};

struct Theme {
    ThemeMode mode = THEME_DARK;
    Col accent      { 0.298f, 0.553f, 1.000f };
    char accent_name[16] = "Blue";

    // surfaces
    Col bg;            // window background
    Col surface;       // cards, panels
    Col surface_2;     // inputs, nested wells
    Col surface_3;     // hover / pressed fills
    Col sidebar;       // navigation rail
    Col titlebar;
    Col border;        // 1 px separators
    Col border_strong; // focused or emphasised borders

    // text
    Col text;          // primary
    Col text_dim;      // secondary
    Col text_faint;    // tertiary / captions
    Col on_accent;     // text on accent fills

    // semantic
    Col ok, warn, err, info;
    Col ok_dim, warn_dim, err_dim;

    // data visualisation (graphs keep the accent family)
    Col chart_grid;
    Col chart_line;
    Col chart_fill_top;
    Col chart_fill_bottom;

    // motion
    f32 transition_ms = 150.0f;
    bool animations = true;

    void apply(ThemeMode m, Col accent_col, const char* name);
    void set_accent(Col c, const char* name) { accent = c; apply(mode, c, name); }
    bool is_light() const { return mode == THEME_LIGHT; }
    Col accent_soft(f32 a = 0.16f) const { return accent.with_a(a); }
    Col alpha(f32 a) const { return text.with_a(a); }
};

// Preset accents: the 8 documented choices plus free colour picker.
struct AccentPreset { const char* name; u32 rgb; };
extern const AccentPreset kAccentPresets[8];
extern const int kAccentPresetCount;

void theme_init(Theme* t, ThemeMode mode, u32 accent_rgb, const char* accent_name);

} // namespace mob
