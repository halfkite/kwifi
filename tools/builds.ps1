# List or restore the per-build archives that native\build.ps1 writes.
#
#   powershell -ExecutionPolicy Bypass -File tools\builds.ps1
#       list every archived build, newest first
#
#   powershell -ExecutionPolicy Bypass -File tools\builds.ps1 -Restore 20261118-145023
#       copy that build back to kwifi.exe in the repository root
#
#   powershell -ExecutionPolicy Bypass -File tools\builds.ps1 -Prune 20
#       keep the newest 20 archives and delete the rest

param(
  [string]$Restore = '',
  [int]$Prune = 0,
  [string]$ArchiveRoot = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $ArchiveRoot) { $ArchiveRoot = Join-Path $root 'builds' }

if (-not (Test-Path $ArchiveRoot)) {
  Write-Host "no builds yet: $ArchiveRoot"
  return
}

$builds = Get-ChildItem $ArchiveRoot -Directory | Sort-Object Name -Descending

if ($Prune -gt 0) {
  if ($Prune -ge $builds.Count) {
    Write-Host "nothing to prune ($($builds.Count) builds, keeping $Prune)"
    return
  }
  $builds | Select-Object -Skip $Prune | ForEach-Object {
    Remove-Item $_.FullName -Recurse -Force
    Write-Host "removed $($_.Name)"
  }
  return
}

if ($Restore) {
  $match = $builds | Where-Object { $_.Name -like "$Restore*" } | Select-Object -First 1
  if (-not $match) { throw "no build matching '$Restore'" }

  $src = Join-Path $match.FullName 'kwifi.exe'
  if (-not (Test-Path $src)) { throw "that archive has no kwifi.exe: $($match.FullName)" }

  $dst = Join-Path $root 'kwifi.exe'
  try {
    Copy-Item $src $dst -Force -ErrorAction Stop
  } catch {
    # the running program holds its own exe; do not leave the user stuck
    $alt = Join-Path $root "kwifi-$($match.Name).exe"
    Copy-Item $src $alt -Force
    Write-Host "kwifi.exe is in use (close the program first)."
    Write-Host "copied to $alt instead"
    return
  }
  Write-Host "restored $($match.Name) -> $dst"

  $info = Join-Path $match.FullName 'build.txt'
  if (Test-Path $info) { Get-Content $info }
  return
}

if (-not $builds) { Write-Host "no builds in $ArchiveRoot"; return }

$rows = foreach ($b in $builds) {
  $info = Join-Path $b.FullName 'build.txt'
  $map = @{}
  if (Test-Path $info) {
    foreach ($line in Get-Content $info) {
      $i = $line.IndexOf(':')
      if ($i -gt 0) { $map[$line.Substring(0, $i).Trim()] = $line.Substring($i + 1).Trim() }
    }
  }
  $exe = Join-Path $b.FullName 'kwifi.exe'
  [pscustomobject]@{
    build  = $b.Name
    version = $map['version']
    commit = $map['commit']
    size   = if (Test-Path $exe) { '{0:N0} KB' -f ((Get-Item $exe).Length / 1KB) } else { '-' }
    sha256 = if ($map['sha256']) { $map['sha256'].Substring(0, 12) } else { '-' }
  }
}
$rows | Format-Table -AutoSize
Write-Host "$($builds.Count) build(s) in $ArchiveRoot"
