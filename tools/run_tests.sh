#!/usr/bin/env bash
# ============================================================================
#  MOBILADOR - tools/run_tests.sh
#  Builds and runs the platform independent core tests on the host (Linux).
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
# Protocol and platform checks. These do not need a compiler: they compare the
# two implementations of the wire protocol and verify that the phone-side Java
# only references platform APIs that exist in the bundled platform jar.
# ---------------------------------------------------------------------------
echo "  checking the C++ <-> Java protocol"
python3 "$ROOT/tools/check_protocol.py"

if [ -f "$ROOT/tools/android-stubs/android-33.jar" ]; then
  echo "  checking the Android module against the platform jar"
  python3 "$ROOT/tools/check_java_api.py" > "$OUT/java_api_check.txt" || {
      cat "$OUT/java_api_check.txt"; exit 1; }
  tail -1 "$OUT/java_api_check.txt"
fi
"$OUT/core_tests"

# ---------------------------------------------------------------------------
# Protocol and platform checks. These do not need a compiler: they compare the
# two implementations of the wire protocol and verify that the phone-side Java
# only references platform APIs that exist in the bundled platform jar.
# ---------------------------------------------------------------------------
echo "  checking the C++ <-> Java protocol"
python3 "$ROOT/tools/check_protocol.py"

if [ -f "$ROOT/tools/android-stubs/android-33.jar" ]; then
  echo "  checking the Android module against the platform jar"
  python3 "$ROOT/tools/check_java_api.py" > "$OUT/java_api_check.txt" || {
      cat "$OUT/java_api_check.txt"; exit 1; }
  tail -1 "$OUT/java_api_check.txt"
fi
