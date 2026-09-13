param(
  [string]$RepoRoot = "C:\Users\Dan\Downloads\hi5tech-chat-pass\chatpass_agent",
  [string]$CertPath = "C:\Users\Dan\Desktop\Hi5Central-Test-CodeSigning.pfx",
  [string]$CertPassword = $env:HI5_CODESIGN_PASSWORD,
  [string]$TimestampUrl = "http://timestamp.digicert.com",
  [string]$InnoSetupCompiler = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
  [string]$VcpkgRoot = "C:\vcpkg",
  [string]$LibDataChannelRoot = "C:\Users\Dan\Desktop\libdatachannel-install-static"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($CertPassword)) {
  throw "Set HI5_CODESIGN_PASSWORD or pass -CertPassword explicitly before using the optional signing test build."
}

function Find-SignTool {
  $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }

  $kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
  $found = Get-ChildItem -Path $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\x64\\signtool\.exe$" } |
    Sort-Object FullName -Descending |
    Select-Object -First 1

  if (-not $found) { throw "signtool.exe not found. Install Windows 10/11 SDK." }
  return $found.FullName
}

function Sign-File($Path) {
  if (-not (Test-Path $Path)) { throw "Cannot sign missing file: $Path" }
  & $SignTool sign /fd SHA256 /f $CertPath /p $CertPassword /tr $TimestampUrl /td SHA256 $Path
  if ($LASTEXITCODE -ne 0) { throw "signtool failed for $Path" }
  & $SignTool verify /pa /v $Path
  if ($LASTEXITCODE -ne 0) { throw "signtool verify failed for $Path" }
}

$SignTool = Find-SignTool
Write-Host "signtool: $SignTool"

Push-Location $RepoRoot
try {
  if (Test-Path build) { Remove-Item build -Recurse -Force }

  cmake -S . -B build `
    -G "Visual Studio 18 2026" `
    -A x64 `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
    -DHI5_REQUIRE_STATIC_CRT=ON `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static `
    -DLIBDATACHANNEL_ROOT="$LibDataChannelRoot" `
    -DCMAKE_TOOLCHAIN_FILE="$VcpkgRoot\scripts\buildsystems\vcpkg.cmake"

  if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

  cmake --build build --config Release --target native_vp8_stream -j
  if ($LASTEXITCODE -ne 0) { throw "native_vp8_stream build failed" }

  cmake --build build --config Release --target hi5tech_sas_helper -j
  if ($LASTEXITCODE -ne 0) { throw "hi5tech_sas_helper build failed" }

  Sign-File "$RepoRoot\build\Release\native_vp8_stream.exe"
  Sign-File "$RepoRoot\build\Release\hi5central_sas_helper.exe"

  New-Item -ItemType Directory -Force "$RepoRoot\dist\installer" | Out-Null

  & $InnoSetupCompiler `
    "/DSourceDir=$RepoRoot\build" `
    "/DOutputDir=$RepoRoot\dist\installer" `
    "/DAgentExePath=$RepoRoot\build\Release\native_vp8_stream.exe" `
    "$RepoRoot\installer\windows\Hi5CentralAgentSetup.iss"

  if ($LASTEXITCODE -ne 0) { throw "Inno Setup compile failed" }

  Sign-File "$RepoRoot\dist\installer\Hi5CentralAgentSetup.exe"

  Write-Host ""
  Write-Host "Done:"
  Write-Host "$RepoRoot\dist\installer\Hi5CentralAgentSetup.exe"
}
finally {
  Pop-Location
}
