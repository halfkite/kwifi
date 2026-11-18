# Build kwifi.exe: native shell + embedded UI + backend (no Node runtime).
#
#   kwifi.exe   - tray, WebView2 window and the HTTP backend, ~1 MB
#   ttlfix.exe  - data plane helper (embedded into kwifi.exe as well)
#
# Requires zig in PATH. Usage:
#   powershell -ExecutionPolicy Bypass -File native\build.ps1

param(
  [string]$Version = '0.6.0.0',
  # rebuild ttlfix.exe even if a copy is already present (CI should always do this)
  [switch]$RebuildHelper
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root '.zig\global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root '.zig\local'

if (-not (Get-Command zig -ErrorAction SilentlyContinue)) {
  throw 'zig not found in PATH (https://ziglang.org/download/)'
}

Push-Location $PSScriptRoot
try {
  if ($RebuildHelper -or -not (Test-Path (Join-Path $PSScriptRoot 'win\ttlfix.exe'))) {
    Write-Host '==> ttlfix.exe'
    zig cc -target x86_64-windows-gnu -O2 -Wall -I include -o win\ttlfix.exe ttlfix.c win\WinDivert.lib
    if ($LASTEXITCODE -ne 0) { throw "ttlfix build failed (exit $LASTEXITCODE)" }
  }

  Write-Host '==> embedded.h'
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gen-embedded.ps1')
  if ($LASTEXITCODE -ne 0) { throw 'embedded.h generation failed' }

  Write-Host '==> kwifi.exe'
  zig cc -target x86_64-windows-gnu -O2 -Wall -municode -c kwifi.c -o kwifi.o
  if ($LASTEXITCODE -ne 0) { throw "kwifi.c compile failed (exit $LASTEXITCODE)" }

  zig cc -target x86_64-windows-gnu -O2 -Wall -municode -c server.c -o server.o
  if ($LASTEXITCODE -ne 0) { throw "server.c compile failed (exit $LASTEXITCODE)" }

  zig c++ -target x86_64-windows-gnu -O2 -Wno-nullability-completeness -I wv2shim -c kgwebview.cpp -o kgwebview.o
  if ($LASTEXITCODE -ne 0) { throw "kgwebview.cpp compile failed (exit $LASTEXITCODE)" }

  zig c++ -target x86_64-windows-gnu -O2 -municode -o ..\kwifi.exe kwifi.o server.o kgwebview.o `
    -lws2_32 -lshell32 -luser32 -lgdi32 -lole32 -loleaut32 -ladvapi32
  if ($LASTEXITCODE -ne 0) { throw "kwifi link failed (exit $LASTEXITCODE)" }

  $exePath = Join-Path $root 'kwifi.exe'

  Write-Host '==> resources'
  # always rebuild: a stale respatch.exe silently keeps the old version strings
  zig cc -target x86_64-windows-gnu -O2 -municode -o respatch.exe respatch.c
  if ($LASTEXITCODE -ne 0) { throw "respatch build failed (exit $LASTEXITCODE)" }
  if (-not (Test-Path (Join-Path $PSScriptRoot 'app.ico'))) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gen-icon.ps1')
  }
  & (Join-Path $PSScriptRoot 'respatch.exe') $exePath (Join-Path $PSScriptRoot 'app.manifest') (Join-Path $PSScriptRoot 'app.ico') $Version 'kwifi hotspot console'
  if ($LASTEXITCODE -ne 0) { throw "resource patch failed (exit $LASTEXITCODE)" }
  $bytes = [System.IO.File]::ReadAllBytes($exePath)
  $peOff = [BitConverter]::ToInt32($bytes, 0x3C) + 24 + 68
  if ([BitConverter]::ToUInt16($bytes, $peOff) -ne 2) {
    $bytes[$peOff] = 2
    $bytes[$peOff + 1] = 0
    [System.IO.File]::WriteAllBytes($exePath, $bytes)
    Write-Host '    subsystem -> windows (no console window)'
  }

  Get-Item $exePath, (Join-Path $PSScriptRoot 'win\ttlfix.exe') |
    Select-Object FullName, @{n = 'KB'; e = { [math]::Round($_.Length / 1KB) } }
} finally {
  Pop-Location
}


