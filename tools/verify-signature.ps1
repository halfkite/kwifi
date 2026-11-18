# Show the Authenticode state of kwifi.exe in a form users can paste into an
# issue: signature status, signer, timestamp, and the file hash.
#
#   powershell -ExecutionPolicy Bypass -File tools\verify-signature.ps1
#   powershell -ExecutionPolicy Bypass -File tools\verify-signature.ps1 -Path .\kwifi.exe

param(
  [string]$Path = (Join-Path (Split-Path $PSScriptRoot -Parent) 'kwifi.exe')
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Path)) { throw "not found: $Path" }
$file = Get-Item $Path

Write-Host ''
Write-Host "file    : $($file.FullName)"
Write-Host "size    : $($file.Length) bytes"
Write-Host "sha256  : $((Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLower())"

$vi = $file.VersionInfo
Write-Host "product : $($vi.ProductName) $($vi.FileVersion)"
Write-Host "desc    : $($vi.FileDescription)"

$sig = Get-AuthenticodeSignature $file.FullName
Write-Host "signature: $($sig.Status)"

switch ($sig.Status) {
  'Valid' {
    Write-Host "signer  : $($sig.SignerCertificate.Subject)"
    Write-Host "issuer  : $($sig.SignerCertificate.Issuer)"
    Write-Host "expires : $($sig.SignerCertificate.NotAfter)"
    if ($sig.TimeStamperCertificate) {
      Write-Host "timestamp: $($sig.TimeStamperCertificate.Subject)"
    }
    Write-Host ''
    Write-Host 'OK - the file was signed and the certificate chain validates on this machine.'
  }
  'NotSigned' {
    Write-Host ''
    Write-Host 'This build is NOT signed.'
    Write-Host 'Compare the sha256 above with the SHA256SUMS.txt published next to the release.'
    Write-Host 'Windows shows "unknown publisher" for unsigned programs - that is expected.'
  }
  default {
    Write-Host "reason  : $($sig.StatusMessage)"
    Write-Host ''
    Write-Host 'The signature exists but does not validate here. Either the file was modified'
    Write-Host 'after signing, or the root certificate is not trusted on this machine.'
  }
}

Write-Host ''
