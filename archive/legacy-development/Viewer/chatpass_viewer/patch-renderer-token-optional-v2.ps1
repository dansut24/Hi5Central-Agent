$path = "web\renderer.js"
$lines = [System.Collections.Generic.List[string]](Get-Content $path)

# Find startSession function.
$start = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'function\s+startSession\s*\(params\)') {
        $start = $i
        break
    }
}

if ($start -lt 0) {
    throw "Could not find function startSession(params)."
}

# Find validation block start: const sessionId...
$valStart = -1
for ($i = $start; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'const\s+sessionId\s*=\s*params\.session_id') {
        $valStart = $i
        break
    }
}

if ($valStart -lt 0) {
    throw "Could not find startSession sessionId validation start."
}

# Find end of invalid params block: the closing brace after return;
$valEnd = -1
$seenReturn = $false
for ($i = $valStart; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'return;') {
        $seenReturn = $true
        continue
    }

    if ($seenReturn -and $lines[$i].Trim() -eq "}") {
        $valEnd = $i
        break
    }
}

if ($valEnd -lt 0) {
    throw "Could not find end of validation block."
}

$newValidation = [string[]]@(
'  params = params || {};',
'',
'  const sessionId = params.session_id || params.sessionId || "";',
'  const token = params.token || params.viewer_token || params.viewerToken || "";',
'  const deviceId = params.device_id || params.deviceId || "";',
'  const wssUrl = params.wss_url || params.wssUrl || params.signaling_url || params.signalingUrl || "";',
'',
'  if (!sessionId || !deviceId || !wssUrl) {',
'    console.error("[viewer] invalid connect params:", params);',
'    disconnect("Invalid connection parameters");',
'    return;',
'  }'
)

$lines.RemoveRange($valStart, ($valEnd - $valStart) + 1)
$lines.InsertRange($valStart, [System.Collections.Generic.List[string]]$newValidation)

# Find URL line after currentSession section.
$urlLine = -1
for ($i = $start; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'const\s+url\s*=\s*`\$\{wssUrl\}\?session_id=') {
        $urlLine = $i
        break
    }
}

if ($urlLine -lt 0) {
    throw "Could not find viewer websocket URL line."
}

$newUrl = [string[]]@(
'  const url =',
'    `${wssUrl}?session_id=${encodeURIComponent(sessionId)}` +',
'    `&device_id=${encodeURIComponent(deviceId)}` +',
'    (token ? `&token=${encodeURIComponent(token)}` : "");'
)

$lines.RemoveAt($urlLine)
$lines.InsertRange($urlLine, [System.Collections.Generic.List[string]]$newUrl)

Set-Content -Path $path -Value $lines -NoNewline:$false

Write-Host "Patched renderer.js to allow tokenless launch and include device_id."