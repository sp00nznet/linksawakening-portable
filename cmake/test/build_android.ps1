# build_android.ps1 — assemble + build the Link's Awakening Android APK.
#
# Android reuses the SDL2 backend (runtime/src/platform_sdl.cpp) — the same
# approach as the PS4 port. SDL2 itself is NOT vendored in this repo: supply
# an SDL2 source tree (2.32.x) and the script copies its native source +
# Java glue into android/app/jni/SDL and android/app/src/main/java (both
# gitignored). The on-screen touch controls live in platform_sdl.cpp behind
# #ifdef __ANDROID__.
#
# Requires: Android SDK + NDK 27 + CMake (via sdkmanager), JDK 17+.
# Output:   build-android/linksawakening.apk
#
# Usage:  powershell -File cmake/test/build_android.ps1 [-SdlSrc <path>] [-SdkDir <path>]

param(
    [string]$SdlSrc = "D:\ports\android-build\SDL2-2.32.10",
    [string]$SdkDir = "D:\Android\Sdk"
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$proj = Join-Path $repo "android"

if (-not (Test-Path "$SdlSrc\include\SDL.h")) {
    throw "SDL2 source not found at $SdlSrc - download the SDL2-2.32.x source release from libsdl.org"
}
if (-not (Test-Path "$SdkDir\platform-tools")) {
    throw "Android SDK not found at $SdkDir"
}

# Point Gradle at the SDK (local.properties is gitignored).
Set-Content -Path "$proj\local.properties" `
    -Value ("sdk.dir=" + $SdkDir.Replace('\','\\')) -Encoding ascii

# Stage SDL2 native source + Java glue into the project (both gitignored).
Write-Host "[1/3] Staging SDL2 source ..."
$null = robocopy $SdlSrc "$proj\app\jni\SDL" /E /XD "android-project" /NFL /NDL /NJH /NJS /NP
$null = robocopy "$SdlSrc\android-project\app\src\main\java" `
                 "$proj\app\src\main\java" /E /NFL /NDL /NJH /NJS /NP

# Build (CMake compiles rom.c + runtime + platform_sdl.cpp into libmain.so).
Write-Host "[2/3] gradle assembleDebug (compiles the ~115 MB rom.c - slow) ..."
& "$proj\gradlew.bat" -p $proj assembleDebug --console=plain
if ($LASTEXITCODE -ne 0) { throw "gradle build failed" }

$apk = "$proj\app\build\outputs\apk\debug\app-debug.apk"
$out = Join-Path $repo "build-android"
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item $apk "$out\linksawakening.apk" -Force
Write-Host ("[3/3] APK: build-android\linksawakening.apk  (" +
            [math]::Round((Get-Item $apk).Length/1MB,1) + " MB)")
