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
"$OUT/core_tests"
