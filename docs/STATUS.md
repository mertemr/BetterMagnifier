# BetterMagnifier — Current State

**As of:** 2026-08-07
**Branch:** `refactor/improve-compability`
**Builds:** Debug x64 and Release x64 both clean (`TreatWarningAsError` is on)

## What this is

A live screen magnifier for Windows. Not a screenshot tool: it magnifies the
desktop continuously, like Windows Magnifier, but with the one thing Windows
Magnifier cannot do — **an independent zoom level per monitor**.

That single requirement drives every hard decision in the codebase. See
[The central tension](#the-central-tension).

## Build and run

```bash
.\bm.ps1            # build Debug
.\bm.ps1 run        # build Debug, then launch
.\bm.ps1 release    # build Release
.\bm.ps1 errors     # WARN/ERROR lines from the newest log
.\bm.ps1 log        # full newest log
.\bm.ps1 kill       # kill stray instances
.\bm.ps1 clean      # delete bin and obj
```

The app normally **requires administrator elevation** (`UACExecutionLevel` in
the vcxproj; see `src/app.manifest` for why it isn't set there). **Temporarily
switched to `AsInvoker`** (2026-08-07) so development and panel testing don't
hit a UAC prompt on every launch — restore `RequireAdministrator` in all four
`ItemDefinitionGroup` blocks before shipping; see the comment left at each
site.

Runtime shortcuts: `Ctrl+Alt+Z` toggle, `Ctrl+Alt+X` freeze, `Win+Plus` /
`Win+Minus` step, `Ctrl+Alt+wheel` step, `Win+middle-click` freeze,
**`Ctrl+Alt+Shift+Q` panic exit**.

Settings live at `%APPDATA%\BetterMagnifier\settings.ini` and are editable at
runtime from the control panel: **tray icon, right-click, Settings**.

The build now needs `/restore`, because the control panel pulls in the Windows
App SDK through `PackageReference`. `bm.ps1` passes it.

## Architecture

Three threads, no shared mutable state on the hot path.

```
RENDER THREAD (main, MTA)
  D3D11 device, per-monitor swap chains, DXGICapture, overlay windows,
  the hidden message window, RegisterHotKey.
        ▲ PostMessage (WM_APP_*)          │ atomic writes
        │                                 ▼
  INPUT THREAD                      StatusSnapshot  ◄── read at 10 Hz
  WH_MOUSE_LL, WH_KEYBOARD_LL,      (lock-free)         by the GUI thread
  EVENT_OBJECT_FOCUS / _SHOW,
  EVENT_SYSTEM_MENUPOPUPSTART,
  its own GetMessage loop.

  GUI THREAD (STA)
  WinUI 3 XAML island in a Win32 host window. Its own GetMessage loop, not
  Application::Start, so PostThreadMessage(WM_QUIT) ends it and work can be
  handed to it with either PostThreadMessage or DispatcherQueue.TryEnqueue.
```

**Why the input thread exists:** low-level hooks run on the message queue of
the thread that installed them. The render thread blocks in `Present`, so a
hook living there puts every mouse and key event in the system behind our
frame, and exceeding `LowLevelHooksTimeout` (300 ms) makes Windows silently
uninstall the hook. Hook callbacks only `PostMessage` and return.

**The magnification transform** is anchored, not centred:

```
srcOrigin = anchor * (1 - 1/zoom)
srcSize   = monitorSize / zoom
```

Screen position `anchor` therefore shows source pixel `anchor`: the real cursor
sits exactly on top of the magnified content under it, so click-through lands
where the user sees the target with **no coordinate remapping**. The formula
also stays in bounds for every anchor position by construction.

## Status by area

### Works, verified

- Multi-monitor enumeration, DXGI adapter/output matching, per-monitor DPI
- Shader-based magnification (fullscreen triangle, bilinear)
- Per-monitor independent zoom, freeze, anchor tracking
- `Ctrl+Alt+wheel` zoom stepping, swallowed so the page does not also scroll
- Auto-close when stepping down to `minZoom`
- INI settings round-trip, 11 assert cases in a Debug self-check
- Hooks confirmed on a different thread from the render loop (log evidence)
- Single-instance guard (named mutex)
- `Ctrl+Alt+Shift+Q` panic exit
- Elevation manifest (verified with `mt.exe` against the built exe) — currently
  disabled, see [Build and run](#build-and-run)
- Recovery after locking the workstation (see below)
- The control panel's XAML island comes up and lays out the full tree
  (`Control panel opened`, `Panel root Loaded`/`SizeChanged`, dispatcher
  ticking, all in the log; confirmed visually with a screenshot). Tested
  un-elevated (`AsInvoker`); not re-verified elevated since the flip.

### Measured, 2026-08-07

**`MagShowSystemCursor` works here, without UIAccess.** `SystemCursor::Probe()`
loads `magnification.dll`, calls `MagInitialize`, and round-trips
`MagShowSystemCursor(FALSE)`/`(TRUE)` rather than trusting that the export
resolved. Result on this machine (Windows 11 Pro 26200): `AVAILABLE`.

This settles the gate in the pointer-compositing design: cursor compositing may
be **on by default**, and the `SetSystemCursor` fallback — which outlives the
process and would leave the user with no pointer after a Task Manager kill —
stays a dead path behind `allowUnsafeCursorHide`.

`--self-check` runs the pure-logic assertions and exits, and is exempt from the
single-instance mutex, so it works while the app is running. `BM_SELFCHECK`
rather than `assert`: `_wassert` opens a message box in a `/SUBSYSTEM:WINDOWS`
build and hangs the caller, which `_set_error_mode` and `_CrtSetReportMode` do
not prevent — measured, not assumed.

### Works, not verified by me

Visual or interactive behaviour that cannot be checked from a script:

- Click alignment while zoomed
- `Win+middle-click` freeze
- Whether elevation now lets the hook swallow `Win+Plus`
- **Everything the control panel does after opening**: whether the cards read
  correctly, whether the sliders and toggles drive the right monitor, whether a
  settings change survives a restart, and whether closing the app with the panel
  open still exits cleanly. See [Control panel](#control-panel).

Automated testing hits a wall here, and it is worth writing down: the app
requires administrator rights, so a script in a normal shell cannot reach its
windows. `PostMessage` is dropped by UIPI, and on this machine `FindWindow`
could not see the app's windows even for a non-elevated build, while
`EnumWindows` could. `BM_OPEN_PANEL` exists to get around all of it - the app
opens the panel itself and reports through its own log.

### Known broken or unresolved

**1. Popups cannot be magnified correctly.** Two flawed options, switchable:

| Mode | Popup | Problem |
|---|---|---|
| Fight z-order (default) | Magnified only | Frozen. A fully occluded window stops repainting, so the desktop composition keeps its last painted state and we magnify that. Menu highlight does not follow the mouse. |
| `BM_NO_TOPMOST_FIGHT=1` | Live and correct | Not magnified, and appears twice since our magnified copy is behind it. |

**Needs a decision on which default is less annoying.**

**2. `Win+Plus` may double-trigger.** Windows Magnifier runs with UIAccess, and
a hook from a normal-integrity process cannot block its shortcut, so both
magnifiers respond. Elevation may change this — untested. Otherwise the OS
Magnifier shortcut has to be disabled, which is a system setting and therefore
the user's call, not something this project changes.

**3. Freezing was reported once** and could not be reproduced after disabling
vSync in layered mode. The panic exit exists because of it. If it recurs, the
next step is instrumentation, not guessing: time `Present` and log the
durations.

## Control panel

> **OFF by default.** Set `BM_PANEL=1` to enable it; the tray's Settings entry
> only appears when it is on. Both known crashes are fixed: the blank-window
> bug (island needed `WindowsXamlManager::InitializeForCurrentThread()`) and a
> stowed-exception crash from any XAML control that embeds a `TextBox`
> (`TextBox` itself, and `NumberBox`, replaced with `Slider` for the zoom-limit
> fields). The full tree — monitor cards and every settings section — now opens
> and lays out without crashing, confirmed with a screenshot. Still off by
> default because interactive behavior (does a slider drag actually reach
> `PushSettings()` and persist) has not been verified by hand. Full measurements
> in [`PANEL-BLANK.md`](PANEL-BLANK.md).

WinUI 3, built in code, hosted as a XAML island on its own STA thread. One
scrolling page (TabView was dropped: heavy templated control, same information
fits on a single page).

**Status tab** - one card per monitor, refreshed at 10 Hz from `StatusSnapshot`:
device name, resolution, refresh rate, DPI, an on/off switch, a zoom slider, the
live zoom factor, FPS, capture health and a freeze button. The switch, slider and
freeze button drive that monitor only, which is what per-monitor zoom is for.

**Settings tab** - everything in `settings.ini`, applied without a restart: both
hotkeys, the Magnifier-shortcut takeover, follow mode, zoom minimum/maximum/step,
start-with-Windows, remember-zoom. It also shows the INI's path and has a *Reload
from disk* button, so the file can be edited by hand and picked up live.

Order matters on the way out: the panel writes `SettingsStore`, saves it, and only
then posts `WM_APP_SETTINGS_CHANGED`. Reversed, the engine would read values that
had not been written yet.

Three things in here are not what the plan said, because the plan was written
before the spike was fixed. All three are load-bearing:

- `Application::Start` is called, and its callback constructs an `Application`.
  Constructing one directly gives `RPC_E_WRONG_THREAD`, and skipping the
  construction makes `Application::Current()` null - then the first call on it is
  an access violation with no message.
- `XamlControlsResources` is **not** assigned. WinUI 3 already has the default
  theme dictionary; assigning that object replaces the dictionary and wipes the
  brushes it references itself.
- `MddBootstrapInitialize` is **not** called. `WindowsPackageType=None` puts the
  SDK's auto-initializer in the binary and it runs at module load.

Because `Application::Start` owns the thread's message loop, the panel cannot be
driven with `PostThreadMessage`; `DispatcherQueue.TryEnqueue` is the only channel.
`Application::Start` is also once-per-process, so a panel that failed to start
cannot be retried, and closing the window hides it rather than destroying it.

`Stop()` waits on the thread's own promise with a three-second deadline instead of
joining flat, and abandons the thread if that expires. If `Application::Exit()`
ever fails to end the loop, that is a three-second delay on exit rather than the
hang that already cost this project a trip to Task Manager once.

Known rough edges:

- The build prints `MSB8027` and `LNK4042` for `WindowsAppRuntimeAutoInitializer.cpp`,
  which two SDK sub-packages both contribute. Warnings only, output is correct.
- Start-with-Windows writes an HKCU `Run` entry, and Windows may refuse to launch
  an elevation-requiring entry at logon. The panel says so in a hint; the reliable
  route is a Task Scheduler task with highest privileges, which is not built.
- Zoom limits are one set for all monitors. Per-monitor limits would need a picker.

## Requested: edge-push panning

The view should stay put while the cursor moves inside it and only scroll once
the cursor reaches an edge, the way Windows Magnifier's fullscreen mode
behaves. That makes corners and edges reachable without the pointer sliding
onto the next monitor.

**This conflicts with click alignment, for a concrete reason.**

The current transform is anchored: `srcOrigin = cursor * (1 - 1/zoom)`, which
makes screen position `C` show source pixel `C`. That identity is exactly what
makes clicks land where they appear — and it also forces the view to track the
cursor on every move, so there is no edge behaviour to speak of.

Under edge-push, `srcOrigin` moves independently. Source pixel `C` then appears
at `(C - srcOrigin) * zoom`, which is not `C`. The OS still draws the cursor at
`C`, so the drawn cursor sits over source pixel `srcOrigin + C/zoom` instead of
the thing it points at. Clicks go to `C`. Misaligned again.

Windows Magnifier escapes this because it magnifies the whole composed desktop
**including the cursor**, so the pointer is scaled and placed along with
everything else.

To have both, we would have to do the same thing ourselves:

1. Composite our own cursor into the overlay at `(C - srcOrigin) * zoom`.
   Desktop Duplication already hands the cursor over separately, via
   `frameInfo.PointerPosition` and `GetFramePointerShape`, and the three
   pointer shape types (monochrome, colour, masked colour) each need handling
   plus shape caching.
2. Hide the real cursor. This is the ugly part: there is no per-application way
   to hide a system-wide cursor. `SetSystemCursor` with a blank cursor is
   global and has to be restored, which makes it a system setting change and a
   liability if the process dies.

So it is a real feature, not a tweak, and step 2 is genuinely invasive.
Worth deciding alongside the `WC_MAGNIFIER` question, since that path would get
correct cursor handling from the OS for free.

## The central tension

Three separate walls were hit in one day, and all three have the same root:

- `Win+Plus` cannot be swallowed (UIAccess)
- Click-through requires `WS_EX_LAYERED`, which forbids the flip model
- Popups draw over us, and covering them freezes their painting

Windows provides the right primitive for all of it:
`MagSetFullscreenTransform` magnifies the composed desktop at the composition
level, so menus, popups and the cursor all behave, and the system resolves
input coordinates. **But it applies to the whole desktop, not per monitor**,
which destroys the one feature this project exists for.

So the overlay-over-desktop architecture is not a preference. It is the price
of per-monitor zoom, and these fights are the instalments.

One unexplored middle path: host a `WC_MAGNIFIER` control per monitor in our
own window (`MagSetWindowSource` + `MagSetWindowFilterList`). That is what
Windows Magnifier's lens and docked modes use, it shows menus correctly, it
does not need UIAccess, and per-monitor survives because each monitor gets its
own control. Cost: the DXGI capture and shader pipeline go away. **Worth a
spike before committing to it** — the last two architectural calls made from
assumption both turned out wrong.

## Decisions made, then reversed

Recorded because the reasons matter more than the outcomes.

**Dropped `WS_EX_LAYERED` to make the flip model work.** Justified as "a
fullscreen magnifier does not need transparency". Visually true, destructive
for input: the overlay swallowed every click. `WS_EX_TRANSPARENT` plus
`HTTRANSPARENT` is not enough for click-through; `LAYERED | TRANSPARENT` is the
reliable recipe. Restored, and the swap chain dropped to the blt model since
flip refuses layered windows.

**Moved the cursor on focus change** to keep the "anchor == cursor" invariant
while following keyboard focus. Moving the pointer triggers hover and focus in
whatever is under the new position, which fires another focus event and another
move: a feedback loop. Observed as the cursor being flung off screen while
walking down a context menu. A "skip if the cursor is already inside the
focused window" guard did not save it, because a menu can leave focus on its
owner window while the cursor sits in the menu. Removed entirely. Focus
following now moves only the anchor, and the accepted cost is that clicks no
longer align in that mode — hence `FollowMode::Mouse` by default.

**Concluded WinUI 3 does not work in this environment** after
`Application::Start` access-violated in every apartment configuration. Wrong:
the spike had two bugs of its own (the callback must construct an `Application`
instance, and `XamlControlsResources` must not be assigned manually). See
`spike/xaml-island-thread/FINDINGS.md`.

**Reported a measurement that had not measured anything.** A synthetic-input
harness returned "Start menu did not open" while its nested struct writes were
being silently discarded by PowerShell, so no key was ever injected. Later,
with a working harness, the same result became real. The hooks now ignore
`LLKHF_INJECTED` / `LLMHF_INJECTED` input by default so automation cannot drive
the app; `BM_ALLOW_INJECTED=1` re-enables it for verification.

## Environment switches

Kept deliberately, for testing across machines.

| Variable | Effect |
|---|---|
| `BM_OVERLAY_FLIP=1` | Non-layered flip-model overlay: lower latency, no click-through |
| `BM_NO_TOPMOST_FIGHT=1` | Stop fighting popup z-order: popups live but unmagnified and doubled |
| `BM_ALLOW_INJECTED=1` | Let synthetic (SendInput) events drive the app |
| `BM_DUMP_FRAME=<path>` | Dump one back buffer to BMP; the overlay is excluded from capture, so this is the only outside view of the render |
| `BM_DUMP_AFTER=<n>` | Which frame to dump (default 60) |
| `BM_PANEL=1` | Enable the control panel: adds the tray entry and opens it at startup |

**These have to be set persistently now.** Elevation broke the obvious way of
using them: a process elevated through UAC gets a fresh environment built from
the user's registry, not the launching process' in-memory one, so
`$env:BM_X = '1'; Start-Process ...` silently has no effect. Use:

```powershell
[Environment]::SetEnvironmentVariable('BM_PANEL', '1', 'User')
```

and clear it the same way with `$null` afterwards.

## Remaining work

**Cleanup, in progress.** Headers are done: English, comments only where the
reasoning is not obvious, dead code removed (net -489 lines). Still Turkish:

| File | Turkish comment hits |
|---|---|
| `App.cpp` | 107 |
| `D3DRenderer.cpp` | 51 |
| `InputThread.cpp` | 34 |
| `OverlayWindow.cpp` | 29 |
| `DXGICapture.cpp` | 20 |
| `SettingsStore.cpp` | 19 |
| `main.cpp`, `MonitorManager.cpp`, `app.manifest`, others | 14 and below |

`Logger.h` and everything under `docs/` are also Turkish.

A lazier option than translating: **delete** most `.cpp` body comments. Much of
it explains what the code does, which the code already says. Shorter diff, less
to maintain.

**Feature work,** from `docs/superpowers/plans/2026-08-04-control-panel-gui.md`:
all of it is now in. Tasks 1 to 4 were done earlier; 5, 6 and 7 are the control
panel described above. Tasks 8 and 9 were overtaken by work done in between -
focus following exists as `FollowMode::MouseAndFocus`, and the `Win+Z` takeover
became the broader `hijackMagnifierKeys`, which covers `Win+Plus`/`Win+Minus`,
`Ctrl+Alt+wheel` and `Win+middle-click`.

## Decisions waiting on the user

0. The panel no longer crashes (see [`PANEL-BLANK.md`](PANEL-BLANK.md)) but is
   still off by default pending a five-minute manual check that the controls
   actually drive settings and that a change survives a restart. The magnifier
   does not depend on it either way.
1. Which popup mode becomes the default
2. Whether `Win+Plus` still double-triggers under elevation, which decides
   whether the OS Magnifier shortcut has to be turned off
3. Translate or delete the `.cpp` comments
4. Edge-push panning: accept losing click alignment, or build cursor
   compositing plus cursor hiding
5. Whether to spike the `WC_MAGNIFIER` path, given how much the current
   architecture is fighting the OS, and that it would settle both 4 and the
   popup problem at once

## Lock and unlock recovery

Locking the workstation switches to the secure desktop, where Desktop
Duplication is unavailable and low-level hooks are detached. Two separate bugs
came out of that:

**Capture could never recover.** On `DXGI_ERROR_ACCESS_LOST` the code called
`Cleanup()`, which nulls `m_device` and resets `m_output1` — precisely what
`Reinitialize()` needs. It then failed with "device or output is gone" forever.
`Initialize()` did the same on failure, so starting up while locked was equally
fatal. Recoverable failures now drop only the duplication session, and retries
are throttled to twice a second so a locked machine does not spam the log.

**Hooks stayed dead after unlock.** Windows detaches low-level hooks on the
secure desktop and does not restore them, so `Win+Plus` and `Ctrl+Alt+wheel`
went quiet while `Ctrl+Alt+Z` (a `RegisterHotKey` binding) kept working. The
app now takes `WTSRegisterSessionNotification` and reinstalls the input thread
and the hotkeys on `WTS_SESSION_UNLOCK`.
