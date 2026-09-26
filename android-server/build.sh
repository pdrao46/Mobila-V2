#!/usr/bin/env bash
# ============================================================================
#  MOBILADOR - android-server/build.sh
#  Builds the on-device server module (mobilador.dex).
#
#  Requirements (any one of these paths works):
#    * JDK 8..21        -> javac
#    * Android build-tools (d8) OR the d8.jar that ships in tools/
#
#  Usage:
#     ./build.sh                       # auto-detect everything
#     ./build.sh --api 33              # compile against a specific android.jar
#     JAVAC=/path/to/javac D8=/path/to/d8 ./build.sh
#
#  Output: dist/mobilador.dex   (copy next to Mobilador.exe, folder "server")
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$ROOT/build/android"
DIST="$ROOT/dist"
SRC="$HERE/src"
API="${API:-33}"

JAVAC="${JAVAC:-$(command -v javac || true)}"
D8="${D8:-$(command -v d8 || true)}"
D8_JAR="${D8_JAR:-$ROOT/tools/d8.jar}"
ADB="${ADB:-$(command -v adb || true)}"

# --- locate an android.jar (SDK, or the stubs bundled in this repository) ----
find_android_jar() {
  local candidates=(
    "${ANDROID_JAR:-}"
    "$ANDROID_HOME/platforms/android-$API/android.jar"
    "$ANDROID_SDK_ROOT/platforms/android-$API/android.jar"
    "$HOME/Android/Sdk/platforms/android-$API/android.jar"
    "$HOME/Library/Android/sdk/platforms/android-$API/android.jar"
    "$ROOT/tools/android-stubs/android-$API.jar"
  )
  for c in "${candidates[@]}"; do
    [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}

if [ -z "$JAVAC" ]; then
  echo "ERROR: javac not found. Install a JDK (any of 8, 11, 17, 21 works)." >&2
  echo "       Windows: winget install EclipseAdoptium.Temurin.17.JDK" >&2
  exit 2
fi

ANDROID_JAR="$(find_android_jar || true)"
if [ -z "$ANDROID_JAR" ]; then
  echo "ERROR: android.jar not found." >&2
  echo "       Set ANDROID_JAR=/path/to/android.jar or install the Android SDK platform $API." >&2
  exit 3
fi

echo "  javac       : $JAVAC  ($($JAVAC -version 2>&1 | head -1))"
echo "  android.jar : $ANDROID_JAR"
if [ -n "$D8" ]; then
  echo "  d8          : $D8"
elif [ -f "$D8_JAR" ]; then
  JAVA_EXE="$(dirname "$JAVAC")/java"
  echo "  d8 (jar)    : $D8_JAR via $JAVA_EXE"
else
  echo "ERROR: d8 not found. Install Android build-tools or place d8.jar in tools/." >&2
  exit 4
fi

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$DIST"

echo "  compiling..."
find "$SRC" -name '*.java' > "$OUT/sources.txt"

"$JAVAC" -source 8 -target 8 -nowarn -Xlint:none \
  -bootclasspath "$ANDROID_JAR" -classpath "$ANDROID_JAR" \
  -d "$OUT/classes" @"$OUT/sources.txt" 2>&1 | grep -v "bootstrap class path" || true

if [ ! -d "$OUT/classes/com/mobilador/server" ]; then
  echo "ERROR: compilation produced no classes." >&2
  exit 5
fi

echo "  dexing..."
if [ -n "$D8" ]; then
  "$D8" --release --min-api 21 --lib "$ANDROID_JAR" --output "$OUT" $(find "$OUT/classes" -name '*.class')
else
  "$(dirname "$JAVAC")/java" -jar "$D8_JAR" --release --min-api 21 --lib "$ANDROID_JAR" \
      --output "$OUT" $(find "$OUT/classes" -name '*.class')
fi

if [ ! -f "$OUT/classes.dex" ]; then
  echo "ERROR: dexing failed." >&2
  exit 6
fi

cp "$OUT/classes.dex" "$DIST/mobilador.dex"
cp "$OUT/classes.dex" "$ROOT/dist/server-dex/mobilador.dex" 2>/dev/null || {
  mkdir -p "$ROOT/dist/server-dex" && cp "$OUT/classes.dex" "$ROOT/dist/server-dex/mobilador.dex"; }

SIZE=$(stat -c%s "$DIST/mobilador.dex" 2>/dev/null || stat -f%z "$DIST/mobilador.dex")
echo "  -> dist/mobilador.dex  ($((SIZE/1024)) KB)"

if [ -n "$ADB" ]; then
  echo
  echo "  to push it to a connected phone:"
  echo "     $ADB push dist/mobilador.dex /data/local/tmp/mobilador.dex"
fi
