from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

if "std::string PowerShellUtf8Preamble()" not in text:
    raise SystemExit("PowerShellUtf8Preamble not found. Apply services/task-manager patches first.")

anchor = """            void PostJobResult(const AgentIdentity& ident, const std::string& jobId, bool success, const json& result, const std::string& errorMessage = std::string()) {
"""

insert = r'''
            std::string BuildFilesListScript(const json& payload) {
                std::string targetPath = payload.value("path", std::string("C:\\"));
                if (targetPath.empty()) targetPath = "C:\\";
                const std::string psPath = PsSingleQuote(targetPath);

                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
function ConvertTo-Hi5SafeString($Value) {
    if ($null -eq $Value) { return '' }
    $s = [string]$Value
    return ($s -replace '[\x00-\x08\x0B\x0C\x0E-\x1F]', '')
}

$targetPath = )HI5PS") + psPath + R"HI5PS(

if ([string]::IsNullOrWhiteSpace($targetPath)) {
    $targetPath = 'C:\'
}

if (-not (Test-Path -LiteralPath $targetPath)) {
    throw "Path not found: $targetPath"
}

$item = Get-Item -LiteralPath $targetPath -Force -ErrorAction Stop
if (-not $item.PSIsContainer) {
    $targetPath = Split-Path -LiteralPath $targetPath -Parent
    if ([string]::IsNullOrWhiteSpace($targetPath)) { $targetPath = 'C:\' }
}

$current = Get-Item -LiteralPath $targetPath -Force -ErrorAction Stop
$parent = $null
try { $parent = Split-Path -LiteralPath $current.FullName -Parent } catch {}

$entries = Get-ChildItem -LiteralPath $current.FullName -Force -ErrorAction SilentlyContinue | Sort-Object @{Expression='PSIsContainer';Descending=$true}, Name | ForEach-Object {
    [pscustomobject]@{
        name = ConvertTo-Hi5SafeString $_.Name
        full_path = ConvertTo-Hi5SafeString $_.FullName
        type = if ($_.PSIsContainer) { 'folder' } else { 'file' }
        extension = ConvertTo-Hi5SafeString $_.Extension
        size_bytes = if ($_.PSIsContainer) { $null } else { [int64]$_.Length }
        created_at = $_.CreationTimeUtc.ToString('o')
        modified_at = $_.LastWriteTimeUtc.ToString('o')
        is_hidden = [bool](($_.Attributes -band [IO.FileAttributes]::Hidden) -ne 0)
        is_system = [bool](($_.Attributes -band [IO.FileAttributes]::System) -ne 0)
        attributes = ConvertTo-Hi5SafeString $_.Attributes
    }
}

$drives = Get-PSDrive -PSProvider FileSystem | Sort-Object Name | ForEach-Object {
    [pscustomobject]@{
        name = $_.Name
        root = $_.Root
        used_bytes = if ($null -ne $_.Used) { [int64]$_.Used } else { $null }
        free_bytes = if ($null -ne $_.Free) { [int64]$_.Free } else { $null }
    }
}

[pscustomobject]@{
    action = 'files.list'
    status = 'ok'
    path = ConvertTo-Hi5SafeString $current.FullName
    parent = ConvertTo-Hi5SafeString $parent
    count = @($entries).Count
    drives = @($drives)
    entries = @($entries)
    collected_at = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 8 -Compress
)HI5PS";
            }

'''

if "std::string BuildFilesListScript(" not in text:
    if anchor not in text:
        raise SystemExit("PostJobResult anchor not found")
    text = text.replace(anchor, insert + anchor, 1)

branch_anchor = '''                    if (jobType == "services.list") {
'''

branch = r'''                    if (jobType == "files.list") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildFilesListScript(payload), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

'''

if 'jobType == "files.list"' not in text:
    if branch_anchor not in text:
        raise SystemExit("services.list branch anchor not found")
    text = text.replace(branch_anchor, branch + branch_anchor, 1)

path.write_text(text, encoding="utf-8")
