# Hi5Central Go Video Streaming Lab (VP8 / VP9)

This is a separate **read-only video experiment** for comparing the existing C++ Hi5Central RMM stream with a Go/Pion WebRTC implementation.

**Live viewer:** https://go-streamlab.hi5central.com/

## Architecture

```
Windows desktop → FFmpeg gdigrab → libvpx (VP8 or VP9) → IVF encoded video frames
                → Go Pion WebRTC/RTP/SRTP → TURN (existing primary VPS) → browser <video>
```

This is real WebRTC video: **not JPEG frames** and not a JPEG stream with a video wrapper. Go is responsible for the transport/signaling/concurrency. The video codecs are provided by libvpx through FFmpeg; Go does not ship native VP8/VP9 encoders.

## Test without modifying C++ agent

1. Download the unsigned Windows x64 test ZIP from [Hi5Central downloads](https://downloads.hi5central.com/go-streamlab/Hi5Central-Go-VideoLab-Win64-v0.1.0.zip).
2. Make sure `ffmpeg.exe` on the Windows test machine has `gdigrab`, `libvpx` and `libvpx-vp9`. If needed, `winget install -e --id Gyan.FFmpeg` then open a new terminal.
3. Run `Start-GoStreamLab.ps1` in an interactive Windows desktop session. Do **not** install as a service.
4. The console prints a short-lived eight-digit pairing code. Open https://go-streamlab.hi5central.com/ on your PC or phone and enter it.
5. Choose VP8 or VP9, 15/30/45/60 FPS target, width and bitrate; press **Start video test**.
6. Generate movement on the Windows desktop. Compare the **Go sender encoded FPS** and **browser decoded FPS**. Export diagnostic JSON.
7. Repeat with the existing independent C++ Streaming Lab and the same resolution, content and network, to compare fairly.

The video is read-only. No mouse, keyboard, file management, command execution, RMM enrollment, software deployment, or Windows service replacement is implemented in this video experiment. JPEG is a **future optional diagnostic**, not a replacement for VP8/VP9.

## Build

Requires Go 1.24+ with Internet access to download the pinned modules.

```powershell
cd Agent\go-streamlab
go mod download
go test ./...
$env:GOOS = "windows"; $env:GOARCH = "amd64"; $env:CGO_ENABLED = "0"
go build -trimpath -o Hi5CentralGoStreamLab-Win64.exe ./cmd/go-streamlab
```

The Windows capture/codec process is separate from the Go WebRTC sender. FFmpeg is **not included** in the ZIP; its redistribution licence is separate.

## Relay deployment

`portal/server.js` and `portal/remote-viewer.html` are independently hosted on the primary Hi5Central VPS behind HTTPS at `go-streamlab.hi5central.com`. The Node relay has an independent room map, origin restriction, short-lived single-use pairing codes and connects to the platform's existing TURN server. Media is end-to-end protected by WebRTC SRTP; only signaling passes through the relay.

Production C++ RMM signaling, remote control sessions, agent identity and tenant API remain unchanged.

## Comparison next steps

- Qualify the Go Windows sender against a live desktop and verify VP8 then VP9 on desktop and mobile.
- Capture true observed encoder FPS, browser decoded FPS, jitter, lost packets, p95 glass-to-glass latency, CPU/RAM and Windows session reliability.
- Add a native Windows capture experiment and measure GDI frame-age improvements.
- After qualification, integrate the Go WebRTC module into the independently enrolled Go RMM agent; parity with C++ device management is **not yet complete**.
