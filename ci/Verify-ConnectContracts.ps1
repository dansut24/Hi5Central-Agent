$ErrorActionPreference = 'Stop'

function Require-Literal([string]$Text, [string]$Needle, [string]$Message) {
  if (-not $Text.Contains($Needle)) { throw $Message }
}

$main = Get-Content 'Agent/chatpass_agent/src/main.cpp' -Raw
$senderHeader = Get-Content 'Agent/chatpass_agent/src/webrtc_sender.h' -Raw
$senderSource = Get-Content 'Agent/chatpass_agent/src/webrtc_sender.cpp' -Raw
$workflow = Get-Content '.github/workflows/windows-installers.yml' -Raw

Require-Literal $main 'ConnectTicketFromArgs' 'Portable Connect ticket parser is missing.'
Require-Literal $main 'Hi5CentralConnect-' 'Portable Connect must derive its one-time ticket from the downloaded filename.'
Require-Literal $main 'RunConnectHost' 'Portable Connect host runtime is missing.'
Require-Literal $main 'wss://rmm.hi5central.com/connect/host/ws?ticket=' 'Portable Connect must use the dedicated ad-hoc host WebSocket.'
Require-Literal $main 'No managed Hi5Central Agent is being installed.' 'Portable Connect must visibly tell the customer that no managed Agent is being installed.'
Require-Literal $main 'Close this window at any time to end remote access.' 'Portable Connect must visibly tell the customer how to revoke access.'
Require-Literal $main '{"type", "connect_hello"}' 'Portable Connect must announce only ad-hoc host metadata after authentication.'
Require-Literal $main 'type == "switch_monitor"' 'Portable Connect must support monitor switching.'
Require-Literal $main 'type == "input_event"' 'Portable Connect must preserve the Viewer WebSocket input fallback.'
Require-Literal $main 'type == "end_session" || type == "session_terminated"' 'Portable Connect must terminate on server-side session end.'
Require-Literal $senderHeader 'void handleInputEvent(const nlohmann::json& msg);' 'WebRTC sender must expose the existing input injector for portable fallback input.'
Require-Literal $senderSource 'm_injector.handleMessage(msg);' 'Portable fallback input must use the existing Windows input injector.'
Require-Literal $workflow 'Hi5CentralConnect.exe' 'Windows CI must publish the portable Connect executable.'
if ($main -match 'cout\s*<<\s*ticket' -or $main -match 'cerr\s*<<\s*ticket') {
  throw 'Portable Connect must never print its one-time host ticket.'
}

Write-Host 'Hi5Central Connect contract check passed.'
