$path = "src\service\service_main.cpp"
$lines = [System.Collections.Generic.List[string]](Get-Content $path)

$start = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'PatchWorkerConfig\s+patchConfig;') {
        $start = $i
        break
    }
}

if ($start -lt 0) {
    throw "Could not find PatchWorkerConfig patchConfig line."
}

$end = -1
for ($i = $start; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'patchWorker_\.Start\(patchConfig\);') {
        $end = $i
        break
    }
}

if ($end -lt 0) {
    throw "Could not find patchWorker_.Start(patchConfig) line."
}

$newBlock = [string[]]@(
'                const std::string enablePatchWorker = ReadConfigValue("HI5_ENABLE_PATCH_WORKER");',
'                const bool patchWorkerEnabled =',
'                    enablePatchWorker == "1" ||',
'                    enablePatchWorker == "true" ||',
'                    enablePatchWorker == "TRUE" ||',
'                    enablePatchWorker == "yes" ||',
'                    enablePatchWorker == "YES";',
'',
'                if (patchWorkerEnabled) {',
'                    PatchWorkerConfig patchConfig;',
'                    patchConfig.enabled = true;',
'                    patchConfig.apiBaseUrl = ReadConfigValue("HI5_PATCH_API_BASE_URL");',
'                    if (patchConfig.apiBaseUrl.empty()) {',
'                        patchConfig.apiBaseUrl = "https://api.hi5central.com";',
'                    }',
'                    patchConfig.apiKey = ReadConfigValue("HI5_PATCH_API_KEY");',
'                    patchConfig.deviceId = ident.deviceId;',
'                    patchConfig.pollSeconds = ReadConfigInt("HI5_PATCH_POLL_SECONDS", 300, 30, 3600);',
'                    LogI("[patch] worker enabled");',
'                    patchWorker_.Start(patchConfig);',
'                } else {',
'                    LogI("[patch] worker disabled by default; set HI5_ENABLE_PATCH_WORKER=1 to enable");',
'                }'
)

$lines.RemoveRange($start, ($end - $start) + 1)
$lines.InsertRange($start, [System.Collections.Generic.List[string]]$newBlock)

Set-Content -Path $path -Value $lines -NoNewline:$false

Write-Host "Patched patch worker to be disabled by default."