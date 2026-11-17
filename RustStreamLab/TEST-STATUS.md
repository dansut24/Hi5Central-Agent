# Hi5Central Rust StreamLab — Test status, 2026-11-17

## Completed
- Isolated HTTPS and Node/WebSocket pairing relay responds at https://rust-streamlab.hi5central.com/
- Native Windows GDI capture, Rust BGRA→I420 conversion, direct libvpx VP8/VP9 encoder, Rust WebRTC source are implemented
- Linux dependency build/check and 5 unit tests passed
- No FFmpeg process, executable or dependency in Rust StreamLab Windows host code
- Go StreamLab and production C++ RMM agent were not changed

## Not yet qualified
- Windows x64 native build with libvpx DLL packaged
- Windows GDI desktop capture in an interactive unlocked user session
- TURN/ICE, VP8/VP9 browser playback on mobile and desktop
- 15/30/45/60 FPS, bitrate, quality and 30-second performance profiles
- Fair C++ / Go / Rust comparative measurements

## Acceptance test
1. Download the Windows CI artifact named Hi5CentralRustStreamLab-Win64-native from the Rust StreamLab Native Windows workflow.
2. Extract EXE plus bundled DLL to one folder and run with `--version` first.
3. Run EXE locally in PowerShell on a Windows 10/11 PC with an unlocked desktop.
4. Open https://rust-streamlab.hi5central.com/ in a second browser or iPhone.
5. Pair with short-lived code printed by EXE, choose VP8, 1280px, 30 FPS, 8 Mbps, Start.
6. Move/scroll windows continuously and confirm browser decoded frames with host encoded frames increasing.
7. Repeat at 60 FPS, then repeat for VP9; export JSON from viewer each time.
8. Confirm Disconnect/Stop closes WebRTC and EXE closes without changes to installed Hi5CentralAgent.
9. Run from an external mobile network, checking TURN connectivity and browser frame statistics.

Do not claim production or end-to-end readiness unless these pass.
