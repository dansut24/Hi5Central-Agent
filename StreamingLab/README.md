# Hi5Central Streaming Lab — isolated cross-device remote streaming test

**Status:** VP8 standalone Windows sender + browser viewer + one-time pairing + short-lived TURN credentials. **This is not the RMM agent.** It never enrols a device, runs privileged background service code, injects user input, or uses RMM API/database/session infrastructure.

## Use it now

1. On the **feature/streaming-core** GitHub Actions run, download the green **Hi5StreamingLab-Remote-Windows** build artifact (not the older "Hi5StreamingLab-Windows" localhost release).
2. Extract all files into the same folder on the Lenovo/Dell Windows desktop.
3. Double-click **Start-Hi5StreamingLab.cmd** (or run \`Hi5StreamingLab.exe --remote\` from a console).
4. Wait for the EXE to display its **eight-digit one-time code**, valid for 10 minutes. Keep the console open.
5. On your iPhone, Android or another PC, open **https://streamlab.hi5central.com/**, enter that code and press **Pair device**.
6. Select software VP8, 30 FPS, 8 Mbps and **Start VP8 test**. Repeat at 45 then 60 FPS where the hardware can sustain it.
7. Export diagnostic JSON from the browser. The host console and browser event log report signalling/SDP/ICE failures instead of silently hanging.

**Security:** Every pairing code is random, single use and short-lived. Only the paired viewer can trigger capture. The lab has no mouse or keyboard control, remote shell, file transfer or unattended access. Stop with Ctrl+C or by closing the EXE. Viewers on mobile data reach the public page over HTTPS, but the actual stream is end-to-end WebRTC media (TURN relay only when needed).

## Architecture

- \`native/main.cpp\`: Windows GDI/BitBlt capture → libyuv BGRA/I420 → software libvpx VP8 → RTP → libdatachannel WebRTC.
- \`relay/server.js\`: isolated pairing & SDP/ICE exchange on the original VPS, exposed only at \`https://streamlab.hi5central.com/\` and \`wss://streamlab.hi5central.com/ws\` (the existing Windows EXE may continue using \`wss://hi5central.com/streamlab/ws\`). No production RMM services are used.
- \`relay/remote-viewer.html\`: standalone iOS/Android/desktop browser viewer with bitrate/FPS controls and host/browser metrics.
- \`Start-Hi5StreamingLab.cmd\`: normal-user Windows console launcher. No installer or scheduled task.
- The relay issues TURN REST credentials with a short lifetime from the VPS's **existing coturn shared secret**, which stays on the VPS. Nothing secret is bundled into the app. The TURN server and RMM services are unchanged.
- **Optional legacy local diagnostic**: \`Hi5StreamingLab.exe --local\` then \`http://127.0.0.1:8787\` on that same Windows computer, using bundled \`viewer.html\`. This mode does not use public pairing.

## Test plan

1. Verify the Windows EXE displays a code and the webpage shows "Paired — ready".
2. Verify a live desktop frame appears and VP8 is reported in browser WebRTC stats.
3. Test the same workload at 30/45/60 FPS and export JSON (capture, convert, encode, send, browser decode, jitter, RTT, dropped frames).
4. Repeat on the iPhone over Wi-Fi, then mobile data with relay candidates where required.
5. Qualify VP9, AV1, H.264 (software + hardware), and H.265 incrementally only after the VP8 pipeline is proven. They are **not yet implemented in this standalone lab**.
6. In the next release, add a diagnostic connection summary for relay vs direct ICE and monitor codec/latency over reconnects.

## Limits

- The current EXE captures the primary Windows desktop in an interactive user session. It does not support UAC, Session 0 or secure-desktop capture yet.
- This is a controlled diagnostic tool, not an unattended remote-support application.
- Real-world WebRTC is not fully verified just because GitHub compiles. Only actual on-device streaming confirms runtime behaviour.
- TURN is relay infrastructure, not signalling: the isolated, short-lived pairing relay is necessary for browser-to-host connection establishment.

## Build locally

Use Visual Studio's x64 Developer Command Prompt, CMake/Ninja and vcpkg with \`libdatachannel[srtp]\`, \`libvpx\`, \`libyuv\` and \`nlohmann-json\` in \`x64-windows-static\`. Then:

\`\`\`cmd
cmake -S StreamingLab -B StreamingLab/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_CXX_COMPILER=cl
cmake --build StreamingLab/build --config Release
\`\`\`

## Changes / rollback

The lab source is on \`feature/streaming-core\`; no merge into \`main\`. The webpage is a separate Docker service, routed through a narrow existing Caddy marketing-host path. Rollback: remove the isolated Caddy path block, reload Caddy, stop/remove the \`hi5central-streamlab\` container. Existing production routes and services remain unaffected.
