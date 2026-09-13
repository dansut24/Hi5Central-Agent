$path = "src\agent_identity.cpp"
$lines = [System.Collections.Generic.List[string]](Get-Content $path)

# 1) Replace the existingDeviceKey declaration block.
$start = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'std::string existingDeviceKey;') {
        $start = $i
        break
    }
}

if ($start -lt 0) {
    throw "Could not find: std::string existingDeviceKey;"
}

# Expected old block:
# std::string existingDeviceKey;
# const auto legacySecretBytes = ...
# if (!legacySecretBytes.empty()) ...
$end = $start
while ($end -lt $lines.Count -and $lines[$end] -notmatch 'EnrollResult enrolled = enrollWindowsDevice') {
    $end++
}

if ($end -ge $lines.Count) {
    throw "Could not find enrollWindowsDevice after existingDeviceKey block."
}

$newBlock = @(
'    std::string existingDeviceKey = getFirst(legacyConfig, {',
'        "device_key",',
'        "devicekey",',
'        "agent_secret",',
'        "agentsecret",',
'        "secret"',
'    });',
'',
'    const auto legacySecretBytes = readAllBytesIfExists(legacySecretsPath);',
'    if (existingDeviceKey.empty() && !legacySecretBytes.empty()) {',
'        existingDeviceKey = decryptDpapiBlobToUtf8(legacySecretBytes);',
'    }',
'',
'    if (!existingDeviceId.empty() && !existingDeviceKey.empty()) {',
'        AgentState directState;',
'        directState.deviceId = existingDeviceId;',
'        directState.deviceKey = existingDeviceKey;',
'        directState.apiBaseUrl = apiBase;',
'        directState.agentWsBaseUrl = agentWsBase;',
'        directState.tenantId = getFirst(legacyConfig, { "tenant_id", "tenantid" });',
'        directState.groupId = getFirst(legacyConfig, { "group_id", "groupid" });',
'        directState.enrollmentPackageId = getFirst(legacyConfig, { "package_id", "packageid", "enrollment_package_id" });',
'        directState.fingerprint = fingerprint;',
'',
'        writeState(statePath, directState);',
'',
'        AgentIdentity ident;',
'        ident.deviceId = directState.deviceId;',
'        ident.deviceKey = directState.deviceKey;',
'        ident.agentWsBaseUrl = directState.agentWsBaseUrl;',
'        return ident;',
'    }',
''
)

$lines.RemoveRange($start, $end - $start)
$lines.InsertRange($start, $newBlock)

Set-Content -Path $path -Value $lines -NoNewline:$false

Write-Host "Patched agent_identity.cpp direct device key support."