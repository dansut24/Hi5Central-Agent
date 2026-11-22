# Hi5Central Virtual Display — v0.1 canary (22 November 2026)

**Status:** Experimental source and automated build. Not installed or qualified.

Purpose: supply one Windows virtual display for headless computers and closed-lid laptops, with a preferred **1920×1080 at 60 Hz** signal. Fallback 1600×900 and 1280×720 modes are also fixed at 60 Hz. This does **not** guarantee that StreamCore captures, encodes or presents 60 unique frames/second.

## Implementation

- Based on Microsoft's **Windows-driver-samples** IddCx UMDF reference driver, pinned to commit `d49bb7d72f3651952b2b6e4ad28d14fe2247eb69`.
- Rebranded PnP/software-device ID `Hi5VirtualDisplay` and `Root\Hi5VirtualDisplay`.
- EDID-less virtual output, not the sample's fictitious Dell/Lenovo EDIDs.
- `Hi5DisplayHost.exe` is a dedicated Windows Service Control Manager host. While the service is running, it creates the virtual PnP device; stopping the service removes the virtual monitor. Console diagnostics are available via `--console`. It is built, **not yet signed or verified on Windows**.
- Windows-driver-samples licence (Microsoft Public License) is included in the artifacts.
- No driver is installed, signed, or enabled in production by this workflow.

### Build

Use GitHub Actions **Build Hi5Central Virtual Display (canary)** or, on a Windows x64 machine with VS2026, WDK, NuGet and MSBuild:

```powershell
pwsh -File .\drivers\virtual-display\build.ps1
```

The script checks the exact upstream commit; if that commit changes, the build refuses to proceed until reviewed. It produces unsigned `Hi5VirtualDisplay.dll`, INF/catalog where generated, and `Hi5DisplayHost.exe`. Driver installation needs a valid Microsoft-accepted signature on standard Secure Boot systems. Do not disable code-integrity protection on production endpoints.

### Planned validation order

1. Build passes, package contents and signatures inspected. Obtain appropriate driver signing.
2. Install on a **VM first** and verify the virtual display appears as 1920x1080/60 Hz; verify clean removal.
3. Check the stream capture path sees the virtual output and truly captures from it.
4. Canary Lenovo with lid open, lid closed on AC power (Windows lid close action set to **Do nothing**), reopening, no monitor, UAC/secure desktop, reconnect/reboot.
5. Record capture FPS, encoded FPS, received/decoded/presented FPS separately at 30 and 60 FPS, including 10-second minimums and frame-age p95.
6. Integrate the service with the agent and implement display selection/fallback. Offer auto enable only when appropriate; preserve physical monitors when available.
7. Only after signed/qualified, add installer/uninstaller, Windows DisplayConfig mode control, health and rollback.

### Important limitations

- The virtual driver does not keep a sleeping, hibernating or powered-off machine running.
- The initial driver does not automatically activate on lid closure and does not replace the existing capture engine. A signed canary can be installed with `Install-Canary.ps1` (manual service start); `Uninstall-Canary.ps1` stops/removes only the host service.
- The upstream IddCx sample processes and releases swap-chain frames but is not a full production display driver. Profiling, crash recovery and lifecycle integration are outstanding.
