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
$streamer = Get-Content 'Agent/chatpass_agent/src/streamer_main.cpp' -Raw
$h264Mf = Get-Content 'Agent/chatpass_agent/src/h264_mf_encoder.cpp' -Raw
$vp9Mf = Get-Content 'Agent/chatpass_agent/src/vp9_mf_encoder.cpp' -Raw

# Desktop Viewer parity / recovery
Require-Literal $viewer 'scheduleViewerReconnect' 'Desktop Viewer signaling reconnect engine is missing.'
Require-Literal $viewer 'scheduleViewerTransportProbe' 'Desktop Viewer delayed transport probe is missing.'
Require-Literal $viewer 'endpointRestartUntil' 'Desktop Viewer endpoint-restart suppression state is missing.'
Require-Literal $viewer 'case "agent_reconnecting"' 'Desktop Viewer does not handle Agent restart interruptions.'
Require-Literal $viewer 'Waiting for endpoint' 'Desktop Viewer must visibly wait through endpoint restart.'
Require-Literal $viewer 'media-negotiation-timeout' 'Desktop Viewer media-negotiation timeout is missing.'
Require-Literal $viewer 'DESKTOP_ADAPTIVE_TIERS' 'Desktop Viewer adaptive native-quality profile is missing.'
Require-Literal $viewer '["video/vp9", "video/h264", "video/vp8"' 'Desktop Viewer must preserve VP9-first native desktop quality with H.264 and VP8 fallback.'
Require-Literal $viewer "Native · 16 Mbps · 30 fps" 'Desktop Viewer must preserve the high-quality native 16 Mbps tier.'
Require-Literal $sender 'ProbeHardwareCodecAvailable("h264", &h264EncoderName)' 'Stable Auto must qualify H.264 hardware before building the SDP offer.'
Require-Literal $sender 'SDP stable auto offer VP9=98 preferred' 'Stable Auto must prefer native-resolution VP9 when the endpoint resource gate allows it.'
Require-Literal $sender 'action=stay_h264_cpu_fallback' 'Negotiated H.264 must fall back to CPU/software H.264 if the zero-copy path fails.'
Require-Literal $viewer 'packetTotal>=120' 'Desktop Viewer quality loss calculation must require a meaningful RTP sample.'
Require-Literal $viewer 'sendDesktopStreamProfile' 'Desktop Viewer live stream-profile control is missing.'
Require-Literal $viewer 'REMOTE_DESKTOP_JITTER_BUFFER_TARGET_MS = 20' 'Desktop Viewer low-latency jitter-buffer target is missing.'
Require-Literal $viewer 'applyLowLatencyReceiverHint(ev.receiver' 'Desktop Viewer does not apply the receive-side low-latency hint.'
Require-Literal $viewer 'jitterBufferTargetDelay' 'Desktop Viewer does not measure the browser jitter-buffer target delay.'
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
Require-Literal $sender 'action=reduce_vp9_fps_in_place' 'Negotiated VP9 no longer degrades in-place under sustained encoder pressure.'
Require-Literal $sender 'm_codecPressureLevel' 'VP9 encoder-pressure recovery state is missing.'
Require-Literal $h264Mf 'CODECAPI_AVEncVideoForceKeyFrame' 'H.264 Media Foundation force-keyframe control is missing.'
Require-Literal $vp9Mf 'CODECAPI_AVEncVideoForceKeyFrame' 'VP9 Media Foundation force-keyframe control is missing.'
Require-Literal $vp9Mf 'MFSampleExtension_CleanPoint' 'VP9 must use the encoder output clean-point flag for real keyframe detection.'
Require-Literal $vp9Mf 'pendingInputs' 'VP9 Media Foundation buffered-output metadata tracking is missing.'
Forbid-Literal $vp9Mf 'forceKeyframe || m_frameIndex <= 2' 'VP9 must not fabricate keyframe status from a request or frame index.'
Forbid-Literal $h264Mf 'ContainsH264Idr(encoded.data) || meta.forceKeyframe' 'H.264 must not mark a non-IDR frame as keyframe.'
Require-Literal $service 'HandleRemoteFileUploadCancel' 'Endpoint upload cancellation is missing.'
Require-Literal $streamer 'cmd.key.vk == VK_PAUSE || cmd.key.vk == VK_SNAPSHOT' 'Pause/Print Screen special Windows injection path is missing.'
Require-Literal $streamer 'in.ki.wVk = cmd.key.vk' 'Pause/Print Screen must use Windows virtual-key synthesis.'
Require-Literal $service 'std::filesystem::remove(target' 'Cancelled/abandoned upload partial-file cleanup is missing.'

# CAD / secure desktop responsiveness keeps safety checks but starts fallback quickly.
Require-Literal $service 'uacDetectedAt >= std::chrono::milliseconds(220)' 'Secure-desktop fallback timing drifted from the qualified fast path.'
Require-Literal $service 'lastSecureLaunchAttempt >= std::chrono::milliseconds(200)' 'Secure helper retry timing drifted from the qualified fast path.'
Require-Literal $service 'secureTransitionBlank' 'Secure transition blank-frame safeguard is missing.'
Require-Literal $service 'desktop_handoff_ready' 'Desktop handoff-ready signaling is missing.'
Require-Literal $service 'secure_desktop_ready' 'Secure desktop ready signaling is missing.'
Require-Literal $viewer 'completeDesktopSourceTransition' 'Desktop Viewer must release the UI immediately when the secure/normal source is ready.'
Require-Literal $viewer 'if (!force && (secureDesktopActive || desktopHandoffActive)) return;' 'Desktop Viewer must suppress remote input during source handoff without blocking local UI interaction.'
Require-Literal $viewer 'elOverlayTitle.textContent = ""' 'Desktop transition hold must clear stale Connecting overlay text.'
Require-Literal $viewerHtml 'pointer-events: none;' 'Desktop transition overlay must not intercept input while the existing stream remains visible.'

Write-Host 'Remote Viewer / Agent contract check passed.'
