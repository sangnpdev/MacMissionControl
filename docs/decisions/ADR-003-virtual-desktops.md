# ADR-003: Virtual Desktop Provider

- Status: Accepted (Phase 3)
- Date: 2026-11-23

## Context

`IVirtualDesktopManager` (the only public API) can tell which desktop a window is on, and can move
the calling process's own windows. It cannot list, switch, create or remove desktops, and it sends
no notifications (`windows-apis.md` §4). The shell's internal COM interfaces can do all of that, but
their IIDs and vtable layouts change between Windows builds. A wrong vtable slot is not a clean
failure: it calls a different method inside Explorer.

## Decision

One provider, `VirtualDesktopProvider`, built only on public mechanisms and a read-only registry key:

| Capability | Mechanism | Classification | Measured on build 26200 |
|---|---|---|---|
| List desktops, order, current | `HKCU\...\Explorer\VirtualDesktops`: `VirtualDesktopIDs`, `CurrentVirtualDesktop` (Win10 fallback: `SessionInfo\<n>\VirtualDesktops`) | Undocumented values, read-only, unchanged since Windows 10 | works |
| Desktop names | `...\VirtualDesktops\Desktops\{GUID}\Name` | Undocumented, read-only; stale subkeys exist, so the ID list decides | works |
| Change notification | `RegNotifyChangeKeyValue` (subtree, thread-agnostic, event) | Public API on an undocumented key | event arrives ~8 ms after the shell cloaks the old desktop's windows |
| Switch | `SendInput` Ctrl+Win+Left/Right, one press per confirmed change | Public API + documented user shortcut | 250–391 ms per step until the registry shows it |
| Create | `SendInput` Ctrl+Win+D (also switches to it) | Public + documented shortcut | 78 ms |
| Remove | `SendInput` Ctrl+Win+F4, current desktop only | Public + documented shortcut | removal visible in the registry at the next read |
| Keep the overlay visible across switches | nothing needed: `WS_EX_TOOLWINDOW` windows show on every desktop | Verified behaviour | `desktop_tests` |

Rules:

- No internal COM interfaces, no byte patterns or offsets, no registry writes.
- If the key cannot be opened, desktop features are disabled and logged; the overview still works.
- Injected chords skip modifiers the user is physically holding, so Ctrl+Arrow repeats work.

## Rejected

| Option | Why |
|---|---|
| `IVirtualDesktopManagerInternal` / notification service (internal COM) | Build-specific IIDs and vtables; a mismatch calls the wrong Explorer method. Could be added later behind a build allow-list, with these shortcuts as the fallback, if a need appears that shortcuts cannot cover (direct jump without intermediate desktops, removing a non-current desktop, renaming). |
| Polling the registry | The change event makes polling unnecessary; idle CPU stays 0. |
| `MoveWindowToDesktop` for the overlay | Not needed (tool windows are on all desktops). Removed from the provider. |

## Known limits

- Switching to a desktop several positions away passes through the ones between (one shortcut per
  step, each with the shell's own slide). Measured 250–391 ms per step.
- Only the current desktop can be removed; its windows move to the adjacent desktop (shell behaviour).
- UIPI: when the foreground window belongs to an elevated process, injected shortcuts are blocked.
  While the overview is open, our overlay is in the foreground, so this does not apply there.
- Renaming desktops is not possible through public means.
- The registry layout is undocumented. If a future build changes it, `read()` returns nothing and
  desktop features turn off without affecting the rest of the app.
