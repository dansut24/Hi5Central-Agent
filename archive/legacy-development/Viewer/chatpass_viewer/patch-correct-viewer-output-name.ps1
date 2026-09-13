$ErrorActionPreference = "Stop"

$root = Get-Location
$cmake = "CMakeLists.txt"

if (-not (Test-Path $cmake)) {
    throw "CMakeLists.txt not found. Are you in the viewer root?"
}

$text = Get-Content $cmake -Raw

# Add/replace output name for the existing CMake target.
if ($text -notmatch 'set_target_properties\s*\(\s*hi5tech-viewer[\s\S]*?OUTPUT_NAME\s+"Hi5CentralViewer"') {
    $text += @'

# Hi5Central rebrand: keep internal target for compatibility, but output Hi5CentralViewer.exe
set_target_properties(hi5tech-viewer PROPERTIES
    OUTPUT_NAME "Hi5CentralViewer"
)
'@
}

$text = $text.Replace("project(hi5tech_viewer", "project(hi5central_viewer")
$text = $text.Replace("hi5tech_viewer", "hi5central_viewer")

Set-Content -Path $cmake -Value $text -NoNewline

Write-Host "Patched CMake output name."