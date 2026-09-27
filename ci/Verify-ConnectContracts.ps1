$ErrorActionPreference = 'Stop'

function Require-Literal([string]$Text, [string]$Needle, [string]$Message) {
  if (-not $Text.Contains($Needle)) { throw $Message }
}

$main = Get-Content 'Agent/chatpass_agent/src/main.cpp' -Raw
$senderHeader = Get-Content 'Agent/chatpass_agent/src/webrtc_sender.h' -Raw
$senderSource = Get-Content 'Agent/chatpass_agent/src/webrtc_sender.cpp' -Raw
$windowsInput = Get-Content 'Agent/chatpass_agent/src/platform/windows/windows_input.cpp' -Raw
$connectWindow = Get-Content 'Agent/chatpass_agent/src/ui/native_connect_window.cpp' -Raw
$cmake = Get-Content 'Agent/chatpass_agent/CMakeLists.txt' -Raw
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
Require-Literal $main 'ShowWindow(console, SW_HIDE)' 'Portable Connect must hide the legacy console window.'
Require-Literal $main 'hi5::NativeConnectWindow supportWindow' 'Portable Connect must use its dedicated attended-support window.'
if ($main.Contains('hi5::NativeBanner supportPanel')) { throw 'Portable Connect must not reuse the unattended transparency banner.' }
Require-Literal $main 'supportWindow.AppendMessage(chat)' 'Portable Connect customer chat must stay inside the attended-support window.'
Require-Literal $main '"customer_ended_session"' 'Portable Connect local End session must revoke technician access.'
Require-Literal $main '{"type", "chat_message"}' 'Portable Connect customer chat must relay through the authenticated session.'
Require-Literal $senderHeader 'void handleInputEvent(const nlohmann::json& msg);' 'WebRTC sender must expose the existing input injector for portable fallback input.'
Require-Literal $senderSource 'm_injector.handleMessage(msg);' 'Portable fallback input must use the existing Windows input injector.'
Require-Literal $cmake 'src/platform/windows/windows_input.cpp' 'Windows builds must compile the platform input injector checked by this contract.'
Require-Literal $cmake 'src/ui/native_connect_window.cpp' 'Windows builds must compile the dedicated Connect customer window.'
Require-Literal $connectWindow 'WS_MINIMIZEBOX' 'Connect customer window must be minimisable.'
Require-Literal $connectWindow 'WS_EX_APPWINDOW' 'Connect customer window must appear as a normal taskbar application.'
Require-Literal $connectWindow 'CenterWindow()' 'Connect customer window must open centred on the customer screen.'
Require-Literal $connectWindow 'L"End session"' 'Connect customer window must expose an obvious End session action.'
Require-Literal $connectWindow 'L"Hi5Central Remote Support"' 'Connect customer window must clearly identify the active support application.'
Require-Literal $windowsInput 'kind == "mouse_click"' 'Compiled Windows input must support mobile tap/click events.'
Require-Literal $windowsInput 'sendMouseClick(button, clickCount)' 'Compiled Windows input must inject complete mobile clicks.'
Require-Literal $windowsInput 'kind == "text_input"' 'Compiled Windows input must support mobile text entry.'
Require-Literal $windowsInput 'kind == "clipboard_paste"' 'Compiled Windows input must support mobile clipboard paste.'
Require-Literal $windowsInput 'sendUnicodeText' 'Compiled Windows text entry must use Windows Unicode input.'
Require-Literal $windowsInput 'sendShortcut' 'Compiled Windows input must support common mobile Windows shortcuts.'
Require-Literal $workflow 'Hi5CentralConnect.exe' 'Windows CI must publish the portable Connect executable.'
Require-Literal $workflow '/SUBSYSTEM:WINDOWS' 'Portable Connect must be converted to the Windows GUI subsystem so no console window appears.'
if ($main -match 'cout\s*<<\s*ticket' -or $main -match 'cerr\s*<<\s*ticket') {
  throw 'Portable Connect must never print its one-time host ticket.'
}

Write-Host 'Hi5Central Connect contract check passed.'
