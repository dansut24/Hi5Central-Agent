# Hi5Central Rust StreamLab — Native Software Video

Isolated, read-only streaming experiment **without FFmpeg**. Existing production C++ Agent and Go StreamLab are untouched.

- Site: https://rust-streamlab.hi5central.com/
- Windows host: `RustStreamLab/target/release/hi5-rust-streamlab.exe`
- Capture: native Windows GDI BitBlt, a reusable DIB
- Conversion: Rust BGRA→I420, optional native nearest-neighbor scaling
- Encoding: libvpx through direct Rust FFI (VP8/VP9); no JPEG fallback
- WebRTC: Rust `webrtc` crate with ICE/TURN returned by dedicated relay
- Pairing: isolated, expiring 8-digit code; 1 viewer per pairing code
- RMM: **none** (no service changes, enrollment, credentials, remote input, patching)

## Windows requirements
Windows 10/11 x64, active and unlocked interactive user desktop, signed-off local test EXE built by the isolated GitHub workflow, and `vpx.dll` next to EXE if built dynamically. No FFmpeg is needed.

1. From GitHub Actions download **Hi5CentralRustStreamLab-Win64-native**, extract the ZIP.
2. Run `hi5-rust-streamlab.exe` from PowerShell (in the extracted directory).
3. A short-lived eight-digit pairing code is printed.
4. On a second machine or mobile, open `https://rust-streamlab.hi5central.com/` and pair using that code.
5. Choose VP8 or VP9, output width, 15/30/45/60 FPS target, and bitrate. Press **Start video test**.
6. Move/windows scroll on target PC. Compare **measured** host FPS to independently decoded browser FPS.
7. Press **Export JSON**, then **Stop stream**. Repeat for VP9 and other profiles.
8. Close the EXE to stop pairing. It never installs as a service.

## Correctness and safety
- Only existing TURN host is reused, with short-lived TURN REST credentials. No JWTs or RMM account tokens are issued.
- GDI cannot capture secure desktop/UAC without interactive access; don't use this test for UAC scenarios.
- No end-user consent/authorization UI other than local EXE and pairing code; use only on devices you own/are authorised to view.
- Screen pixels are transmitted over WebRTC and visible to the paired browser. Avoid confidential content.
- This is experimental software, not a production remote access agent.
- Host FPS measures **compressed frame production**. Browser FPS measures **decoded frames**. A 60 FPS target is not evidence of actual 60 FPS.
- Native WebRTC and TURN require an actual Windows host/client integration run before claiming end-to-end validation.
- The native codec is still C libvpx, called through Rust; it is not a purely Rust codec implementation.

## Comparative testing
Compare C++, Go and Rust sequentially on the same Lenovo device in the same network setup. Fix resolution, codec, FPS and bitrate; run 30 seconds with identical moving on-screen content; export diagnostics; repeat 3 times per profile. Measure CPU %, RAM, captured/encoded/decoded FPS, p95 capture/convert/encode, missed frame deadlines, jitter, packet loss and interactivity.

## Build manually
Use Rust MSVC stable, libvpx headers/import libs (e.g. vcpkg `libvpx:x64-windows`), LLVM/libclang for generated bindgen and environment variables `VPX_LIB_DIR`, `VPX_INCLUDE_DIR`, `VPX_VERSION` (label), and `VPX_DYNAMIC=1`. Build with `cargo build --release --locked --manifest-path RustStreamLab/Cargo.toml`. Bundle the matching `vpx.dll` with executable.

## 2026-11-17 Safari black-screen diagnostic and fix

The initial viewer export showed Safari rejecting remote ICE candidates (each candidate was assigned a fabricated sdpMid "video") and staying in ICE state "new". The webrtc-rs 0.13 library returns an empty MID but sdpMLineIndex=0 in candidate JSON. The revised agent forwards the m-line index and never invents a MID; the viewer likewise consumes the index and queues early candidates. The Rust agent must be recompiled for the signalling fix. The viewer JS is deployed independently on rust-streamlab.hi5central.com.

GDI capture baseline: BitBlt with CAPTUREBLT averaged ~157ms at 1706x1066 and could only generate ~5.9 FPS. New Windows build defaults to plain SRCCOPY without CAPTUREBLT to test the lower-overhead capture path. For A/B or layered-window compatibility, launch PowerShell with `$env:HI5_RUST_CAPTUREBLT='1'; .\hi5-rust-streamlab.exe`. This is experimental until measured on the Lenovo. Use a fresh viewer JSON after upgrading the EXE.
