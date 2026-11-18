# Sign kwifi.exe with a locally trusted code-signing certificate.
#
# Windows shows "unknown publisher" for any unsigned program that asks for
# administrator rights. The only real fix is a signature, so this script
# creates a self-signed code-signing certificate, trusts it on this machine and
# signs the exe. The UAC prompt then shows "kwifi" instead of an anonymous blob.
#
# Run once with administrator rights:
#   powershell -ExecutionPolicy Bypass -File tools\sign.ps1
#
# -Remove  delete the certificate and the trust entries again
# -Export  also write kwifi-codesign.cer next to the exe, so other machines can
#          trust it the same way

param(
  [string]$Exe = (Join-Path (Split-Path $PSScriptRoot -Parent) 'kwifi.exe'),
  [string]$Subject = 'CN=kwifi, O=kwifi, C=CN',
  [switch]$Remove,
  [switch]$Export
)

$ErrorActionPreference = 'Stop'

function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
  throw 'need administrator rights: open PowerShell as administrator and retry'
}

if ($Remove) {
  Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher, Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -eq $Subject } |
    ForEach-Object { Remove-Item $_.PSPath -Force -ErrorAction SilentlyContinue }
  Write-Host 'removed the kwifi signing certificate and trust entries'
  return
}

if (-not (Test-Path $Exe)) { throw "not found: $Exe" }

$cert = Get-ChildItem Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey } |
        Select-Object -First 1

if (-not $cert) {
  Write-Host '==> creating code-signing certificate'
  $cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -CertStoreLocation Cert:\LocalMachine\My `
    -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA -KeyLength 3072 `
    -NotAfter (Get-Date).AddYears(10) `
    -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
} else {
  Write-Host "==> reusing certificate $($cert.Thumbprint)"
}

# Root lets the chain validate; TrustedPublisher silences the unknown-publisher
# wording in UAC on this machine
foreach ($store in 'Root', 'TrustedPublisher') {
  $path = "Cert:\LocalMachine\$store"
  $existing = Get-ChildItem $path -ErrorAction SilentlyContinue |
              Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
  if (-not $existing) {
    Write-Host "==> importing into $store"
    $tmp = [System.IO.Path]::GetTempFileName() + '.cer'
    Export-Certificate -Cert $cert -FilePath $tmp -Type CERT | Out-Null
    Import-Certificate -FilePath $tmp -CertStoreLocation $path | Out-Null
    Remove-Item $tmp -Force
  }
}

Write-Host "==> signing $Exe"
$sig = Set-AuthenticodeSignature -FilePath $Exe -Certificate $cert `
        -TimestampServer 'http://timestamp.digicert.com' -HashAlgorithm SHA256
if ($sig.Status -ne 'Valid') {
  Write-Warning "signature status: $($sig.Status) - $($sig.StatusMessage)"
} else {
  Write-Host "signed (thumbprint $($cert.Thumbprint))"
}

if ($Export) {
  $out = Join-Path (Split-Path $Exe) 'kwifi-codesign.cer'
  Export-Certificate -Cert $cert -FilePath $out -Type CERT | Out-Null
  Write-Host "certificate exported: $out"
  Write-Host 'on another machine: double-click it and install into Trusted Root Certification Authorities'
}

Get-AuthenticodeSignature $Exe |
  Select-Object Status, @{n = 'Signer'; e = { $_.SignerCertificate.Subject } } |
  Format-List
