$path = "installer\windows\Build-ViewerInstaller.ps1"

if (-not (Test-Path $path)) {
    throw "Build-ViewerInstaller.ps1 not found"
}

$text = Get-Content $path -Raw

# Build the real CMake target, but keep packaging the renamed EXE.
$text = $text.Replace("--target Hi5CentralViewer", "--target hi5tech-viewer")
$text = $text.Replace("--target `"Hi5CentralViewer`"", "--target `"hi5tech-viewer`"")

Set-Content -Path $path -Value $text -NoNewline

Write-Host "Patched Build-ViewerInstaller.ps1 to build target hi5tech-viewer."