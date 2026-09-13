param(
    [string]$Version = "0.1.0-ci",
    [switch]$SkipViewer
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot ".." )).Path
$agentRoot = Join-Path $repoRoot "Agent\chatpass_agent"
$agentScript = Join-Path $agentRoot "installer\windows\Build-AgentInstaller.ps1"
$viewerScript = Join-Path $repoRoot "Viewer\chatpass_viewer\installer\windows\Build-ViewerInstaller.ps1"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found on this Windows build host."
}

$vsPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if ([string]::IsNullOrWhiteSpace($vsPath)) {
    throw "Visual Studio C++ build tools were not found."
}

$devShellModule = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $devShellModule
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$vcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
if ([string]::IsNullOrWhiteSpace($vcpkgRoot) -and (Test-Path 'C:\vcpkg')) {
    $vcpkgRoot = 'C:\vcpkg'
}
if ([string]::IsNullOrWhiteSpace($vcpkgRoot)) {
    throw "vcpkg was not found. Set VCPKG_INSTALLATION_ROOT or install it at C:\vcpkg."
}

$iscc = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
if (-not (Test-Path $iscc)) {
    $iscc = 'C:\Program Files\Inno Setup 6\ISCC.exe'
}
if (-not (Test-Path $iscc)) {
    throw "Inno Setup 6 is required to package the installers."
}

$webView2Sdk = Join-Path $agentRoot "external\Microsoft.Web.WebView2.1.0.3912.50"
if (-not (Test-Path (Join-Path $webView2Sdk "build\native\include\WebView2.h"))) {
    $package = Join-Path $webView2Sdk "Microsoft.Web.WebView2.1.0.3912.50.nupkg"
    if (-not (Test-Path $package)) { throw "Pinned WebView2 package was not found: $package" }
    $extractRoot = Join-Path $agentRoot ".ci\webview2"
    $zip = Join-Path $agentRoot ".ci\webview2.zip"
    if (Test-Path $extractRoot) { Remove-Item $extractRoot -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Split-Path $zip -Parent) | Out-Null
    Copy-Item $package $zip -Force
    Expand-Archive -Path $zip -DestinationPath $extractRoot -Force
    Remove-Item $zip -Force
    $webView2Sdk = $extractRoot
}
$env:WEBVIEW2_SDK = $webView2Sdk

Write-Host "Hi5Central CI Windows build"
Write-Host "Visual Studio: $vsPath"
Write-Host "vcpkg: $vcpkgRoot"
Write-Host "Inno Setup: $iscc"
Write-Host "WebView2 SDK: $webView2Sdk"
Write-Host "Version: $Version"

& $agentScript `
    -Clean `
    -Generator "Visual Studio 17 2022" `
    -Platform x64 `
    -VcpkgRoot $vcpkgRoot `
    -VcpkgTriplet "x64-windows-static" `
    -FetchLibDataChannel `
    -InnoSetupCompiler $iscc `
    -AgentVersion $Version

if ($LASTEXITCODE -ne 0) {
    throw "Agent installer build failed with exit code $LASTEXITCODE."
}
if (-not $SkipViewer) {
    & $viewerScript `
        -Clean `
        -Generator "Visual Studio 17 2022" `
        -Platform x64 `
        -InnoSetupCompiler $iscc `
        -AppVersion $Version

    if ($LASTEXITCODE -ne 0) {
        throw "Viewer installer build failed with exit code $LASTEXITCODE."
    }
}

$outputs = @((Join-Path $agentRoot "dist\installer\Hi5CentralAgentSetup.exe"))
if (-not $SkipViewer) {
    $outputs += (Join-Path $repoRoot "Viewer\chatpass_viewer\dist\installer\Hi5CentralViewerSetup.exe")
}

foreach ($file in $outputs) {
    if (-not (Test-Path $file)) { throw "Expected build output missing: $file" }
    $sig = Get-AuthenticodeSignature $file
    $hash = Get-FileHash -Algorithm SHA256 $file
    Write-Host "Output: $file"
    Write-Host "Signature: $($sig.Status)"
    Write-Host "SHA256: $($hash.Hash)"
}

Write-Host "Windows installer build complete."
