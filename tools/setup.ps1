# Build Link's Awakening DX for every console in scripts/targets.tsv, from
# your ROM, using Docker Desktop (Windows; Linux and macOS run setup.sh, which
# does the same). Everything happens in containers, and your ROM never leaves
# this machine.
#
#   -Game <folder or file>   where your ROM is (.gbc, or a .zip holding one)
#   -Yes                     install Docker Desktop if it's missing, no questions
#   -Targets wii,psp         build only these
#
# Each step prints "[n/N] ..." and a failure ends with "Setup stopped: ...",
# which the recomp launcher shows (launcher/launcher.json). A console that
# fails to build doesn't stop the rest; the summary says which, and its log is
# in build-logs\. Finished work is skipped on a rerun.
param([switch]$Yes, [Alias('GameDir')][string]$Game = '', [string]$Targets = '')

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
New-Item -ItemType Directory -Force build-logs, builds | Out-Null
Start-Transcript -Path (Join-Path $Root 'setup.log') -Force | Out-Null

$only = @($Targets -split ',' | Where-Object { $_ })
$rows = @(Get-Content scripts\targets.tsv | Where-Object { $_ -and $_ -notmatch '^#' } | ForEach-Object {
    $c = $_ -split "`t"
    [pscustomobject]@{ T = $c[0]; Image = $c[1]; Cmd = $c[2]; Files = $c[3] -split ' '; State = $c[4] }
} | Where-Object { $only.Count -eq 0 -or $only -contains $_.T })
# $Total, not $N: PowerShell names ignore case, and Step's $n would shadow it.
$Total = 3 + $rows.Count

function Step($n, $t) { Write-Host ''; Write-Host "[$n/$Total] $t" -ForegroundColor Cyan }
function Fail($t) {
    Write-Host ''; Write-Host "Setup stopped: $t" -ForegroundColor Red
    Write-Host "Details are in $Root\setup.log"
    Stop-Transcript | Out-Null; exit 1
}
function Ask($q) { if ($Yes) { Write-Host "$q yes"; return $true }; (Read-Host "$q [y/N]") -match '^(y|yes)$' }
# Run a native command with its output going to $log (UTF-8) or the screen,
# and return only its exit code.
function Exec($log, [string[]]$a) {
    $ErrorActionPreference = 'Continue'   # stderr lines are output here, not errors
    $lines = & $a[0] $a[1..($a.Count - 1)] 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    if ($log) { Set-Content -Encoding utf8 $log $lines } else { $lines | ForEach-Object { Write-Host "  $_" } }
    return $code
}

Step 1 'Checking Docker'
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host '  Docker Desktop runs every console toolchain in a container (about 600 MB, plus WSL 2).'
    if (-not (Ask '  Install Docker Desktop now with winget?')) { Fail 'Docker Desktop is required: https://www.docker.com/products/docker-desktop/' }
    Exec $null @('winget', 'install', '-e', '--id', 'Docker.DockerDesktop', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
    Fail 'Docker Desktop was installed. Start it once (it finishes setting up and may ask to restart), then run this again.'
}
if ((Exec "$env:TEMP\docker-info.txt" @('docker', 'info')) -ne 0) { Fail 'Docker Desktop is not running. Start it, wait for it to say it is running, then run this again.' }
Write-Host "  Docker $(docker version --format '{{.Server.Version}}')"

Step 2 'Checking the source'
if (-not (Test-Path 'LinksAwakening\gbrecomp\runtime\src\gbrt.c')) {
    if (-not (Test-Path '.git')) { Fail 'LinksAwakening\ is empty and this is not a git clone. Clone with --recurse-submodules.' }
    if ((Exec $null @('git', '-c', 'core.longpaths=true', 'submodule', 'update', '--init', '--recursive')) -ne 0) { Fail 'fetching the LinksAwakening submodule failed.' }
}
Write-Host '  LinksAwakening and gb-recompiled are in place.'

Step 3 'Turning your ROM into C'
$rom = @()
if (-not (Test-Path 'LinksAwakening\rom.gbc')) {
    $g = $Game.Trim('"', ' ')
    if (-not $g) {
        if ($Yes) { Fail 'no ROM given. Pass the folder it is in with -Game.' }
        $g = (Read-Host "Folder (or file) with your Link's Awakening DX ROM (.gbc or .zip)").Trim('"', ' ')
    }
    if (-not (Test-Path $g)) { Fail "there is nothing at '$g'." }
    if (Test-Path $g -PathType Leaf) { $g = Split-Path $g }
    $rom = @('-v', "$((Resolve-Path $g).Path):/rom:ro")
}
$log = Join-Path $Root 'build-logs\recomp.log'
$code = Exec $log (@('docker', 'run', '--rm', '-u', '0', '-v', "${Root}:/src") + $rom + @('devkitpro/devkitppc', 'bash', '/src/scripts/recomp.sh'))
$out = Get-Content $log
# Indented: a tool's own "[3/12] ..." line must not look like one of our steps.
$out | Select-Object -Last 8 | ForEach-Object { Write-Host "  $_" }
if ($code -ne 0) {
    $why = ($out | Select-String 'Setup stopped: (.*)' | Select-Object -Last 1)
    if ($why) { Fail $why.Matches[0].Groups[1].Value } else { Fail 'recompiling the ROM failed (the lines above say why).' }
}

$n = 3; $ok = @(); $bad = @()
foreach ($r in $rows) {
    $n++
    Step $n "$($r.T): building (5 to 15 minutes; the first run also downloads its toolchain)"
    $log = Join-Path $Root "build-logs\$($r.T).log"
    if ($r.Image -eq 'linksawakening-android' -and
        (Exec $log @('docker', 'build', '-t', $r.Image, '-f', 'docker/android.Dockerfile', 'docker')) -ne 0) {
        Write-Host '  Building the Android toolchain image failed.'; $bad += $r.T; continue
    }
    if ((Exec $log @('docker', 'run', '--rm', '-u', '0', '-v', "${Root}:/src", '-w', '/src', $r.Image, 'sh', '-c', $r.Cmd)) -eq 0) {
        $dest = New-Item -ItemType Directory -Force "builds\$($r.T)"
        foreach ($f in $r.Files) {
            Get-ChildItem "build-$($r.T)\$f" -File -ErrorAction SilentlyContinue |
                ForEach-Object { Copy-Item $_.FullName (Join-Path $dest ($_.Name -replace '^la360', 'linksawakening')) }
        }
        $ok += $r.T; Write-Host "  Built. $($r.State)."
    } else {
        $bad += $r.T; Write-Host "  Failed. The last lines of build-logs\$($r.T).log:"
        Get-Content $log -Tail 5 | ForEach-Object { Write-Host "    $_" }
    }
}

$summary = @("Link's Awakening DX console builds ($(Get-Date -Format yyyy-MM-dd)). How to install each: docs/BUILDING.md.", '')
foreach ($r in $rows) {
    $summary += if ($ok -contains $r.T) { "  $($r.T): built. $($r.State)." } else { "  $($r.T): FAILED (build-logs\$($r.T).log)" }
}
Set-Content builds\README.txt $summary
Write-Host ''; $summary | ForEach-Object { Write-Host $_ }
if ($ok.Count -eq 0) { Fail 'no console built. The logs in build-logs\ say why.' }

Set-Content -Encoding ascii 'Console builds (recomp).cmd' "@echo off`r`nrem Opens the folder with the console builds.`r`nstart """" ""%~dp0builds"""
Write-Host ''
$failed = if ($bad.Count) { " (failed: $($bad -join ', '))" } else { '' }
Write-Host "Done: $($ok.Count) of $($rows.Count) consoles built$failed. They're in builds\." -ForegroundColor Green
Stop-Transcript | Out-Null
