// ============================================================================
//  MOBILADOR - src/ui/widgets.h
//  Immediate-mode widget layer on top of ui2d.
//
//  Design: widgets have no allocation, no retained state beyond a small
//  hot/active/focus table, and no layout engine in the frame loop.  Every
//  control is drawn with the analytic shape shader, so the visual language is
//  identical for cards, buttons, sliders and charts.
// ============================================================================
#pragma once

#include "theme.h"
#include "../render/ui2d.h"

namespace mob {

// Ring size used by every telemetry graph (samples are read modulo this).
#define MOB_GRAPH_RING 1024

enum MouseButton : int { MB_LEFT = 0, MB_RIGHT = 1, MB_MIDDLE = 2 };

struct UiInput {
    f32  mouse_x = 0, mouse_y = 0;
    f32  mouse_dx = 0, mouse_dy = 0;
    bool down[3]     = { false, false, false };
    bool pressed[3]  = { false, false, false };
    bool released[3] = { false, false, false };
    bool double_click = false;
    f32  wheel = 0;
    // keyboard for text fields
    u32  text_input[16] = { 0 };
    u32  text_input_count = 0;
    bool key_backspace = false, key_delete = false, key_enter = false,
         key_tab = false, key_left = false, key_right = false, key_up = false, key_down = false,
         key_escape = false, key_home = false, key_end = false;
    bool ctrl = false, shift = false, alt = false;
    f32  dt = 1.0f / 60.0f;
    u64  time_us = 0;

    void new_frame() {
        for (int i = 0; i < 3; ++i) { pressed[i] = false; released[i] = false; }
        double_click = false;
        wheel = 0; text_input_count = 0;
        key_backspace = key_delete = key_enter = key_tab = false;
        key_left = key_right = key_up = key_down = false;
        key_escape = key_home = key_end = false;
        mouse_dx = mouse_dy = 0;
    }
};

struct TextFieldState {
    char  buf[256] = { 0 };
    u32   len = 0;
    u32   caret = 0;
    u32   sel_anchor = 0;
    f32   scroll = 0;
    f32   caret_blink = 0;
};

struct Toast {
    char text[160];
    Col  color;
    f32  ttl;
    f32  life;
    IconId icon;
};

struct WidgetCtx {
    Ui2D*  ui = nullptr;
    Theme* theme = nullptr;
    UiInput in;
    u32    hot = 0;        // hovered
    u32    active = 0;     // pressed / dragging
    u32    focus = 0;      // keyboard focus (text fields)
    u32    hot_next = 0;
    bool   game_mode = false;
    // animation bookkeeping: value -> displayed value
    struct Anim { u32 id; f32 value; f32 from; f32 start_ms; f32 dur_ms; };
    Anim   anims[64];
    u32    anim_count = 0;
    f32    now_ms = 0;
    StrMap<f32>* anim_map = nullptr;
    Arena* arena = nullptr;

    // toasts
    Toast  toasts[6];
    u32    toast_count = 0;
    // tooltips
    struct Tip { u32 id; Str text; f32 x, y; f32 delay; };
    Tip    tip{ 0, Str(), 0, 0, 0 };
    // modals
    u32    modal_id = 0;
    Vec<u32>* open_dropdowns = nullptr;
    bool   dropdown_open = false;
    u32    open_dropdown_id = 0;
    u32    scroll_capture = 0;
    f32*   scroll_offsets = nullptr;
    u32    accent_picker_open = 0;

    f32    ui_scale = 1.0f;

    void init(Arena* a, Ui2D* ui, Theme* theme);
    void begin_frame(f32 dt, u64 time_us);
    void end_frame();
    bool is_hot(u32 id) const { return hot == id; }
    bool is_active(u32 id) const { return active == id; }
    // Eased value used for every hover/selection/size transition.
    f32 anim(u32 id, f32 target, f32 duration_ms = 140.0f);
    void toast(Str text, Col color, IconId icon = ICON_INFO, f32 seconds = 3.0f);
    void draw_toasts();

    // ---------------------------------------------------------------- layout
    f32 row_h = 34.0f;
};

u32 w_id(Str label);
u32 w_id2(Str label, int index);
u32 w_idptr(const void* p);

// ------------------------------------------------------------------ buttons
enum BtnKind : int { BTN_PRIMARY = 0, BTN_SECONDARY, BTN_GHOST, BTN_DANGER, BTN_SUCCESS };
struct BtnResult { bool clicked = false; bool hovered = false; bool held = false; };

BtnResult
button(WidgetCtx* c, Str label, Rect r, BtnKind kind = BTN_SECONDARY, IconId icon = ICON_NONE, bool enabled = true);
BtnResult
button_icon(WidgetCtx* c, IconId icon, Rect r, const char* tip = nullptr, bool enabled = true, f32 icon_size = 16.0f);
BtnResult
button_big(WidgetCtx* c, Str label, IconId icon, Rect r, BtnKind kind, bool enabled, Str sub = Str());

// ------------------------------------------------------------------ controls
bool toggle(WidgetCtx* c, Str label, Rect r, bool* value, bool enabled = true);
bool segmented(WidgetCtx* c, Str label, Rect r, const char* const* items, u32 count, u32* index);
bool dropdown(WidgetCtx* c, Str label, Rect r, const char* const* items, u32 count, u32* index,
              const char* const* hints = nullptr);
bool slider(WidgetCtx* c, Str label, Rect r, f32* value, f32 min, f32 max, f32 step,
            const char* fmt = "%.0f", bool enabled = true);
bool checkbox(WidgetCtx* c, Str label, Rect r, bool* value);
bool text_field(WidgetCtx* c, Str label, Rect r, TextFieldState* state, Str placeholder = Str());
bool keybind_field(WidgetCtx* c, Str label, Rect r, u32* vk, bool* waiting);

// ------------------------------------------------------------------- display
void section_header(WidgetCtx* c, Str title, Rect r, IconId icon = ICON_NONE);
void card(WidgetCtx* c, Rect r, bool elevated = false);
void divider(WidgetCtx* c, f32 x, f32 y, f32 w);
void label(WidgetCtx* c, Str text, f32 x, f32 y, FontId font, Col color);
void label_dim(WidgetCtx* c, Str text, f32 x, f32 y);
void kv_row(WidgetCtx* c, Str key, Str value, Rect r, Col value_color = Col(-1, -1, -1, -1));
void status_dot(WidgetCtx* c, f32 cx, f32 cy, Col color, bool pulse);
void badge(WidgetCtx* c, Str text, f32 x, f32 y, Col color);
void meter_row(WidgetCtx* c, Str label, f32 t, Str value_text, Rect r, Col fill);
void stat_tile(WidgetCtx* c, Str label, Str value, Str unit, Rect r, IconId icon, Col value_color);
void graph(WidgetCtx* c, const f32* samples, u32 count, u32 head, Rect r, Col line, f32 min, f32 max,
           const char* y_label = nullptr, bool fill_area = true);
void progress_ring(WidgetCtx* c, f32 cx, f32 cy, f32 radius, f32 t, Col color, f32 thickness = 3.0f);
bool list_row(WidgetCtx* c, Str text, Str sub, Rect r, bool selected, IconId icon = ICON_NONE);
void tooltip(WidgetCtx* c, u32 id, Str text, bool condition = true);
void empty_state(WidgetCtx* c, Rect r, IconId icon, Str title, Str text);
void scrollbar(WidgetCtx* c, Rect track, f32 offset, f32 content_h, f32 view_h);
bool modal_begin(WidgetCtx* c, Str title, f32 width, f32 height, Rect* content);
void modal_end(WidgetCtx* c);

} // namespace mob
