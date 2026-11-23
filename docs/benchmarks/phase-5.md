# Phase 5 — System Integration: Results

Measured 2026-11-24 on build 26200. Design: `docs/decisions/ADR-005-wintab-takeover.md`.

## What was built

| Requirement | Implementation |
|---|---|
| Replace the Win+Tab experience | `WH_KEYBOARD_LL` hook (own thread) + pure `WinTabDetector` + bridge hotkey Ctrl+Alt+F24 |
| Do not destroy Task View | Nothing in the shell or system settings changes; taskbar Task View button still opens Windows Task View |
| On/off switch | `"integration": {"takeoverWinTab": true|false}` (default `true`), `--no-takeover` |
| Fallback hotkey | `Ctrl+Alt+Space`, always registered |
| Restore defaults | Turn the option off, or quit the app (`--quit`): Win+Tab is Windows' again at once |
| No automatic system changes | No registry, policy or settings writes |
| Task View after a crash | Hook dies with the process; Windows drops a stalled hook after `LowLevelHooksTimeout`; re-installed after every overview close, display change and resume |

## Verification

### Unit tests

`wintab_detector`: plain Tab, Win+Tab trigger, auto-repeat (one trigger), swallowed key-up, Win
never swallowed, right Win, Win+Shift+Tab, Ctrl/Alt combinations passed through, other keys,
synthetic input ignored by default and accepted in test mode. All pass (37/37 unit tests total).

### Live test (`scripts/takeover-smoke.ps1`, injects keys on the real desktop)

| Step | Result |
|---|---|
| App running, Win+Tab | log `open: win+tab`; foreground `MissionControlOverlay \| Mission Control`; Start / Task View not shown |
| Win+Tab again | log `close: win+tab`; foreground back to the previous app (Edge) |
| `Stop-Process -Force` on the app, then Win+Tab | foreground `XamlExplorerHostIslandWindow \| Task View`: Windows Task View works again |
| Esc | Task View closed |

The live test uses `--hook-accept-injected` because injected keys are ignored by design in normal
runs. Physical key presses were not tested by a script (they cannot be generated without injection);
the code path after the detector is identical.

### Crash handling

`MissionControl.exe --crash-test` → exit code `0xC0000005`, log line
`CRASH: exception 0xC0000005 at ..., dump ...\crash-20261124-001243.dmp`, dump file 59 KB.

## Touchpad gestures

| Gesture | Who handles it | Result |
|---|---|---|
| Three-finger swipe up (default Task View) | the shell, not a keystroke | still opens Windows Task View |
| Same gesture set to custom shortcut `Ctrl+Alt+Space` (user setting) | our fallback hotkey | opens this overview (not tested: needs the user to change the setting) |
| Two-finger scroll inside the open overview | the app (`WM_MOUSEWHEEL`) | scrubs the exit, see Phase 4 |

## Known limits

1. Elevated (admin) foreground windows: Win+Tab may reach Task View there (UIPI). Not tested.
2. If Windows drops the hook by timeout while idle, Win+Tab goes to Task View until the next
   re-install (overview close, display change, resume, restart).
3. ~~No autostart~~ — added after Phase 6: tray menu **Start with Windows** / `--autostart on|off`
   writes or removes the per-user Run value only on that explicit request. If the user switched the
   app off in Settings › Startup apps, the menu says so and opens that page instead of overriding it.
   Verified: integration test round trip (value restored afterwards), CLI on/off, menu screenshot.
