$ErrorActionPreference = 'Stop'

function Require-Literal {
    param([string]$Content, [string]$Literal, [string]$Message)
    if (-not $Content.Contains($Literal)) { throw $Message }
}
function Forbid-Literal {
    param([string]$Content, [string]$Literal, [string]$Message)
    if ($Content.Contains($Literal)) { throw $Message }
}

$viewer = Get-Content 'Viewer/chatpass_viewer/web/renderer.js' -Raw
$viewerHtml = Get-Content 'Viewer/chatpass_viewer/web/index.html' -Raw
$service = Get-Content 'Agent/chatpass_agent/src/platform/windows/windows_service.cpp' -Raw
$sender = Get-Content 'Agent/chatpass_agent/src/webrtc_sender.cpp' -Raw

# Desktop Viewer parity / recovery
Require-Literal $viewer 'scheduleViewerReconnect' 'Desktop Viewer signaling reconnect engine is missing.'
Require-Literal $viewer 'scheduleViewerTransportProbe' 'Desktop Viewer delayed transport probe is missing.'
Require-Literal $viewer 'endpointRestartUntil' 'Desktop Viewer endpoint-restart suppression state is missing.'
Require-Literal $viewer 'case "agent_reconnecting"' 'Desktop Viewer does not handle Agent restart interruptions.'
Require-Literal $viewer 'Waiting for endpoint' 'Desktop Viewer must visibly wait through endpoint restart.'
Require-Literal $viewer 'media-negotiation-timeout' 'Desktop Viewer media-negotiation timeout is missing.'
Require-Literal $viewer 'DESKTOP_ADAPTIVE_TIERS' 'Desktop Viewer adaptive native-quality profile is missing.'
Require-Literal $viewer 'packetTotal>=120' 'Desktop Viewer quality loss calculation must require a meaningful RTP sample.'
Require-Literal $viewer 'sendDesktopStreamProfile' 'Desktop Viewer live stream-profile control is missing.'
Require-Literal $viewer 'viewerReconnectCooldownUntil = Date.now() + 10000' 'Desktop Viewer post-recovery cooldown is missing.'
Require-Literal $viewer 'createDataChannel("viewer-mouse-move", { ordered: false, maxRetransmits: 0 })' 'Low-latency unordered desktop mouse channel is missing.'
Require-Literal $viewer 'const inputChannel = controlDc && controlDc.readyState === "open" ? controlDc : inputDc' 'Desktop clicks/keyboard must prefer the dedicated low-latency control channel.'
Require-Literal $viewer 'desktopResolutionProfile' 'Desktop Viewer resolution preference must feed the live stream profile.'
Require-Literal $viewer "localStorage.setItem('hi5.viewer.resolution'" 'Desktop resolution preference persistence is missing.'
Require-Literal $viewer 'applyDesktopScalePreference' 'Desktop scale preference wiring is missing.'
Require-Literal $viewer "window.addEventListener('offline'" 'Desktop Viewer offline recovery handling is missing.'
Require-Literal $viewer "scheduleViewerTransportProbe('network-online'" 'Desktop Viewer online recovery probe is missing.'
Require-Literal $viewerHtml 'desktop-quality-indicator' 'Desktop Viewer visible quality indicator is missing.'

# First-valid-frame handoff. Never restore the old two-frame / 700ms delay.
Require-Literal $viewer 'const firstRenderedFrame = !hasEverRenderedFrame' 'Desktop Viewer first-frame reveal contract is missing.'
Forbid-Literal $viewer 'desktopModePendingFrames >= 2' 'Desktop Viewer reintroduced the multi-frame handoff delay.'

# Keyboard correctness / shortcut semantics
Require-Literal $viewer "getModifierState?.('AltGraph')" 'Desktop Viewer AltGraph handling is missing.'
Require-Literal $viewer 'commandModified' 'Desktop Viewer must keep Ctrl/Alt/Meta printable keys on the physical-key path.'
foreach ($mapping in @('Backquote','Minus','Equal','BracketLeft','BracketRight','Backslash','IntlBackslash','Semicolon','Quote','Comma','Period','Slash','NumpadMultiply','NumpadAdd','NumpadSubtract','NumpadDecimal','NumpadDivide','NumpadEnter','NumpadEqual','NumpadComma','CapsLock','NumLock','ScrollLock','Pause','PrintScreen','ContextMenu','BrowserBack','BrowserForward','AudioVolumeMute','AudioVolumeDown','AudioVolumeUp','MediaTrackNext','MediaTrackPrevious','MediaStop','MediaPlayPause','LaunchMail','LaunchMediaPlayer','LaunchApp1','LaunchApp2','Sleep')) {
    Require-Literal $service ('code == "' + $mapping + '"') "Windows key map is missing $mapping."
}
Require-Literal $service 'VK_F13' 'Windows key map must support F13-F24.'

# Adaptive profile / transfer cleanup contracts
Require-Literal $sender 'target_bitrate_kbps' 'Viewer-requested bitrate target support is missing.'
Require-Literal $service 'HandleRemoteFileUploadCancel' 'Endpoint upload cancellation is missing.'
Require-Literal $service 'std::filesystem::remove(target' 'Cancelled/abandoned upload partial-file cleanup is missing.'

# CAD / secure desktop responsiveness keeps safety checks but starts fallback quickly.
Require-Literal $service 'uacDetectedAt >= std::chrono::milliseconds(220)' 'Secure-desktop fallback timing drifted from the qualified fast path.'
Require-Literal $service 'lastSecureLaunchAttempt >= std::chrono::milliseconds(200)' 'Secure helper retry timing drifted from the qualified fast path.'
Require-Literal $service 'secureTransitionBlank' 'Secure transition blank-frame safeguard is missing.'
Require-Literal $service 'desktop_handoff_ready' 'Desktop handoff-ready signaling is missing.'
Require-Literal $service 'secure_desktop_ready' 'Secure desktop ready signaling is missing.'

Write-Host 'Remote Viewer / Agent contract check passed.'
