from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# 1) Add a safe PowerShell UTF-8 preamble helper if not present.
anchor = """            std::string BuildServicesListScript() {
"""

preamble_helper = r'''
            std::string PowerShellUtf8Preamble() {
                return R"HI5PS(
$ErrorActionPreference = 'Continue'
try {
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
} catch {}
)HI5PS";
            }

'''

if "std::string PowerShellUtf8Preamble()" not in text:
    if anchor not in text:
        raise SystemExit("BuildServicesListScript anchor not found")
    text = text.replace(anchor, preamble_helper + anchor, 1)

# 2) Replace services.list script with UTF-8-safe + compressed JSON.
start = text.find("            std::string BuildServicesListScript() {")
if start == -1:
    raise SystemExit("BuildServicesListScript start not found")

end = text.find("            std::string BuildServiceControlScript", start)
if end == -1:
    raise SystemExit("BuildServicesListScript end not found")

new_services_list = r'''            std::string BuildServicesListScript() {
                return PowerShellUtf8Preamble() + R"HI5PS(
function ConvertTo-Hi5SafeString($Value) {
    if ($null -eq $Value) { return '' }
    $s = [string]$Value
    # Remove control characters that can break downstream JSON display/parsing.
    return ($s -replace '[\x00-\x08\x0B\x0C\x0E-\x1F]', '')
}

$items = Get-CimInstance Win32_Service | Sort-Object DisplayName | ForEach-Object {
    [pscustomobject]@{
        name = ConvertTo-Hi5SafeString $_.Name
        display_name = ConvertTo-Hi5SafeString $_.DisplayName
        state = ConvertTo-Hi5SafeString $_.State
        status = ConvertTo-Hi5SafeString $_.Status
        start_mode = ConvertTo-Hi5SafeString $_.StartMode
        start_name = ConvertTo-Hi5SafeString $_.StartName
        process_id = [int]$_.ProcessId
        path_name = ConvertTo-Hi5SafeString $_.PathName
        description = ConvertTo-Hi5SafeString $_.Description
    }
}

[pscustomobject]@{
    action = 'services.list'
    status = 'ok'
    count = @($items).Count
    services = @($items)
    collected_at = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 8 -Compress
)HI5PS";
            }

'''

text = text[:start] + new_services_list + text[end:]

# 3) Make service control script also use the UTF-8 preamble.
old = '''                return std::string(R"HI5PS(
$ErrorActionPreference = 'Stop'
$serviceName = )HI5PS") + psServiceName + R"HI5PS(
'''
new = '''                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
$ErrorActionPreference = 'Stop'
$serviceName = )HI5PS") + psServiceName + R"HI5PS(
'''

if old in text:
    text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
