$path = "src\agent_identity.cpp"
$text = Get-Content $path -Raw

$old = @'
    std::string existingDeviceId = getFirst(legacyConfig, { "device_id", "deviceid", "id" });
    std::string existingDeviceKey;
    const auto legacySecretBytes = readAllBytesIfExists(legacySecretsPath);
    if (!legacySecretBytes.empty()) existingDeviceKey = decryptDpapiBlobToUtf8(legacySecretBytes);

    EnrollResult enrolled = enrollWindowsDevice(
        apiBase,
        enrollmentToken,
        hostname,
        fingerprint,
        existingDeviceId,
        existingDeviceKey,
        legacyConfig
    );
'@

$new = @'
    std::string existingDeviceId = getFirst(legacyConfig, { "device_id", "deviceid", "id" });
    std::string existingDeviceKey = getFirst(legacyConfig, { "device_key", "devicekey", "agent_secret", "agentsecret", "secret" });

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

    EnrollResult enrolled = enrollWindowsDevice(
        apiBase,
        enrollmentToken,
        hostname,
        fingerprint,
        existingDeviceId,
        existingDeviceKey,
        legacyConfig
    );
'@

if (-not $text.Contains($old)) {
    throw "Could not find existingDeviceKey/enroll block in agent_identity.cpp"
}

$text = $text.Replace($old, $new)
Set-Content -Path $path -Value $text -NoNewline

Write-Host "Patched agent_identity.cpp to accept device_key/agent_secret directly from config.ini."