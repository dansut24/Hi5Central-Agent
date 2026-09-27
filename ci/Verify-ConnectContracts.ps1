$ErrorActionPreference = 'Stop'

function Require-Literal([string]$Text, [string]$Needle, [string]$Message) {
  if (-not $Text.Contains($Needle)) { throw $Message }
}

$main = Get-Content 'Agent/chatpass_agent/src/main.cpp' -Raw
$senderHeader = Get-Content 'Agent/chatpass_agent/src/webrtc_sender.h' -Raw
$senderSource = Get-Content 'Agent/chatpass_agent/src/webrtc_sender.cpp' -Raw
$windowsInput = Get-Content 'Agent/chatpass_agent/src/platform/windows/windows_input.cpp' -Raw
$connectWindow = Get-Content 'Agent/chatpass_agent/src/ui/native_connect_window.cpp' -Raw
$connectFiles = Get-Content 'Agent/chatpass_agent/src/connect_file_browser.cpp' -Raw
$connectCapture = Get-Content 'Agent/chatpass_agent/src/connect_capture_bridge.cpp' -Raw
$signalingSource = Get-Content 'Agent/chatpass_agent/src/signaling_client.cpp' -Raw
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
Require-Literal $main 'type == "connect_permission_request"' 'Connect must process explicit customer permission requests.'
Require-Literal $main 'permission == "elevation"' 'Connect must gate elevation behind customer approval.'
Require-Literal $main 'ConnectLaunchElevatedCopy' 'Connect must support a customer-approved UAC handoff to an elevated copy.'
Require-Literal $main 'lpVerb = L"runas"' 'Connect elevation must use the standard Windows UAC runas flow.'
Require-Literal $main 'connectElevated' 'Connect must retain the elevated-host capability state after UAC handoff.'
Require-Literal $main 'connect_uac_customer_action_required' 'Connect must tell the Viewer when the first Windows UAC prompt requires local customer approval.'
Require-Literal $main 'connect_uac_cancelled' 'Connect must tell the Viewer when Windows UAC is cancelled so the frozen-frame hold can be released.'
Require-Literal $main 'fileAccessGranted.store(msg.value("files_granted", false))' 'Connect must preserve already-approved file access across host reconnect/elevation handoff.'
Require-Literal $main 'hi5::ConnectCaptureBridge connectCapture' 'Elevated attended Connect must use the managed-Agent capture bridge rather than direct user-process capture.'
Require-Literal $main 'WebRtcSender::Mode::ExternalFeed' 'Elevated attended Connect must keep WebRTC separate from the LocalSystem capture helpers.'
Require-Literal $main 'RunConnectCaptureBrokerService' 'Portable Connect must dispatch its temporary LocalSystem capture broker before normal host startup.'
Require-Literal $connectCapture 'LaunchInElevatedDefaultSessionForSession' 'Connect must launch its normal desktop helper with the same session-bound LocalSystem method as the managed Agent.'
Require-Literal $connectCapture 'LaunchOnSecureDesktopForSession' 'Connect must use the managed Agent Winlogon secure-desktop helper for UAC.'
Require-Literal $connectCapture '--dynamic-desktop' 'Connect normal capture helper must retain the managed Agent dynamic-desktop path.'
Require-Literal $connectCapture 'std::chrono::milliseconds(220)' 'Connect must retain the managed Agent short secure-helper fallback window.'
Require-Literal $connectCapture 'GetDesktopTransitionTickNs' 'Connect must fence pre-transition frames before exposing the UAC desktop.'
Require-Literal $connectCapture 'secure_desktop_entering' 'Connect broker must announce secure desktop entry to the Viewer.'
Require-Literal $connectCapture 'secure_desktop_ready' 'Connect broker must announce only a post-transition secure desktop frame as ready.'
Require-Literal $connectCapture 'secure_desktop_exited' 'Connect broker must announce return to the normal desktop.'
Require-Literal $connectCapture 'SERVICE_DEMAND_START' 'Connect capture broker must be temporary/on-demand rather than a permanent service.'
Require-Literal $connectCapture 'DeleteService' 'Connect capture broker must remove its temporary service when the attended session ends.'
Require-Literal $main 'ConnectSetRestartResume' 'Connect must support temporary restart-resume persistence only when approved.'
Require-Literal $main 'CurrentVersion\\RunOnce' 'Connect restart persistence must be one-shot rather than a permanent service or startup entry.'
Require-Literal $main 'type == "connect_hold_request"' 'Connect must expose customer-approved session hold.'
Require-Literal $main 'startSupportUi("", "Waiting for technician", "Hi5Central")' 'Connect must show its attended window immediately rather than waiting silently for signaling.'
Require-Literal $main 'supportWindow.SetConnectionState("Connecting securely...", false)' 'Connect startup must visibly show that the secure support channel is being established.'
Require-Literal $main 'Hi5CentralConnectSingleInstanceV2' 'Connect must suppress duplicate attended launches in the same Windows logon session.'
Require-Literal $main 'duplicate attended launch ignored' 'Connect duplicate-launch handling must remain explicit and diagnosable.'
Require-Literal $main 'FindWindowW(nullptr, L"Hi5Central Connect")' 'A duplicate Connect launch must restore the existing attended window rather than spawning another host.'
Require-Literal $main 'supportWindow.SetConnectionState("Reconnecting...", false)' 'Connect must visibly enter reconnect state after a transport loss.'
Require-Literal $main 'reconnectBackoffSeconds = std::min(reconnectBackoffSeconds * 2, 30)' 'Connect host reconnect must use bounded exponential backoff.'
Require-Literal $main 'type.rfind("remote_file_", 0) == 0' 'Connect file operations must be dispatched only through the consent-gated file channel.'
Require-Literal $senderHeader 'void handleInputEvent(const nlohmann::json& msg);' 'WebRTC sender must expose the existing input injector for portable fallback input.'
Require-Literal $senderSource 'm_injector.handleMessage(msg);' 'Portable fallback input must use the existing Windows input injector.'
Require-Literal $cmake 'src/platform/windows/windows_input.cpp' 'Windows builds must compile the platform input injector checked by this contract.'
Require-Literal $cmake 'src/ui/native_connect_window.cpp' 'Windows builds must compile the dedicated Connect customer window.'
Require-Literal $cmake 'src/connect_file_browser.cpp' 'Windows builds must compile the attended Connect file browser.'
Require-Literal $cmake 'src/connect_capture_bridge.cpp' 'Windows builds must compile the temporary managed-Agent capture bridge used after attended elevation.'
Require-Literal $connectFiles 'remote_file_list_request' 'Connect file browser must support consent-gated folder listing.'
Require-Literal $connectFiles 'remote_file_download_request' 'Connect file browser must support downloads.'
Require-Literal $connectFiles 'remote_file_upload_start' 'Connect file browser must support chunked uploads.'
Require-Literal $connectFiles 'IsProtectedPath' 'Connect file browser must retain protected/system-path safeguards.'
Require-Literal $signalingSource 'return m_ws == candidate;' 'Signaling reconnect must ignore stale WebSocket callbacks.'
Require-Literal $connectWindow 'WS_MINIMIZEBOX' 'Connect customer window must be minimisable.'
Require-Literal $connectWindow 'WS_EX_APPWINDOW' 'Connect customer window must appear as a normal taskbar application.'
Require-Literal $connectWindow 'CenterWindow()' 'Connect customer window must open centred on the customer screen.'
Require-Literal $connectWindow 'L"End session"' 'Connect customer window must expose an obvious End session action.'
Require-Literal $connectWindow 'L"Hi5Central Remote Support"' 'Connect customer window must clearly identify the active support application.'
Require-Literal $connectWindow 'CreateBrandIcon' 'Connect attended window must use Hi5Central branding rather than the generic Windows information icon.'
Require-Literal $connectWindow 'PaintSessionCard' 'Connect attended window must render the modern session summary card.'
Require-Literal $connectWindow 'PaintTrustCard' 'Connect attended window must render the temporary-session trust card.'
Require-Literal $connectWindow 'PaintChat' 'Connect attended window must render its custom chat surface.'
Require-Literal $connectWindow 'kTechBubble' 'Connect chat must retain technician bubble styling.'
Require-Literal $connectWindow 'kUserBubble' 'Connect chat must retain customer bubble styling.'
Require-Literal $connectWindow 'PaintComposer' 'Connect attended window must render the modern chat composer.'
Require-Literal $connectWindow 'PaintFooter' 'Connect attended window must render the dedicated destructive-session footer.'
Require-Literal $connectWindow 'CenteredSquareRect' 'Connect End session stop glyph must be centred mathematically at every DPI.'
Require-Literal $connectWindow 'DrawInfoIcon' 'Connect attended window must use a crisp filled information icon.'
Require-Literal $connectWindow 'DrawAttachmentIcon' 'Connect attended window must use the crisp attachment glyph.'
Require-Literal $connectWindow 'DrawPaperPlaneIcon' 'Connect attended window must use the crisp filled Send glyph.'
Require-Literal $connectWindow 'ANTIALIASED_QUALITY' 'Connect taskbar/titlebar branding must avoid ClearType colour fringing inside icon bitmaps.'
Require-Literal $connectWindow 'L"End session"' 'Connect attended window must expose the styled End session action.'
if ($connectWindow.Contains('CreateWindowExW(0, L"BUTTON"')) { throw 'Connect attended window must not regress to generic Win32 push buttons.' }
if ($connectWindow.Contains('WS_EX_CLIENTEDGE')) { throw 'Connect attended window must not regress to generic client-edge chat controls.' }
Require-Literal $connectWindow 'MAKEINTRESOURCEW(32512)' 'Connect customer window must use a wide Win32 cursor resource with the explicit W API.'
Require-Literal $connectWindow 'MAKEINTRESOURCEW(32649)' 'Connect custom actions must expose a hand cursor on hover.'
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
