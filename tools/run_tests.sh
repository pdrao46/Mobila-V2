#!/usr/bin/env bash
# ============================================================================
#  MOBILADOR - tools/run_tests.sh
#  Host-side test suite: core tests, static checks and the UI/input invariants.
#  Everything here runs on Linux, with no Windows and no device.
# ============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/host"
mkdir -p "$OUT"
CXX="${CXX:-g++}"

echo "  compiling core tests with $CXX"
"$CXX" -std=c++17 -O1 -g -fno-exceptions -fno-rtti -pthread \
      -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
      "$ROOT/tests/test_core.cpp" "$ROOT/src/core/base.cpp" "$ROOT/src/core/threads.cpp" \
      "$ROOT/src/core/log.cpp" -o "$OUT/core_tests"

# ---------------------------------------------------------------------------
# Static checks. These do not need a compiler: they compare the two
# implementations of the wire protocol, verify that the phone-side Java only
# references platform APIs that exist in the bundled jar, and guard the
# rendering/input invariants whose violations are silent at runtime (see
# tools/check_ui_invariants.py for the list and the bugs behind each one).
#
# The block below used to be duplicated verbatim, which ran every check twice.
# ---------------------------------------------------------------------------
echo "  checking the UI/input invariants"
python3 "$ROOT/tools/check_ui_invariants.py"

echo "  checking the C++ <-> Java protocol"
python3 "$ROOT/tools/check_protocol.py"

if [ -f "$ROOT/tools/android-stubs/android-33.jar" ]; then
  echo "  checking the Android module against the platform jar"
  python3 "$ROOT/tools/check_java_api.py" > "$OUT/java_api_check.txt" || {
      cat "$OUT/java_api_check.txt"; exit 1; }
  tail -1 "$OUT/java_api_check.txt"
fi

echo "  running core tests"
"$OUT/core_tests"
