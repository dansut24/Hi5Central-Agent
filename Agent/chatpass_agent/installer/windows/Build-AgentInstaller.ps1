param(
    [string]$InnoSetupCompiler = "",
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [switch]$SkipBuild,
    [string]$AgentVersion = "1.0.0",

    # Static CRT build settings
    [string]$Generator = "Visual Studio 18 2026",
    [string]$Platform = "x64",
    [string]$VcpkgRoot = "",
    [string]$VcpkgTriplet = "x64-windows-static",
    [string]$LibDataChannelRoot = "C:\Users\Dan\Desktop\libdatachannel-install-static",
    [switch]$FetchLibDataChannel,

    # Safety checks
    [switch]$Clean,
    [switch]$SkipDependencyCheck
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir "..\..")).Path
$buildPath = Join-Path $repoRoot $BuildDir
$distPath = Join-Path $repoRoot "dist\installer"
$issPath = Join-Path $scriptDir "Hi5CentralAgentSetup.iss"

function Resolve-ToolPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ToolName
    )

    $found = & where.exe $ToolName 2>$null

    if ($LASTEXITCODE -eq 0 -and $found) {
        return ($found -split "`r?`n")[0]
    }

    return ""
}

function Invoke-NativeChecked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter()]
        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $true)]
        [string]$ErrorMessage
    )

    Write-Host ""
    Write-Host ("> " + $FilePath + " " + ($Arguments -join " "))

    & $FilePath @Arguments

    $success = $?
    $exitCode = $LASTEXITCODE

    if ($null -eq $exitCode) {
        if ($success) {
            $exitCode = 0
        } else {
            $exitCode = 1
        }
    }

    if (-not $success -or $exitCode -ne 0) {
        throw "$ErrorMessage ExitCode=$exitCode"
    }
}

function Assert-X64Toolchain {
    $clPath = Resolve-ToolPath "cl.exe"
    $linkPath = Resolve-ToolPath "link.exe"

    Write-Host ""
    Write-Host "== Toolchain check =="
    Write-Host "cl:   $clPath"
    Write-Host "link: $linkPath"

    if ([string]::IsNullOrWhiteSpace($clPath) -or [string]::IsNullOrWhiteSpace($linkPath)) {
        throw @"
MSVC compiler/linker not found.

Run this from:
x64 Native Tools Command Prompt for VS 2026

Then invoke this script through PowerShell, for example:
powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-Location 'C:\Users\Dan\Downloads\hi5tech-chat-pass\chatpass_agent'; & '.\installer\windows\Build-AgentInstaller.ps1' -Clean -InnoSetupCompiler 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' -VcpkgRoot 'C:\vcpkg' -VcpkgTriplet 'x64-windows-static' -Generator 'Visual Studio 18 2026' -Platform 'x64' -LibDataChannelRoot 'C:\Users\Dan\Desktop\libdatachannel-install-static'"
"@
    }

    if ($clPath -match "Hostx86\\x86" -or $linkPath -match "Hostx86\\x86") {
        throw @"
The current MSVC toolchain is x86, but this agent must be built x64.

Detected:
cl:   $clPath
link: $linkPath

Open "x64 Native Tools Command Prompt for VS 2026" and run the build command from there.
"@
    }

    if ($clPath -notmatch "Hostx64\\x64" -and $linkPath -notmatch "Hostx64\\x64") {
        Write-Warning "Could not clearly confirm Hostx64\x64 toolchain from PATH. Continuing, but linker may fail if the environment is not x64."
    }
}

function Assert-NoDynamicVcRuntimeDependency {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath
    )

    if ($SkipDependencyCheck) {
        Write-Warning "Skipping dependency check because -SkipDependencyCheck was supplied."
        return
    }

    $dumpbin = Resolve-ToolPath "dumpbin.exe"

    if ([string]::IsNullOrWhiteSpace($dumpbin) -or -not (Test-Path $dumpbin)) {
        throw @"
dumpbin.exe was not found in PATH.

Open "x64 Native Tools Command Prompt for VS" or "Visual Studio Developer PowerShell",
then run this installer build again.

This check is required so we do not accidentally ship an agent that depends on:
VCRUNTIME140.dll, VCRUNTIME140_1.dll, MSVCP140.dll, CONCRT140.dll, or UCRTBASE.dll.

To bypass temporarily only for debugging:
-SkipDependencyCheck
"@
    }

    Write-Host ""
    Write-Host "== Dependency check =="
    Write-Host "dumpbin: $dumpbin"
    Write-Host "exe:     $ExePath"

    $output = & $dumpbin /dependents $ExePath 2>&1
    $text = ($output | Out-String)

    Write-Host $text

    $blocked = @(
        "VCRUNTIME140.dll",
        "VCRUNTIME140_1.dll",
        "MSVCP140.dll",
        "MSVCP140_1.dll",
        "CONCRT140.dll",
        "UCRTBASE.dll",
        "api-ms-win-crt"
    )

    $hits = @()

    foreach ($name in $blocked) {
        if ($text -match [regex]::Escape($name)) {
            $hits += $name
        }
    }

    if ($hits.Count -gt 0) {
        $joined = $hits -join ", "

        throw @"
Static runtime dependency check failed.

The built agent still depends on dynamic Microsoft VC/C runtime DLLs:
$joined

This means one of these is true:
1. The agent was not built with /MT.
2. One of the linked libraries was built with /MD.
3. The wrong vcpkg triplet was used.
4. The manual libdatachannel package was built with the dynamic runtime.

Fix:
- Rebuild with -DVCPKG_TARGET_TRIPLET=x64-windows-static
- Rebuild libdatachannel/usrsctp/srtp2/juice with /MT
- Delete the build folder and build again

The installer was NOT packaged.
"@
    }

    Write-Host "OK: no VC++ runtime DLL dependency detected."
}

Write-Host "== Hi5Central Agent installer build =="
Write-Host "Repo root: $repoRoot"
Write-Host "Configuration: $Configuration"
Write-Host "Generator: $Generator"
Write-Host "Platform: $Platform"
Write-Host "VcpkgTriplet: $VcpkgTriplet"
Write-Host "LibDataChannelRoot: $LibDataChannelRoot"
Write-Host "FetchLibDataChannel: $FetchLibDataChannel"

Assert-X64Toolchain

if ($Clean -and (Test-Path $buildPath)) {
    Write-Host ""
    Write-Host "== Clean build folder =="
    Remove-Item -Recurse -Force $buildPath
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    if ($env:VCPKG_ROOT) {
        $VcpkgRoot = $env:VCPKG_ROOT
    } elseif (Test-Path "C:\vcpkg") {
        $VcpkgRoot = "C:\vcpkg"
    }
}

$cmakeExe = Resolve-ToolPath "cmake.exe"

if ([string]::IsNullOrWhiteSpace($cmakeExe) -or -not (Test-Path $cmakeExe)) {
    $cmakeExe = "C:\Program Files\CMake\bin\cmake.exe"
}

if (-not (Test-Path $cmakeExe)) {
    throw "cmake.exe was not found. Install CMake or add it to PATH."
}

$cmakeArgs = @(
    "-S", $repoRoot,
    "-B", $buildPath,
    "-G", $Generator
)

# Visual Studio generators need an explicit platform. Ninja uses the current x64 toolchain environment.
if ($Generator -match "Visual Studio") {
    $cmakeArgs += @("-A", $Platform)
}

$cmakeArgs += @(
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    "-DHI5_REQUIRE_STATIC_CRT=ON",
    "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
)

if ($FetchLibDataChannel) {
    $cmakeArgs += "-DHI5_FETCH_LIBDATACHANNEL=ON"
} elseif (-not [string]::IsNullOrWhiteSpace($LibDataChannelRoot)) {
    $cmakeArgs += "-DLIBDATACHANNEL_ROOT=$LibDataChannelRoot"
} else {
    throw "Provide -LibDataChannelRoot for a local static build or use -FetchLibDataChannel for the pinned CI build."
}

if (-not [string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $toolchainFile = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"

    if (Test-Path $toolchainFile) {
        $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile"
        Write-Host "VcpkgRoot: $VcpkgRoot"
    } else {
        Write-Warning "VCPKG_ROOT was set but toolchain file was not found: $toolchainFile"
    }
} else {
    Write-Warning "VcpkgRoot not provided and VCPKG_ROOT not set. CMake will use whatever packages are already discoverable."
}

if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "== Configure agent with static CRT =="
    Invoke-NativeChecked `
        -FilePath $cmakeExe `
        -Arguments $cmakeArgs `
        -ErrorMessage "CMake configure failed."

    Write-Host ""
    Write-Host "== Build agent =="

    $buildArgs = @(
        "--build", $buildPath,
        "--config", $Configuration,
        "--target", "native_vp8_stream", "hi5central_user", "hi5central_remote_host",
        "-j"
    )

    Invoke-NativeChecked `
        -FilePath $cmakeExe `
        -Arguments $buildArgs `
        -ErrorMessage "CMake build failed."
}

$agentCandidates = @(
    (Join-Path $buildPath "Hi5CentralAgentService.exe"),
    (Join-Path (Join-Path $buildPath $Configuration) "Hi5CentralAgentService.exe")
)
$agentExe = $agentCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $agentExe) {
    $found = Get-ChildItem -Path $buildPath -Filter "Hi5CentralAgentService.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $agentExe = $found.FullName }
}
if (-not $agentExe -or -not (Test-Path $agentExe)) {
    throw "Agent service executable not found under build directory: $buildPath"
}

$userExe = Get-ChildItem -Path $buildPath -Filter "Hi5CentralUser.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
$remoteHostExe = Get-ChildItem -Path $buildPath -Filter "Hi5CentralRemoteHost.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $userExe) { throw "Hi5CentralUser.exe not found under build directory: $buildPath" }
if (-not $remoteHostExe) { throw "Hi5CentralRemoteHost.exe not found under build directory: $buildPath" }

Assert-NoDynamicVcRuntimeDependency -ExePath $agentExe
Assert-NoDynamicVcRuntimeDependency -ExePath $userExe.FullName
Assert-NoDynamicVcRuntimeDependency -ExePath $remoteHostExe.FullName

New-Item -ItemType Directory -Force -Path $distPath | Out-Null

if ([string]::IsNullOrWhiteSpace($InnoSetupCompiler)) {
    $candidates = @(
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            $InnoSetupCompiler = $candidate
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($InnoSetupCompiler) -or -not (Test-Path $InnoSetupCompiler)) {
    throw "Inno Setup Compiler was not found. Install Inno Setup 6 or pass -InnoSetupCompiler 'C:\Path\To\ISCC.exe'."
}

Write-Host ""
Write-Host "== Build installer =="
Write-Host "ISCC: $InnoSetupCompiler"
Write-Host "SourceDir: $buildPath"
Write-Host "OutputDir: $distPath"

$env:HI5_AGENT_VERSION = $AgentVersion

$isccArgs = @(
    "/DSourceDir=$buildPath",
    "/DOutputDir=$distPath",
    "/DAgentExePath=$agentExe",
    "/DUserExePath=$($userExe.FullName)",
    "/DRemoteHostExePath=$($remoteHostExe.FullName)",
    $issPath
)

Invoke-NativeChecked `
    -FilePath $InnoSetupCompiler `
    -Arguments $isccArgs `
    -ErrorMessage "Inno Setup failed."

$outFile = Join-Path $distPath "Hi5CentralAgentSetup.exe"

if (-not (Test-Path $outFile)) {
    throw "Installer build completed but output was not found: $outFile"
}

Write-Host ""
Write-Host "== Done =="
Write-Host "Installer: $outFile"
Write-Host ""
Write-Host "Static CRT check passed. This installer should not require the VC++ Redistributable for Hi5CentralAgent.exe."