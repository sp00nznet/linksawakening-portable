#!/bin/bash
# Build the Android APK (x86_64; add arm64-v8a to abiFilters in
# android/app/build.gradle for phones).
#
# Android reuses the SDL2 backend (platform_sdl.cpp). SDL2 isn't vendored:
# this stages an SDL2 source tree into android/ (gitignored) and runs Gradle.
# Runs in the image from docker/android.Dockerfile, which sets ANDROID_HOME
# and SDL2_SRC; anywhere else, set those two yourself.
#
# Usage:  bash build_android.sh [output-dir]
set -e
. "$(dirname "$0")/env.sh"
OUT="${1:-$ROOT/build-android}"
: "${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
: "${SDL2_SRC:?set SDL2_SRC to an SDL2 2.32.x source tree}"
P="$ROOT/android"
mkdir -p "$OUT"

echo '[1/3] Staging SDL2 source'
mkdir -p "$P/app/jni/SDL" "$P/app/src/main/java"
cp -r "$SDL2_SRC"/. "$P/app/jni/SDL/"
rm -rf "$P/app/jni/SDL/android-project"
cp -r "$SDL2_SRC/android-project/app/src/main/java/." "$P/app/src/main/java/"
echo "sdk.dir=$ANDROID_HOME" > "$P/local.properties"

echo '[2/3] gradle assembleDebug (compiles the ~109 MB rom.c: slow)'
# Gradle's own cache goes under OUT, so a rerun doesn't download it again.
GRADLE_USER_HOME="$OUT/.gradle" bash "$P/gradlew" -p "$P" assembleDebug --console=plain

cp "$P/app/build/outputs/apk/debug/app-debug.apk" "$OUT/linksawakening.apk"
echo "[3/3] APK: $OUT/linksawakening.apk"
