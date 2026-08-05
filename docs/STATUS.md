# BetterMagnifier — Current State

**As of:** 2026-08-05
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

The app now **requires administrator elevation** (manifest
`requireAdministrator`). UAC appears on every launch.

Runtime shortcuts: `Ctrl+Alt+Z` toggle, `Ctrl+Alt+X` freeze, `Win+Plus` /
`Win+Minus` step, `Ctrl+Alt+wheel` step, `Win+middle-click` freeze,
**`Ctrl+Alt+Shift+Q` panic exit**.

Settings live at `%APPDATA%\BetterMagnifier\settings.ini`.

## Architecture

Three threads, no shared mutable state on the hot path.

```
RENDER THREAD (main, MTA)
  D3D11 device, per-monitor swap chains, DXGICapture, overlay windows,
  the hidden message window, RegisterHotKey.
        ▲ PostMessage (WM_APP_*)          │ atomic writes
        │                                 ▼
  INPUT THREAD                      StatusSnapshot
  WH_MOUSE_LL, WH_KEYBOARD_LL,      (lock-free, nothing reads it yet)
  EVENT_OBJECT_FOCUS / _SHOW,
  EVENT_SYSTEM_MENUPOPUPSTART,
  its own GetMessage loop.
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
- Elevation manifest (verified with `mt.exe` against the built exe)

### Works, not verified by me

Visual or interactive behaviour that cannot be checked from a script:

- Click alignment while zoomed
- `Win+middle-click` freeze
- Whether elevation now lets the hook swallow `Win+Plus`

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
Tasks 1 to 4 are done. Tasks 5 to 9 build the WinUI 3 control panel, and the
spike that gates them now passes.

**`StatusSnapshot` is written every frame but nothing reads it.** It is
groundwork for that control panel. Delete it if the plan is dropped.

## Decisions waiting on the user

1. Which popup mode becomes the default
2. Whether `Win+Plus` still double-triggers under elevation, which decides
   whether the OS Magnifier shortcut has to be turned off
3. Translate or delete the `.cpp` comments
4. Whether to spike the `WC_MAGNIFIER` path, given how much the current
   architecture is fighting the OS
