param(
    [switch]$Clean,
    [string]$Configuration = "Release",
    [string]$Generator = "Visual Studio 18 2026",
    [string]$Platform = "x64",
    [string]$InnoSetupCompiler = "",
    [string]$AppVersion = "1.0.0"
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$BuildDir = Join-Path $RepoRoot "build"
$DistDir = Join-Path $RepoRoot "dist\viewer"
$InstallerOutDir = Join-Path $RepoRoot "dist\installer"
$IssPath = Join-Path $PSScriptRoot "Hi5CentralViewerSetup.iss"

function Find-Tool {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [string[]]$Candidates = @()
    )

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    throw "$Name was not found."
}

function Find-InnoCompiler {
    param([string]$ExplicitPath)

    if ($ExplicitPath -and (Test-Path $ExplicitPath)) {
        return (Resolve-Path $ExplicitPath).Path
    }

    $pf86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    $pf = [Environment]::GetEnvironmentVariable("ProgramFiles")

    return Find-Tool -Name "ISCC.exe" -Candidates @(
        (Join-Path $pf86 "Inno Setup 6\ISCC.exe"),
        (Join-Path $pf "Inno Setup 6\ISCC.exe")
    )
}

function Invoke-Native {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$FilePath,
        [string[]]$Arguments = @()
    )

    Write-Host "`n== $Name ==" -ForegroundColor Cyan
    Write-Host "> $FilePath $($Arguments -join ' ')" -ForegroundColor DarkGray

    $global:LASTEXITCODE = $null
    & $FilePath @Arguments

    $exitCode = $global:LASTEXITCODE
    if ($null -eq $exitCode) {
        if ($?) {
            $exitCode = 0
        } else {
            $exitCode = 1
        }
    }

    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode"
    }
}

function Get-BuiltViewerExe {
    $candidates = @(
        (Join-Path $BuildDir "$Configuration\Hi5CentralViewer.exe"),
        (Join-Path $BuildDir "Hi5CentralViewer.exe"),
        (Join-Path $BuildDir "src\$Configuration\Hi5CentralViewer.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    $found = Get-ChildItem -Path $BuildDir -Filter "Hi5CentralViewer.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) {
        return $found.FullName
    }

    throw "Viewer executable not found under build directory: $BuildDir"
}

function Copy-OptionalFile {
    param(
        [string[]]$Candidates,
        [string]$DestinationName
    )

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            Copy-Item $candidate (Join-Path $DistDir $DestinationName) -Force
            Write-Host "Copied $DestinationName from $candidate"
            return
        }
    }

    Write-Host "Optional file not found: $DestinationName" -ForegroundColor Yellow
}

Write-Host "== Hi5Central Viewer ISS build ==" -ForegroundColor Cyan
Write-Host "Repo root:       $RepoRoot"
Write-Host "Configuration:   $Configuration"
Write-Host "Generator:       $Generator"
Write-Host "Platform:        $Platform"
Write-Host "App version:     $AppVersion"

if (!(Test-Path $IssPath)) {
    throw "ISS file was not found: $IssPath"
}

$CMakeExe = Find-Tool -Name "cmake.exe" -Candidates @()
$ISCC = Find-InnoCompiler -ExplicitPath $InnoSetupCompiler

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "`n== Clean build directory ==" -ForegroundColor Cyan
    Remove-Item $BuildDir -Recurse -Force
}

if (!(Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

$configureArgs = @("-S", $RepoRoot, "-B", $BuildDir, "-G", $Generator)

if ($Generator -like "Visual Studio*") {
    $configureArgs += @("-A", $Platform)
} else {
    $configureArgs += @("-DCMAKE_BUILD_TYPE=$Configuration")
}

Invoke-Native -Name "Configure viewer" -FilePath $CMakeExe -Arguments $configureArgs

$buildArgs = @(
    "--build",
    $BuildDir,
    "--config",
    $Configuration,
    "--target",
    "hi5tech-viewer",
    "--parallel"
)
Invoke-Native -Name "Build viewer" -FilePath $CMakeExe -Arguments $buildArgs

$ExePath = Get-BuiltViewerExe
Write-Host "Viewer exe: $ExePath" -ForegroundColor Green

Write-Host "`n== Prepare dist folder ==" -ForegroundColor Cyan
if (Test-Path $DistDir) {
    Remove-Item $DistDir -Recurse -Force
}
New-Item -ItemType Directory -Path $DistDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DistDir "web") | Out-Null

Copy-Item $ExePath (Join-Path $DistDir "Hi5CentralViewer.exe") -Force

$SourceWebDir = Join-Path $RepoRoot "web"
if (!(Test-Path $SourceWebDir)) {
    throw "Viewer web folder not found: $SourceWebDir"
}
Copy-Item (Join-Path $SourceWebDir "*") (Join-Path $DistDir "web") -Recurse -Force

$ExeDir = Split-Path $ExePath -Parent

Copy-OptionalFile -DestinationName "webview.dll" -Candidates @(
    (Join-Path $ExeDir "webview.dll"),
    (Join-Path $BuildDir "_deps\webview-build\core\$Configuration\webview.dll"),
    (Join-Path $BuildDir "_deps\webview-build\core\webview.dll")
)

Copy-OptionalFile -DestinationName "WebView2Loader.dll" -Candidates @(
    (Join-Path $ExeDir "WebView2Loader.dll"),
    (Join-Path $BuildDir "_deps\microsoft_web_webview2-src\build\native\x64\WebView2Loader.dll"),
    (Join-Path $BuildDir "_deps\microsoft_web_webview2-src\runtimes\win-x64\native\WebView2Loader.dll"),
    (Join-Path $BuildDir "_deps\microsoft_web_webview2-src\build\native\arm64\WebView2Loader.dll"),
    (Join-Path $BuildDir "_deps\microsoft_web_webview2-src\runtimes\win-arm64\native\WebView2Loader.dll")
)

if (!(Test-Path $InstallerOutDir)) {
    New-Item -ItemType Directory -Path $InstallerOutDir | Out-Null
}

$issArgs = @("/DMyAppVersion=$AppVersion", $IssPath)
Invoke-Native -Name "Build installer" -FilePath $ISCC -Arguments $issArgs

$Installer = Join-Path $InstallerOutDir "Hi5CentralViewerSetup.exe"
if (!(Test-Path $Installer)) {
    throw "Installer build completed but output was not found: $Installer"
}

Write-Host "`nDone." -ForegroundColor Green
Write-Host "Installer: $Installer" -ForegroundColor Green
