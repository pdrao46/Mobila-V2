// ============================================================================
//  MOBILADOR - src/ui/app_screens.cpp
//  Every screen of the application.
//
//  LAYOUT CONVENTION
//  Widgets work in device pixels; the layout below is written in logical units
//  and multiplied by the DPI scale through SP(). Nothing here re-derives a
//  colour: all of it comes from the theme, so swapping the accent or the mode
//  updates the whole application with no per-screen work.
//
//  TEXT LANGUAGE
//  The interface is in Portuguese (the product's language). Code, identifiers
//  and comments are English on purpose: the source stays maintainable for any
//  developer, the product stays native for the user.
// ============================================================================
#include "app.h"
#include "../core/log.h"
#include <stdio.h>

// Layout units: the screens are written in logical pixels and scaled
// through the global UI scale published by the batcher.
#define SP(v) (mob::g_ui_scale * (f32)(v))

using namespace mob;

namespace mob {

static const char* kScreenTitles[SCREEN_COUNT] = {
    "Painel", "Desempenho", "Latencia", "Benchmark", "Diagnostico", "Configuracoes", "Sobre",
};
static const IconId kScreenIcons[SCREEN_COUNT] = {
    ICON_GAUGE, ICON_PERFORMANCE, ICON_LATENCY, ICON_BENCHMARK, ICON_DIAGNOSTICS, ICON_SETTINGS, ICON_INFO,
};

// ---------------------------------------------------------------------------
// small drawing helpers
// ---------------------------------------------------------------------------
static Col status_col(Theme* t, int status) {
    return status == DIAG_OK ? t->ok : (status == DIAG_WARN ? t->warn : t->err);
}
static const char* status_word(int status) {
    return status == DIAG_OK ? "OK" : (status == DIAG_WARN ? "ATENCAO" : "ERRO");
}

// One row of the diagnostics list: status dot, name, measured value, detail.
static void diag_row(WidgetCtx* c, const DiagItem* d, Rect r) {
    Theme* t = c->theme;
    Ui2D* ui = c->ui;
    Col col = status_col(t, d->status);
    card(c, r);
    ui->rect(r.x, r.y + SP(6), SP(3), r.h - SP(12), col);      // status key line
    ui->icon_centered(ICON_DOT, r.x + SP(20), r.cy(), SP(10), col);
    ui->text(Str(d->name), r.x + SP(34), r.cy() - ui->line_h(FONT_LABEL) * 0.5f, FONT_LABEL, t->text);
    f32 value_x = r.x + SP(190);
    ui->text(Str(d->value), value_x, r.cy() - ui->line_h(FONT_MONO) * 0.5f, FONT_MONO, col);
    f32 detail_x = r.x + SP(360);
    if (detail_x < r.r() - SP(20)) {
        ui->text_ellipsis(Str(d->detail), detail_x, r.cy() - ui->line_h(FONT_SMALL) * 0.5f,
                          r.r() - detail_x - SP(12), FONT_SMALL, t->text_faint);
    }
}

// FPS / latency readout used by the dashboard and the overlay.
static void readout(Ui2D* ui, Theme* t, Str label, Str value, Str unit, f32 x, f32 y, f32 w, Col vcol) {
    ui->text(label, x, y, FONT_SMALL, t->text_faint);
    f32 vw = ui->measure(value, FONT_DISPLAY);
    ui->text(value, x, y + SP(14), FONT_DISPLAY, vcol);
    if (unit.n) ui->text(unit, x + vw + SP(4), y + SP(28), FONT_SMALL, t->text_faint);
    (void)w;
}

// ===========================================================================
//  SHELL: navigation rail + top bar + content dispatch
// ===========================================================================
void App::draw_shell(Rect full) {
    const f32 sidebar_w = SP(212);
    const f32 topbar_h = SP(58);
    Rect sidebar{ full.x, full.y, sidebar_w, full.h };
    Rect topbar{ full.x + sidebar_w, full.y, full.w - sidebar_w, topbar_h };
    Rect content{ full.x + sidebar_w, full.y + topbar_h, full.w - sidebar_w, full.h - topbar_h };

    ui.rect(sidebar.x, sidebar.y, sidebar.w, sidebar.h, theme.sidebar);
    ui.rect(content.x, content.y, content.w, topbar_h, theme.titlebar);
    ui.rect(content.x, content.b() - SP(1), content.w, SP(1), theme.border);
    ui.rect(sidebar.r() - SP(1), sidebar.y, SP(1), sidebar.h, theme.border);

    // A whisper of the accent under the top bar: the reference background is not
    // flat, it is lit from above. Two vertices, no blur pass, no cost per frame.
    ui.rect_grad(content.x, content.y + topbar_h, content.w, SP(260),
                 theme.accent.with_a(theme.is_light() ? 0.05f : 0.07f),
                 theme.accent.with_a(0.0f));

    draw_sidebar(sidebar);
    draw_topbar(topbar);

    // ---- scrollable content
    const f32 pad = SP(22);
    Rect view{ content.x + pad, content.y + pad, content.w - pad * 2, content.h - pad * 2 - SP(28) };
    if (view.w < SP(200) || view.h < SP(100)) return;

    // The scroll range comes from the height each screen reported on the last
    // frame. The per-screen constants that used to live here (SP(690) for the
    // dashboard) fell behind their layouts, which is why "QUICK PERFORMANCE" sat
    // half outside the window with no way to scroll down to it.
    f32 content_h = screen_content_h[screen];
    if (content_h <= 0.0f) content_h = view.h;      // first frame: nothing to scroll yet
    f32 max_scroll = mob_max(content_h - view.h, 0.0f);
    f32* scroll = &screen_scroll[screen];
    if (view.contains(w.in.mouse_x, w.in.mouse_y)) {
        if (w.in.wheel != 0) *scroll -= w.in.wheel * SP(48);
    }
    *scroll = mob_clamp(*scroll, 0.0f, max_scroll);

    ui.push_clip(view.x, view.y, view.w, view.h);
    Rect page{ view.x, view.y - *scroll, view.w, content_h };
    f32 used = 0.0f;
    switch (screen) {
        case SCREEN_DASHBOARD:   used = draw_dashboard(page); break;
        case SCREEN_PERFORMANCE: used = draw_performance(page); break;
        case SCREEN_LATENCY:     used = draw_latency(page); break;
        case SCREEN_BENCHMARK:   used = draw_benchmark(page); break;
        case SCREEN_DIAGNOSTICS: used = draw_diagnostics(page); break;
        case SCREEN_SETTINGS:    used = draw_settings(page); break;
        case SCREEN_ABOUT:       used = draw_about(page); break;
        default: break;
    }
    ui.pop_clip();
    // Re-measure for the next frame: a screen that grows simply gets a bigger
    // scroll range, so nothing can be clipped out of reach again.
    if (used > 0.0f) {
        screen_content_h[screen] = used + pad;
        content_h = screen_content_h[screen];
        *scroll = mob_clamp(*scroll, 0.0f, mob_max(content_h - view.h, 0.0f));
    }
    if (max_scroll > 0) {
        Rect track{ view.r() + SP(6), view.y, SP(4), view.h };
        scrollbar(&w, track, *scroll, content_h, view.h);
    }

    // ---- status strip
    Rect strip{ content.x, content.b() - SP(26), content.w, SP(26) };
    ui.rect(strip.x, strip.y, strip.w, strip.h, theme.surface_2);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "%s   |   %s   |   FPS  %.0f / %.0f / %.0f   |   %.1f ms   |   %s",
             MOB_VERSION_STR,
             adb.have_device() ? adb.device()->model : "sem celular",
             sampler.fps.source_fps, sampler.fps.stream_fps, sampler.fps.display_fps,
             sampler.metrics[MET_TOTAL_US].last / 1000.0f,
             session_running() ? "USB ATIVO" : "ocioso");
    ui.text(Str(buf), strip.x + SP(14), strip.cy() - ui.line_h(FONT_SMALL) * 0.5f, FONT_SMALL, theme.text_faint);
}

void App::draw_sidebar(Rect r) {
    // Brand: symbol + wordmark. No gradient, no glow: the mark is the identity.
    // Brand block: mark inside a soft accent square, wordmark beside it.
    Rect mark{ r.x + SP(16), r.y + SP(18), SP(34), SP(34) };
    ui.rrect(mark.x, mark.y, mark.w, mark.h, SP(10), theme.accent.with_a(0.14f));
    ui.icon_centered(ICON_LOGO_MARK, mark.cx(), mark.cy(), SP(20), theme.accent, 1.9f);
    ui.text(Str("MOBILADOR"), mark.r() + SP(12), mark.y + SP(3), FONT_TITLE, theme.text);
    ui.text(Str("USB STREAMING"), mark.r() + SP(12), mark.y + SP(23), FONT_TINY, theme.text_faint);

    f32 y = r.y + SP(84);
    ui.text("NAVEGACAO", r.x + SP(26), y, FONT_TINY, theme.text_faint);
    y += SP(24);
    for (u32 i = 0; i < SCREEN_COUNT; ++i) {
        Rect item{ r.x + SP(12), y, r.w - SP(24), SP(40) };
        bool active = screen == (ScreenId)i;
        bool hot = item.contains(w.in.mouse_x, w.in.mouse_y);
        const f32 rad = item.h * 0.5f;
        if (active) {
            // Filled accent pill with dark text: the selected item is the only
            // saturated surface on the rail.
            ui.rrect(item.x, item.y, item.w, item.h, rad, theme.accent);
        } else if (hot) {
            ui.rrect(item.x, item.y, item.w, item.h, rad, theme.surface_2);
        }
        Col fg = active ? theme.on_accent : (hot ? theme.text : theme.text_dim);
        ui.icon_centered(kScreenIcons[i], item.x + SP(22), item.cy(), SP(16), fg, 1.7f);
        ui.text(Str(kScreenTitles[i]), item.x + SP(44), item.cy() - ui.line_h(FONT_BODY) * 0.5f,
                active ? FONT_LABEL : FONT_BODY, fg);
        if (w.in.pressed[MOB_MB_LEFT] && hot) {
            screen = (ScreenId)i;
            if (screen == SCREEN_DIAGNOSTICS) run_diagnostics(true);
        }
        y += SP(44);
    }

    // ---- connection block (bottom of the rail)
    Rect cs{ r.x + SP(12), r.b() - SP(148), r.w - SP(24), SP(136) };
    card(&w, cs, true);
    bool ok = adb.have_device();
    status_dot(&w, cs.x + SP(16), cs.y + SP(20), ok ? theme.ok : (adb.state == ADB_NO_DEVICES ? theme.text_faint : theme.err), ok);
    ui.text(ok ? "CELULAR CONECTADO" : "SEM CONEXAO", cs.x + SP(28), cs.y + SP(14), FONT_SMALL,
            ok ? theme.ok : theme.text_dim);
    ui.text_ellipsis(ok ? Str(adb.device()->model) : Str(adb.status_text), cs.x + SP(12), cs.y + SP(38),
                     cs.w - SP(24), FONT_SMALL, theme.text_dim);
    char line[128];
    if (ok) {
        snprintf(line, sizeof(line), "Android %s  -  USB %s",
                 mir_device_android(), adb.device()->usb ? "CONNECTED" : "WIFI (nao recomendado)");
    } else {
        snprintf(line, sizeof(line), "Conecte o cabo e ative a depuracao USB");
    }
    ui.text_wrapped(Str(line), cs.x + SP(12), cs.y + SP(58), cs.w - SP(24), SP(14), FONT_TINY, theme.text_faint, 3);

    Rect btn{ cs.x + SP(12), cs.b() - SP(38), cs.w - SP(24), SP(28) };
    BtnResult b = button(&w, session_running() ? "PARAR" : "INICIAR", btn,
                         session_running() ? BTN_DANGER : BTN_PRIMARY, ICON_POWER);
    if (b.clicked) toggle_session();
}

// Android version as reported by the phone itself (empty until it answers).
const char* App::mir_device_android() const {
    return mirror.device.valid ? mirror.device.android : "-";
}

void App::draw_topbar(Rect r) {
    ui.text(Str(kScreenTitles[screen]), r.x + SP(22), r.cy() - ui.line_h(FONT_H1) * 0.5f, FONT_H1, theme.text);

    f32 right = r.r() - SP(18);
    // Game mode switch (the single most used control, so it lives in the bar)
    Rect gm{ right - SP(150), r.y + SP(13), SP(150), SP(32) };
    BtnResult g = button(&w, game_mode ? "GAME MODE ON" : "GAME MODE", gm,
                         game_mode ? BTN_PRIMARY : BTN_SECONDARY, ICON_GAMEPAD);
    if (g.clicked) set_game_mode(!game_mode);
    right -= SP(162);

    // Live readouts as chips, so they read as data instead of stray text. Only
    // measured values appear: nothing here is invented.
    char buf[64];
    {
        snprintf(buf, sizeof(buf), "%.0f FPS", sampler.fps.display_fps);
        f32 w1 = ui.measure(Str(buf), FONT_MONO_BOLD) + SP(26);
        snprintf(buf, sizeof(buf), "%.1f ms", sampler.metrics[MET_TOTAL_US].last / 1000.0f);
        f32 w2 = ui.measure(Str(buf), FONT_MONO) + SP(26);
        const f32 ch = SP(30);
        Rect chip2{ right - w2, r.cy() - ch * 0.5f, w2, ch };
        ui.rrect(chip2.x, chip2.y, chip2.w, chip2.h, ch * 0.5f, theme.surface_2);
        snprintf(buf, sizeof(buf), "%.1f ms", sampler.metrics[MET_TOTAL_US].last / 1000.0f);
        ui.text(Str(buf), chip2.cx(), chip2.cy() - ui.line_h(FONT_MONO) * 0.5f, FONT_MONO,
                theme.text_dim, ALIGN_CENTER);
        Rect chip1{ chip2.x - SP(8) - w1, chip2.y, w1, ch };
        ui.rrect(chip1.x, chip1.y, chip1.w, chip1.h, ch * 0.5f,
                 theme.accent.with_a(0.14f));
        snprintf(buf, sizeof(buf), "%.0f FPS", sampler.fps.display_fps);
        ui.text(Str(buf), chip1.cx(), chip1.cy() - ui.line_h(FONT_MONO_BOLD) * 0.5f, FONT_MONO_BOLD,
                theme.accent, ALIGN_CENTER);
        right = chip1.x - SP(12);
    }

    bool ok = session_running();
    badge(&w, ok ? "USB" : "IDLE", right - SP(54), r.cy() - SP(10), ok ? theme.ok : theme.text_faint);
}

// ===========================================================================
//  DASHBOARD
// ===========================================================================
f32 App::draw_dashboard(Rect r) {
    const f32 gap = SP(16);
    f32 half = (r.w - gap) * 0.5f;

    // ---- status card
    Rect st{ r.x, r.y, r.w, SP(112) };
    card(&w, st, true);
    bool dev = adb.have_device();
    bool live = session_running();
    Col scol = live ? theme.ok : (dev ? theme.warn : theme.text_faint);
    status_dot(&w, st.x + SP(26), st.y + SP(32), scol, live);
    ui.text(live ? "CELULAR CONECTADO - TRANSMITINDO" : (dev ? "CELULAR CONECTADO" : "AGUARDANDO O CELULAR"),
            st.x + SP(44), st.y + SP(20), FONT_TITLE, theme.text);
    ui.text_ellipsis(Str(mirror.session_state_text()), st.x + SP(44), st.y + SP(44), st.w - SP(70),
                     FONT_SMALL, theme.text_dim);
    ui.text(Str(dev ? adb.status_text : "Conecte o celular por USB e autorize a depuracao"), st.x + SP(44),
            st.y + SP(64), FONT_SMALL, theme.text_faint);

    Rect start{ st.x + SP(16), st.b() - SP(52), SP(260), SP(38) };
    BtnResult s = button_big(&w, live ? "PARAR SESSAO" : "START FREE FIRE", ICON_PLAY, start,
                             live ? BTN_DANGER : BTN_PRIMARY, !dev && !live, Str("USB"));
    if (s.clicked) {
        if (live) stop_session();
        else { start_session(); if (session_running()) set_game_mode(true); }
    }
    if (!dev) tooltip(&w, w_id(Str("starthint")), "O botao fica disponivel quando um celular USB autorizado e detectado.",
                      s.hovered);

    // ---- quick stats grid
    char v1[32], v2[32], v3[32], v4[32], v5[32], v6[32];
    snprintf(v1, sizeof(v1), "%.0f", sampler.fps.source_fps);
    snprintf(v2, sizeof(v2), "%.0f", sampler.fps.stream_fps);
    snprintf(v3, sizeof(v3), "%.0f", sampler.fps.display_fps);
    snprintf(v4, sizeof(v4), "%.1f", sampler.metrics[MET_TOTAL_US].last / 1000.0f);
    snprintf(v5, sizeof(v5), "%.0f", sampler.sys.gpu_percent);
    snprintf(v6, sizeof(v6), "%.0f", sampler.sys.cpu_percent);

    // One card, six compartments, hairline separators: the metric block of the
    // product reference. The numbers carry the weight, the captions recede.
    MetricCell cells[6] = {
        { "FPS ORIGEM",  Str(v1), Str(""),   ICON_FPS,     theme.text },
        { "FPS STREAM",  Str(v2), Str(""),   ICON_USB,     theme.text },
        { "FPS DISPLAY", Str(v3), Str(""),   ICON_MONITOR, theme.text },
        { "LATENCIA",    Str(v4), Str("ms"), ICON_LATENCY, theme.accent },
        { "GPU",         Str(v5), Str("%"),  ICON_GPU,     theme.text },
        { "CPU",         Str(v6), Str("%"),  ICON_CPU,     theme.text },
    };
    // Two rows of three compartments inside one card. The geometry comes from
    // metric_grid_height(), never from the two-column half width: deriving the
    // tile size from `half` (the bug in 1.0.0-1.0.5) pushed two of every three
    // tiles past the right edge of the window.
    const f32 tiles_top = r.y + SP(112) + gap;
    Rect grid{ r.x, tiles_top, r.w, metric_grid_height(&w, 6, 3) };
    metric_grid(&w, grid, cells, 6, 3);

    // ---- device information + pipeline
    f32 ly = grid.b() + gap;
    Rect dc{ r.x, ly, half, SP(226) };
    card(&w, dc);
    section_header(&w, "CELULAR", Rect{ dc.x + SP(16), dc.y + SP(10), dc.w - SP(32), SP(26) }, ICON_PHONE);
    const DeviceReport& d = mirror.device;
    char v[8][64];
    snprintf(v[0], sizeof(v[0]), "%s", dev ? adb.device()->model : "-");
    snprintf(v[1], sizeof(v[1]), "%s", d.valid ? d.android : "-");
    snprintf(v[2], sizeof(v[2]), "%s", dev && adb.device()->usb ? "CONNECTED" : "-");
    snprintf(v[3], sizeof(v[3]), "%ux%u", d.display_w, d.display_h);
    snprintf(v[4], sizeof(v[4]), "%.0f Hz", d.valid ? d.refresh_hz : 0.0f);
    snprintf(v[5], sizeof(v[5]), "%u fps", d.valid ? d.max_fps_hint : 0);
    snprintf(v[6], sizeof(v[6]), "%s", d.abi);
    snprintf(v[7], sizeof(v[7]), "%s", d.input_backend[0] ? d.input_backend : "-");
    const char* keys[8] = { "Modelo", "Android", "USB", "Tela", "Taxa", "FPS maximo", "ABI", "Entrada" };
    for (u32 i = 0; i < 8; ++i) {
        Rect row{ dc.x + SP(16), dc.y + SP(42) + SP(20) * (f32)i, dc.w - SP(32), SP(18) };
        kv_row(&w, Str(keys[i]), Str(v[i]), row, i == 2 ? (dev && adb.device()->usb ? theme.ok : theme.warn) : Col(-1, -1, -1, -1));
    }

    Rect pc{ r.x + half + gap, ly, half, SP(226) };
    card(&w, pc);
    section_header(&w, "PIPELINE", Rect{ pc.x + SP(16), pc.y + SP(10), pc.w - SP(32), SP(26) }, ICON_LAYERS);
    struct { const char* k; MetricId m; } rows[6] = {
        { "Captura", MET_CAPTURE_US }, { "Encoder", MET_ENCODE_US }, { "USB", MET_USB_US },
        { "Decode", MET_DECODE_US },   { "Render", MET_RENDER_US }, { "Entrada (RTT/2)", MET_INPUT_US },
    };
    for (u32 i = 0; i < 6; ++i) {
        const MetricSeries& ms = sampler.metrics[rows[i].m];
        char val[48];
        snprintf(val, sizeof(val), "%.2f ms  (p95 %.2f)", ms.last / 1000.0f, ms.p95 / 1000.0f);
        Rect row{ pc.x + SP(16), pc.y + SP(44) + SP(26) * (f32)i, pc.w - SP(32), SP(20) };
        kv_row(&w, Str(rows[i].k), Str(val), row);
    }
    char fpsline[96];
    snprintf(fpsline, sizeof(fpsline), "buffer %u  -  drops %.1f/s  -  %.1f Mbps",
             settings.buffer_size, sampler.fps.dropped_fps, sampler.bitrate_mbps);
    ui.text(Str(fpsline), pc.x + SP(16), pc.b() - SP(24), FONT_SMALL, theme.text_faint);

    // ---- secondary actions
    f32 ay = ly + SP(238);
    const f32 bw = (r.w - gap * 3) / 4.0f;
    const char* labels[4] = { "DESEMPENHO", "LATENCIA", "CONFIGURACOES", "DIAGNOSTICO" };
    IconId icons[4] = { ICON_PERFORMANCE, ICON_LATENCY, ICON_SETTINGS, ICON_DIAGNOSTICS };
    ScreenId targets[4] = { SCREEN_PERFORMANCE, SCREEN_LATENCY, SCREEN_SETTINGS, SCREEN_DIAGNOSTICS };
    for (u32 i = 0; i < 4; ++i) {
        Rect b{ r.x + (f32)i * (bw + gap), ay, bw, SP(40) };
        if (button(&w, Str(labels[i]), b, BTN_SECONDARY, icons[i]).clicked) {
            screen = targets[i];
            if (screen == SCREEN_DIAGNOSTICS) run_diagnostics(true);
        }
    }

    // ---- differential features
    Rect fx{ r.x, ay + SP(52), r.w, SP(168) };
    card(&w, fx);
    section_header(&w, "RECURSOS RAPIDOS", Rect{ fx.x + SP(16), fx.y + SP(10), fx.w - SP(32), SP(26) }, ICON_BOLT);
    Rect q1{ fx.x + SP(16), fx.y + SP(46), (fx.w - SP(48)) * 0.5f, SP(44) };
    Rect q2{ q1.r() + SP(16), q1.y, q1.w, q1.h };
    Rect q3{ q1.x, q1.b() + SP(10), q1.w, q1.h };
    Rect q4{ q2.x, q3.y, q2.w, q3.h };
    // Both actions change the settings the next (or current) session uses and
    // report the resulting numbers, instead of only re-reading diagnostics.
    if (button_big(&w, "QUICK PERFORMANCE", ICON_BOLT, q1, BTN_PRIMARY, true,
                   Str("preset de menor latencia")).clicked) {
        settings.apply_preset(PRESET_ULTRA_LOW_LATENCY);
        Settings::Caps caps;
        collect_caps(&caps);
        settings.hardware_acceleration = caps.hardware_decode;
        settings.gpu_decoder = caps.hardware_decode;
        settings.vsync = false;
        settings.frame_pacing = false;
        on_settings_changed("quick_performance");
        char msg[176];
        char fps_txt[16];
        if (settings.target_fps) snprintf(fps_txt, sizeof(fps_txt), "%u fps", settings.target_fps);
        else snprintf(fps_txt, sizeof(fps_txt), "MAX FPS");
        snprintf(msg, sizeof(msg), "QUICK PERFORMANCE: %ux%u @ %s - decode %s",
                 settings.resolution_w, settings.resolution_h, fps_txt,
                 settings.gpu_decoder ? "na GPU" : "na CPU");
        w.toast(msg, theme.ok, ICON_BOLT, 4.0f);
        MOB_INFO("%s", msg);
    }
    if (button_big(&w, "AUTO OPTIMIZE", ICON_WAND, q2, BTN_SECONDARY, true,
                   Str("analise do dispositivo + PC")).clicked) {
        apply_auto_optimize(true);
    }
    if (button_big(&w, "BENCHMARK", ICON_BENCHMARK, q3, BTN_SECONDARY, !session_running(),
                   Str("compara configuracoes reais")).clicked) {
        screen = SCREEN_BENCHMARK;
    }
    if (button_big(&w, "LOGS / RELATORIO", ICON_LIST, q4, BTN_SECONDARY, true,
                   Str("performance log + diagnostico")).clicked) {
        show_log = !show_log;
    }
    return fx.b() - r.y;
}

// ===========================================================================
//  PERFORMANCE
// ===========================================================================
f32 App::draw_performance(Rect r) {
    const f32 gap = SP(16);
    f32 y = r.y;

    // ---- live strip: what the pipeline is doing right now
    char p1[32], p2[32], p3[32], p4[32];
    snprintf(p1, sizeof(p1), "%.0f", sampler.fps.display_fps);
    snprintf(p2, sizeof(p2), "%.1f", sampler.metrics[MET_TOTAL_US].last / 1000.0f);
    snprintf(p3, sizeof(p3), "%.0f", sampler.sys.gpu_percent);
    snprintf(p4, sizeof(p4), "%.0f", sampler.sys.cpu_percent);
    MetricCell pcells[4] = {
        { "FPS DISPLAY", Str(p1), Str(""),   ICON_MONITOR, theme.text },
        { "LATENCIA",    Str(p2), Str("ms"), ICON_LATENCY, theme.accent },
        { "GPU",         Str(p3), Str("%"),  ICON_GPU,     theme.text },
        { "CPU",         Str(p4), Str("%"),  ICON_CPU,     theme.text },
    };
    Rect pgr{ r.x, y, r.w, metric_grid_height(&w, 4, 4) };
    metric_grid(&w, pgr, pcells, 4, 4);
    y = pgr.b() + gap;

    // ---- presets
    Rect pr{ r.x, y, r.w, SP(96) };
    card(&w, pr, true);
    section_header(&w, "PRESETS", Rect{ pr.x + SP(16), pr.y + SP(10), pr.w - SP(32), SP(26) }, ICON_BOLT);
    const char* names[5] = { "ULTRA LOW LATENCY", "MAX FPS", "BALANCED", "QUALITY", "CUSTOM" };
    const char* hints[5] = { "menor latencia possivel", "maior taxa estavel", "equilibrio",
                             "melhor imagem", "ajustes manuais" };
    const f32 bw = (pr.w - SP(32) - gap * 4) / 5.0f;
    for (u32 i = 0; i < 5; ++i) {
        Rect b{ pr.x + SP(16) + (f32)i * (bw + gap), pr.y + SP(44), bw, SP(40) };
        BtnKind k = (settings.preset == (PresetId)i) ? BTN_PRIMARY : BTN_SECONDARY;
        if (button_big(&w, Str(names[i]), ICON_NONE, b, k, true).clicked) {
            settings.apply_preset((PresetId)i);
            on_settings_changed("preset");
            w.toast("Preset aplicado", theme.ok, ICON_CHECK, 2.0f);
        }
        ui.text_ellipsis(Str(hints[i]), b.x, b.b() + SP(2), b.w, FONT_TINY, theme.text_faint);
    }
    y = pr.b() + gap;

    // ---- automatic actions
    Rect au{ r.x, y, r.w, SP(64) };
    card(&w, au);
    Rect a1{ au.x + SP(16), au.y + SP(12), (au.w - SP(48)) * 0.5f, SP(40) };
    Rect a2{ a1.r() + SP(16), a1.y, a1.w, a1.h };
    if (button(&w, "AUTO OPTIMIZE", a1, BTN_SECONDARY, ICON_WAND).clicked) {
        // Runs the real optimiser (resolution, fps, bitrate, codec, buffers,
        // decode path) and applies the result to the live session.
        apply_auto_optimize(true);
    }
    if (button(&w, "RESTAURAR PADROES", a2, BTN_GHOST, ICON_RESTORE).clicked) {
        settings.apply_preset(PRESET_BALANCED);
        on_settings_changed("reset");
        w.toast("Padroes restaurados", theme.info, ICON_RESTORE, 2.5f);
    }
    y = au.b() + gap;

    // ---- groups
    const f32 row_h = SP(30);
    const f32 desc_h = SP(30);
    auto group_tab = [&](const char* label, PerfGroup g, f32 x, f32 width) {
        Rect b{ x, y, width, SP(34) };
        BtnKind k = perf_group == g ? BTN_PRIMARY : BTN_GHOST;
        if (button(&w, Str(label), b, k, ICON_NONE).clicked) perf_group = g;
    };
    const f32 gw = (r.w - gap * 2) / 3.0f;
    group_tab("CAPTURA E ENCODER", PERF_GROUP_CAPTURE, r.x, gw);
    group_tab("PIPELINE E RENDER", PERF_GROUP_PIPELINE, r.x + gw + gap, gw);
    group_tab("ENTRADA E JANELA", PERF_GROUP_INPUT, r.x + (gw + gap) * 2, gw);
    y += SP(46);

    Rect body{ r.x, y, r.w, 0 };
    body.h = SP(560);
    card(&w, body);

    f32 ry = body.y + SP(14);
    const f32 rx = body.x + SP(16);
    const f32 rw = body.w - SP(32);
    const f32 label_w = SP(230);

    auto row_rect = [&]() { Rect rr{ rx, ry, rw, row_h }; ry += row_h + desc_h + SP(6); return rr; };

    if (perf_group == PERF_GROUP_CAPTURE) {
        // Resolution
        {
            Rect rr = row_rect();
            static const char* res_items[6] = { "1280x720", "1600x900", "1920x1080", "2560x1440", "Auto (tela do celular)", "Original" };
            u32 idx = 2;
            u32 w_ = settings.resolution_w;
            if (w_ == 1280) idx = 0; else if (w_ == 1600) idx = 1; else if (w_ == 1920) idx = 2;
            else if (w_ == 2560) idx = 3;
            ui.text("Resolucao", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
            Rect box{ rr.x + label_w, rr.y, SP(300), rr.h - SP(6) };
            if (dropdown(&w, Str("res"), box, res_items, 4, &idx, nullptr)) {
                const u32 dims[4][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 } };
                if (idx < 4) {
                    settings.resolution_w = dims[idx][0];
                    settings.resolution_h = dims[idx][1];
                    settings.preset = PRESET_CUSTOM;
                    on_settings_changed("resolution");
                }
            }
            help_line("resolution", rr.x, rr.b() - desc_h + SP(2), rr.w);
        }
        // FPS
        {
            Rect rr = row_rect();
            static const char* fps_items[6] = { "30", "60", "90", "120", "144", "MAX FPS" };
            u32 idx = 1;
            switch (settings.target_fps) { case 30: idx = 0; break; case 60: idx = 1; break; case 90: idx = 2; break;
                case 120: idx = 3; break; case 144: idx = 4; break; default: idx = 5; break; }
            ui.text("FPS alvo", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
            Rect box{ rr.x + label_w, rr.y, SP(300), rr.h - SP(6) };
            if (segmented(&w, Str("fps"), box, fps_items, 6, &idx)) {
                const u32 vals[6] = { 30, 60, 90, 120, 144, 0 };
                settings.target_fps = vals[idx];
                settings.preset = PRESET_CUSTOM;
                on_settings_changed("fps");
            }
            help_line("fps", rr.x, rr.b() - desc_h + SP(2), rr.w);
        }
        // Bitrate
        {
            Rect rr = row_rect();
            ui.text("Bitrate", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
            Rect box{ rr.x + label_w, rr.y, SP(420), rr.h - SP(6) };
            f32 mbps = (f32)settings.bitrate_kbps / 1000.0f;
            if (slider(&w, Str("bitrate"), box, &mbps, 2.0f, 50.0f, 0.5f, "%.1f Mbps")) {
                settings.bitrate_kbps = (u32)(mbps * 1000.0f);
                settings.preset = PRESET_CUSTOM;
                on_settings_changed("bitrate");
            }
            help_line("bitrate", rr.x, rr.b() - desc_h + SP(2), rr.w);
        }
        // Codec
        {
            Rect rr = row_rect();
            static const char* codecs[3] = { "H.264 (mais compativel)", "H.265 / HEVC (mais eficiente)", "AV1" };
            u32 idx = (u32)settings.codec;
            ui.text("Codec", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
            Rect box{ rr.x + label_w, rr.y, SP(420), rr.h - SP(6) };
            if (dropdown(&w, Str("codec"), box, codecs, 3, &idx, nullptr)) {
                settings.codec = (Codec)idx;
                settings.preset = PRESET_CUSTOM;
                on_settings_changed("codec");
            }
            help_line("codec", rr.x, rr.b() - desc_h + SP(2), rr.w);
        }
        // Quality / color format
        {
            Rect rr = row_rect();
            static const char* q[4] = { "PERFORMANCE", "BALANCED", "HIGH", "MAX" };
            u32 idx = (u32)settings.quality;
            ui.text("Qualidade do encoder", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
            Rect box{ rr.x + label_w, rr.y, SP(420), rr.h - SP(6) };
            if (segmented(&w, Str("quality"), box, q, 4, &idx)) {
                settings.quality = (QualityLevel)idx;
                settings.preset = PRESET_CUSTOM;
                on_settings_changed("quality");
            }
            help_line("quality", rr.x, rr.b() - desc_h + SP(2), rr.w);
        }
        {
            Rect rr = row_rect();
            static const char* cf[3] = { "YUV 4:2:0", "YUV 4:4:4", "NV12" };
            u32 idx = (u32)settings.color_format;
            ui.text("Formato de cor", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
            Rect box{ rr.x + label_w, rr.y, SP(420), rr.h - SP(6) };
            if (segmented(&w, Str("colorfmt"), box, cf, 3, &idx)) {
                settings.color_format = (ColorFormatId)idx;
                on_settings_changed("color_format");
            }
            help_line("color_format", rr.x, rr.b() - desc_h + SP(2), rr.w);
        }
    } else if (perf_group == PERF_GROUP_PIPELINE) {
        {
            Rect rr = row_rect();
            ui.text("Buffer de frames", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
            Rect box{ rr.x + label_w, rr.y, SP(420), rr.h - SP(6) };
            f32 v = (f32)settings.buffer_size;
            if (slider(&w, Str("buffer"), box, &v, 1, 4, 1, "%.0f")) {
                settings.buffer_size = (u32)(v + 0.5f);
                on_settings_changed("buffer_size");
            }
            help_line("buffer_size", rr.x, rr.b() - desc_h + SP(2), rr.w);
        }
        { Rect rr = row_rect(); setting_row_toggle("hardware_acceleration", "Aceleracao por hardware", &settings.hardware_acceleration, rr); }
        { Rect rr = row_rect(); setting_row_toggle("gpu_decoder", "Decodificador na GPU", &settings.gpu_decoder, rr); }
        { Rect rr = row_rect(); setting_row_toggle("vsync", "VSync", &settings.vsync, rr); }
        { Rect rr = row_rect(); setting_row_toggle("frame_pacing", "Frame Pacing", &settings.frame_pacing, rr); }
        { Rect rr = row_rect(); setting_row_toggle("allow_frame_dropping", "Permitir descarte de frames", &settings.allow_frame_dropping, rr); }
        { Rect rr = row_rect(); setting_row_toggle("latest_frame_priority", "Priorizar frame mais recente", &settings.latest_frame_priority, rr); }
        { Rect rr = row_rect(); setting_row_toggle("low_latency_render", "Renderizacao de baixa latencia", &settings.low_latency_render, rr); }
        {
            Rect rr = row_rect();
            static const char* rm[3] = { "DIRECT (1 quad)", "SHARPEN (+CAS)", "QUALITY" };
            u32 idx = (u32)settings.render_mode;
            ui.text("Modo de render", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
            Rect box{ rr.x + label_w, rr.y, SP(420), rr.h - SP(6) };
            if (segmented(&w, Str("rendermode"), box, rm, 3, &idx)) {
                settings.render_mode = (RenderModeId)idx;
                on_settings_changed("render_mode");
            }
            help_line("render_mode", rr.x, rr.b() - desc_h + SP(2), rr.w);
        }
        {
            Rect rr = row_rect();
            static const char* sm[4] = { "ASPECT (sem distorcao)", "FILL", "INTEGER", "1:1" };
            u32 idx = (u32)settings.scaling_mode;
            ui.text("Escalonamento", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
            Rect box{ rr.x + label_w, rr.y, SP(420), rr.h - SP(6) };
            if (segmented(&w, Str("scaling"), box, sm, 4, &idx)) {
                settings.scaling_mode = (ScalingModeId)idx;
                on_settings_changed("scaling_mode");
            }
            help_line("scaling_mode", rr.x, rr.b() - desc_h + SP(2), rr.w);
        }
    } else {
        { Rect rr = row_rect(); setting_row_toggle("mouse_capture", "Captura do mouse", &settings.mouse_capture, rr); }
        { Rect rr = row_rect(); setting_row_toggle("input_priority", "Prioridade de entrada", &settings.input_priority, rr); }
        { Rect rr = row_rect(); setting_row_toggle("keyboard_passthrough", "Teclado para o celular", &settings.keyboard_passthrough, rr); }
        {
            Rect rr = row_rect();
            ui.text("Sensibilidade do mouse", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
            Rect box{ rr.x + label_w, rr.y, SP(420), rr.h - SP(6) };
            if (slider(&w, Str("sens"), box, &settings.mouse_sensitivity, 0.2f, 3.0f, 0.05f, "%.2fx")) {
                on_settings_changed("sensitivity");
            }
            ui.text("Ajuste fino; o mapeamento pertence ao GG Mouse Pro 3", rr.x, rr.b() - desc_h + SP(4),
                    FONT_SMALL, theme.text_faint);
        }
        {
            Rect rr = row_rect();
            static const char* um[3] = { "FAST", "SAFE", "AUTO" };
            u32 idx = (u32)settings.usb_mode;
            ui.text("Modo USB", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
            Rect box{ rr.x + label_w, rr.y, SP(420), rr.h - SP(6) };
            if (segmented(&w, Str("usbmode"), box, um, 3, &idx)) {
                settings.usb_mode = (UsbMode)idx;
                on_settings_changed("usb_mode");
            }
            help_line("usb_mode", rr.x, rr.b() - desc_h + SP(2), rr.w);
        }
        { Rect rr = row_rect(); setting_row_toggle("auto_fullscreen", "Tela cheia automatica", &settings.auto_fullscreen, rr); }
        { Rect rr = row_rect(); setting_row_toggle("auto_hide_cursor", "Ocultar cursor no jogo", &settings.auto_hide_cursor, rr); }
        { Rect rr = row_rect(); setting_row_toggle("reconnect_auto", "Reconexao automatica", &settings.reconnect_auto, rr); }
        { Rect rr = row_rect(); setting_row_toggle("keep_screen_awake", "Manter tela do celular acesa", &settings.keep_screen_awake, rr); }
    }

    Rect apply{ r.x, body.b() + SP(14), r.w, SP(46) };
    card(&w, apply);
    ui.text("As alteracoes sao aplicadas imediatamente e salvas automaticamente.",
            apply.x + SP(16), apply.cy() - ui.line_h(FONT_SMALL) * 0.5f, FONT_SMALL, theme.text_dim);
    Rect sb{ apply.r() - SP(220), apply.y + SP(8), SP(204), SP(30) };
    if (button(&w, "SALVAR PERFIL", sb, BTN_SECONDARY, ICON_SAVE).clicked) {
        if (paths) {
            settings.save_profile(paths->profile_dir, Str("Perfil"));
            w.toast("Perfil salvo", theme.ok, ICON_SAVE, 3.0f);
        }
    }

    // Height actually used, so the shell can size the scroll range.
    return apply.b() - r.y;
}

// ===========================================================================
//  LATENCY ANALYZER
// ===========================================================================
f32 App::draw_latency(Rect r) {
    const f32 gap = SP(16);
    f32 y = r.y;

    // ---- stage table with percentiles
    Rect tb{ r.x, y, r.w, SP(300) };
    card(&w, tb, true);
    section_header(&w, "LATENCY ANALYZER", Rect{ tb.x + SP(16), tb.y + SP(10), tb.w - SP(32), SP(26) }, ICON_LATENCY);
    const char* hdr[6] = { "ESTAGIO", "ATUAL", "MEDIA", "p50", "p95", "p99" };
    f32 cx[6] = { tb.x + SP(18), tb.x + SP(240), tb.x + SP(360), tb.x + SP(470), tb.x + SP(580), tb.x + SP(690) };
    for (u32 i = 0; i < 6; ++i) {
        ui.text(Str(hdr[i]), cx[i], tb.y + SP(46), FONT_TINY, theme.text_faint);
    }
    struct { const char* k; MetricId m; IconId ic; } rows[7] = {
        { "Captura -> chegada", MET_CAPTURE_US, ICON_SCREEN },
        { "Encoder (celular)",  MET_ENCODE_US,  ICON_BOLT },
        { "USB / transporte",   MET_USB_US,     ICON_USB },
        { "Decode",             MET_DECODE_US,  ICON_GPU },
        { "Render",             MET_RENDER_US,  ICON_MONITOR },
        { "Entrada (RTT/2)",    MET_INPUT_US,   ICON_MOUSE },
        { "TOTAL ESTIMADO",     MET_TOTAL_US,   ICON_LATENCY },
    };
    for (u32 i = 0; i < 7; ++i) {
        const MetricSeries& ms = sampler.metrics[rows[i].m];
        f32 ry = tb.y + SP(68) + SP(30) * (f32)i;
        bool total = rows[i].m == MET_TOTAL_US;
        if (total) {
            ui.rect(tb.x + SP(12), ry - SP(4), tb.w - SP(24), SP(26), theme.accent_soft(0.12f));
        }
        ui.icon(rows[i].ic, tb.x + SP(18), ry + SP(3), SP(14), total ? theme.accent : theme.text_dim);
        ui.text(Str(rows[i].k), tb.x + SP(40), ry + SP(1), total ? FONT_MONO_BOLD : FONT_MONO,
                total ? theme.text : theme.text_dim);
        char v[32];
        const f32 vals[4] = { ms.last, ms.avg(), ms.p50, ms.p95 };
        for (u32 k = 0; k < 4; ++k) {
            snprintf(v, sizeof(v), "%.2f ms", vals[k] / 1000.0f);
            ui.text(Str(v), cx[k + 1], ry + SP(1), FONT_MONO, k == 0 ? (total ? theme.accent : theme.text) : theme.text_dim);
        }
        snprintf(v, sizeof(v), "%.2f ms", ms.p99 / 1000.0f);
        ui.text(Str(v), cx[5], ry + SP(1), FONT_MONO, ms.p99 > 40000 ? theme.warn : theme.text_dim);
    }
    ui.text("Captura e medida com o relogio do celular reconstruido no PC (offset por RTT minimo).",
            tb.x + SP(16), tb.b() - SP(22), FONT_TINY, theme.text_faint);
    y = tb.b() + gap;

    // ---- graphs
    Rect g{ r.x, y, r.w, SP(170) };
    card(&w, g);
    section_header(&w, "HISTORICO", Rect{ g.x + SP(16), g.y + SP(10), g.w - SP(32), SP(26) }, ICON_CHART);
    Rect g1{ g.x + SP(16), g.y + SP(44), (g.w - SP(48)) * 0.5f, SP(110) };
    Rect g2{ g1.r() + SP(16), g1.y, g1.w, g1.h };
    graph(&w, sampler.metrics[MET_TOTAL_US].samples, MOB_GRAPH_RING,
          sampler.metrics[MET_TOTAL_US].head, g1, theme.chart_line, 0, 60.0f, "ms", true);
    graph(&w, sampler.metrics[MET_USB_US].samples, MOB_GRAPH_RING,
          sampler.metrics[MET_USB_US].head, g2, theme.accent, 0, 40.0f, "ms", true);
    y = g.b() + gap;

    // ---- counters and resources
    Rect c{ r.x, y, r.w, SP(150) };
    card(&w, c);
    section_header(&w, "CONTADORES E RECURSOS", Rect{ c.x + SP(16), c.y + SP(10), c.w - SP(32), SP(26) }, ICON_ACTIVITY);
    char v[12][48];
    snprintf(v[0], sizeof(v[0]), "%.2f ms", sampler.fps.frame_time_ms);
    snprintf(v[1], sizeof(v[1]), "%.1f /s", sampler.fps.dropped_fps);
    snprintf(v[2], sizeof(v[2]), "%u frames", settings.buffer_size);
    snprintf(v[3], sizeof(v[3]), "%.1f Mbps", sampler.bitrate_mbps);
    snprintf(v[4], sizeof(v[4]), "%ux%u", mirror.stream_w, mirror.stream_h);
    snprintf(v[5], sizeof(v[5]), "%.0f %%", sampler.sys.cpu_percent);
    snprintf(v[6], sizeof(v[6]), "%.0f %%", sampler.sys.gpu_percent);
    snprintf(v[7], sizeof(v[7]), "%.0f %%", sampler.sys.ram_percent);
    snprintf(v[8], sizeof(v[8]), "%u", (u32)mirror.frames_overwritten());
    snprintf(v[9], sizeof(v[9]), "%.0f /s", sampler.fps.input_hz);
    const char* k[10] = { "Frame time", "Drops", "Buffer", "Bitrate", "Resolucao",
                          "CPU", "GPU", "RAM", "Frames substituidos", "Eventos de entrada/s" };
    const f32 col_w = c.w / 2 - SP(24);
    for (u32 i = 0; i < 10; ++i) {
        u32 col = i / 5, row = i % 5;
        Rect rowr{ c.x + SP(16) + (f32)col * (col_w + SP(16)), c.y + SP(44) + SP(20) * (f32)row, col_w, SP(18) };
        kv_row(&w, Str(k[i]), Str(v[i]), rowr);
    }
    y = c.b() + gap;

    Rect act{ r.x, y, r.w, SP(52) };
    card(&w, act);
    Rect b1{ act.x + SP(16), act.y + SP(11), SP(220), SP(30) };
    Rect b2{ b1.r() + SP(12), b1.y, SP(220), SP(30) };
    Rect b3{ b2.r() + SP(12), b1.y, SP(220), SP(30) };
    if (button(&w, "EXPORTAR CSV", b1, BTN_SECONDARY, ICON_UPLOAD).clicked) {
        Arena a; a.init(1 << 18);
        StrBuilder sb; sb.init(&a, 4096);
        sb.append("tempo_s,origem_fps,stream_fps,display_fps,total_us,captura_us,decode_us,render_us,entrada_us,drops,bitrate_mbps,cpu,gpu,ram\n");
        SessionLogSample snap[256];
        u32 n = sampler.history_snapshot(snap, 256);
        for (u32 i = 0; i < n; ++i) {
            const SessionLogSample& s = snap[i];
            sb.append_fmt("%.2f,%.1f,%.1f,%.1f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.2f,%.0f,%.0f,%.0f\n",
                          s.t_ms / 1000.0f, s.source_fps, s.stream_fps, s.display_fps, s.latency_total_us,
                          s.latency_capture_us, s.latency_decode_us, s.latency_render_us, s.latency_input_us,
                          s.dropped, s.bitrate_mbps, s.cpu, s.gpu, s.ram);
        }
        if (n) write_text_file("latencia.csv", sb.str());
        else w.toast("Nada medido ainda - inicie a sessao", theme.warn, ICON_INFO, 3.0f);
        a.shutdown();
    }
    if (button(&w, "RESETAR MEDICOES", b2, BTN_GHOST, ICON_RESET).clicked) {
        sampler.reset_metrics();
        w.toast("Medicoes zeradas", theme.info, ICON_RESET, 2.0f);
    }
    if (button(&w, "BENCHMARK", b3, BTN_SECONDARY, ICON_BENCHMARK).clicked) screen = SCREEN_BENCHMARK;

    // Height actually used, so the shell can size the scroll range.
    return act.b() - r.y;
}

// ===========================================================================
//  BENCHMARK
// ===========================================================================
f32 App::draw_benchmark(Rect r) {
    const f32 gap = SP(16);
    Rect hdr{ r.x, r.y, r.w, SP(104) };
    card(&w, hdr, true);
    section_header(&w, "MOBILADOR BENCHMARK", Rect{ hdr.x + SP(16), hdr.y + SP(10), hdr.w - SP(32), SP(26) }, ICON_BENCHMARK);
    ui.text_wrapped(
        "Comparacao objetiva: cada etapa troca a configuracao real da sessao por 8 segundos e mede latencia, "
        "FPS e drops do mesmo jeito que o LATENCY ANALYZER. Nenhum numero e estimado.",
        hdr.x + SP(16), hdr.y + SP(42), hdr.w - SP(32), SP(14), FONT_SMALL, theme.text_dim, 2);
    Rect startb{ hdr.x + SP(16), hdr.b() - SP(44), SP(200), SP(32) };
    Rect stopb{ startb.r() + SP(12), startb.y, SP(160), SP(32) };
    if (button(&w, bench_running ? "EXECUTANDO..." : "INICIAR", startb, BTN_PRIMARY, ICON_PLAY, !bench_running).clicked) bench_start();
    if (button(&w, "CANCELAR", stopb, BTN_GHOST, ICON_STOP, !bench_running).clicked) bench_stop(true);

    f32 y = hdr.b() + gap;
    u32 n = bench_step_count();
    for (u32 i = 0; i < n; ++i) {
        u32 w_ = 0, h_ = 0, fps = 0, br = 0;
        const char* codec = "";
        const char* label = bench_step_info(i, &w_, &h_, &fps, &br, &codec);
        Rect row{ r.x, y + (f32)i * SP(64), r.w, SP(56) };
        bool active = bench_running && bench_step == i;
        card(&w, row, active);
        if (active) ui.rect(row.x, row.y, SP(3), row.h, theme.accent);
        status_dot(&w, row.x + SP(22), row.cy(), i < bench_run_count ? theme.ok : (active ? theme.accent : theme.text_faint), active);
        ui.text(Str(label), row.x + SP(38), row.y + SP(9), FONT_LABEL, theme.text);
        char sub[160];
        snprintf(sub, sizeof(sub), "%u x %u  -  %u fps  -  %u kbps  -  %s", w_, h_, fps, br, codec);
        ui.text(Str(sub), row.x + SP(38), row.y + SP(28), FONT_SMALL, theme.text_faint);
        if (i < bench_run_count) {
            const BatchRun& run = bench_runs[i];
            char res[200];
            snprintf(res, sizeof(res), "%.1f fps media  -  %.1f ms latencia  -  p99 %.1f ms  -  %.1f%% drops",
                     run.avg_fps, run.avg_latency_ms, run.p99_latency_ms, run.dropped_percent);
            ui.text(Str(res), row.x + SP(420), row.cy() - ui.line_h(FONT_MONO) * 0.5f, FONT_MONO, theme.text_dim);
        } else if (active) {
            f32 prog = (f32)((now_us() - bench_step_start_us) / 8000000.0);
            Rect pb{ row.x + SP(420), row.cy() - SP(5), row.w - SP(440), SP(10) };
            ui.meter(pb.x, pb.y, pb.w, pb.h, mob_clamp(prog, 0.0f, 1.0f), theme.surface_3, theme.accent);
        } else {
            ui.text("aguardando", row.x + SP(420), row.cy() - ui.line_h(FONT_SMALL) * 0.5f, FONT_SMALL, theme.text_faint);
        }
    }
    Rect ex{ r.x, y + (f32)n * SP(64) + gap, r.w, SP(52) };
    card(&w, ex);
    Rect b1{ ex.x + SP(16), ex.y + SP(11), SP(240), SP(30) };
    if (button(&w, "EXPORTAR RESULTADOS", b1, BTN_SECONDARY, ICON_SAVE, bench_run_count > 0).clicked) {
        Arena a; a.init(1 << 18);
        StrBuilder sb; sb.init(&a, 2048);
        sb.append("etapa,fps_alvo,fps_media,fps_1pct,latencia_media_ms,latencia_p99_ms,drops_pct,cpu_medio,gpu_medio\n");
        for (u32 i = 0; i < bench_run_count; ++i) {
            const BatchRun& b = bench_runs[i];
            sb.append_fmt("%s,%u,%.1f,%.1f,%.2f,%.2f,%.2f,%.0f,%.0f\n", b.label, b.target_fps, b.avg_fps,
                          b.p1_low_fps, b.avg_latency_ms, b.p99_latency_ms, b.dropped_percent, b.cpu_avg, b.gpu_avg);
        }
        write_text_file("benchmark.csv", sb.str());
        a.shutdown();
    }

    // Height actually used, so the shell can size the scroll range.
    return ex.b() - r.y;
}

void App::bench_start() {
    bench_running = true;
    bench_step = 0;
    bench_run_count = 0;
    bench_step_start_us = now_us();
    bench_step_sample_count = 0;
    for (u32 i = 0; i < 5; ++i) bench_step_samples[i] = 0;
    if (!session_running()) {
        start_session();
        if (!session_running()) {
            bench_running = false;
            w.toast("Benchmark precisa de um celular conectado", theme.err, ICON_WARNING, 4.0f);
            return;
        }
    }
    w.toast("Benchmark iniciado - nao mexa no mouse durante a medicao", theme.accent, ICON_BENCHMARK, 4.0f);
}

void App::bench_tick() {
    if (!bench_running) return;
    u64 now = now_us();
    if (now - bench_step_start_us < 8000000ull) return;

    // ---- store the step that just finished (measured values only)
    u32 w_ = 0, h_ = 0, fps = 0, br = 0;
    const char* codec = "";
    const char* label = bench_step_info(bench_step, &w_, &h_, &fps, &br, &codec);
    if (label && bench_run_count < 16) {
        BatchRun run{};
        snprintf(run.label, sizeof(run.label), "%s", label);
        run.resolution_w = w_; run.resolution_h = h_;
        run.target_fps = fps; run.bitrate_kbps = br;
        snprintf(run.codec, sizeof(run.codec), "%s", codec ? codec : "");
        run.avg_fps = sampler.fps.display_fps;
        run.p1_low_fps = sampler.metrics[MET_TOTAL_US].p95 > 0 ? sampler.fps.display_fps * 0.9f : 0;
        run.avg_latency_ms = sampler.metrics[MET_TOTAL_US].avg() / 1000.0f;
        run.p99_latency_ms = sampler.metrics[MET_TOTAL_US].p99 / 1000.0f;
        run.max_latency_ms = sampler.metrics[MET_TOTAL_US].max / 1000.0f;
        run.dropped_percent = sampler.frames_received.load() > 0
            ? 100.0f * (f32)sampler.frames_dropped.load() / (f32)sampler.frames_received.load() : 0;
        run.cpu_avg = sampler.sys.cpu_percent;
        run.gpu_avg = sampler.sys.gpu_percent;
        run.duration_s = 8.0f;
        run.frames = sampler.frames_presented.load();
        bench_runs[bench_run_count++] = run;
    }

    ++bench_step;
    if (bench_step >= bench_step_count()) {
        bench_stop(false);
        w.toast("Benchmark concluido", theme.ok, ICON_CHECK, 4.0f);
        return;
    }
    // ---- apply the next configuration to the live session
    u32 nw = 0, nh = 0, nfps = 0, nbr = 0;
    const char* ncodec = "";
    bench_step_info(bench_step, &nw, &nh, &nfps, &nbr, &ncodec);
    settings.resolution_w = nw;
    settings.resolution_h = nh;
    settings.target_fps = nfps;
    settings.bitrate_kbps = nbr;
    settings.codec = (ncodec && ncodec[0] == 'H' && ncodec[1] == '.' && ncodec[2] == '2' && ncodec[3] == '6' && ncodec[4] == '5')
                     ? CODEC_H265 : CODEC_H264;
    settings.preset = PRESET_CUSTOM;
    on_settings_changed("benchmark");
    sampler.reset_metrics();
    bench_step_start_us = now_us();
}

void App::bench_stop(bool cancel) {
    if (!bench_running) return;
    bench_running = false;
    if (cancel) w.toast("Benchmark cancelado", theme.warn, ICON_STOP, 3.0f);
}

// ===========================================================================
//  DIAGNOSTICS
// ===========================================================================
void App::run_diagnostics(bool force) {
    u64 now = now_us();
    if (!force && now - diag_checked_us < 900000ull) return;
    diag_checked_us = now;
    u32 n = 0;
    const DeviceReport& d = mirror.device;

    auto add = [&](const char* name, DiagStatus st, Str value, Str detail) {
        if (n >= MOB_DIAG_COUNT) return;
        DiagItem& it = diag[n++];
        snprintf(it.name, sizeof(it.name), "%s", name);
        snprintf(it.value, sizeof(it.value), "%.*s", (int)mob_min(value.n, (u32)sizeof(it.value) - 1), value.p);
        snprintf(it.detail, sizeof(it.detail), "%.*s", (int)mob_min(detail.n, (u32)sizeof(it.detail) - 1), detail.p);
        it.status = st;
    };

    Arena a; a.init(1 << 16);
    // USB
    {
        bool ok = adb.have_device() && adb.device()->usb;
        add("USB", ok ? DIAG_OK : (adb.have_device() ? DIAG_WARN : DIAG_ERR),
            Str(ok ? "conectado" : "ausente"),
            adb.have_device() ? (Str(adb.device()->usb ? "Cabo USB ativo, canal de baixa latencia"
                                                      : "Dispositivo Wi-Fi: latencia maior, use o cabo"))
                              : Str("Conecte o celular e autorize a depuracao USB"));
    }
    // ADB
    {
        Str value = adb.exe[0] ? Str(path_basename(Str(adb.exe))) : Str("nao encontrado");
        add("ADB", adb.exe[0] ? DIAG_OK : DIAG_ERR, value,
            adb.exe[0] ? Str("Servidor ADB disponivel para criar os canais USB")
                       : Str("Instale o platform-tools do Android ou coloque adb.exe em tools\\"));
    }
    // Server module
    {
        bool present = server_module_present;
        add("SERVIDOR NO CELULAR", present ? DIAG_OK : DIAG_WARN,
            Str(present ? "instalado" : "nao instalado"),
            Str(present ? "Modulo de captura e injecao pronto" :
                          "Abra CONFIGURACOES > SERVIDOR e instale o modulo (uma vez por celular)"));
    }
    // GPU
    {
        char v[96];
        snprintf(v, sizeof(v), "%.*s", (int)mob_min((u32)strlen(gfx.gpu.name), 40u), gfx.gpu.name);
        char det[160];
        snprintf(det, sizeof(det), "%llu MB VRAM - feature level 0x%X - %s",
                 (unsigned long long)(gfx.gpu.dedicated_vram / (1024 * 1024)), gfx.feature_level,
                 gfx.mode_name());
        add("GPU", gfx.gpu.is_software ? DIAG_ERR : (gfx.gpu.is_discrete ? DIAG_OK : DIAG_WARN),
            Str(v), Str(det));
    }
    // CPU
    {
        char v[64], det[128];
        snprintf(v, sizeof(v), "%.0f %%", sampler.sys.cpu_percent);
        snprintf(det, sizeof(det), "%u nucleos logicos - fila: %.0f%%",
                 (u32)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), sampler.sys.cpu_percent);
        add("CPU", sampler.sys.cpu_percent > 88.0f ? DIAG_WARN : DIAG_OK, Str(v), Str(det));
    }
    // RAM
    {
        char v[64], det[128];
        snprintf(v, sizeof(v), "%.0f %%", sampler.sys.ram_percent);
        snprintf(det, sizeof(det), "%.0f MB de %.0f MB em uso",
                 sampler.sys.ram_used_mb, sampler.sys.ram_total_mb);
        add("RAM", sampler.sys.ram_percent > 92.0f ? DIAG_WARN : DIAG_OK, Str(v), Str(det));
    }
    // Decoder
    {
        bool ready = mirror.decoder_ready;
        char v[96], det[192];
        snprintf(v, sizeof(v), "%.*s", (int)mob_min((u32)strlen(mirror.decoder.backend_name), 28u),
                 mirror.decoder.backend_name);
        snprintf(det, sizeof(det), "%s", ready ? mirror.decoder.decoder_name : "sem sessao ativa");
        DiagStatus st = !ready ? DIAG_WARN
                       : (mirror.decoder.backend == DECODE_HW_D3D11 ? DIAG_OK
                          : (mirror.decoder.backend == DECODE_HW_SYSTEM ? DIAG_WARN : DIAG_ERR));
        add("DECODIFICADOR", st, Str(v), Str(det));
    }
    // Renderer
    {
        char v[64], det[160];
        snprintf(v, sizeof(v), "%s", settings.vsync ? "VSYNC" : "sem vsync (menor latencia)");
        snprintf(det, sizeof(det), "%s - scaling %s - %u frames no buffer",
                 gfx.mode_name(),
                 settings.scaling_mode == SCALE_MODE_ASPECT ? "ASPECT" :
                 settings.scaling_mode == SCALE_MODE_FILL ? "FILL" :
                 settings.scaling_mode == SCALE_MODE_INTEGER ? "INTEGER" : "1:1",
                 settings.buffer_size);
        add("RENDERIZADOR", DIAG_OK, Str(v), Str(det));
    }
    // Display
    {
        char v[64], det[192];
        u32 hz = 60;
        for (u32 i = 0; i < monitors.count; ++i) if (monitors.items[i].primary) hz = monitors.items[i].refresh_hz();
        snprintf(v, sizeof(v), "%u Hz", hz);
        snprintf(det, sizeof(det), "%ux%u - saida medida %.1f fps%s",
                 gfx.width, gfx.height, display_fps_measured,
                 (settings.target_fps && settings.target_fps > hz) ? " - alvo acima da taxa do monitor" : "");
        add("DISPLAY", settings.target_fps > hz ? DIAG_WARN : DIAG_OK, Str(v), Str(det));
    }
    // FPS
    {
        char v[96], det[192];
        snprintf(v, sizeof(v), "%.0f / %.0f / %.0f", sampler.fps.source_fps, sampler.fps.stream_fps,
                 sampler.fps.display_fps);
        snprintf(det, sizeof(det), "origem / stream / display   -  frame time %.2f ms", sampler.fps.frame_time_ms);
        DiagStatus st = !session_running() ? DIAG_WARN
            : (sampler.fps.display_fps + 2.0f < sampler.fps.stream_fps ? DIAG_WARN : DIAG_OK);
        add("FPS", st, Str(v), Str(det));
    }
    // Input
    {
        char v[64], det[192];
        snprintf(v, sizeof(v), "%.1f ms RTT", sampler.metrics[MET_INPUT_US].last * 2.0f / 1000.0f);
        snprintf(det, sizeof(det), "%s - %.0f eventos/s - fila %u",
                 d.input_backend[0] ? d.input_backend : "uinput / InputManager",
                 sampler.fps.input_hz, mirror.input_queue.count());
        DiagStatus st = d.input_backend[0] ? DIAG_OK : DIAG_WARN;
        add("ENTRADA", st, Str(v), Str(det));
    }
    // Mouse + keyboard combined row
    {
        char v[96], det[192];
        snprintf(v, sizeof(v), "%s", mouse_captured ? "capturado" : "liberado");
        snprintf(det, sizeof(det), "raw input %s - game mode %s - cursor %s",
                 settings.mouse_raw_input ? "on" : "off", game_mode ? "on" : "off",
                 cursor_hidden() ? "oculto" : "visivel");
        add("MOUSE E TECLADO", DIAG_OK, Str(v), Str(det));
    }
    diag_count = n;
    a.shutdown();
    build_diag_report();
}

void App::build_diag_report() {
    Arena a; a.init(1 << 16);
    StrBuilder sb; sb.init(&a, 4096);
    sb.append_fmt("MOBILADOR %s - relatorio de diagnostico\n", MOB_VERSION_STR);
    sb.append_fmt("build: %s\n", MOB_BUILD_STR);
    sb.append_fmt("sistema: Windows, %u nucleos, %.1f GB RAM\n",
                  (u32)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), sampler.sys.ram_total_mb / 1024.0);
    sb.append_fmt("gpu: %s (%llu MB)\n", gfx.gpu.name, (unsigned long long)(gfx.gpu.dedicated_vram / (1024 * 1024)));
    sb.append_fmt("celular: %s / Android %s / sdk %u / %s\n", mirror.device.model, mirror.device.android,
                  mirror.device.sdk, mirror.device.input_backend);
    sb.append("-----------------------------------------------------------\n");
    for (u32 i = 0; i < diag_count; ++i) {
        sb.append_fmt("[%s] %s: %s - %s\n", status_word(diag[i].status), diag[i].name, diag[i].value,
                      diag[i].detail);
    }
    sb.append("-----------------------------------------------------------\n");
    sb.append_fmt("fps origem/stream/display: %.1f / %.1f / %.1f\n", sampler.fps.source_fps,
                  sampler.fps.stream_fps, sampler.fps.display_fps);
    for (u32 i = 0; i < MET_COUNT; ++i) {
        const MetricSeries& ms = sampler.metrics[i];
        static const char* names[MET_COUNT] = { "captura", "encoder", "usb", "decode", "render", "entrada", "total", "fila" };
        sb.append_fmt("latencia %s: atual %.2f ms - media %.2f - p95 %.2f - p99 %.2f\n", names[i],
                      ms.last / 1000.0f, ms.avg() / 1000.0f, ms.p95 / 1000.0f, ms.p99 / 1000.0f);
    }
    snprintf(diag_report, sizeof(diag_report), "%.*s", (int)mob_min(sb.str().n, (u32)sizeof(diag_report) - 1),
             sb.str().p);
    a.shutdown();
}

f32 App::draw_diagnostics(Rect r) {
    const f32 gap = SP(12);
    Rect hdr{ r.x, r.y, r.w, SP(58) };
    card(&w, hdr, true);
    section_header(&w, "MOBILADOR DIAGNOSTICS", Rect{ hdr.x + SP(16), hdr.y + SP(8), hdr.w - SP(32), SP(26) }, ICON_DIAGNOSTICS);
    Rect b1{ hdr.r() - SP(420), hdr.y + SP(14), SP(130), SP(30) };
    Rect b2{ b1.r() + SP(8), b1.y, SP(130), SP(30) };
    Rect b3{ b2.r() + SP(8), b1.y, SP(130), SP(30) };
    if (button(&w, "VERIFICAR", b1, BTN_SECONDARY, ICON_REFRESH).clicked) run_diagnostics(true);
    if (button(&w, "SALVAR", b2, BTN_SECONDARY, ICON_SAVE).clicked) {
        write_text_file("diagnostico.txt", Str(diag_report));
    }
    if (button(&w, "COPIAR", b3, BTN_GHOST, ICON_COPY).clicked) {
        if (OpenClipboard(win->hwnd)) {
            EmptyClipboard();
            usize len = strlen(diag_report) + 1;
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len);
            if (mem) {
                void* dst = GlobalLock(mem);
                memcpy(dst, diag_report, len);
                GlobalUnlock(mem);
                SetClipboardData(CF_TEXT, mem);
            }
            CloseClipboard();
            w.toast("Relatorio copiado", theme.ok, ICON_COPY, 2.0f);
        }
    }
    f32 y = hdr.b() + gap;
    for (u32 i = 0; i < diag_count; ++i) {
        Rect row{ r.x, y, r.w, SP(40) };
        diag_row(&w, &diag[i], row);
        y += SP(46);
    }

    // server module maintenance
    Rect sm{ r.x, y + SP(4), r.w, SP(96) };
    card(&w, sm);
    section_header(&w, "SERVIDOR NO CELULAR", Rect{ sm.x + SP(16), sm.y + SP(8), sm.w - SP(32), SP(26) }, ICON_DOWNLOAD);
    ui.text_wrapped(server_module_status, sm.x + SP(16), sm.y + SP(38), sm.w - SP(32), SP(14),
                    FONT_SMALL, theme.text_dim, 3);
    Rect ib{ sm.x + SP(16), sm.b() - SP(36), SP(220), SP(28) };
    if (button(&w, "INSTALAR / ATUALIZAR", ib, BTN_SECONDARY, ICON_UPLOAD).clicked) {
        install_server_module();
    }
    y = sm.b() + gap;

    // log
    Rect lg{ r.x, y, r.w, SP(180) };
    card(&w, lg);
    section_header(&w, "LOG", Rect{ lg.x + SP(16), lg.y + SP(8), lg.w - SP(32), SP(26) }, ICON_LIST);
    static LogLine lines[64];
    u32 n = g_log.snapshot(lines, 64);
    for (u32 i = 0; i < n && i < 10; ++i) {
        const LogLine& l = lines[i];
        Col c = l.level >= LOG_ERROR ? theme.err : (l.level >= LOG_WARN ? theme.warn : theme.text_dim);
        char line[256];
        snprintf(line, sizeof(line), "%6llu.%03llu  %s", (unsigned long long)(l.t_ms / 1000),
                 (unsigned long long)(l.t_ms % 1000), l.text);
        ui.text_ellipsis(Str(line), lg.x + SP(16), lg.y + SP(40) + SP(13) * (f32)i, lg.w - SP(32), FONT_TINY, c);
    }

    // Height actually used, so the shell can size the scroll range.
    return lg.b() - r.y;
}

} // namespace mob
