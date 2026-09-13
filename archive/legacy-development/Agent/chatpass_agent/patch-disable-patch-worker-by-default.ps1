$path = "src\service\service_main.cpp"
$text = Get-Content $path -Raw

$old = @'
                PatchWorkerConfig patchConfig;
                patchConfig.enabled = true;
                patchConfig.apiBaseUrl = ReadConfigValue("HI5_PATCH_API_BASE_URL");
                if (patchConfig.apiBaseUrl.empty()) {
                    patchConfig.apiBaseUrl = "https://hi5tech-software-intelligence.vercel.app";
                }
                patchConfig.apiKey = ReadConfigValue("HI5_PATCH_API_KEY");
                patchConfig.deviceId = ident.deviceId;
                patchConfig.pollSeconds = ReadConfigInt("HI5_PATCH_POLL_SECONDS", 30, 5, 3600);
                patchWorker_.Start(patchConfig);
'@

$new = @'
                const std::string enablePatchWorker = ReadConfigValue("HI5_ENABLE_PATCH_WORKER");
                const bool patchWorkerEnabled =
                    enablePatchWorker == "1" ||
                    enablePatchWorker == "true" ||
                    enablePatchWorker == "TRUE" ||
                    enablePatchWorker == "yes" ||
                    enablePatchWorker == "YES";

                if (patchWorkerEnabled) {
                    PatchWorkerConfig patchConfig;
                    patchConfig.enabled = true;
                    patchConfig.apiBaseUrl = ReadConfigValue("HI5_PATCH_API_BASE_URL");
                    if (patchConfig.apiBaseUrl.empty()) {
                        patchConfig.apiBaseUrl = "https://api.hi5central.com";
                    }
                    patchConfig.apiKey = ReadConfigValue("HI5_PATCH_API_KEY");
                    patchConfig.deviceId = ident.deviceId;
                    patchConfig.pollSeconds = ReadConfigInt("HI5_PATCH_POLL_SECONDS", 300, 30, 3600);
                    LogI("[patch] worker enabled");
                    patchWorker_.Start(patchConfig);
                } else {
                    LogI("[patch] worker disabled by default; set HI5_ENABLE_PATCH_WORKER=1 to enable");
                }
'@

if (-not $text.Contains($old)) {
    throw "Could not find patch worker startup block."
}

$text = $text.Replace($old, $new)
Set-Content -Path $path -Value $text -NoNewline

Write-Host "Patched patch worker to be disabled by default."