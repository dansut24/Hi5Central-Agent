# Hi5Central VDD development-only code signing — never use for production.
# Runs on an ephemeral Windows CI runner after an independently successful WDK build.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$artifacts = Join-Path $root 'artifacts'
$catalog = Join-Path $artifacts 'Hi5VirtualDisplay.cat'
$inf = Join-Path $artifacts 'Hi5VirtualDisplay.inf'
$dll = Join-Path $artifacts 'Hi5VirtualDisplay.dll'
foreach ($file in @($catalog,$inf,$dll)) { if (-not (Test-Path $file)) { throw "Missing $file" } }
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Hi5Central Virtual Display DEV TEST ONLY' -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable -HashAlgorithm SHA256 -KeyAlgorithm RSA -KeyLength 3072 -NotAfter (Get-Date).AddDays(14)
if (!$cert.HasPrivateKey) { throw 'No development signing key created' }
$public = Join-Path $artifacts 'Hi5VirtualDisplay-DEV-TEST.cer'
Export-Certificate -Cert $cert -FilePath $public -Force | Out-Null
$src= Join-Path $root '.build-src'
$signTools = @(Get-ChildItem (Join-Path $src 'packages') -Recurse -File -Filter 'signtool.exe' -ErrorAction SilentlyContinue | Where-Object { $_.Directory.Name -ieq 'x64' -and $_.FullName -like '*WDK.x64*' })
if ($signTools.Count -eq 0) {
  $signTools = @(Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue | Where-Object { $_.Directory.Name -ieq 'x64' })
}
if ($signTools.Count -eq 0) { throw 'Required SignTool x64 not found' }
$signTool = $signTools[0].FullName
# The catalog was generated AFTER compilation, so its DLL/INF hashes remain intact.
# Do not Authenticode-sign the DLL *after* catalog generation: that would change its hash.
& $signTool sign /v /fd SHA256 /sha1 $cert.Thumbprint /s My $catalog
if ($LASTEXITCODE -ne 0) { throw "SignTool signing failed: $LASTEXITCODE" }
# Verify against developer test root *on this ephemeral CI runner only*.
Import-Certificate -FilePath $public -CertStoreLocation Cert:\CurrentUser\Root | Out-Null
Import-Certificate -FilePath $public -CertStoreLocation Cert:\CurrentUser\TrustedPublisher | Out-Null
& $signTool verify /pa /v $catalog
if ($LASTEXITCODE -ne 0) { throw "Developer signature verification failed: $LASTEXITCODE" }
$manifest = [ordered]@{
 schemaVersion=1
 purpose='Hi5Central Virtual Display dev testing ONLY'
 sourceCommit=$env:GITHUB_SHA
 sourceWorkflowRun=$env:GITHUB_RUN_ID
 certificateThumbprint=$cert.Thumbprint
 certificateExpiryUTC=$cert.NotAfter.ToUniversalTime().ToString('o')
 developerSelfSigned=$true
 privateKeyIncluded=$false
 requiresWindowsTestSigning=$true
 secureBootChangeAutomated=$false
 catalogSha256=(Get-FileHash $catalog -Algorithm SHA256).Hash.ToLowerInvariant()
 publicCertificateSha256=(Get-FileHash $public -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $artifacts 'DEV-TEST-MANIFEST.json') -Encoding utf8
Get-ChildItem $artifacts -File | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } | Sort-Object Name | ForEach-Object {
  "$($_.Name) $((Get-FileHash -Algorithm SHA256 -Path $_.FullName).Hash.ToLowerInvariant())"
} | Set-Content (Join-Path $artifacts 'SHA256SUMS.txt') -Encoding ascii
Write-Host "DEV_SIGNED_CERT_THUMBPRINT=$($cert.Thumbprint)"
Write-Host 'Development catalog signed, verified, and public certificate exported. Private key was not exported.'
