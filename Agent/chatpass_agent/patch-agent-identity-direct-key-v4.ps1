$path = "src\agent_identity.cpp"
$text = Get-Content $path -Raw

$newBlock = @'
    std::string existingDeviceKey = getFirst(legacyConfig, {
        "device_key",
        "devicekey",
        "agent_secret",
        "agentsecret",
        "secret"
    });

    const auto legacySecretBytes = readAllBytesIfExists(legacySecretsPath);
    if (existingDeviceKey.empty() && !legacySecretBytes.empty()) {
        existingDeviceKey = decryptDpapiBlobToUtf8(legacySecretBytes);
    }

    if (!existingDeviceId.empty() && !existingDeviceKey.empty()) {
        AgentState directState;
        directState.deviceId = existingDeviceId;
        directState.deviceKey = existingDeviceKey;
        directState.apiBaseUrl = apiBase;
        directState.agentWsBaseUrl = agentWsBase;
        directState.tenantId = getFirst(legacyConfig, { "tenant_id", "tenantid" });
        directState.groupId = getFirst(legacyConfig, { "group_id", "groupid" });
        directState.enrollmentPackageId = getFirst(legacyConfig, { "package_id", "packageid", "enrollment_package_id" });
        directState.fingerprint = fingerprint;

        writeState(statePath, directState);

        AgentIdentity ident;
        ident.deviceId = directState.deviceId;
        ident.deviceKey = directState.deviceKey;
        ident.agentWsBaseUrl = directState.agentWsBaseUrl;
        return ident;
    }

'@

# Remove old minimal block if still present.
$text = $text -replace '(?s)\s*std::string existingDeviceKey;\s*const auto legacySecretBytes = readAllBytesIfExists\(legacySecretsPath\);\s*if \(!legacySecretBytes\.empty\(\)\) existingDeviceKey = decryptDpapiBlobToUtf8\(legacySecretBytes\);\s*', "`r`n"

# If v3 partially inserted/duplicated anything, avoid double insert.
if ($text -match 'directState\.deviceId = existingDeviceId;') {
    Write-Host "Direct-key block already appears present. No insertion needed."
} else {
    $marker = '    EnrollResult enrolled = enrollWindowsDevice('
    $idx = $text.IndexOf($marker)

    if ($idx -lt 0) {
        throw "Could not find EnrollResult enrolled marker."
    }

    $text = $text.Insert($idx, $newBlock)
}

Set-Content -Path $path -Value $text -NoNewline

Write-Host "Patched agent_identity.cpp direct device_key/agent_secret support."