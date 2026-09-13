$path = "src\core\deep_link.cpp"
$text = Get-Content $path -Raw

$oldSupported = @'
    const bool supportedScheme =
        raw.rfind("hi5tech://connect", 0) == 0 ||
        raw.rfind("hi5viewer://connect", 0) == 0 ||
        raw.rfind("hi5viewer://session", 0) == 0 ||
        raw.rfind("hi5tech://session", 0) == 0;
'@

$newSupported = @'
    const bool supportedScheme =
        raw.rfind("hi5central-viewer://connect", 0) == 0 ||
        raw.rfind("hi5central-viewer://session", 0) == 0 ||
        raw.rfind("hi5viewer://connect", 0) == 0 ||
        raw.rfind("hi5viewer://session", 0) == 0 ||
        raw.rfind("hi5tech://connect", 0) == 0 ||
        raw.rfind("hi5tech://session", 0) == 0;
'@

if (-not $text.Contains($oldSupported)) {
    throw "Could not find supportedScheme block."
}

$text = $text.Replace($oldSupported, $newSupported)

$oldAssign = @'
    launch.sessionId = get("session_id");
    launch.token = get("token");
    launch.deviceId = get("device_id");
    launch.wssUrl = get("wss_url");
    launch.mode = get("mode");
'@

$newAssign = @'
    launch.sessionId = get("session_id");
    if (launch.sessionId.empty()) launch.sessionId = get("sessionId");

    launch.token = get("token");
    if (launch.token.empty()) launch.token = get("viewer_token");
    if (launch.token.empty()) launch.token = get("viewerToken");

    launch.deviceId = get("device_id");
    if (launch.deviceId.empty()) launch.deviceId = get("deviceId");

    launch.wssUrl = get("wss_url");
    if (launch.wssUrl.empty()) launch.wssUrl = get("wssUrl");
    if (launch.wssUrl.empty()) launch.wssUrl = get("signaling_url");
    if (launch.wssUrl.empty()) launch.wssUrl = get("signalingUrl");

    launch.mode = get("mode");
'@

if (-not $text.Contains($oldAssign)) {
    throw "Could not find launch assignment block."
}

$text = $text.Replace($oldAssign, $newAssign)

$text = $text.Replace(
    '    launch.valid = !launch.sessionId.empty() && !launch.token.empty();',
    '    launch.valid = !launch.sessionId.empty() && !launch.deviceId.empty() && !launch.wssUrl.empty();'
)

Set-Content -Path $path -Value $text -NoNewline

Write-Host "Patched deep_link.cpp for hi5central-viewer scheme and parameter aliases."