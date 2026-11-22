# Hi5Central Virtual Display driver canary builder
# Microsoft Windows-driver-samples derivative. See LICENSE-UPSTREAM.txt in the build output.
[CmdletBinding()]
param([switch]$SkipCompile)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$fullSha = "d49bb7d72f3651952b2b6e4ad28d14fe2247eb69"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$src = Join-Path $PSScriptRoot ".build-src"
$out = Join-Path $PSScriptRoot "artifacts"
if (Test-Path $src) { Remove-Item $src -Recurse -Force }
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item $out -ItemType Directory -Force | Out-Null
& git clone --quiet --depth=1 --filter=blob:none --sparse https://github.com/microsoft/Windows-driver-samples.git $src
if ($LASTEXITCODE -ne 0) { throw "Upstream checkout failed" }
& git -C $src sparse-checkout set video/IndirectDisplay
if ($LASTEXITCODE -ne 0) { throw "Upstream sparse checkout failed" }
$sha = (& git -C $src rev-parse HEAD).Trim()
if ($sha -ne $fullSha) { throw "Upstream changed: expected $fullSha; got $sha. Review upstream before updating the pinned SHA." }
Copy-Item (Join-Path $src "LICENSE") (Join-Path $out "LICENSE-UPSTREAM.txt")
$base = Join-Path $src "video\IndirectDisplay"
$driverDir = Join-Path $base "IddSampleDriver"
$appDir = Join-Path $base "IddSampleApp"
$driverFile = Join-Path $driverDir "Driver.cpp"
$code = [IO.File]::ReadAllText($driverFile)
if (-not $code.Contains("IDD_SAMPLE_MONITOR_COUNT = 3;")) { throw "Upstream monitor declaration changed" }
$code = $code.Replace("IDD_SAMPLE_MONITOR_COUNT = 3;", "IDD_SAMPLE_MONITOR_COUNT = 1;")
# Force an EDID-less monitor: the sample's embedded Dell/Lenovo EDIDs are *not* real Hi5Central hardware.
$needle = "if (ConnectorIndex >= ARRAYSIZE(s_SampleMonitors))"
if (-not $code.Contains($needle)) { throw "Upstream EDID branch changed" }
$code = $code.Replace($needle, "if (true) // Hi5Central: advertise our EDID-less virtual display")
# The first EDID-less monitor mode is preferred: 1080p at exactly 60 Hz.
$code = $code.Replace("{ 1024,  768, 75 },", "{ 1280,  720, 60 },")
$pattern = '(?s)TargetModes\.push_back\(CreateIddCxTargetMode\(3840, 2160, 60\)\);.*?TargetModes\.push_back\(CreateIddCxTargetMode\(1024,  768, 60\)\);'
$targetModes = @'
TargetModes.push_back(CreateIddCxTargetMode(1920, 1080, 60));
    TargetModes.push_back(CreateIddCxTargetMode(1600,  900, 60));
    TargetModes.push_back(CreateIddCxTargetMode(1280,  720, 60));
'@
$patched = [regex]::Replace($code, $pattern, $targetModes)
if ($patched -eq $code) { throw "Upstream target mode block changed" }
[IO.File]::WriteAllText($driverFile, $patched)
# Build a branded driver identity, avoiding collisions with other IDD sample installations.
$files = Get-ChildItem $base -Recurse -File | Where-Object { $_.Extension -in @(".cpp",".h",".inf",".sln",".vcxproj",".filters",".rc") }
foreach ($f in $files) {
    $contents = [IO.File]::ReadAllText($f.FullName)
    $contents = $contents.Replace("IddSampleDriver","Hi5VirtualDisplay").Replace("IddSampleApp","Hi5DisplayHost")
    if ($f.Extension -eq ".inf") {
        $contents = $contents.Replace('ManufacturerName="<Your manufacturer name>"', 'ManufacturerName="Hi5Central"')
        $contents = $contents.Replace('DeviceName="Hi5VirtualDisplay Device"', 'DeviceName="Hi5Central Virtual Display (Canary)"')
        $contents = $contents.Replace("IddSampleDriverGroup","Hi5VirtualDisplayGroup")
    }
    [IO.File]::WriteAllText($f.FullName, $contents)
}
$appFile = Join-Path $appDir "main.cpp"
# Replace sample interactive host with our own SCM service / console diagnostic host.
Copy-Item (Join-Path $PSScriptRoot "Hi5DisplayHost.cpp") $appFile -Force
# Rename project and INF filenames after updating content references.
Rename-Item (Join-Path $driverDir "IddSampleDriver.vcxproj") "Hi5VirtualDisplay.vcxproj"
Rename-Item (Join-Path $driverDir "IddSampleDriver.vcxproj.filters") "Hi5VirtualDisplay.vcxproj.filters"
Rename-Item (Join-Path $driverDir "IddSampleDriver.inf") "Hi5VirtualDisplay.inf"
Rename-Item (Join-Path $driverDir "IddSampleDriver.rc") "Hi5VirtualDisplay.rc"
Rename-Item (Join-Path $appDir "IddSampleApp.vcxproj") "Hi5DisplayHost.vcxproj"
Rename-Item (Join-Path $appDir "IddSampleApp.vcxproj.filters") "Hi5DisplayHost.vcxproj.filters"
Rename-Item (Join-Path $base "IddSampleDriver.sln") "Hi5VirtualDisplay.sln"
# This older Microsoft sample enables legacy PREfast modules that do not load under
# the NuGet WDK x64 on hosted runners. Compile without this legacy analyser here;
# validate with CodeQL, InfVerif and Windows hardware certification separately.
$project = Join-Path $driverDir "Hi5VirtualDisplay.vcxproj"
$projectXml = [IO.File]::ReadAllText($project)
$projectXml = $projectXml.Replace("<RunCodeAnalysis>true</RunCodeAnalysis>", "<RunCodeAnalysis>false</RunCodeAnalysis>")
$projectXml = $projectXml.Replace("<EnablePREfast>true</EnablePREfast>", "<EnablePREfast>false</EnablePREfast>")
[IO.File]::WriteAllText($project, $projectXml)
# Solution project references were rebranded too: match the on-disk directory names.
Rename-Item $driverDir "Hi5VirtualDisplay"
Rename-Item $appDir "Hi5DisplayHost"
$driverDir = Join-Path $base "Hi5VirtualDisplay"
$appDir = Join-Path $base "Hi5DisplayHost"
# The host executable is an interactive canary helper, NOT yet a Windows service.
# No driver installation, code-integrity bypass, or change to an existing display is performed.
Copy-Item (Join-Path $driverDir "Hi5VirtualDisplay.inf") (Join-Path $out "Hi5VirtualDisplay.inf.source")
if ($SkipCompile) { Write-Host "Source prepared; Windows build skipped."; exit 0 }
Push-Location $src
try {
    & nuget restore "packages.config" -PackagesDirectory "packages" -NonInteractive
    if ($LASTEXITCODE -ne 0) { throw "NuGet WDK/SDK restore failed" }
    # CI images may not expose NuGet-provided WDK tool binaries in their PATH.
    # Resolve the actual WDK tools rather than disabling INF stamping/code integrity.
    $kitRoots = @((Join-Path $src "packages"), "${env:ProgramFiles(x86)}\Windows Kits\10\bin")
    $foundTools = @()
    foreach ($kitRoot in $kitRoots) {
        if (Test-Path $kitRoot) {
            $foundTools += @(Get-ChildItem -Path $kitRoot -Recurse -File -Filter "stampinf.exe" -ErrorAction SilentlyContinue)
        }
    }
    Write-Host ("Found stampinf tool(s): " + ($foundTools | ForEach-Object FullName | Out-String))
    foreach ($tool in $foundTools) {
        $env:PATH = "$($tool.DirectoryName);$env:PATH"
    }
    if ($foundTools.Count -eq 0) {
        Write-Host "WDK stampinf.exe was not found. WDK NuGet restore did not expose the required build tools."
        throw "WDK stamping tool missing: no driver was compiled. A complete WDK environment is required."
    }
    # WDK INF verification task P/Invokes a relative x86/InfVerif.dll.
    # Copy the official Microsoft NuGet tool directory to the MSBuild working
    # directory so verification remains ENABLED and can run in isolated CI.
    $verifiers = @(Get-ChildItem (Join-Path $src "packages") -Recurse -File -Filter "InfVerif.dll" -ErrorAction SilentlyContinue)
    Write-Host ("Found InfVerif libraries: " + ($verifiers | ForEach-Object FullName | Out-String))
    $verifier = @($verifiers | Where-Object { $_.Directory.Name -ieq "x86" } | Select-Object -First 1)
    if ($verifier.Count) {
        $toolDir = $verifier[0].DirectoryName
        Copy-Item $toolDir (Join-Path $src "x86") -Recurse -Force
        Write-Host "Staged official INF verifier beside MSBuild working directory."
    } else {
        Write-Warning "INF verification DLL missing from kit; build may fail. INF verification was NOT disabled."
    }
    & msbuild (Join-Path $base "Hi5VirtualDisplay.sln") /m /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "WDK build failed" }
} finally { Pop-Location }
foreach ($name in @("Hi5VirtualDisplay.dll","Hi5VirtualDisplay.inf","Hi5VirtualDisplay.cat","Hi5DisplayHost.exe")) {
    $matches = @(Get-ChildItem $base -Recurse -File -Filter $name | Where-Object { $_.FullName -notmatch '[\\/]obj[\\/]' -and $_.FullName -notmatch '[\\/]artifacts[\\/]' })
    if ($matches.Count -gt 0) {
        Copy-Item $matches[0].FullName (Join-Path $out $name)
        Write-Host "Collected: $name"
    }
}
if (-not (Test-Path (Join-Path $out "Hi5VirtualDisplay.dll"))) { throw "No compiled driver DLL found" }
if (-not (Test-Path (Join-Path $out "Hi5DisplayHost.exe"))) { throw "No compiled host EXE found" }
Get-ChildItem $out -File | Get-FileHash -Algorithm SHA256 | ForEach-Object { "$($_.Hash)  $([IO.Path]::GetFileName($_.Path))" } | Set-Content (Join-Path $out "SHA256SUMS.txt")
Write-Host "Built canary, unsigned. No installation was performed."
