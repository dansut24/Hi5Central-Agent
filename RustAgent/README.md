# Hi5Central Rust Agent Lab (isolated v0.1.0)

This is a **separate** experimental Rust executable for performance comparison with the existing C++ agent and the planned Go agent. It does **not** install a service, enroll a device, alter the production C++ agent, or contact the live API.

## What is implemented

- Real Windows primary-desktop capture through top-down GDI/BitBlt into a reusable DIB section.
- Deterministic synthetic moving-content test for repeatable comparisons.
- BGRA -> I420 (software) conversion in Rust with unit tests.
- 15/30/45/60 FPS deadline pacing with stale-frame skipping rather than backlog.
- Optional real VP8 and VP9 **software encoding via libvpx**, with explicit VP8 CPUUSED=8, VP9 CPUUSED=6 and row-mt settings through Rust native bindings. This is not JPEG and does not silently switch to it.
- Machine-readable JSON for capture, conversion, encoder and total frame p50/p95 latency, achieved FPS, missed deadlines, bytes and output packets.

**No remote viewing yet.** Signalling, WebRTC media transport, input, agent enrollment and secure portal integration require a subsequent phase. The unencoded test is NOT a video-streaming performance result.

## Build and run

Rust stable + Cargo. The no-codec smoke test needs no native dependencies:

```sh
cargo test --manifest-path RustAgent/Cargo.toml --no-default-features
cargo build --manifest-path RustAgent/Cargo.toml --release
./RustAgent/target/release/hi5-rust-agent-lab bench --source synthetic --codec none --width 1280 --height 720 --fps 60 --seconds 10 --json rust-no-codec.json
```

For **actual VP8/VP9** measurements, install libvpx headers and libraries plus pkg-config (on Windows use the same vcpkg libvpx version as the C++ build and ensure `vpx.pc` is reachable via `PKG_CONFIG_PATH`):

```sh
cargo build --manifest-path RustAgent/Cargo.toml --release --features vpx
./RustAgent/target/release/hi5-rust-agent-lab bench --source gdi --codec vp8 --fps 60 --seconds 30 --bitrate-kbps 8000 --json rust-vp8.json
./RustAgent/target/release/hi5-rust-agent-lab bench --source gdi --codec vp9 --fps 60 --seconds 30 --bitrate-kbps 8000 --json rust-vp9.json
```

On Linux, select `--source synthetic`. On Windows, `--source gdi` requires an **unlocked interactive desktop**; it cannot measure the Windows desktop from a headless VPS.

## Comparison protocol

1. Run the C++ and Rust binaries on the **same Windows device**, same user session and resolution, one after another; never concurrently.
2. Use software-only VP8 and VP9, fixed bitrate (e.g. 8 Mbps), fixed target 60 FPS, a 30-second motion workload, and the same codec library/version and screen content.
3. Record frame rate, missed deadlines, p95 capture/colour/encode/total latency, CPU, process memory, quality and input-to-display latency. The latter three require OS/process/video telemetry in a follow-up step.
4. Repeat three runs per language, warm up before measuring, alternate execution order, and compare medians.
5. Do not compare synthetic/no-codec Rust figures against live C++/Go video output.

## Safety and limitations

- Rust libvpx wraps a native C codec; a Rust frontend alone does not make VP9 encoding faster.
- No GDI secure-desktop support, remote control, WebRTC/TURN signalling, updater, installer, patching or Windows service in this first prototype.
- The existing production C++ baseline must remain untouched; develop the Rust integration behind separate service and test-device identifiers.
