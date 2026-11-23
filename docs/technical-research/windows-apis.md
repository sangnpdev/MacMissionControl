# Windows API Research

Status labels used below:

- **Public**: documented on Microsoft Learn, declared in the Windows SDK, supported.
- **Undocumented**: callable, but not documented or supported. May change in any build.
- **Internal COM**: undocumented COM interfaces of the shell. IIDs change between builds.
- **Verified**: behaviour confirmed by a PoC run on this machine (see `docs/benchmarks/phase-1.md`).
- **Unverified**: from documentation or community knowledge, not yet tested here.

---

## 1. Window previews

### 1.1 DWM Thumbnail API — Public, chosen

`DwmRegisterThumbnail(dest, src, &h)`, `DwmUpdateThumbnailProperties(h, &props)`,
`DwmQueryThumbnailSourceSize`, `DwmUnregisterThumbnail`.

- DWM draws the live content of `src` into the top-level window `dest`. Our process never
  receives pixels. No texture is copied into our process and no GPU memory is allocated by us
  per preview.
- Properties: `rcDestination`, `rcSource`, `opacity` (0–255), `fVisible`,
  `fSourceClientAreaOnly`.
- `DwmQueryThumbnailSourceSize` returns the full window size, including the invisible resize
  borders of Windows 10/11 frames. To show only the visible frame, set `rcSource` to
  `DWMWA_EXTENDED_FRAME_BOUNDS` translated into window-relative coordinates.
- **Limitation (architecture-relevant):** a DWM thumbnail is not a composition visual. It cannot
  be placed inside a DirectComposition or `Windows.UI.Composition` tree, and it cannot be animated
  by a GPU-side composition animation. The only public way to move or fade it is to call
  `DwmUpdateThumbnailProperties` again. Animation therefore needs a per-frame driver on the CPU.
  The cost is one cheap call per visible thumbnail per frame, only while an animation runs.
- Thumbnails are drawn on top of the destination window's own content, including its
  DirectComposition content. A dim layer drawn by our window therefore always sits *below*
  every thumbnail. **Verified** in the Phase 1 PoC.
- When the source window is destroyed, the thumbnail stops drawing and
  `DwmUpdateThumbnailProperties` fails. **Verified:** `DwmUnregisterThumbnail` then returns
  `E_INVALIDARG` (DWM already dropped it), and it also returns `E_INVALIDARG` for every
  thumbnail once the *destination* window is destroyed. So thumbnails must be released before
  the overlay window is destroyed.
- **Verified** (integration test `thumbnails_draw_in_registration_order`, pixel readback, both
  orders): DWM draws thumbnails of one destination window in registration order, later on top.
  Registering bottom-most windows first keeps the real Z-order.
- **Verified:** the shell adopts a new top-level window in steps. It is `DWM_CLOAKED_SHELL`
  (value 2) right after `ShowWindow`, can be uncloaked and then cloaked again, and
  `IsWindowOnCurrentVirtualDesktop` can return FALSE for it until the shell assigns a desktop. The
  filter excludes it until all steps are done (typically < 0.5 s, longer when the system is busy).
  The shell sends synchronous messages to top-level windows during this; a process that owns a
  window and does not pump messages can delay the adoption of other processes' windows.
- **Verified:** after the source window is destroyed, `DwmUpdateThumbnailProperties` can still
  succeed for a short time (DWM processes the destruction asynchronously). Failure of the update is
  therefore a late signal; the app relies on `EVENT_OBJECT_DESTROY` first.
- **Corrected (Phase 3):** Phase 2 reported that a third-party dock (MyDockFinder, which injects
  into processes: windows carry a `MyDockFinderWindowClosePtr` property) hides windows on minimize.
  That came from a faulty PowerShell P/Invoke probe. A native probe shows `SC_MINIMIZE` and
  cross-process `ShowWindow(SW_MINIMIZE)` both produce a normal iconic, visible window
  (`showCmd=2`, `WS_MINIMIZE`, events `0x8017`, `0x0016`, `0x8018`). Minimized windows are therefore
  in the overview. Why the Phase 2 integration test's cross-process minimize sometimes had no effect
  is not known.
- **Verified:** thumbnails render correctly into a `WS_EX_NOREDIRECTIONBITMAP` window that spans
  two monitors with different DPI and refresh rate (4800 x 1800 virtual screen).

### 1.2 `DwmpCreateSharedThumbnailVisual` — Undocumented, rejected

An unnamed export of `dwmapi.dll` (by ordinal) that returns a thumbnail as a DComp visual. It
would allow GPU-side animation. Its signature has changed between Windows builds and it is not
documented. Rejected under the project rules (no undocumented API in a core path). It can be
re-evaluated later as an optional, version-gated provider.

### 1.3 Windows Graphics Capture (WGC) — Public, not chosen

`Windows.Graphics.Capture` captures a window into D3D11 textures in a frame pool.

- Each captured window costs at least one full-size texture, and usually 2–3 for the frame
  pool. A 2880x1800 BGRA texture is about 20 MB. 20 windows would exceed the 100 MB budget on
  GPU memory alone.
- It shows a yellow capture border unless the app requests borderless capture
  (`GraphicsCaptureAccess.RequestAccessAsync(Borderless)`, Windows 11).
- Valid use: a one-shot snapshot for a window whose DWM thumbnail is unavailable. Not needed
  for Phase 1.

---

## 2. Composition and animation

### 2.1 DirectComposition (`dcomp.h`) — Public, chosen for overlay chrome

- Plain COM API, no WinRT runtime, no extra package. Works on a window with
  `WS_EX_NOREDIRECTIONBITMAP`, so the window has no GDI redirection surface. Transparent areas
  show the desktop behind it.
- `IDCompositionAnimation` can run cubic-polynomial animations on the compositor, independent
  of our threads. Spring curves can be approximated with segments.
- `DCompositionWaitForCompositorClock` (Windows 11) blocks until the next compositor frame, or
  until one of the given handles is signaled. This allows a frame loop paced exactly by DWM at
  60/120/240 Hz, with no timers and no busy wait. **Verified:** ticks every 4.167 ms (240 Hz)
  on this machine with a 240 Hz and a 120 Hz monitor active.
- **Verified:** DComp objects can be created on a thread other than the one that owns the
  `HWND` (isolation test, and the PoC compositor thread).

### 2.2 `Windows.UI.Composition` (system WinRT) — Public, deferred

- Usable from Win32 through `CreateDispatcherQueueController` and
  `ICompositorDesktopInterop::CreateDesktopWindowTarget`.
- Gives richer GPU animations (expression animations, `SpringVector3NaturalMotionAnimation`)
  and effect brushes.
- It cannot host DWM thumbnails either (see 1.1), so it does not remove the CPU frame driver.
  It adds the WinRT runtime to the working set (estimate only, not measured).
- Deferred: reconsider in Phase 4 for blur and the desktop strip, if DComp is not enough.

### 2.3 `Microsoft.UI.Composition` (Windows App SDK) — Public, rejected for now

Needs the Windows App SDK NuGet package and bootstrapper. Only the runtime is installed on
this machine. It adds deployment complexity and gives no capability that the system
compositor lacks for this use.

---

## 3. Window enumeration and state

All **Public**:

| Need | API |
|---|---|
| Enumerate top-level windows in Z-order | `EnumWindows` (returns top to bottom) |
| Visible | `IsWindowVisible` |
| Cloaked (other virtual desktop, suspended UWP, shell-hidden) | `DwmGetWindowAttribute(DWMWA_CLOAKED)` |
| Tool window / no-activate | `GetWindowLongPtr(GWL_EXSTYLE)`: `WS_EX_TOOLWINDOW`, `WS_EX_NOACTIVATE`, `WS_EX_APPWINDOW` |
| Owned popups | `GetWindow(GW_OWNER)` |
| Visible bounds | `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` (physical pixels for a per-monitor-DPI-aware process) |
| Minimized / maximized | `IsIconic`, `IsZoomed`, `GetWindowPlacement` (`rcNormalPosition` is in **workspace** coordinates) |
| Monitor | `MonitorFromWindow`, `GetMonitorInfo` (`rcWork`) |
| Title / icon | `GetWindowTextW`; `WM_GETICON` / `GetClassLongPtr(GCLP_HICON)` |
| Create/destroy notifications | `SetWinEventHook(EVENT_OBJECT_CREATE..EVENT_OBJECT_HIDE, WINEVENT_OUTOFCONTEXT)` (event-driven, no polling) |
| On current virtual desktop | `IVirtualDesktopManager::IsWindowOnCurrentVirtualDesktop` |
| Activate | `SetForegroundWindow` (allowed because our overlay is foreground at that moment), `ShowWindow(SW_RESTORE)` if minimized |

The filter used by the shell's own Alt+Tab is not documented. Ours is an approximation:
visible, not cloaked, not a tool window (unless `WS_EX_APPWINDOW`), no owner (unless
`WS_EX_APPWINDOW`), not `WS_EX_NOACTIVATE`, non-empty visible bounds, not our own process.
The rules are data-driven (`WindowFilterRules`) and unit-tested.

---

## 4. Virtual desktops

### 4.1 `IVirtualDesktopManager` — Public, limited

Exactly three methods: `IsWindowOnCurrentVirtualDesktop`, `GetWindowDesktopId`,
`MoveWindowToDesktop` (only for windows of the calling process).

It **cannot** enumerate, create, remove, rename or switch desktops, and it gives no change
notifications.

### 4.2 Registry state — Undocumented, read-only, stable since Windows 10

`HKCU\...\Explorer\VirtualDesktops\VirtualDesktopIDs` (GUID list, in order) and
`CurrentVirtualDesktop` (GUID). `RegNotifyChangeKeyValue` gives event-driven change
notifications. Names are under `...\Desktops\{GUID}\Name`. Stale subkeys exist (seen on this
machine), so the ID list is the source of truth. Read-only use only.

### 4.3 Keyboard shortcuts via `SendInput` — Public mechanism, relative only

`Ctrl+Win+Left/Right` switches to the adjacent desktop. `Ctrl+Win+D` creates one and
`Ctrl+Win+F4` closes the current one. These are documented user shortcuts. Injecting them uses
public APIs and works on every build. A switch to desktop *k* becomes |k − current| steps,
each with its own animation.

### 4.4 `IVirtualDesktopManagerInternal` / `IVirtualDesktopNotificationService` — Internal COM

Obtained through `IServiceProvider` on `CLSID_ImmersiveShell`. Provides direct switch, create,
remove and notifications. IIDs and vtable layouts changed in builds 20348, 22000, 22621,
22631 and 26100 (community-documented; unverified here). Plan for Phase 3: isolate it in
`InternalComVirtualDesktopProvider`, enable it only for an allow-list of tested builds, and fall
back to 4.2 + 4.3 everywhere else. A failure must disable the provider, not crash the app.

---

## 5. Taking over Win+Tab

| Option | Status | Assessment |
|---|---|---|
| `RegisterHotKey(MOD_WIN, VK_TAB)` | Public | **Verified to fail** on build 26200: `GetLastError` = 1409 (`ERROR_HOTKEY_ALREADY_REGISTERED`). The shell owns it. Probe: `MissionControl.exe --probe-hotkeys`. |
| `WH_KEYBOARD_LL` hook that swallows Win+Tab | Public | Works without system changes. The hook dies with the process, so Task View returns automatically after a crash. Windows 7+ removes a hook that exceeds `LowLevelHooksTimeout`, so a hung app cannot block the key either. Needs a responsive hook thread. **Chosen and verified in Phase 5** (ADR-005): combined with a registered "bridge" hotkey (Ctrl+Alt+F24, injected by the hook) so the overlay gets foreground rights and Start does not open. After killing the app, Win+Tab opened Windows Task View again. |
| Disable Task View via policy/registry | Public settings | Changes system settings. Rejected by the project rules. |
| Replace/patch Explorer | — | Forbidden by the project rules. |

Phase 1 uses a normal `RegisterHotKey` fallback hotkey (`Ctrl+Alt+Space`, see module design).

## 6. Touchpad gestures

| Gesture | Who handles it | Can we use it? |
|---|---|---|
| 3-finger swipe up (default: Task View) | Shell | Not interceptable. The user can rebind it in Settings → Touchpad → Advanced gestures to a custom shortcut (for example our hotkey). Reliable, but the user must do it. |
| Progress of a 3/4-finger swipe | Shell | The shell does not give swipe progress to apps. |
| Raw contacts (`RegisterRawInputDevices`, HID usage page 0x0D) | Public | Gives per-contact positions, so gesture progress can be computed while our overlay is focused. Does not suppress the shell gesture. Unverified; Phase 4. |

Gesture-driven animation (spec 5.6 E) is therefore reliable only while our overlay is open:
mouse wheel, precision-touchpad pan (`WM_POINTER*` / `WM_MOUSEWHEEL` with high-resolution
deltas), or raw HID.

## 7. DPI

The process is Per-Monitor-V2 DPI aware (application manifest). With PMv2,
`DWMWA_EXTENDED_FRAME_BOUNDS`, `GetMonitorInfo`, and thumbnail `rcDestination` all use physical
pixels. Layout is computed per monitor in physical pixels, so mixed DPI needs no conversion.
Text and chrome (Phase 4) must scale with `GetDpiForMonitor`.
