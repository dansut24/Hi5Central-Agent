$ErrorActionPreference = 'Stop'

function Require-Literal([string]$Text, [string]$Needle, [string]$Message) {
  if (-not $Text.Contains($Needle)) { throw $Message }
}

$inventory = Get-Content 'Agent/chatpass_agent/src/platform/windows/windows_inventory.cpp' -Raw
$service = Get-Content 'Agent/chatpass_agent/src/platform/windows/windows_service.cpp' -Raw
$patchHost = Get-Content 'Agent/chatpass_agent/src/patching/patch_host_main.cpp' -Raw
$logging = Get-Content 'Agent/chatpass_agent/src/util/log.h' -Raw
$h264 = Get-Content 'Agent/chatpass_agent/src/h264_mf_encoder.cpp' -Raw

Require-Literal $inventory 'json LocalUsers()' 'Local-user inventory collector is missing.'
Require-Literal $inventory '{"local_users", localUsers}' 'Inventory snapshot no longer publishes local_users.'
Require-Literal $inventory 'Get-LocalGroupMember -Group ''Administrators''' 'Local administrator membership collection is missing.'
Require-Literal $inventory 'is_admin' 'Local-user administrator annotation is missing.'
Require-Literal $inventory 'saneLinkSpeed' 'Network inventory must suppress Windows unknown-speed sentinel values.'
Require-Literal $inventory 'Microsoft\Windows NT\CurrentVersion\ProfileList' 'Software inventory must enumerate real Windows user profiles instead of LocalSystem HKCU.'
Require-Literal $inventory '"user:" + sidUtf8' 'Per-user software inventory must retain the owning user SID.'
if ($inventory.Contains('HKEY_CURRENT_USER')) { throw 'Service software inventory must not treat LocalSystem HKCU/systemprofile as an end-user software source.' }
Require-Literal $patchHost 'hiveUtf8 == "S-1-5-18"' 'Patch verification must reject LocalSystem uninstall registrations.'
Require-Literal $patchHost 'hiveUtf8 == "S-1-5-19"' 'Patch verification must reject LocalService uninstall registrations.'
Require-Literal $patchHost 'hiveUtf8 == "S-1-5-20"' 'Patch verification must reject NetworkService uninstall registrations.'
Require-Literal $patchHost 'DetectInstallerTechnology(' 'PatchHost must inspect downloaded EXE installer technology before choosing silent arguments.'
Require-Literal $patchHost 'installerTechnologyAutoSelected' 'PatchHost must record when runtime installer-technology detection overrides a generic catalogue hint.'
Require-Literal $patchHost 'VendorInstallStrategies(executionManifest)' 'PatchHost must execute silent strategies from the runtime-detected installer technology.'

Require-Literal $service 'BuildNetworkStatsResponse' 'Native live network statistics response is missing.'
Require-Literal $service 'static std::string NowIsoUtc();' 'Network stats UTC helper must be declared before use.'
Require-Literal $service 'GetIfTable(' 'Live network statistics must use the broadly supported native Windows interface counters.'
Require-Literal $service 'network_stats_request' 'Agent no longer handles network_stats_request.'
Require-Literal $service 'network_stats_response' 'Agent no longer emits network_stats_response.'
Require-Literal $service 'receive_bytes' 'Live network receive counters are missing.'
Require-Literal $service 'send_bytes' 'Live network send counters are missing.'
Require-Literal $service 'receive_link_speed_bps' 'Live receive link speed is missing.'
Require-Literal $service 'transmit_link_speed_bps' 'Live transmit link speed is missing.'

# Endpoint storage/log hygiene.
Require-Literal $service 'PurgeProgramDataHousekeeping' 'ProgramData housekeeping must run on Agent startup and periodically.'
Require-Literal $service 'C:\ProgramData\Hi5CentralUpgrade' 'Legacy standalone updater storage must be scavenged safely.'
Require-Literal $service 'PatchHost" / L"jobs", 6' 'Abandoned PatchHost job payloads must age out after the safe grace period.'
Require-Literal $service 'Qualification" / L"jobs", 24 * 7' 'Qualification evidence must have a bounded endpoint retention period.'
Require-Literal $service 'Hi5Central\Agent\Temp' 'Temporary uninstall diagnostics must use Agent Temp instead of permanent Logs.'
Require-Literal $service '[MSI log tail]' 'Failed MSI uninstall diagnostics must be condensed into the job result.'
Require-Literal $service 'Remove-Item -LiteralPath $script:hi5MsiLog' 'Temporary MSI uninstall logs must be removed after the attempt.'
if ($service.Contains('CadStatusPath()')) { throw 'CAD state must not maintain a duplicate unbounded CadStatus.txt log.' }
if ($service.Contains('Hi5Central\Agent\Logs\uninstall-')) { throw 'Verbose MSI uninstall logs must not be written to permanent Agent Logs.' }
Require-Literal $logging 'kDiagnosticMaxBytes = 4ull * 1024ull * 1024ull' 'Diagnostic log cap drifted from the bounded production policy.'
Require-Literal $logging 'kDiagnosticBackups = 2' 'Diagnostic log backup count must remain bounded.'
Require-Literal $logging 'kSupportBackups = 2' 'Support log backup count must remain bounded.'
Require-Literal $h264 'Hi5Central\\Agent\\Temp\\hi5-h264-dump.h264' 'Opt-in raw H.264 diagnostics must live in disposable Agent Temp.'
if ($h264.Contains('Hi5Central\\Agent\\Logs\\hi5-h264-dump.h264')) { throw 'Raw H.264 diagnostics must not pollute the bounded Agent Logs directory.' }

Write-Host 'Device Details Agent contract check passed.'
