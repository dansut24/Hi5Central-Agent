$path = "web\renderer.js"
$lines = [System.Collections.Generic.List[string]](Get-Content $path)

# Replace validation from tokenless to token-required.
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'if \(!sessionId \|\| !deviceId \|\| !wssUrl\)') {
        $lines[$i] = '  if (!sessionId || !deviceId || !token || !wssUrl) {'
        break
    }
}

Set-Content -Path $path -Value $lines -NoNewline:$false

Write-Host "Patched renderer.js to require viewer token again."