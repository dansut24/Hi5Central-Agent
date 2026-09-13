$path = "web\renderer.js"
$text = Get-Content $path -Raw

$old = @'
  const sessionId = params.session_id || params.sessionId;
  const token = params.token;
  const deviceId = params.device_id || params.deviceId;
  const wssUrl = params.wss_url || params.wssUrl;

  if (!sessionId || !token || !wssUrl) {
    console.error("[viewer] invalid connect params:", params);
    disconnect("Invalid connection parameters");
    return;
  }
'@

$new = @'
  params = params || {};

  const sessionId = params.session_id || params.sessionId || "";
  const token = params.token || params.viewer_token || params.viewerToken || "";
  const deviceId = params.device_id || params.deviceId || "";
  const wssUrl = params.wss_url || params.wssUrl || params.signaling_url || params.signalingUrl || "";

  if (!sessionId || !deviceId || !wssUrl) {
    console.error("[viewer] invalid connect params:", params);
    disconnect("Invalid connection parameters");
    return;
  }
'@

if (-not $text.Contains($old)) {
    throw "Could not find startSession validation block."
}

$text = $text.Replace($old, $new)

$oldUrl = @'
  const url = `${wssUrl}?session_id=${encodeURIComponent(sessionId)}&token=${encodeURIComponent(token)}`;
'@

$newUrl = @'
  const url =
    `${wssUrl}?session_id=${encodeURIComponent(sessionId)}` +
    `&device_id=${encodeURIComponent(deviceId)}` +
    (token ? `&token=${encodeURIComponent(token)}` : "");
'@

if (-not $text.Contains($oldUrl)) {
    throw "Could not find viewer websocket URL line."
}

$text = $text.Replace($oldUrl, $newUrl)

Set-Content -Path $path -Value $text -NoNewline

Write-Host "Patched renderer.js to allow tokenless native viewer launch and include device_id."