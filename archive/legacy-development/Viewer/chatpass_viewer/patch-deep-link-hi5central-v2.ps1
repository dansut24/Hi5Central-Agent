$path = "src\core\deep_link.cpp"
$lines = [System.Collections.Generic.List[string]](Get-Content $path)

# Replace supportedScheme block: from "const bool supportedScheme =" down to the line ending with ";"
$schemeStart = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'const bool supportedScheme\s*=') {
        $schemeStart = $i
        break
    }
}

if ($schemeStart -lt 0) {
    throw "Could not find const bool supportedScheme block start."
}

$schemeEnd = -1
for ($i = $schemeStart; $i -lt $lines.Count; $i++) {
    if ($lines[$i].Trim().EndsWith(";")) {
        $schemeEnd = $i
        break
    }
}

if ($schemeEnd -lt 0) {
    throw "Could not find const bool supportedScheme block end."
}

$newScheme = [string[]]@(
'    const bool supportedScheme =',
'        raw.rfind("hi5central-viewer://connect", 0) == 0 ||',
'        raw.rfind("hi5central-viewer://session", 0) == 0 ||',
'        raw.rfind("hi5viewer://connect", 0) == 0 ||',
'        raw.rfind("hi5viewer://session", 0) == 0 ||',
'        raw.rfind("hi5tech://connect", 0) == 0 ||',
'        raw.rfind("hi5tech://session", 0) == 0;'
)

$lines.RemoveRange($schemeStart, ($schemeEnd - $schemeStart) + 1)
$lines.InsertRange($schemeStart, [System.Collections.Generic.List[string]]$newScheme)

# Replace assignment lines.
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'launch\.sessionId\s*=\s*get\("session_id"\);') {
        $assignStart = $i
        break
    }
}

if ($assignStart -lt 0) {
    throw "Could not find launch.sessionId assignment."
}

$assignEnd = -1
for ($i = $assignStart; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'launch\.mode\s*=\s*get\("mode"\);') {
        $assignEnd = $i
        break
    }
}

if ($assignEnd -lt 0) {
    throw "Could not find launch.mode assignment."
}

$newAssign = [string[]]@(
'    launch.sessionId = get("session_id");',
'    if (launch.sessionId.empty()) launch.sessionId = get("sessionId");',
'',
'    launch.token = get("token");',
'    if (launch.token.empty()) launch.token = get("viewer_token");',
'    if (launch.token.empty()) launch.token = get("viewerToken");',
'',
'    launch.deviceId = get("device_id");',
'    if (launch.deviceId.empty()) launch.deviceId = get("deviceId");',
'',
'    launch.wssUrl = get("wss_url");',
'    if (launch.wssUrl.empty()) launch.wssUrl = get("wssUrl");',
'    if (launch.wssUrl.empty()) launch.wssUrl = get("signaling_url");',
'    if (launch.wssUrl.empty()) launch.wssUrl = get("signalingUrl");',
'',
'    launch.mode = get("mode");'
)

$lines.RemoveRange($assignStart, ($assignEnd - $assignStart) + 1)
$lines.InsertRange($assignStart, [System.Collections.Generic.List[string]]$newAssign)

# Make token optional for direct native viewer testing.
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'launch\.valid\s*=') {
        $lines[$i] = '    launch.valid = !launch.sessionId.empty() && !launch.deviceId.empty() && !launch.wssUrl.empty();'
        break
    }
}

Set-Content -Path $path -Value $lines -NoNewline:$false

Write-Host "Patched deep_link.cpp for hi5central-viewer scheme and parameter aliases."