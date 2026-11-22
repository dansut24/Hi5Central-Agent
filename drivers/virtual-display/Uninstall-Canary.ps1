#requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'
$service = Get-Service -Name 'Hi5VirtualDisplayHost' -ErrorAction SilentlyContinue
if ($service) {
    if ($service.Status -ne 'Stopped') {
        Stop-Service -Name 'Hi5VirtualDisplayHost' -ErrorAction Stop
        (Get-Service -Name 'Hi5VirtualDisplayHost').WaitForStatus('Stopped',[TimeSpan]::FromSeconds(30))
    }
    & sc.exe delete Hi5VirtualDisplayHost
    if ($LASTEXITCODE -ne 0) { throw "Failed to remove service: $LASTEXITCODE" }
}
Write-Host 'Virtual display disabled and host service removed.'
Write-Host 'Driver Store package is retained for safety. Identify its published INF with pnputil /enum-drivers before removing it manually.'
