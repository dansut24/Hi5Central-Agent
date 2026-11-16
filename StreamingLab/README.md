# Hi5Central Streaming Lab — isolated experimental harness

This is **not the Hi5Central RMM agent**. It is a separate Windows console process and browser viewer, built only for capture/encode/transport benchmarking. It must never enrol into RMM, call RMM APIs, or install a Windows service.

## Stage 1 (VP8 local baseline)
- Native Windows EXE: persistent GDI/BitBlt capture, libyuv BGRA→I420, libvpx VP8, RTP over libdatachannel WebRTC
- Loopback-only HTTP signalling and standalone HTML viewer at `http://127.0.0.1:8787`
- FPS selection: 15/30/45/60. No adaptive FPS or quality changes hidden from the tester.
- Frame age and capture/convert/encode/send times, dropped-before-send frames, browser WebRTC stats
- No user input injection. No credentials or privileged service needed. Stop the EXE to stop sharing.
- Optional external TURN supplied via environment variables only. No RMM backend, database, production session signalling or other VPS services.

**Important:** The first milestone is local (same-machine) diagnostic streaming. This is deliberate: public remote access needs a separate authenticated signalling mechanism; the development lab must **not** expose an unauthenticated desktop to the internet. TURN alone does not supply session signalling.

## Run
1. Download the `Hi5StreamingLab-Windows` artifact from GitHub Actions on this branch.
2. Extract `Hi5StreamingLab.exe` and `viewer.html` into one directory.
3. Run `Hi5StreamingLab.exe` on a Windows user desktop (not as a Windows service).
4. Open `http://127.0.0.1:8787` in Edge or Chrome on the *same machine*. Press **Start VP8 test**.
5. Change FPS with the browser controls (requires a new lab session), move windows, scroll, run 2-5 minutes, download metrics JSON, and compare with Chrome Remote Desktop on the same PC.

**Optional TURN configuration** (not needed for localhost): set `HI5_LAB_TURN_URL` to `turn:turn.hi5central.com:3478`, `HI5_LAB_TURN_USER` and `HI5_LAB_TURN_PASS` using short-lived credentials. Never put secrets in Git, console logs or issue screenshots. Windows env vars apply to the running process and can be cleared after tests.

## Scope and benchmarks
- Stage 1: stable VP8 capture→decode at 1080p/30, then 60 where feasible, with per-stage timings
- Stage 2: VP9 and software H.264, AV1/H.265 and hardware H.264 *only after capability-specific implementation and negotiation*. Do not pretend the browser decodes a codec we haven't implemented.
- Stage 3: separate authenticated cross-network lab signalling with TURN relay, input controls and secure-desktop validation
- Keep a strict frame-age budget: never accumulate a multi-frame encode queue.
- If capture+convert or encoder consistently exceed 16.7ms at 60fps, diagnose that first rather than masking by auto-dropping resolution.

## Safety
- Listener is hardcoded to loopback. No LAN/WAN bind option. This lab is diagnostic-only and must not become an unattended access component.
- The EXE streams the primary user's desktop when the local viewer presses Start. Do not run on a machine showing sensitive content.
- No uploads or telemetry collection. Metrics remain local and can be exported manually.
- The RMM platform and its production TURN configuration remain unchanged.

## Build (Windows, vcpkg)
Install `libdatachannel[srtp]:x64-windows-static`, `libvpx:x64-windows-static`, `libyuv:x64-windows-static`, then:
```pwsh
cmake -S StreamingLab -B StreamingLab/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build StreamingLab/build --config Release
```
