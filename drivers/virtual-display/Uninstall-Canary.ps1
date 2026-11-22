#requires -RunAsAdministrator
# Hi5Central Virtual Display TEST-ONLY cleanup (does not change BCD, Secure Boot, or physical displays).
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$name = 'Hi5VirtualDisplayHost'
$folder = Join-Path $env:ProgramFiles 'Hi5Central\VirtualDisplay'
$service = Get-Service -Name $name -ErrorAction SilentlyContinue
if ($service) {
    if ($service.Status -ne 'Stopped') {
        Stop-Service -Name $name -ErrorAction Stop
        (Get-Service -Name $name).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(30))
    }
    & sc.exe delete $name
    if ($LASTEXITCODE -ne 0) { throw "Failed to remove $name service: $LASTEXITCODE" }
}
$manifestPath = Join-Path $folder 'DEV-TEST-MANIFEST.json'
if (Test-Path $manifestPath) {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.developerSelfSigned -eq $true -and $manifest.certificateThumbprint -match '^[A-Fa-f0-9]{40}$') {
        $thumb = ([string]$manifest.certificateThumbprint).ToUpperInvariant()
        foreach ($store in @('Root','TrustedPublisher')) {
            $path = 'Cert:\LocalMachine\' + $store + '\' + $thumb
            if (Test-Path $path) { Remove-Item -LiteralPath $path -Force -ErrorAction Stop }
        }
        Write-Host "Removed scoped development certificate trust: $thumb"
    }
}
if (Test-Path $folder) { Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host 'Hi5Central VDD canary service stopped/removed; dev certificate trust cleared.'
Write-Host 'Driver Store copy remains for audit; published INF can be removed separately by pnputil after confirming no device depends on it.'
Write-Host 'Windows TESTSIGNING setting was not changed; it must be restored separately after all test drivers are removed.'
