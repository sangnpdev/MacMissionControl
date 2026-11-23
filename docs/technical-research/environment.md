# Environment Inspection (Phase 0)

Inspected 2026-11-23 on the development machine. All values below were read from the
machine, not assumed.

## Operating system

| Item | Value | Source |
|---|---|---|
| Edition | Windows 11 Pro (registry `ProductName` still says "Windows 10 Pro", a known Windows quirk) | `HKLM\...\Windows NT\CurrentVersion` |
| Version | 25H2 | `DisplayVersion` |
| Build | 26200.9457 | `CurrentBuild`, `UBR` |
| Architecture | x64 | |

## Hardware

| Item | Value | Note |
|---|---|---|
| CPU | AMD Ryzen 7 H 255 w/ Radeon 780M Graphics | matches target |
| RAM | 30 GB visible (32 GB installed, part reserved for iGPU) | |
| GPU | AMD Radeon 780M (integrated, shared memory) | GPU "dedicated" memory is a carve-out; most GPU allocations show as *shared* |
| Display 1 (`\\.\DISPLAY2`, **primary**) | 1920 x 1080 @ 240 Hz, at (0,0) | matches the stated target |
| Display 2 (`\\.\DISPLAY1`, laptop panel) | 2880 x 1800 @ 120 Hz, at (-2880,0) | extra monitor, different DPI and refresh rate |

The virtual screen is therefore 4800 x 1800 starting at x = -2880, with **mixed DPI and mixed
refresh rates**. `Win32_VideoController` reports only the laptop panel (2880 x 1800 @ 120 Hz),
so per-monitor values were read with `EnumDisplaySettings` and a Per-Monitor-V2 aware
`Screen.AllScreens`.

Consequence: the code must not assume a resolution or refresh rate. Frame pacing follows the
DWM compositor clock. Measured: the clock ticks at 240 Hz (4.167 ms) with both monitors active.

## Toolchain

| Tool | Version | Location |
|---|---|---|
| Visual Studio | 2026 Community (major 18) | `C:\Program Files\Microsoft Visual Studio\18\Community` |
| MSVC | 14.50.35717 | `VC\Tools\MSVC` |
| CMake | 4.2.3-msvc3 (bundled with VS) | `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` |
| Ninja | bundled with VS | `...\CMake\Ninja` |
| Windows SDK | 10.0.26100.0 (includes `cppwinrt` headers) | `C:\Program Files (x86)\Windows Kits\10` |
| Windows App SDK | **runtime only** (1.7, 1.8, 2.x packages installed); no NuGet dev package | `Get-AppxPackage` |
| MSYS2 g++ | present on PATH | not used (project targets MSVC) |

None of these tools are on the default PATH. Builds run from a VS Developer environment
(`scripts/build.ps1` sets it up with `vswhere` + `Launch-VsDevShell.ps1`).

Nothing was installed or upgraded during Phase 0.

## API availability verified in SDK headers (10.0.26100.0)

| API | Header | Present |
|---|---|---|
| `DwmRegisterThumbnail`, `DWM_TNP_RECTSOURCE` | `um/dwmapi.h` | yes |
| `DWMWA_CLOAKED`, `DWMWA_EXTENDED_FRAME_BOUNDS`, `DWMWA_SYSTEMBACKDROP_TYPE` | `um/dwmapi.h` | yes |
| `DCompositionWaitForCompositorClock`, `DCompositionGetFrameId`, `DCompositionGetStatistics` | `um/dcomp.h` | yes (Windows 11+) |
| `IVirtualDesktopManager` | `um/ShObjIdl_core.h` | yes |
| C++/WinRT `Windows.UI.Composition` projection | `cppwinrt/` | yes |

## Virtual desktop state (read-only inspection)

- `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops\VirtualDesktopIDs`
  is a binary list of 16-byte GUIDs (16 bytes = 1 desktop at inspection time).
- `...\VirtualDesktops\Desktops\{GUID}` contained **2** subkeys while the ID list held 1.
  Stale subkeys exist, so the `Desktops` subkey list is not a reliable desktop list. Only
  `VirtualDesktopIDs` is.

## Performance counters

`\GPU Process Memory(*)\Dedicated Usage`, `Shared Usage`, `Local Usage`, `Non Local Usage`,
`Total Committed` are available. Benchmarks use them for per-process GPU memory.

## Input settings

- `HKCU\Control Panel\Desktop\LowLevelHooksTimeout` is not set (system default applies).
- No Precision Touchpad gesture overrides found under `HKCU\...\PrecisionTouchPad`.
