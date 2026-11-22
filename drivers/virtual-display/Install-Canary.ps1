#requires -RunAsAdministrator
# Hi5Central virtual display TEST-ONLY install. Never enables Windows test signing or changes Secure Boot.
[CmdletBinding()]
param(
    [string]$PackageDir = $PSScriptRoot,
    [switch]$TrustDevelopmentCertificate
)
$ErrorActionPreference = 'Stop'
$inf = Join-Path $PackageDir 'Hi5VirtualDisplay.inf'
$cat = Join-Path $PackageDir 'Hi5VirtualDisplay.cat'
$exe = Join-Path $PackageDir 'Hi5DisplayHost.exe'
$dll = Join-Path $PackageDir 'Hi5VirtualDisplay.dll'
$publicCert = Join-Path $PackageDir 'Hi5VirtualDisplay-DEV-TEST.cer'
$manifestPath = Join-Path $PackageDir 'DEV-TEST-MANIFEST.json'
foreach ($p in @($inf,$cat,$exe,$dll,$publicCert,$manifestPath)) {
    if (-not (Test-Path $p)) { throw "Missing required development package component: $p" }
}
if (-not $TrustDevelopmentCertificate) {
    throw 'Explicit -TrustDevelopmentCertificate is required. This imports a self-signed certificate into LocalMachine Trusted Root and Trusted Publishers on a test machine ONLY.'
}
if ((Get-Service Hi5VirtualDisplayHost -ErrorAction SilentlyContinue)) {
    throw 'Hi5VirtualDisplayHost already exists. Refusing to replace it.'
}
$boot = (& bcdedit.exe /enum '{current}' 2>&1 | Out-String)
if ($boot -notmatch '(?im)^\s*testsigning\s+Yes\s*$') {
    throw 'Windows test-signing mode is not enabled. This installer will not alter boot security settings.'
}
try { if (Confirm-SecureBootUEFI -ErrorAction Stop) { throw 'Secure Boot must remain untouched: dev driver install refused.' } } catch {
    if ($_.Exception.Message -like '*Secure Boot must remain*') { throw }
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if (-not $manifest.developerSelfSigned -or $manifest.privateKeyIncluded) { throw 'Unexpected or unsafe dev manifest' }
$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($publicCert)
$thumb = $cert.Thumbprint.ToUpperInvariant()
if ($thumb -ne ([string]$manifest.certificateThumbprint).ToUpperInvariant()) { throw 'Test certificate thumbprint mismatch' }
if ($cert.NotAfter.ToUniversalTime() -le (Get-Date).ToUniversalTime()) { throw 'Development certificate has expired' }
if ((Get-FileHash $publicCert -Algorithm SHA256).Hash.ToLowerInvariant() -ne ([string]$manifest.publicCertificateSha256).ToLowerInvariant()) {
    throw 'Development certificate hash mismatch'
}
if ((Get-FileHash $cat -Algorithm SHA256).Hash.ToLowerInvariant() -ne ([string]$manifest.catalogSha256).ToLowerInvariant()) {
    throw 'Driver catalog hash mismatch'
}
$signer = (Get-AuthenticodeSignature -FilePath $cat).SignerCertificate
if ($null -eq $signer -or $signer.Thumbprint.ToUpperInvariant() -ne $thumb) { throw 'Driver catalog is not signed by the supplied development certificate' }
$rootPath = 'Cert:\LocalMachine\Root\' + $thumb
$publisherPath = 'Cert:\LocalMachine\TrustedPublisher\' + $thumb
$createdRoot = -not (Test-Path $rootPath)
$createdPublisher = -not (Test-Path $publisherPath)
$started = $false
try {
    if ($createdRoot) { Import-Certificate -FilePath $publicCert -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null }
    if ($createdPublisher) { Import-Certificate -FilePath $publicCert -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null }
    $sig = Get-AuthenticodeSignature -FilePath $cat
    if ($sig.Status -ne 'Valid') { throw "Developer-signed catalog is not trusted: $($sig.Status)" }
    & pnputil.exe /add-driver $inf /install
    if ($LASTEXITCODE -ne 0) { throw "PnP driver staging failed with exit code $LASTEXITCODE" }
    $folder = Join-Path $env:ProgramFiles 'Hi5Central\VirtualDisplay'
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $folder 'Hi5DisplayHost.exe') -Force
    Copy-Item -LiteralPath $publicCert -Destination (Join-Path $folder 'Hi5VirtualDisplay-DEV-TEST.cer') -Force
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $folder 'DEV-TEST-MANIFEST.json') -Force
    $bin = Join-Path $folder 'Hi5DisplayHost.exe'
    New-Service -Name 'Hi5VirtualDisplayHost' -DisplayName 'Hi5Central Virtual Display (CANARY)' -Description 'Development-only Windows virtual monitor for Hi5Central remote access.' -BinaryPathName ('"' + $bin + '"') -StartupType Manual | Out-Null
    Write-Host 'CANARY_INSTALLED: unsigned-dev-code-not-permitted; trust applied and driver package staged.'
    Write-Host "DEV_CERT_THUMBPRINT=$thumb"
    Write-Host 'Service is MANUAL and STOPPED. Start only during an attended canary test.'
} catch {
    if ($createdPublisher) { Remove-Item -LiteralPath $publisherPath -Force -ErrorAction SilentlyContinue }
    if ($createdRoot) { Remove-Item -LiteralPath $rootPath -Force -ErrorAction SilentlyContinue }
    throw
}
