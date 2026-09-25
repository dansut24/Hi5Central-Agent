$ErrorActionPreference = 'Stop'

function Require-Literal([string]$Text, [string]$Needle, [string]$Message) {
  if (-not $Text.Contains($Needle)) { throw $Message }
}

$inventory = Get-Content 'Agent/chatpass_agent/src/platform/windows/windows_inventory.cpp' -Raw
$service = Get-Content 'Agent/chatpass_agent/src/platform/windows/windows_service.cpp' -Raw

Require-Literal $inventory 'json LocalUsers()' 'Local-user inventory collector is missing.'
Require-Literal $inventory '{"local_users", localUsers}' 'Inventory snapshot no longer publishes local_users.'
Require-Literal $inventory 'Get-LocalGroupMember -Group ''Administrators''' 'Local administrator membership collection is missing.'
Require-Literal $inventory 'is_admin' 'Local-user administrator annotation is missing.'
Require-Literal $inventory 'saneLinkSpeed' 'Network inventory must suppress Windows unknown-speed sentinel values.'

Require-Literal $service 'BuildNetworkStatsResponse' 'Native live network statistics response is missing.'
Require-Literal $service '#define _WIN32_WINNT 0x0600' 'Windows SDK target must expose MIB_IF_TABLE2/GetIfTable2.'
Require-Literal $service 'static std::string NowIsoUtc();' 'Network stats UTC helper must be declared before use.'
Require-Literal $service 'GetIfTable2' 'Live network statistics must use native Windows interface counters.'
Require-Literal $service 'network_stats_request' 'Agent no longer handles network_stats_request.'
Require-Literal $service 'network_stats_response' 'Agent no longer emits network_stats_response.'
Require-Literal $service 'receive_bytes' 'Live network receive counters are missing.'
Require-Literal $service 'send_bytes' 'Live network send counters are missing.'
Require-Literal $service 'receive_link_speed_bps' 'Live receive link speed is missing.'
Require-Literal $service 'transmit_link_speed_bps' 'Live transmit link speed is missing.'

Write-Host 'Device Details Agent contract check passed.'
