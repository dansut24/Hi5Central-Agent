from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

if "std::string PowerShellUtf8Preamble()" not in text:
    raise SystemExit("PowerShellUtf8Preamble not found. Apply the services UTF-8 patch first.")

# Insert process scripts before PostJobResult.
anchor = """            void PostJobResult(const AgentIdentity& ident, const std::string& jobId, bool success, const json& result, const std::string& errorMessage = std::string()) {
"""

insert = r'''
            std::string BuildProcessesListScript() {
                return PowerShellUtf8Preamble() + R"HI5PS(
function ConvertTo-Hi5SafeString($Value) {
    if ($null -eq $Value) { return '' }
    $s = [string]$Value
    return ($s -replace '[\x00-\x08\x0B\x0C\x0E-\x1F]', '')
}

$items = Get-Process | Sort-Object ProcessName, Id | ForEach-Object {
    $path = ''
    try { $path = $_.Path } catch {}
    [pscustomobject]@{
        pid = [int]$_.Id
        name = ConvertTo-Hi5SafeString $_.ProcessName
        window_title = ConvertTo-Hi5SafeString $_.MainWindowTitle
        session_id = [int]$_.SessionId
        cpu_seconds = if ($null -ne $_.CPU) { [math]::Round([double]$_.CPU, 2) } else { $null }
        working_set_bytes = [int64]$_.WorkingSet64
        private_memory_bytes = [int64]$_.PrivateMemorySize64
        handle_count = [int]$_.HandleCount
        thread_count = [int]$_.Threads.Count
        path = ConvertTo-Hi5SafeString $path
    }
}

[pscustomobject]@{
    action = 'processes.list'
    status = 'ok'
    count = @($items).Count
    processes = @($items)
    collected_at = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 8 -Compress
)HI5PS";
            }

            std::string BuildProcessKillScript(const json& payload) {
                int pid = 0;
                try {
                    if (payload.contains("processId")) pid = payload.value("processId", 0);
                    if (pid <= 0 && payload.contains("pid")) pid = payload.value("pid", 0);
                } catch (...) {
                    pid = 0;
                }

                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
$ErrorActionPreference = 'Stop'
$pidToKill = )HI5PS") + std::to_string(pid) + R"HI5PS(

if ($pidToKill -le 4) {
    throw 'Refusing to terminate a protected/system PID'
}

$proc = Get-Process -Id $pidToKill -ErrorAction Stop
$name = [string]$proc.ProcessName

$blocked = @(
    'native_vp8_stream',
    'Hi5CentralAgent',
    'services',
    'csrss',
    'wininit',
    'winlogon',
    'lsass',
    'smss',
    'system'
)

if ($blocked -contains $name) {
    throw "Refusing to terminate protected process: $name"
}

Stop-Process -Id $pidToKill -Force -ErrorAction Stop
Start-Sleep -Milliseconds 700

$stillRunning = $null -ne (Get-Process -Id $pidToKill -ErrorAction SilentlyContinue)

[pscustomobject]@{
    action = 'process.kill'
    status = if ($stillRunning) { 'failed' } else { 'ok' }
    pid = $pidToKill
    name = $name
    still_running = $stillRunning
    completed_at = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 6 -Compress

if ($stillRunning) { exit 1 }
)HI5PS";
            }

'''

if "std::string BuildProcessesListScript()" not in text:
    if anchor not in text:
        raise SystemExit("PostJobResult anchor not found")
    text = text.replace(anchor, insert + anchor, 1)

# Add ExecuteClaimedJob branches before services.list branch.
branch_anchor = '''                    if (jobType == "services.list") {
'''

branches = r'''                    if (jobType == "processes.list") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildProcessesListScript(), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

                    if (jobType == "process.kill") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildProcessKillScript(payload), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

'''

if 'jobType == "processes.list"' not in text:
    if branch_anchor not in text:
        raise SystemExit("services.list branch anchor not found")
    text = text.replace(branch_anchor, branches + branch_anchor, 1)

path.write_text(text, encoding="utf-8")
