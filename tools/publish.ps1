# Publish kwifi to GitHub in one shot.
#
# Run this in PowerShell on a machine that can reach github.com:
#
#   powershell -ExecutionPolicy Bypass -File tools\publish.ps1
#
# It does, in order:
#   1. point git and gh at the local proxy (Clash/Mihomo on 7897 by default)
#   2. check gh is logged in, and offer to start the login flow
#   3. create the public repository and push main
#   4. tag v0.6.0 and push the tag, which triggers the release workflow
#
# Re-running is safe: an existing repository, remote or tag is detected and skipped.

param(
  [string]$Repo = 'kwifi',
  [string]$Tag = 'v0.6.0',
  [string]$Proxy = 'http://127.0.0.1:7897',
  [switch]$SkipTag
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

# run a native command and return its exit code without tripping $ErrorActionPreference
function Invoke-Quiet {
  param([scriptblock]$Block)
  $prev = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { & $Block 2>&1 | Out-Null } catch { }
  finally { $ErrorActionPreference = $prev }
  return $LASTEXITCODE
}

function Say($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Ok($msg)  { Write-Host "    $msg" -ForegroundColor Green }
function Warn($msg){ Write-Host "    $msg" -ForegroundColor Yellow }

# ---------------------------------------------------------------- proxy

Say "proxy $Proxy"
$env:HTTPS_PROXY = $Proxy
$env:HTTP_PROXY = $Proxy

$proxyHost = ([Uri]$Proxy).Host
$proxyPort = ([Uri]$Proxy).Port
$alive = Test-NetConnection -ComputerName $proxyHost -Port $proxyPort -InformationLevel Quiet -WarningAction SilentlyContinue
if (-not $alive) {
  Warn "$proxy is not accepting connections."
  Warn 'Start your proxy client, or pass the right address: -Proxy http://127.0.0.1:7890'
  exit 1
}
if ((Invoke-Quiet { & git config --global "http.https://github.com.proxy" $Proxy }) -eq 0) {
  Ok 'git will use the proxy for github.com'
} else {
  Warn 'could not write the global git config; setting it for this repo instead'
  & git config "http.https://github.com.proxy" $Proxy
  Ok 'repository-local proxy configured'
}

# ---------------------------------------------------------------- auth

Say 'checking gh login'
if ((Invoke-Quiet { & gh auth status }) -ne 0) {
  Warn 'not logged in. The login flow needs a browser.'
  Write-Host ''
  Write-Host '    gh auth login      # GitHub.com -> HTTPS -> Yes -> Login with a web browser' -ForegroundColor White
  Write-Host ''
  $go = Read-Host 'Start it now? [Y/n]'
  if ($go -eq '' -or $go -match '^[Yy]') {
    & gh auth login
    if ($LASTEXITCODE -ne 0) { throw 'login failed' }
  } else {
    throw 'login is required before publishing'
  }
}
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$user = (& gh api user --jq .login 2>$null | Select-Object -First 1)
$ErrorActionPreference = $prev
if (-not $user) { throw 'could not read the GitHub account name; check the proxy' }
$user = $user.Trim()
Ok "logged in as $user"

# ---------------------------------------------------------------- repo

Say "repository $user/$Repo"
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$existing = (& gh repo view "$user/$Repo" --json name 2>$null | Out-String)
$ErrorActionPreference = $prev
if ($existing -match '"name"') {
  Ok 'already exists, reusing it'
  $remotes = & git remote
  if ($remotes -notcontains 'origin') {
    & git remote add origin "https://github.com/$user/$Repo.git"
    Ok 'origin added'
  }
} else {
  & gh repo create $Repo --public --source=. --remote=origin --description 'Windows hotspot console: per-device limits, priority, stealth mode. One 1 MB exe, no runtime.' --push
  if ($LASTEXITCODE -ne 0) { throw 'repository creation failed' }
  Ok 'created and pushed'
}

if ((& git status --porcelain)) {
  Warn 'there are uncommitted changes; they will NOT be published'
  & git status --short
}

Say 'pushing main'
& git push -u origin main 2>&1 | Select-Object -Last 3
if ($LASTEXITCODE -ne 0) { throw 'push failed' }

# ---------------------------------------------------------------- tag

if (-not $SkipTag) {
  Say "tag $Tag"
  $has = & git tag
  if ($has -contains $Tag) {
    Ok 'tag already exists locally'
  } else {
    & git tag -a $Tag -m "kwifi $Tag"
    Ok 'tag created'
  }
  & git push origin $Tag 2>&1 | Select-Object -Last 3
  Ok 'tag pushed, the release workflow is building'
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host "  repository : https://github.com/$user/$Repo"
Write-Host "  releases   : https://github.com/$user/$Repo/releases"
Write-Host "  actions    : https://github.com/$user/$Repo/actions"
Write-Host ''
Write-Host 'Watch the build with:  gh run watch'
