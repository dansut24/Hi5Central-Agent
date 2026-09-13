$ErrorActionPreference = "Stop"

# Ensure renamed ISS exists.
if ((Test-Path "installer\windows\Hi5TechViewerSetup.iss") -and -not (Test-Path "installer\windows\Hi5CentralViewerSetup.iss")) {
    Copy-Item "installer\windows\Hi5TechViewerSetup.iss" "installer\windows\Hi5CentralViewerSetup.iss"
}

$files = @(
    "installer\windows\Build-ViewerInstaller.ps1",
    "installer\windows\Hi5CentralViewerSetup.iss",
    "installer\windows\Hi5TechViewerSetup.iss"
)

foreach ($file in $files) {
    if (-not (Test-Path $file)) { continue }

    $t = Get-Content $file -Raw

    $t = $t.Replace("Hi5Tech Viewer", "Hi5Central Viewer")
    $t = $t.Replace("Hi5TechViewer", "Hi5CentralViewer")
    $t = $t.Replace("Hi5Tech", "Hi5Central")

    $t = $t.Replace("hi5tech-viewer.exe", "Hi5CentralViewer.exe")
    $t = $t.Replace("hi5tech-viewer", "Hi5CentralViewer")
    $t = $t.Replace("Hi5TechViewerSetup.exe", "Hi5CentralViewerSetup.exe")
    $t = $t.Replace("Hi5TechViewerSetup.iss", "Hi5CentralViewerSetup.iss")

    if ($file -like "*.iss") {
        if ($t -notmatch '(?m)^\[Registry\]') {
            $t += "`r`n`r`n[Registry]`r`n"
        }

        $registryBlock = @'

; Hi5Central Viewer deep link protocol
Root: HKCU; Subkey: "Software\Classes\hi5central-viewer"; ValueType: string; ValueName: ""; ValueData: "URL:Hi5Central Viewer Protocol"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\hi5central-viewer"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\hi5central-viewer\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Hi5CentralViewer.exe,0"
Root: HKCU; Subkey: "Software\Classes\hi5central-viewer\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Hi5CentralViewer.exe"" ""%1"""

; Backwards-compatible alias while migrating old links
Root: HKCU; Subkey: "Software\Classes\hi5viewer"; ValueType: string; ValueName: ""; ValueData: "URL:Hi5Central Viewer Protocol"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\hi5viewer"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\hi5viewer\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Hi5CentralViewer.exe,0"
Root: HKCU; Subkey: "Software\Classes\hi5viewer\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Hi5CentralViewer.exe"" ""%1"""
'@

        if ($t -notmatch 'Software\\Classes\\hi5central-viewer') {
            $t = $t -replace '(?m)^\[Registry\]\s*', "[Registry]`r`n$registryBlock`r`n"
        }
    }

    Set-Content -Path $file -Value $t -NoNewline
}

Write-Host "Patched installer packaging and protocol registration."