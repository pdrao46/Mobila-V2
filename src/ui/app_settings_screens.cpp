// ============================================================================
//  MOBILADOR - src/ui/app_settings_screens.cpp
//  SETTINGS, ABOUT, the in-game overlay, and the reusable setting-row helpers.
//
//  Every performance option is rendered as a row that carries its own plain
//  language explanation underneath (from settings.cpp / setting_help), because
//  a settings screen nobody understands is a settings screen nobody uses.
// ============================================================================
#include "app.h"
#include "../core/log.h"
#include <stdio.h>

// Layout units: the screens are written in logical pixels and scaled
// through the global UI scale published by the batcher.
#define SP(v) (mob::g_ui_scale * (f32)(v))

using namespace mob;

namespace mob {

// ---------------------------------------------------------------------------
// setting row helpers: label on the left, control on the right, help under it
// ---------------------------------------------------------------------------
void App::help_line(const char* key, f32 x, f32 y, f32 w) {
    const char* help = setting_help(key);
    if (!help) return;
    ui.icon(ICON_INFO, x, y + SP(1), SP(11), theme.text_faint, 1.4f);
    ui.text_wrapped(Str(help), x + SP(16), y, w - SP(16), SP(13), FONT_TINY, theme.text_faint, 2);
}

bool App::setting_row_toggle(const char* key, const char* label, bool* value, Rect r) {
    ui.text(Str(label), r.x, r.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
    Rect box{ r.r() - SP(56), r.cy() - SP(11), SP(44), SP(22) };
    bool changed = toggle(&w, Str(key), box, value);
    help_line(key, r.x, r.b() - SP(26), r.w - SP(90));
    return changed;
}

bool App::setting_row_segmented(const char* key, const char* label, Rect r, const char* const* items,
                                u32 count, u32* index) {
    ui.text(Str(label), r.x, r.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
    Rect box{ r.x + SP(230), r.y, SP(420), r.h - SP(6) };
    bool changed = segmented(&w, Str(key), box, items, count, index);
    help_line(key, r.x, r.b() - SP(26), r.w);
    return changed;
}

bool App::setting_row_slider(const char* key, const char* label, Rect r, f32* value, f32 lo, f32 hi,
                             f32 step, const char* fmt) {
    ui.text(Str(label), r.x, r.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
    if (!value || !fmt) { help_line(key, r.x, r.b() - SP(26), r.w); return false; }
    Rect box{ r.x + SP(230), r.y, SP(420), r.h - SP(6) };
    bool changed = slider(&w, Str(key), box, value, lo, hi, step, fmt);
    help_line(key, r.x, r.b() - SP(26), r.w);
    return changed;
}

bool App::setting_row_dropdown(const char* key, const char* label, Rect r, const char* const* items,
                               u32 count, u32* index, const char* const* hints) {
    ui.text(Str(label), r.x, r.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
    Rect box{ r.x + SP(230), r.y, SP(420), r.h - SP(6) };
    bool changed = dropdown(&w, Str(key), box, items, count, index, hints);
    help_line(key, r.x, r.b() - SP(26), r.w);
    return changed;
}

// ===========================================================================
//  SETTINGS
// ===========================================================================
void App::draw_settings(Rect r) {
    const f32 gap = SP(16);
    f32 y = r.y;

    // ------------------------------------------------------------ appearance
    Rect ac{ r.x, y, r.w, SP(300) };
    card(&w, ac, true);
    section_header(&w, "APARENCIA", Rect{ ac.x + SP(16), ac.y + SP(10), ac.w - SP(32), SP(26) }, ICON_PALETTE);

    // accent swatches: 8 presets + free colour, applied to the whole product
    ui.text("COR DE DESTAQUE", ac.x + SP(16), ac.y + SP(44), FONT_TINY, theme.text_faint);
    const f32 sw = SP(38);
    for (u32 i = 0; i < (u32)kAccentPresetCount; ++i) {
        Rect cell{ ac.x + SP(16) + (f32)i * (sw + SP(8)), ac.y + SP(60), sw, sw };
        Col c = Col::from_rgb(kAccentPresets[i].rgb);
        bool sel = settings.accent_rgb == kAccentPresets[i].rgb;
        ui.rrect(cell.x, cell.y, cell.w, cell.h, SP(6), c);
        if (sel) ui.rrect_border(cell.x - SP(2), cell.y - SP(2), cell.w + SP(4), cell.h + SP(4), SP(8), SP(2), theme.text);
        if (cell.contains(w.in.mouse_x, w.in.mouse_y)) {
            if (w.in.pressed[MOB_MB_LEFT]) {
                settings.accent_rgb = kAccentPresets[i].rgb;
                snprintf(settings.accent_name, sizeof(settings.accent_name), "%s", kAccentPresets[i].name);
                theme_init(&theme, (ThemeMode)settings.theme_mode, settings.accent_rgb, settings.accent_name);
                on_settings_changed("accent");
            }
        }
        if (i < 8) ui.text(Str(kAccentPresets[i].name), cell.x - SP(2), cell.b() + SP(4), FONT_TINY, theme.text_faint);
    }
    // custom colour: hue strip, still one accent, still predictable
    {
        Rect strip{ ac.x + SP(16), ac.y + SP(122), ac.w - SP(32), SP(26) };
        const u32 steps = 48;
        for (u32 i = 0; i < steps; ++i) {
            Col c = Col::hsv((f32)i / (f32)steps, 0.75f, 1.0f);
            ui.rect(strip.x + strip.w * (f32)i / (f32)steps, strip.y, strip.w / (f32)steps + 1, strip.h * 0.6f, c);
        }
        ui.rrect_border(strip.x, strip.y, strip.w, strip.h * 0.6f, SP(4), 1.0f, theme.border);
        if (strip.contains(w.in.mouse_x, w.in.mouse_y) && w.in.down[MOB_MB_LEFT]) {
            f32 t = mob_clamp((w.in.mouse_x - strip.x) / strip.w, 0.0f, 0.999f);
            Col c = Col::hsv(t, 0.75f, 1.0f);
            settings.accent_rgb = ((u32)(c.r * 255) << 16) | ((u32)(c.g * 255) << 8) | (u32)(c.b * 255);
            snprintf(settings.accent_name, sizeof(settings.accent_name), "Personalizado");
            custom_accent = settings.accent_rgb;
            theme_init(&theme, (ThemeMode)settings.theme_mode, settings.accent_rgb, settings.accent_name);
            on_settings_changed("accent_custom");
        }
        ui.text("PERSONALIZADO", strip.x, strip.b() + SP(2), FONT_TINY, theme.text_faint);
    }

    // theme mode
    {
        Rect row{ ac.x + SP(16), ac.y + SP(164), ac.w - SP(32), SP(30) };
        static const char* modes[3] = { "DARK", "LIGHT", "AMOLED BLACK" };
        u32 idx = (u32)settings.theme_mode;
        ui.text("TEMA", row.x, row.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
        Rect box{ row.x + SP(230), row.y, SP(420), row.h - SP(6) };
        if (segmented(&w, Str("theme"), box, modes, 3, &idx)) {
            settings.theme_mode = (ThemeMode)idx;
            theme_init(&theme, (ThemeMode)settings.theme_mode, settings.accent_rgb, settings.accent_name);
            on_settings_changed("theme");
        }
    }
    { Rect rr{ ac.x + SP(16), ac.y + SP(200), ac.w - SP(32), SP(30) }; setting_row_toggle("animations", "Micro-animacoes da interface", &settings.animations, rr); theme.animations = settings.animations; }
    {
        Rect rr{ ac.x + SP(16), ac.y + SP(236), ac.w - SP(32), SP(30) };
        ui.text("ESCALA DA INTERFACE", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
        Rect box{ rr.x + SP(230), rr.y, SP(300), rr.h - SP(6) };
        if (slider(&w, Str("uiscale"), box, &settings.ui_scale, 0.8f, 1.6f, 0.05f, "%.2fx x")) {
            // The font atlas and the batcher both take the scale from one place.
            text.set_scale(settings.ui_scale * dpi_scale);
            ui.set_dpi(settings.ui_scale * dpi_scale);
            on_settings_changed("ui_scale");
        }
        ui.text("Escala da interface aplicada a fontes, icones e espacamentos.",
                rr.x, rr.b() - SP(24), FONT_TINY, theme.text_faint);
    }
    y = ac.b() + gap;

    // ------------------------------------------------------------- hotkeys
    Rect hk{ r.x, y, r.w, SP(300) };
    card(&w, hk);
    section_header(&w, "ATALHOS", Rect{ hk.x + SP(16), hk.y + SP(10), hk.w - SP(32), SP(26) }, ICON_KEYBOARD);
    ui.text("Clique em um atalho e pressione a tecla desejada. ESC cancela.",
            hk.x + SP(16), hk.y + SP(42), FONT_SMALL, theme.text_faint);
    struct { const char* label; u32* vk; } hks[8] = {
        { "Tela cheia", &settings.hotkeys.toggle_fullscreen },
        { "Overlay", &settings.hotkeys.toggle_overlay },
        { "Liberar mouse", &settings.hotkeys.release_mouse },
        { "Capturar mouse", &settings.hotkeys.capture_mouse },
        { "Game mode", &settings.hotkeys.toggle_game_mode },
        { "Iniciar / parar sessao", &settings.hotkeys.start_stop_stream },
        { "Screenshot", &settings.hotkeys.screenshot },
        { "Estatisticas", &settings.hotkeys.toggle_stats },
    };
    for (u32 i = 0; i < 8; ++i) {
        u32 col = i % 2, row = i / 2;
        Rect cell{ hk.x + SP(16) + (f32)col * ((hk.w - SP(48)) * 0.5f + SP(16)),
                   hk.y + SP(70) + (f32)row * SP(46), (hk.w - SP(48)) * 0.5f, SP(34) };
        ui.text(Str(hks[i].label), cell.x, cell.cy() - ui.line_h(FONT_SMALL) * 0.5f, FONT_SMALL, theme.text_dim);
        Rect box{ cell.r() - SP(96), cell.y, SP(96), cell.h };
        bool waiting = keybind_waiting && keybind_target == i;
        if (keybind_field(&w, Str("hk"), box, hks[i].vk, &waiting)) {
            keybind_waiting = true;
            keybind_target = i;
        } else if (keybind_waiting && keybind_target == i && !waiting) {
            // the field itself cleared the flag (ESC)
            keybind_waiting = false;
        }
    }
    ui.text("Os atalhos valem com a janela em foco - nunca sao capturados globalmente,",
            hk.x + SP(16), hk.b() - SP(38), FONT_TINY, theme.text_faint);
    ui.text("para nao roubar teclas de outros programas.",
            hk.x + SP(16), hk.b() - SP(24), FONT_TINY, theme.text_faint);
    y = hk.b() + gap;

    // ----------------------------------------------------- transport + audio
    Rect tr{ r.x, y, r.w, SP(232) };
    card(&w, tr);
    section_header(&w, "TRANSPORTE E AUDIO", Rect{ tr.x + SP(16), tr.y + SP(10), tr.w - SP(32), SP(26) }, ICON_USB);
    { Rect rr{ tr.x + SP(16), tr.y + SP(44), tr.w - SP(32), SP(30) }; setting_row_toggle("input_priority", "Prioridade de thread de entrada", &settings.input_priority, rr); }
    { Rect rr{ tr.x + SP(16), tr.y + SP(80), tr.w - SP(32), SP(30) }; setting_row_toggle("reconnect_auto", "Reconexao automatica", &settings.reconnect_auto, rr); }
    { Rect rr{ tr.x + SP(16), tr.y + SP(116), tr.w - SP(32), SP(30) }; setting_row_toggle("log_performance", "Registrar log de desempenho (CSV)", &settings.log_performance, rr); }
    {
        Rect rr{ tr.x + SP(16), tr.y + SP(152), tr.w - SP(32), SP(30) };
        ui.text("BUFFER DE RECEPCAO (KB)", rr.x, rr.cy() - ui.line_h(FONT_LABEL) * 0.5f, FONT_LABEL, theme.text);
        Rect box{ rr.x + SP(230), rr.y, SP(300), rr.h - SP(6) };
        f32 kb = (f32)settings.socket_recv_buffer_kb;
        if (slider(&w, Str("recvbuf"), box, &kb, 256, 8192, 128, "%.0f KB")) {
            settings.socket_recv_buffer_kb = (u32)kb;
            on_settings_changed("recv_buffer");
        }
        help_line("recv_buffer", rr.x, rr.b() - SP(26), rr.w);
    }
    { Rect rr{ tr.x + SP(16), tr.y + SP(188), tr.w - SP(32), SP(30) }; setting_row_toggle("audio_enabled", "Audio do celular no PC", &settings.audio_enabled, rr); }
    y = tr.b() + gap;

    // -------------------------------------------------------------- profiles
    Rect pf{ r.x, y, r.w, SP(148) };
    card(&w, pf);
    section_header(&w, "PERFIS", Rect{ pf.x + SP(16), pf.y + SP(10), pf.w - SP(32), SP(26) }, ICON_FOLDER);
    if (paths) {
        refresh_profiles(false);
        u32 n = profile_count;
        if (n == 0) {
            empty_state(&w, Rect{ pf.x + SP(16), pf.y + SP(44), pf.w - SP(32), SP(60) }, ICON_FOLDER,
                        "Nenhum perfil salvo", "Salve a configuracao atual em DESEMPENHO > SALVAR PERFIL.");
        } else {
            f32 px = pf.x + SP(16);
            for (u32 i = 0; i < n && i < 6; ++i) {
                Rect b{ px, pf.y + SP(44), SP(150), SP(30) };
                Str pname(profile_names[i]);
                if (button(&w, pname, b, BTN_SECONDARY, ICON_FOLDER).clicked) {
                    if (settings.load_profile(paths->profile_dir, pname)) {
                        theme_init(&theme, (ThemeMode)settings.theme_mode, settings.accent_rgb, settings.accent_name);
                        on_settings_changed("profile");
                        w.toast("Perfil carregado", theme.ok, ICON_CHECK, 2.5f);
                    } else {
                        w.toast("Perfil invalido", theme.err, ICON_WARNING, 3.0f);
                    }
                }
                px += SP(158);
            }
        }
    }
    Rect r1{ pf.x + SP(16), pf.b() - SP(44), SP(180), SP(30) };
    Rect r2{ r1.r() + SP(10), r1.y, SP(180), SP(30) };
    Rect r3{ r2.r() + SP(10), r1.y, SP(180), SP(30) };
    if (button(&w, "SALVAR COMO...", r1, BTN_SECONDARY, ICON_SAVE).clicked) save_profile(Str("Perfil"));
    if (button(&w, "RESTAURAR PADROES", r2, BTN_GHOST, ICON_RESTORE).clicked) {
        settings.apply_preset(PRESET_BALANCED);
        theme_init(&theme, (ThemeMode)settings.theme_mode, settings.accent_rgb, settings.accent_name);
        on_settings_changed("restore");
        w.toast("Padroes restaurados", theme.info, ICON_RESTORE, 3.0f);
    }
    if (button(&w, "RESTAURAR TUDO", r3, BTN_DANGER, ICON_RESET).clicked) {
        settings = Settings();
        theme_init(&theme, THEME_DARK, settings.accent_rgb, settings.accent_name);
        on_settings_changed("factory");
        w.toast("Configuracao de fabrica", theme.warn, ICON_RESET, 3.0f);
    }

    // ---- game mode card, placed last because it is the "commit" of the page
    Rect gm{ r.x, pf.b() + gap, r.w, SP(120) };
    card(&w, gm, true);
    section_header(&w, "GAME MODE", Rect{ gm.x + SP(16), gm.y + SP(10), gm.w - SP(32), SP(26) }, ICON_GAMEPAD);
    ui.text_wrapped("Ligado: tela cheia sem borda, cursor oculto, mouse capturado, zero animacoes. "
                    "Desligado: cursor visivel, mouse livre, interface normal. A troca e imediata.",
                    gm.x + SP(16), gm.y + SP(44), gm.w - SP(32), SP(14), FONT_SMALL, theme.text_dim, 2);
    Rect gb{ gm.r() - SP(220), gm.y + SP(36), SP(200), SP(36) };
    if (button(&w, game_mode ? "DESATIVAR" : "ATIVAR AGORA", gb, game_mode ? BTN_DANGER : BTN_PRIMARY, ICON_GAMEPAD).clicked) {
        set_game_mode(!game_mode);
    }
}

// ===========================================================================
//  ABOUT
// ===========================================================================
void App::draw_about(Rect r) {
    const f32 gap = SP(16);
    Rect id{ r.x, r.y, r.w, SP(150) };
    card(&w, id, true);
    ui.icon(ICON_LOGO_MARK, id.x + SP(24), id.y + SP(24), SP(48), theme.accent, 2.2f);
    ui.text(Str("MOBILADOR"), id.x + SP(92), id.y + SP(26), FONT_H1, theme.text);
    char ver[160];
    snprintf(ver, sizeof(ver), "Versao %s   -   build %s", MOB_VERSION_STR, MOB_BUILD_STR);
    ui.text(Str(ver), id.x + SP(92), id.y + SP(54), FONT_SMALL, theme.text_faint);
    ui.text_wrapped("Espelhamento USB de baixa latencia para jogar no PC. O Mobilador transporta imagem, entrada "
                    "e audio. O mapeamento de teclas e mouse pertence ao GG Mouse Pro 3, no celular.",
                    id.x + SP(24), id.y + SP(92), id.w - SP(48), SP(16), FONT_BODY, theme.text_dim, 3);

    Rect li{ r.x, id.b() + gap, r.w, SP(230) };
    card(&w, li);
    section_header(&w, "O QUE O MOBILADOR NAO FAZ", Rect{ li.x + SP(16), li.y + SP(10), li.w - SP(32), SP(26) }, ICON_SHIELD);
    const char* no[6] = {
        "Nao implementa keymapping, editor de teclas ou HUD.",
        "Nao usa aimbot, automacao de mira ou qualquer cheat.",
        "Nao manipula a memoria do Free Fire.",
        "Nao modifica APK nem contorna anti-cheat.",
        "Nao injeta codigo em outros processos.",
        "Nao simula numeros: o que nao foi medido nao aparece.",
    };
    for (u32 i = 0; i < 6; ++i) {
        Rect row{ li.x + SP(16), li.y + SP(46) + SP(24) * (f32)i, li.w - SP(32), SP(20) };
        ui.icon(ICON_BLOCK, row.x, row.cy() - SP(7), SP(14), theme.text_faint, 1.5f);
        ui.text(Str(no[i]), row.x + SP(22), row.cy() - ui.line_h(FONT_SMALL) * 0.5f, FONT_SMALL, theme.text_dim);
    }

    Rect sh{ r.x, li.b() + gap, r.w, SP(150) };
    card(&w, sh);
    section_header(&w, "DESEMPENHO ADICIONAL", Rect{ sh.x + SP(16), sh.y + SP(10), sh.w - SP(32), SP(26) }, ICON_SYSTEM);
    {
        u32 docs_n = 0;
        const SettingDoc* docs = setting_docs(&docs_n);
        for (u32 i = 0; i < docs_n && i < 4; ++i) {
            Rect row{ sh.x + SP(16), sh.y + SP(46) + SP(26) * (f32)i, sh.w - SP(32), SP(22) };
            const char* help = setting_help(docs[i].key);
            (void)help;
            ui.text(Str(docs[i].title), row.x, row.cy() - ui.line_h(FONT_SMALL) * 0.5f, FONT_SMALL, theme.text);
            ui.text_ellipsis(Str(help ? help : ""), row.x + SP(240), row.cy() - ui.line_h(FONT_TINY) * 0.5f,
                             row.w - SP(250), FONT_TINY, theme.text_faint);
        }
    }
}

// ===========================================================================
//  IN-GAME OVERLAY
//  Small, optional, hotkey toggled (F8). Nothing else is drawn over the game.
// ===========================================================================
void App::draw_overlay() {
    const f32 pad = SP(14);
    const f32 w_ = SP(216);
    const f32 h_ = SP(96);
    f32 x = (f32)gfx.width - w_ - pad;
    f32 y = pad;

    ui.rrect(x, y, w_, h_, SP(8), theme.surface.with_a(0.82f));
    ui.rrect_border(x, y, w_, h_, SP(8), 1.0f, theme.border);

    char line[64];
    snprintf(line, sizeof(line), "%.0f", sampler.fps.display_fps);
    ui.text(Str(line), x + SP(14), y + SP(10), FONT_DISPLAY, theme.text);
    ui.text("FPS", x + SP(14) + ui.measure(Str(line), FONT_DISPLAY) + SP(6), y + SP(30), FONT_SMALL, theme.text_faint);

    snprintf(line, sizeof(line), "%.1f ms", sampler.metrics[MET_TOTAL_US].last / 1000.0f);
    ui.text(Str(line), x + SP(120), y + SP(16), FONT_MONO_BOLD, theme.accent);
    ui.text("LATENCIA", x + SP(120), y + SP(32), FONT_TINY, theme.text_faint);

    snprintf(line, sizeof(line), "%.0f / %.0f", sampler.fps.source_fps, sampler.fps.stream_fps);
    ui.text(Str(line), x + SP(14), y + SP(56), FONT_MONO, theme.text_dim);
    ui.text("ORIGEM / STREAM", x + SP(14), y + SP(70), FONT_TINY, theme.text_faint);

    snprintf(line, sizeof(line), "%.0f%%  %.0f%%", sampler.sys.gpu_percent, sampler.sys.cpu_percent);
    ui.text(Str(line), x + SP(120), y + SP(56), FONT_MONO, theme.text_dim);
    ui.text("GPU / CPU", x + SP(120), y + SP(70), FONT_TINY, theme.text_faint);
}

// ===========================================================================
//  SERVER MODULE INSTALLATION
//  One-time per device: upload the DEX, or build it here when a JDK exists.
// ===========================================================================
void App::install_server_module() {
    if (!adb.have_device()) {
        w.toast("Conecte o celular primeiro", theme.err, ICON_WARNING, 3.5f);
        return;
    }
    char msg[1024];
    // Look for a prebuilt module next to the executable first; only build when
    // there is nothing to upload.
    char local_dex[512];
    snprintf(local_dex, sizeof(local_dex), "%s\\mobilador.dex", paths->data_dir);
    char bundled[512];
    snprintf(bundled, sizeof(bundled), "%s\\server\\mobilador.dex", paths->exe_dir);
    const char* dex = file_exists(bundled) ? bundled : local_dex;

    // Where the Java sources may live, in order of likelihood: shipped next to
    // the executable (redistributable layout), the repository checkout, or the
    // current working directory.
    char src_dir[512] = "";
    const char* candidates[5] = { 0 };
    char c0[512], c1[512], c2[512], c3[512];
    snprintf(c0, sizeof(c0), "%s\\server\\src\\com\\mobilador\\server", paths->exe_dir);
    snprintf(c1, sizeof(c1), "%s\\server\\src", paths->exe_dir);
    snprintf(c2, sizeof(c2), "%s\\..\\android-server\\src\\com\\mobilador\\server", paths->exe_dir);
    snprintf(c3, sizeof(c3), "android-server\\src\\com\\mobilador\\server");
    candidates[0] = c0; candidates[1] = c1; candidates[2] = c2; candidates[3] = c3;
    for (u32 i = 0; i < 4; ++i) {
        if (candidates[i] && dir_exists(candidates[i])) {
            snprintf(src_dir, sizeof(src_dir), "%s", candidates[i]);
            break;
        }
    }
    if (!src_dir[0]) snprintf(src_dir, sizeof(src_dir), "%s", c3);

    Adb::ModuleState st = adb.ensure_module(dex, src_dir, msg, sizeof(msg));
    snprintf(server_module_status, sizeof(server_module_status), "%s", msg);
    server_module_present = (st == Adb::MODULE_PRESENT || st == Adb::MODULE_UPDATED);
    switch (st) {
        case Adb::MODULE_PRESENT:
            w.toast("Modulo ja instalado no celular", theme.ok, ICON_CHECK, 3.0f);
            break;
        case Adb::MODULE_UPDATED:
            w.toast("Modulo enviado para o celular", theme.ok, ICON_UPLOAD, 3.5f);
            break;
        case Adb::MODULE_NO_TOOLCHAIN:
            w.toast("Sem JDK para compilar o modulo - veja o diagnostico", theme.warn, ICON_WARNING, 6.0f);
            break;
        default:
            w.toast("Falha ao instalar o modulo", theme.err, ICON_ERROR, 6.0f);
            break;
    }
    run_diagnostics(true);
}


// ---------------------------------------------------------------------------
// Profile cache: the settings screen shows saved profiles, but scanning the
// folder must not happen every frame. Names live in fixed buffers (no arena
// growth per frame, no file system hit in the steady state).
// ---------------------------------------------------------------------------
void App::refresh_profiles(bool force) {
    u64 now = now_us();
    if (!force && now - profiles_scanned_us < 2000000ull) return;
    profiles_scanned_us = now;
    profile_count = 0;
    if (!paths) return;
    Arena a; a.init(1 << 16);
    Vec<Str> names;
    names.init(&a, 8);
    u32 n = settings.list_profiles(&a, paths->profile_dir, &names);
    for (u32 i = 0; i < n && profile_count < 8; ++i) {
        snprintf(profile_names[profile_count], sizeof(profile_names[0]), "%.*s",
                 (int)mob_min(names[i].n, (u32)sizeof(profile_names[0]) - 1), names[i].p);
        ++profile_count;
    }
    a.shutdown();
}

void App::save_profile(Str name) {
    if (!paths) return;
    if (settings.save_profile(paths->profile_dir, name)) {
        w.toast("Perfil salvo", theme.ok, ICON_SAVE, 2.5f);
        refresh_profiles(true);
    } else {
        w.toast("Nao foi possivel salvar o perfil", theme.err, ICON_WARNING, 3.5f);
    }
}

} // namespace mob
