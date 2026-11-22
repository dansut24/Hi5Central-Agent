#requires -RunAsAdministrator
[CmdletBinding()]
param([string]$PackageDir = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$inf = Join-Path $PackageDir 'Hi5VirtualDisplay.inf'
$cat = Join-Path $PackageDir 'Hi5VirtualDisplay.cat'
$exe = Join-Path $PackageDir 'Hi5DisplayHost.exe'
foreach($p in @($inf,$cat,$exe)) { if(-not (Test-Path $p)) { throw "Missing signed driver package component: $p" } }
# This script deliberately refuses to install an unsigned build on a normal Windows endpoint.
$sig = Get-AuthenticodeSignature -FilePath $cat
if ($sig.Status -ne 'Valid') { throw "Driver catalog is not trusted/signed. Obtain Microsoft-accepted production signing first. No changes were made." }
if (Get-Service -Name 'Hi5VirtualDisplayHost' -ErrorAction SilentlyContinue) { throw 'Hi5VirtualDisplayHost already exists; refusing to replace it.' }
& pnputil.exe /add-driver $inf /install
if ($LASTEXITCODE -ne 0) { throw "Driver staging failed with exit code $LASTEXITCODE" }
$folder = Join-Path $env:ProgramFiles 'Hi5Central\VirtualDisplay'
New-Item -ItemType Directory -Force -Path $folder | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $folder 'Hi5DisplayHost.exe') -Force
$bin = Join-Path $folder 'Hi5DisplayHost.exe'
New-Service -Name 'Hi5VirtualDisplayHost' -DisplayName 'Hi5Central Virtual Display (Canary)' -Description 'Manages the virtual monitor used by Hi5Central remote access.' -BinaryPathName ('"' + $bin + '"') -StartupType Manual | Out-Null
Write-Host 'Canary staged with manual start. To activate: Start-Service Hi5VirtualDisplayHost'
Write-Host 'To deactivate: Stop-Service Hi5VirtualDisplayHost'
