$ErrorActionPreference = 'Stop'

function Require-Literal([string]$Text, [string]$Needle, [string]$Message) {
  if (-not $Text.Contains($Needle)) { throw $Message }
}

$inventory = Get-Content 'Agent/chatpass_agent/src/platform/windows/windows_inventory.cpp' -Raw
$service = Get-Content 'Agent/chatpass_agent/src/platform/windows/windows_service.cpp' -Raw
$patchHost = Get-Content 'Agent/chatpass_agent/src/patching/patch_host_main.cpp' -Raw

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

Write-Host 'Device Details Agent contract check passed.'
