# Hi5Central iOS Auto Keyboard — Windows Agent v1.0.506 (12 Nov 2026)

## Confirmed failure of 1.0.505
- User installed iOS v0.5.15 + Lenovo Windows Agent v1.0.505 and enabled `HKLM\SOFTWARE\Hi5Central\Agent\RemoteFocusTrial=1` DWORD.
- Original IONOS VPS RMM sessions verified 19:34:46–19:37:16 UTC on 2026-11-12. VP9 video streamed smoothly and remote text input worked; Windows Search, Notepad and URL-bar clicks did not auto-open the iPhone keyboard.
- Read-only PowerShell through live RMM Agent showed `RemoteFocusTrial` present DWORD1, `FOCUS_HELPER_PROCESS_COUNT=0`, zero `focus-helper` startup log events.
- Running binary `C:\Program Files\Hi5Tech\Agent\Hi5CentralAgent.exe`: version 1.0.505 according to RMM; SHA256 `420AA70A06C2F2554DF8AC052715598D298A9690BC137C3B47DD01F1A802CB80`; compiled string `focus-probe` present but `RemoteFocusTrial` / `input_focus_editable` / focus launch log strings absent.
- **Root cause:** previous v1.0.505 work edited `src/service/service_main.cpp`, which is NOT compiled by the Windows CMake target. The real service is `src/platform/windows/windows_service.cpp`, explicitly included in `CMakeLists.txt`. The separate UIA helper and IPC classes were compiled, but the service never launched/read/forwarded them. GitHub CI succeeded without an executable feature-symbol check.

## Corrected v1.0.506 branch
Branch: `fix/rmm-agent-v1506-compiled-focus-service`
Workflow: https://github.com/dansut24/Hi5Central-Agent/actions/runs/43292976797
- Adds actual focus helper lifecycle into `src/platform/windows/windows_service.cpp`, not the uncompiled duplicate service.
- Gates helper on local `RemoteFocusTrial=1`, active console, normal desktop, no UAC/backstage and at least 5 delivered frames.
- Starts `--mode focus-probe` via the existing session-aware elevated-default-desktop helper launcher. Separate OS process; no UIA on the video capture/RTP thread.
- Reads existing `InputPipeWriter/Reader` shared-memory focus flags; sends only `{type:"session_state",state:"input_focus_editable"|"input_focus_cleared", keyboard_kind:"text"|"url"|"password"}` to authorized RMM session. No text, title, URL or password is transmitted.
- Stops the helper on UAC, streamer reset or session end. Does not restart codec, replace video capture, TURN, WebRTC, or tenant API.
- CI changed to v1.0.506 and fails if the built `build/Release/Hi5CentralAgent.exe` does not contain `RemoteFocusTrial`, `input_focus_editable`, `[focus-helper] launched isolated process`, `--mode focus-probe`. This prevents another green build missing the actual integration.

## Dependencies/recovery
- iOS IPA v0.5.15 already built green: https://github.com/dansut24/Hi5Central-Platform/actions/runs/43286319977
- Original VPS `hi5central-rmm-api-1` inspected on 12 Nov: image `hi5central/rmm-api:agent305-codec-failover-canary-20261101`, healthy. Actual outbound Agent forwarding allowlist `j1` includes `session_state`, not `remote_focus_state`; it forwards payload with `et(v,y)`.
- The live installed Lenovo Agent is STILL v1.0.505 until explicit next upgrade. Do not assert 1.0.506 deployed before checking.
- Registry key remains enabled on Lenovo. No need to set it again after upgrade.
- Preserve working v1.0.505 installer as rollback; registry can be disabled with administrator PowerShell:
  `Set-ItemProperty 'HKLM:\SOFTWARE\Hi5Central\Agent' -Name RemoteFocusTrial -Value 0`
- User wants native iOS keyboard as default, Windows shortcut toolbar, selectable custom laptop keyboard; already in v0.5.15.
- No access to or changes on any second EU server. Only the original IONOS VPS is involved.

## Next verified test sequence (only after green v1.0.506 Windows installer)
1. Verify the workflow passes C++ compile + new executable-string checks; confirm artifact `Hi5Central-Windows-Agent-v1.0.506-CompiledFocus-Service-Test`.
2. Install **Lenovo only**; check RMM online and version v1.0.506, verify 15+ seconds normal VP9 video and touch.
3. Start a fresh iOS v0.5.15 remote session. Windows service logs should contain `[focus-helper] launched isolated process` after 5 normal frames.
4. Click Windows Search, Notepad and Edge address bar. Inspect Agent logs for `[focus-helper] state changed kind=1/2`; on iPhone Connection details should show `Windows text focus: text/url` and open iOS keyboard.
5. Click blank desktop -> `kind=0`, automatically opened keyboard should hide; a manually opened keyboard should remain.
6. Confirm Laptop/iOS switch and Windows key strip still work.
7. Secure desktop transition should stop helper, preserving video; no password/text is sent to server.
8. If helper launches but UI does not open, inspect session_state websocket end-to-end and iOS `NativeRemoteEngine.read`/SwiftUI focus revision. Do not modify working video/capture unless evidence requires.
