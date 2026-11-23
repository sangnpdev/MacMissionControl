# ADR-005: Win+Tab Takeover

- Status: Accepted (Phase 5)
- Date: 2026-11-24

## Context

Win+Tab belongs to the shell: `RegisterHotKey(MOD_WIN, VK_TAB)` fails with 1409 (verified, Phase 0).
Requirements: opt-out, a fallback hotkey, a way back to defaults, no automatic system-setting
changes, and Task View must stay reachable if the app crashes.

## Decision

1. **`WH_KEYBOARD_LL` hook on a dedicated thread** (`src/input/keyboard_hook.*`). The callback runs
   the pure `WinTabDetector` (unit-tested) and returns within microseconds.
2. On Win+Tab (also Win+Shift+Tab; not with Ctrl or Alt) the Tab press, its repeats and its release
   are swallowed, and the hook injects the **bridge chord Ctrl+Alt+F24**. The app registers that chord
   with `RegisterHotKey`, with and without `MOD_WIN` (the user is still holding Win). This does two
   things at once:
   - `WM_HOTKEY` gives our process the right to take the foreground, so the overlay receives the
     keyboard (Esc, arrows, Enter). A hook alone does not grant that.
   - The shell sees Win followed by another key, so releasing Win does not open Start.
3. Synthetic input is ignored by the detector, so the injected bridge keys can never loop back.
   (`--hook-accept-injected` exists only for the live test.)
4. **On by default**, off with `"integration": {"takeoverWinTab": false}` or `--no-takeover`.
   `Ctrl+Alt+Space` always works. Nothing is written to the registry or Windows settings; quitting the
   app (`--quit`) restores the default immediately.
5. **Crash safety is structural**: the hook belongs to the process. Process exit or crash removes it;
   a stalled hook thread is removed by Windows after `LowLevelHooksTimeout`. The app re-installs the
   hook after every overview close, display change and resume, in case Windows dropped it.

## Verified on build 26200 (`scripts/takeover-smoke.ps1`)

| Step | Foreground window afterwards |
|---|---|
| Win+Tab with the app running | `MissionControlOverlay` (overview open, keyboard focus); no Start, no Task View |
| Win+Tab again | previous app (overview closed) |
| App killed with `Stop-Process -Force`, then Win+Tab | `XamlExplorerHostIslandWindow \| Task View` (Windows Task View is back) |

## Not covered

- The taskbar Task View button and the touchpad three-finger swipe (when set to Task View) are
  handled inside the shell, not as keystrokes: they still open Windows Task View. To use this overview
  from the touchpad, set the gesture to a custom shortcut `Ctrl+Alt+Space` in Settings → Bluetooth &
  devices → Touchpad → Advanced gestures (a user choice; the app never changes it).
- An elevated (admin) window in the foreground: UIPI blocks low-level hooks from seeing its input in
  some configurations, so Win+Tab may reach Task View there. Not tested.
- If the hook is dropped by a timeout while idle, Win+Tab opens Task View until the next re-install
  (overview close, display change, resume, or restart).

## Rejected

| Option | Why |
|---|---|
| Disable Task View by policy / registry | Changes system settings; forbidden without explicit approval. |
| Hook + PostMessage only | The overlay could not take the foreground; keyboard would not work. |
| Masking with a dummy key only (e.g. vk 0xE8) | Stops Start, but still no foreground right. The bridge hotkey does both. |
