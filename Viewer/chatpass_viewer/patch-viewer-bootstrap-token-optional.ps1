$path = "src\app\viewer_app.cpp"
$text = Get-Content $path -Raw

$text = $text.Replace(
    "pending.sessionId && pending.token && pending.wssUrl",
    "pending.sessionId && pending.deviceId && pending.wssUrl"
)

Set-Content -Path $path -Value $text -NoNewline

Write-Host "Patched BuildBootstrapScript so token is optional for native viewer launch."