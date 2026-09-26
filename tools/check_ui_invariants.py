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

print()
if FAILED:
    print("  %d verificacao(oes) de UI FALHARAM:" % len(FAILED))
    for f in FAILED:
        print("    - %s" % f)
    sys.exit(1)
print("  ui/input invariants ok")
