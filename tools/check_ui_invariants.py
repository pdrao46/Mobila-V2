#!/usr/bin/env python3
# ============================================================================
#  MOBILADOR - tools/check_ui_invariants.py
#
#  Static checks for the rendering/input bugs that shipped in 1.0.0-1.0.3.
#  They all shared one property: nothing failed loudly. The build was green, the
#  log was quiet and only the screen looked wrong, so the fix for each one is
#  paired with a check here that fails on the source instead of on the user's
#  monitor.
#
#    1. input edges must be cleared once per frame        (1.0.0-1.0.4 bug)
#    2. one index per vertex in Ui2D::verts()             (1.0.4 bug)
#    3. the glyph atlas must be USAGE_DEFAULT, not DYNAMIC (1.0.3 bug)
#    4. coverage textures must use ps_text, not ps_ui      (1.0.2 bug)
#    5. no silently-skipped rebuilds: build_app must call build.py
#    6. the UI scale is applied at the frame boundary            (1.0.5 crash)
#    7. adb.exe ships where find_adb() looks                    (1.0.0-1.0.5 bug)
#    8. the dashboard tile grid is three columns wide            (layout bug)
#    9. no duplicated unit suffix in format strings
#
#  Run: python3 tools/check_ui_invariants.py      (also run by tools/run_tests.sh)
# ============================================================================
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAILED = []


def read(rel):
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        FAILED.append("missing file: %s" % rel)
        return ""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def check(name, ok, detail=""):
    print("  [%s] %s%s" % ("ok" if ok else "FALHOU", name, (" - " + detail) if detail and not ok else ""))
    if not ok:
        FAILED.append(name)


def strip_comments(src):
    """Removes // and /* */ comments so the checks read executable code only."""
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i)
            i = n if j < 0 else j + 2
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


# ---------------------------------------------------------------------------
# 1. input edges are cleared every frame
# ---------------------------------------------------------------------------
app = strip_comments(read("src/ui/app.cpp"))
widgets_h = read("src/ui/widgets.h")

check("UiInput::new_frame() is called from the frame loop",
      bool(re.search(r"\bin\s*\.\s*new_frame\s*\(\s*\)", app)),
      "pressed/released/wheel/key edges would persist after the first click; "
      "controls then react to mouse-over instead of clicks")

check("UiInput::new_frame() clears the mouse edges",
      all(re.search(pat, widgets_h) is not None
          for pat in (r"pressed\[i\]\s*=\s*false", r"released\[i\]\s*=\s*false", r"wheel\s*=\s*0")),
      "new_frame() must reset pressed, released and wheel")

check("new_frame() reset happens after the UI is drawn",
      app.find("w.in.new_frame()") > app.find("draw(dt)"),
      "clearing before draw() would swallow the events the widgets need")

# ---------------------------------------------------------------------------
# 2. Ui2D::verts() emits exactly one index per vertex
# ---------------------------------------------------------------------------
ui2d = strip_comments(read("src/render/ui2d.cpp"))
verts = re.search(r"UiVert\* Ui2D::verts\([^)]*\)\s*\{(.*?)\n\}", ui2d, re.S)
if not verts:
    check("Ui2D::verts() found", False)
else:
    body = verts.group(1)
    check("Ui2D::verts() advances one index per vertex",
          re.search(r"icount\s*\+=\s*n\s*;", body) is not None
          and re.search(r"icount\s*\+=\s*n\s*\*\s*3", body) is None,
          "advancing by n*3 emitted 18 indices per 6-vertex quad: six triangles "
          "per element, pulling geometry from neighbouring elements")
    check("Ui2D::verts() writes sequential indices",
          re.search(r"\(\s*\*\s*idx\s*\)\s*\[i\]\s*=\s*\(u16\)\s*\(\s*\*first_index\s*\+\s*i\s*\)", body) is not None
          and re.search(r"i\s*\*\s*3\s*\+\s*[012]", body) is None,
          "indices must be first+i, the pattern of a 6-vertex quad drawn as "
          "0,1,2 + 3,4,5")
    check("Ui2D::verts() reserves one index slot per vertex",
          re.search(r"icount\s*\+\s*n\s*>\s*icap", body) is not None,
          "the capacity check must match what the body actually reserves")

# the invariant helper has to exist and the app has to call it
check("Ui2D::verts_consistent() exists",
      "bool verts_consistent() const;" in read("src/render/ui2d.h"))
check("the app checks the geometry invariant",
      "ui.verts_consistent()" in app)

# ---------------------------------------------------------------------------
# 3. the glyph atlas is written with UpdateSubresource -> must be DEFAULT
# ---------------------------------------------------------------------------
text = strip_comments(read("src/render/text.cpp"))
check("glyph atlas uses D3D11_USAGE_DEFAULT",
      "D3D11_USAGE_DEFAULT" in text,
      "the atlas is filled with UpdateSubresource, which D3D11 ignores on a "
      "DYNAMIC resource: the upload silently did nothing and every glyph drew "
      "as an opaque black block")
check("glyph atlas is not created DYNAMIC",
      not re.search(r"td\.Usage\s*=\s*D3D11_USAGE_DYNAMIC", text))
check("atlas upload failure is reported",
      "read back EMPTY" in text or "atlas_has_content" in text,
      "a silently empty atlas is exactly the failure that produced black text")

# ---------------------------------------------------------------------------
# 4. coverage textures (the glyph atlas) use ps_text, not ps_ui
# ---------------------------------------------------------------------------
gfx = strip_comments(read("src/render/gfx.cpp"))
check("ps_text shader exists",
      re.search(r"float4\s+ps_text\s*\(", gfx) is not None,
      "sampling an R8 atlas with ps_ui reads .rgb as colour: glyphs came out "
      "red and their transparent padding turned into solid black")
check("ps_text is compiled",
      "ps_text" in gfx and "g->ps_text" in gfx)
check("coverage draws select ps_text",
      "c.coverage ? gfx->ps_text" in ui2d,
      "the command must pick ps_text for coverage textures and ps_ui for ARGB images")
check("the glyph atlas is marked as coverage",
      "/*coverage*/ true" in ui2d or "true);" in ui2d and "f.srv" in ui2d and "coverage" in ui2d)

# ---------------------------------------------------------------------------
# 5. the installer build must not reuse a stale application binary
# ---------------------------------------------------------------------------
bi = strip_comments(read("tools/installer/build_installer.py"))
ba = re.search(r"def build_app\(toolchain\):\s*\n(.*?)\ndef ", bi, re.S)
if ba:
    check("build_installer rebuilds the app instead of reusing dist/Mobilador.exe",
          not re.search(r"if os\.path\.exists\(APP\)", ba.group(1)),
          "reusing the existing binary packaged 1.0.2 inside the 1.0.3 installer")
else:
    check("build_app() found", False)


# ---------------------------------------------------------------------------
# 6. the UI scale must be applied at the frame boundary, never mid-frame
# ---------------------------------------------------------------------------
settings = strip_comments(read("src/ui/app_settings_screens.cpp"))
check("the settings screen defers the UI scale change",
      "request_ui_scale" in settings and "text.set_scale" not in settings,
      "calling set_scale() while drawing frees the atlas textures the current "
      "frame still references: the app closed with no message when the slider moved")
check("App::tick applies the pending scale after draw()",
      "pending_ui_scale" in app and app.find("pending_ui_scale > 0.0f") > app.find("draw(dt)"),
      "the rebuild has to happen once the frame has been submitted")

text_src = read("src/render/text.cpp")
check("the font rebuild reuses its device context",
      "HDC dc = (HDC)f.dc;" in text_src,
      "creating a DC per rebuild leaked nine of them on every scale change")

# ---------------------------------------------------------------------------
# 7. adb.exe has to be found where it is installed
# ---------------------------------------------------------------------------
win = strip_comments(read("src/platform/win.cpp"))
check("find_adb() looks next to the executable",
      re.search(r"resolve_adb_in\s*\(\s*g_paths\.exe_dir", win) is not None,
      "the installer puts adb.exe in the installation root; searching only "
      "<exe>\\tools produced 'adb.exe not found' on every clean install")
check("find_adb() still looks in tools/",
      re.search(r"resolve_adb_in\s*\(\s*g_paths\.tools_dir", win) is not None)
bi_src = read("tools/installer/build_installer.py")
m = re.search(r'for f in \("adb\.exe", "AdbWinApi\.dll", "AdbWinUsbApi\.dll"\):\s*\n(.*?)\n', bi_src, re.S)
check("the installer ships adb.exe under tools/",
      m is not None and '"tools/" + f' in m.group(1),
      "payload layout and find_adb() have to agree, or the app cannot reach a phone")

# ---------------------------------------------------------------------------
# 8. the dashboard is one metric card, not a pile of boxes: the reference
#    layout puts the numbers in a grid whose height is computed, never guessed
# ---------------------------------------------------------------------------
screens = strip_comments(read("src/ui/app_screens.cpp"))
check("the dashboard draws its six metrics in one grid card",
      "metric_grid(&w, grid, cells, 6, 3)" in screens
      and "MetricCell cells[6]" in screens,
      "the reference layout groups the numbers in one card with hairline "
      "separators; six loose tiles read as a pile of boxes")
check("the grid height comes from metric_grid_height()",
      "metric_grid_height(&w, 6, 3)" in screens,
      "a hand-written card height is what clipped QUICK PERFORMANCE out of "
      "the window in 1.0.6")
check("no tile geometry derived from the two-column half width",
      "half * 1.5f" not in screens,
      "the two-column geometry pushed two of every three tiles past the "
      "right edge of the window")
check("the performance strip reuses the same grid",
      "metric_grid(&w, pgr, pcells, 4, 4)" in screens)
widgets_src = strip_comments(read("src/ui/widgets.cpp"))
check("metric_grid separates its compartments with hairlines",
      "th->border" in widgets_src
      and re.search(r"void metric_grid\(", widgets_src) is not None
      and re.search(r"for \(u32 i = 1; i < cols; \+\+i\)", widgets_src) is not None)
check("cards keep one corner radius token",
      re.search(r"const f32 rad = c->ui->sp\(14\);", widgets_src) is not None
      and "sp(10);" in widgets_src)

# ---------------------------------------------------------------------------
# 9. scrolling: the range has to come from what the screens really drew
# ---------------------------------------------------------------------------
apph = strip_comments(read("src/ui/app.h"))
missing = [fn for fn in ("draw_dashboard", "draw_performance", "draw_latency",
                         "draw_benchmark", "draw_diagnostics", "draw_settings",
                         "draw_about")
           if not re.search(r"f32\s+%s\(" % fn, apph)]
check("every screen reports the height it used", not missing,
      "still void: %s - a screen that cannot report its height cannot be "
      "scrolled to its end" % ", ".join(missing))
check("the scroll range comes from the measured content height",
      "screen_content_h[screen] = used + pad" in screens
      and "max_scroll = mob_max(content_h - view.h, 0.0f)" in screens,
      "the per-screen constants (SP(690) for the dashboard) fell behind the "
      "layouts, so the last row was unreachable")
check("no screen keeps a hard-coded content height",
      re.search(r"content_h\s*=\s*SP\(", screens) is None,
      "a constant here is the same bug in a different place")

# ---------------------------------------------------------------------------
# 10. every widget call is consumed: a bare button(...) statement is a control
#     that draws, animates and does nothing when clicked
# ---------------------------------------------------------------------------
# Only widgets that return a click/hover result: drawing helpers (card, graph,
# tooltip, metric_grid) are filtered out on purpose, they have nothing to
# consume.
WIDGETS = ("button", "button_icon", "button_big", "toggle", "segmented", "dropdown",
           "slider", "checkbox", "keybind_field", "text_field", "list_row", "modal_begin")


def statements(src):
    """Split C++ into statements on top-level ';' and brace boundaries.

    Parenthesis/bracket depth is tracked, brace depth is a hard boundary: no
    statement continues across '{' or '}', which is what keeps a widget call
    inside a lambda body from being glued onto the lambda's own statement.
    """
    out, buf = [], []
    pdepth, line, start_line = 0, 1, 1
    for ch in src:
        if ch == "\n":
            line += 1
        elif ch in "([":
            pdepth += 1
        elif ch in ")]":
            pdepth = max(0, pdepth - 1)
        elif ch in "{}" and pdepth == 0:
            txt = "".join(buf)
            if txt.strip():
                out.append((txt, start_line))
            buf = []
            start_line = line
            continue
        if ch == ";" and pdepth == 0:
            txt = "".join(buf)
            if txt.strip():
                out.append((txt, start_line))
            buf = []
            start_line = line + 1
            continue
        if not buf and not ch.isspace():
            start_line = line
        buf.append(ch)
    if "".join(buf).strip():
        out.append(("".join(buf), start_line))
    return out


dropped = []
for rel in ("src/ui/app_screens.cpp", "src/ui/app_settings_screens.cpp", "src/ui/app.cpp"):
    src = strip_comments(read(rel))
    for stmt, line in statements(src):
        for wname in WIDGETS:
            if not re.search(r"\b%s\s*\(" % wname, stmt):
                continue
            head = stmt[:stmt.index("%s(" % wname)]
            consumed = ("if (" in head or "if(" in head or "while (" in head
                        or "=" in head or head.strip().startswith("return"))
            if not consumed:
                dropped.append("%s:%d  %s" % (rel, line, stmt.strip().splitlines()[-1][:70]))
            break
check("no widget call is a bare statement", not dropped,
      "these draw a control that never reacts: " + " | ".join(dropped[:4]))

# the scanner above is itself checked: a check that cannot fail is noise
_self = "void f() {\n  button(&w, \"X\", r, BTN_PRIMARY);\n  if (button(&w, \"Y\", r, BTN_PRIMARY).clicked) g();\n  BtnResult b = button(&w, \"Z\", r, BTN_PRIMARY);\n}\n"
_self_bad = [st for st, _ln in statements(_self)
             if re.search(r"\bbutton\s*\(", st)
             and not ("if (" in st or "if(" in st or "=" in st)]
check("the widget scanner catches a dropped call (self-test)",
      len(_self_bad) == 1,
      "the scanner found %d dropped calls in a sample that has exactly one" % len(_self_bad))

# ---------------------------------------------------------------------------
# 9. format strings must not carry a duplicated unit suffix
# ---------------------------------------------------------------------------
for rel in ("src/ui/app_screens.cpp", "src/ui/app_settings_screens.cpp"):
    bad = [ln for ln in read(rel).splitlines() if 'x x"' in ln or "x x\\n" in ln]
    check("no duplicated unit in format strings (%s)" % os.path.basename(rel),
          not bad, "e.g. \"%.2fx x\" printed \"1.00x x\"")

print()
if FAILED:
    print("  %d verificacao(oes) de UI FALHARAM:" % len(FAILED))
    for f in FAILED:
        print("    - %s" % f)
    sys.exit(1)
print("  ui/input invariants ok")
