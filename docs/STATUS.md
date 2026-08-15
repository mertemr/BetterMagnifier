# BetterMagnifier — Current State

**As of:** 2026-08-15
**Branch:** `main`
**Builds:** Debug x64 and Release x64 both clean (`TreatWarningAsError` is on)
**Self-check:** `SettingsStore`, `ViewportController` and `PointerInput` all pass

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

The app **requires administrator elevation**. `UACExecutionLevel` is set in all
four `ItemDefinitionGroup` blocks of the vcxproj rather than in
`src/app.manifest`, because `mt.exe` cannot merge two different level values
(c1010001). Verified against the built Release binary with `mt.exe`:
`level="requireAdministrator"`.

Runtime shortcuts: `Ctrl+Alt+Z` toggle, `Ctrl+Alt+X` freeze, `Win+Plus` /
`Win+Minus` step, `Ctrl+Alt+wheel` step, `Win+middle-click` freeze,
**`Ctrl+Alt+Shift+Q` panic exit**. All four bindings that go through
`RegisterHotKey` are rebindable from the panel by pressing the combination.

Settings live at `%APPDATA%\BetterMagnifier\settings.ini` and are editable at
runtime from the control panel: **tray icon, right-click, Settings…**.

The build needs `/restore`, because the control panel pulls in the Windows App
SDK through `PackageReference`. `bm.ps1` passes it.

## Architecture

Three threads, no shared mutable state on the hot path.

```
RENDER THREAD (main, MTA)
  D3D11 device, per-monitor swap chains, DXGICapture, overlay windows,
  the hidden message window, RegisterHotKey, the cursor and readout sprites.
        ▲ PostMessage (WM_APP_*)          │ atomic writes
        │                                 ▼
  INPUT THREAD                      StatusSnapshot  ◄── read at 10 Hz
  WH_MOUSE_LL, WH_KEYBOARD_LL,      ViewportSnapshot    by the GUI thread
  EVENT_OBJECT_FOCUS / _SHOW,       (both lock-free)
  EVENT_SYSTEM_MENUPOPUPSTART,
  ViewportController, its own GetMessage loop.

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

### The magnification transform

`ViewportController` owns the source rectangle and is pure math — no Windows
calls, no logging, no allocation. It is the only part of the transform that can
be asserted from a script, and every geometry bug this project has had lived
there.

Three pan modes, selected by `FollowMode` in the settings:

| `PanMode` | `srcOrigin` behaviour | Reached by |
|---|---|---|
| `EdgePush` | holds still, pushes only inside an edge band | `FollowMode::EdgePush` (default) |
| `Anchored` | `local * (1 - 1/zoom)` — tracks the pointer | `FollowMode::Mouse`, `MouseAndFocus` |
| `Fixed` | never moves | not currently selectable |

The anchored identity is worth stating because it looks arbitrary: the
pointer's screen position comes out as `(local - srcOrigin) * zoom = local`, so
screen position `C` shows source pixel `C`, and the sprite drawn there lands
exactly on the real OS cursor. Click alignment is free, with no coordinate
remapping. `SetZoom` preserves the pointer's screen position, which turns out
to preserve this identity exactly — so a zoom step needs no re-anchoring.

Under `EdgePush` that identity does not hold, and the sprite is what recovers
alignment: the real cursor sits at `round(V)`, the sprite is drawn at
`(V - srcOrigin) * zoom`, and clicks land on what the pointer appears to point
at.

## Status by area

### Works, verified

- Multi-monitor enumeration, DXGI adapter/output matching, per-monitor DPI
- Shader-based magnification (fullscreen triangle, bilinear)
- Per-monitor independent zoom, freeze, pointer tracking
- All three pan modes, with `ViewportControllerSelfCheck` covering the anchored
  identity on both a positive-origin and a negative-origin monitor
- Keyboard focus following, through an epoch-gated request in `ViewportSnapshot`
- `Ctrl+Alt+wheel` zoom stepping, swallowed so the page does not also scroll
- Auto-close when stepping down to `minZoom`
- INI settings round-trip, asserted in a Debug self-check
- Hooks confirmed on a different thread from the render loop (log evidence)
- Single-instance guard (named mutex); diagnostic modes are exempt
- `Ctrl+Alt+Shift+Q` panic exit
- Elevation manifest, verified with `mt.exe` against the built exe
- Recovery after locking the workstation
- Application icon, version block and a tray icon that follows zoom state —
  confirmed by extracting both back out of the built binary
- The on-screen readout, verified end to end: a frame dump taken with zoom on
  shows `2.00x` at the bottom centre of the magnified content, and a dump taken
  after the lifetime expires shows the same region clean

### Works, not verified by me

Anything that needs a hand on the mouse or eyes on the screen:

- Click alignment while zoomed
- `Win+middle-click` freeze
- Whether elevation now lets the hook swallow `Win+Plus`
- **Every interactive path in the control panel**: whether a slider drag reaches
  `PushSettings()`, whether a change survives a restart, whether hotkey capture
  feels right, and whether closing the app with the panel open still exits
  cleanly. The panel is now on by default and elevated, which is the one
  combination that has never been exercised by hand.

Automation hits a wall here and it is worth writing down: the app requires
administrator rights, so a script in a normal shell cannot reach its windows.
`PostMessage` is dropped by UIPI, and on this machine `FindWindow` could not see
the app's windows even for a non-elevated build, while `EnumWindows` could.
`BM_PANEL=1` exists to get around it — the app opens the panel itself and
reports through its own log.

### Fixed, 2026-08-15

Four settings the panel offered that did nothing. Worth listing individually,
because the common thread is that each was plausible from the panel and silent
in the code.

**Two of the three follow modes were dead.** `ViewportController::OnPointerMoved`
returned early unless edge push was on, and edge push was the only mode that set
that flag. So "Centred on the pointer" — the project's own original behaviour —
left `srcOrigin` frozen, and only a zoom change moved the view at all. The
machine this was found on had `FollowMode=Mouse` in its settings.ini, so it was
the live configuration, not a corner case. Replaced with an explicit `PanMode`
and the anchored transform above.

**Keyboard focus following did nothing.** It wrote `ZoomState::focalPoint`,
which nothing had read since the viewport moved to `ViewportController`. Now
publishes an epoch-gated request that the input thread applies through
`CenterOn`. The epoch matters: a bare position cannot be told apart from the
same position arriving again, so the view would pin itself to the last focused
control and the pointer could never move it again. `focalPoint` is gone.

**Turning off "Magnified pointer" froze the view entirely.** Panning lived
inside `PointerInput`'s scaled path, so switching the sprite off — or landing on
a machine where `MagShowSystemCursor` is unavailable, which takes the same
branch — left the view stuck wherever it was. Now `DriveViewport` advances the
viewport on the unscaled path too, without clamping, `SetCursorPos` or
swallowing.

**The "Size" slider was not connected to anything.** `cursorScale` round-tripped
through the INI and had a panel slider, while `RenderMonitor` drew the sprite at
`scale = zoom` and never read it.

Also: the panel silently corrected `max <= min` to `min + 1` without putting the
correction back on the slider, so the displayed value disagreed with the saved
one until a restart.

### Measured, 2026-08-13

**`SetCursorPos` produces no `WH_MOUSE_LL` event on this machine.** Counters in
`PointerInput`, logged when pointer scaling turns off, read
`echo=0 staleEcho=0 foreign=0` across several magnified sessions. So the
injected branch in `OnMouseMove` never runs here: applications get their
`WM_MOUSEMOVE` from the cursor move itself, and the hook is not starving them.

The branch is kept because it is correct where injection does happen (RDP,
VMware, games that recentre the pointer), but **it is not the explanation for
the intermittent misclick**, and that fault is still open. The remaining
candidate is render latency — the sprite is drawn from a snapshot the render
thread reads at frame rate, so clicking while the mouse is still moving aims at
where the pointer was drawn, not where it is. Untested.

There is a corollary that costs time if you rediscover it: because
`SetCursorPos` generates no event here, **any script that moves the pointer
programmatically trips the hook liveness check**, which sees the cursor move
with no events and concludes the hook is dead. It reinstalls and carries on, so
it is harmless, but it is why `Mouse hook appears dead` appears in automated
runs and never in real use.

**A time-based lock break-out does not work, and the reason generalises.**
While the clamp holds the pointer still, events that press nothing — motion
along the other axis, jitter that rounds away — arrive constantly between the
ones that do, and a hand that pushes and holds stops generating events at all.
Anything measuring a *duration* of pressure therefore resets constantly or
starves. Hand travel is the measurable quantity: it counts exactly the motion
the clamp threw away. Covered by `PointerInputSelfCheck`.

### Measured, 2026-08-07

**An occluded popup is NOT frozen in the capture. The earlier diagnosis was
wrong, and it was wrong in the direction that mattered.**

Method: `BM_DUMP_FRAME` with `BM_DUMP_COUNT=12`, `BM_DUMP_EVERY=20`,
`BM_DUMP_MONITOR=0`, while an "Open with" context menu was open and the mouse
walked down its items. All twelve back-buffer dumps differ, and the highlight
tracks the mouse. Desktop Duplication composes the menu live even while our
overlay covers it, which is what `WDA_EXCLUDEFROMCAPTURE` on the overlay should
have implied all along.

**Consequence: the overlay + capture architecture stays.** The `WC_MAGNIFIER`
spike is not needed for this, and the magnification pipeline was never the
problem here.

**What this does NOT settle:** the dump shows our render, not the composited
screen, so it says nothing about whether the real menu also appears a second
time, unmagnified, on top of us. That is a z-order question, and UIAccess is
the answer to it — not a capture change.

**`MagShowSystemCursor` works here, without UIAccess.** `SystemCursor::Probe()`
loads `magnification.dll`, calls `MagInitialize`, and round-trips
`MagShowSystemCursor(FALSE)`/`(TRUE)` rather than trusting that the export
resolved. Result on this machine (Windows 11 Pro 26200): `AVAILABLE`.

This settles the gate in the pointer-compositing design: cursor compositing may
be **on by default**, and the `SetSystemCursor` fallback — which outlives the
process and would leave the user with no pointer after a Task Manager kill —
stays a dead path behind `allowUnsafeCursorHide`.

`BM_SELFCHECK` rather than `assert`: `_wassert` opens a message box in a
`/SUBSYSTEM:WINDOWS` build and hangs the caller, which `_set_error_mode` and
`_CrtSetReportMode` do not prevent — measured, not assumed.

### Known broken or unresolved

**1. Popups cannot be magnified correctly.** Two flawed options, switchable:

| Mode | Popup | Problem |
|---|---|---|
| Fight z-order (default) | Magnified | Our magnified copy sits under the real popup, so it appears twice |
| `BM_NO_TOPMOST_FIGHT=1` | Live and correct | Not magnified |

**Still needs a decision on which default is less annoying.**

**2. `Win+Plus` may double-trigger.** Windows Magnifier runs with UIAccess, and
a hook from a normal-integrity process cannot block its shortcut, so both
magnifiers respond. Elevation may change this — untested. The panel detects the
clash and offers to close the OS Magnifier, which is the mitigation until this
is settled.

**3. Freezing was reported once** and could not be reproduced after disabling
vSync in layered mode. The panic exit exists because of it. If it recurs, the
next step is instrumentation, not guessing: time `Present` and log the
durations.

**4. `ZoomState::targetZoom` is write-only.** Three call sites set it, nothing
reads it. It is a marker for smooth zoom interpolation that was never built —
delete it or build the feature, but it should not stay as it is. (`focalPoint`,
its neighbour, had the same shape and turned out to be a live bug.)

**5. Settings are written from two threads.** The panel writes `SettingsStore`
and saves, and so does the engine when a hotkey capture completes. The fields
are disjoint and a user cannot drag a slider and press a key at the same
instant, so it has not bitten — but two concurrent `Save()` calls on one INI is
not something to rely on.

## Control panel

WinUI 3, built in code, hosted as a XAML island on its own STA thread. One
scrolling page (TabView was dropped: heavy templated control, same information
fits on a single page).

**On by default**, reachable from the tray. `BM_PANEL=1` now only makes it open
at *startup*, which is a test affordance rather than a feature.

**Monitors** — one card per monitor, refreshed at 10 Hz from `StatusSnapshot`:
device name, resolution, refresh rate, DPI, an on/off switch, a zoom slider, the
live zoom factor, FPS, capture health and a freeze button. The switch, slider and
freeze button drive that monitor only, which is what per-monitor zoom is for.

**Settings** — hotkeys (captured by pressing the combination), the
Magnifier-shortcut takeover, follow mode, zoom limits, pointer speed / zoom drag
/ size, the edge-push band, start-with-Windows, remember-zoom. It also shows the
INI's path and has a *Reload from disk* button, so the file can be edited by hand
and picked up live.

Order matters on the way out: the panel writes `SettingsStore`, saves it, and only
then posts `WM_APP_SETTINGS_CHANGED`. Reversed, the engine would read values that
had not been written yet.

Three things in here are load-bearing and not obvious:

- `WindowsXamlManager::InitializeForCurrentThread()` is what brings XAML up on
  the thread. Without it `Content()` is accepted, a `XamlRoot` even appears, and
  nothing is ever laid out or composited — a blank white window with no error.
- `XamlControlsResources` is **not** assigned. WinUI 3 already has the default
  theme dictionary; assigning that object replaces it and wipes the brushes it
  references itself.
- `MddBootstrapInitialize` is **not** called. `WindowsPackageType=None` puts the
  SDK's auto-initializer in the binary and it runs at module load.

**No control that embeds a `TextBox` may go in this tree.** `TextBox` itself and
`NumberBox` both kill the process on first layout with `STATUS_STOWED_EXCEPTION`
(0xC000027B), bisected to exactly those controls while `CheckBox`, `RadioButton`,
`Slider`, `Button`, `ToggleSwitch` and `ToggleButton` in the same tree are fine.
Text input services do not come up for an island on a secondary STA thread in an
unpackaged process. That is why zoom limits are sliders and why hotkeys are
captured through the keyboard hook — which is the better UI anyway: nothing to
mistype, no format to explain, and the extended-key and left/right-modifier
questions never arise.

`Stop()` waits on the thread's own promise with a three-second deadline instead of
joining flat, and abandons the thread if that expires. If the loop ever fails to
end, that is a three-second delay on exit rather than the hang that already cost
this project a trip to Task Manager once.

Known rough edges:

- The build prints `MSB8027` and `LNK4042` for `WindowsAppRuntimeAutoInitializer.cpp`,
  which two SDK sub-packages both contribute. Warnings only, output is correct.
- Start-with-Windows writes an HKCU `Run` entry, and Windows may refuse to launch
  an elevation-requiring entry at logon. The panel says so in a hint; the reliable
  route is a Task Scheduler task with highest privileges, which is not built.
- Zoom limits are one set for all monitors. Per-monitor limits would need a picker.

## The on-screen readout

`OsdRenderer` draws a short string — `2.50x`, `Frozen`, `Live` — into a
premultiplied BGRA bitmap with GDI, uploads it as one texture, and the existing
`D3DRenderer::RenderSprite` path composites it at the bottom centre of the
monitor whose state changed. Cached by (text, pixel height), so a zoom ramp
revisiting `2.00x` costs a hash lookup.

Two things in it are easy to get wrong:

- **`DrawTextW` does not write alpha.** A 32-bit DIB comes back zeroed, so text
  drawn onto it is fully transparent, with no error anywhere. The alpha channel
  is built by hand: the pill's coverage geometrically, the glyph's from the
  luminance of white-on-black text, composited together.
- **The readout has to be part of the nothing-changed test.** It appears and
  expires without anything else on screen moving, so without that it is either
  never drawn or drawn once and never cleared. Same reasoning as the cursor
  sprite, and the sprite learned it the hard way when edge push started holding
  the source rect still.

Freeze is treated as a state rather than an event: the readout stays up for as
long as the freeze lasts, because a view that has silently stopped following the
pointer looks exactly like one that is broken.

## Requested: edge-push panning

**Built, and it is the default.** The view holds still while the cursor moves
inside it and scrolls once the cursor reaches a band at the edge.

The conflict with click alignment that made this hard is resolved rather than
avoided: we composite our own cursor. `ViewportController` decides where the
source rect sits, `PointerInput` keeps the real OS cursor at `round(V)`, and the
sprite is drawn at `(V - srcOrigin) * zoom`. Clicks go to the real cursor, which
is under the sprite, so they land on what the sprite points at.

Hiding the real pointer is the part that stays delicate. `MagShowSystemCursor`
is the safe route and is probed at startup; the `SetSystemCursor` fallback,
whose effect outlives the process, is a dead path behind `allowUnsafeCursorHide`
and should stay one. When the safe hide is unavailable the whole feature turns
itself off and the pointer behaves natively — and, since 2026-08-15, the view
still pans in that state.

## The central tension

Three separate walls, all with the same root:

- `Win+Plus` cannot be swallowed (UIAccess)
- Click-through requires `WS_EX_LAYERED`, which forbids the flip model
- Popups draw over us

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
own control. Cost: the DXGI capture and shader pipeline go away, and so does the
cursor compositing that now works. **Worth a spike before committing to it** —
the last two architectural calls made from assumption both turned out wrong.

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
move: a feedback loop, observed as the cursor being flung off screen while
walking down a context menu. A "skip if the cursor is already inside the focused
window" guard did not save it, because a menu can leave focus on its owner window
while the cursor sits in the menu. Focus following moves the view and never the
pointer. The accepted cost is that while focus is driving, the real cursor can
end up outside the source window and the sprite is simply not on screen; one
mouse movement re-anchors and brings it back.

**Concluded WinUI 3 does not work in this environment** after
`Application::Start` access-violated in every apartment configuration. Wrong:
the spike had two bugs of its own. See `spike/xaml-island-thread/FINDINGS.md`.

**Reported a measurement that had not measured anything.** A synthetic-input
harness returned "Start menu did not open" while its nested struct writes were
being silently discarded by PowerShell, so no key was ever injected. The hooks
now ignore `LLKHF_INJECTED` / `LLMHF_INJECTED` input by default so automation
cannot drive the app; `BM_ALLOW_INJECTED=1` re-enables it for verification.

**Left `focalPoint` in place "because the state is still correct".** It was not
a harmless leftover; it was the reason a whole follow mode did nothing, and it
stayed that way for a week because the panel still offered the mode and the code
still looked like it was doing something. Write-only state is a bug with a
grace period — see open item 4.

## Environment switches

Kept deliberately, for testing across machines.

| Variable | Effect |
|---|---|
| `BM_OVERLAY_FLIP=1` | Non-layered flip-model overlay: lower latency, no click-through |
| `BM_NO_TOPMOST_FIGHT=1` | Stop fighting popup z-order: popups live but unmagnified |
| `BM_ALLOW_INJECTED=1` | Let synthetic (SendInput) events drive the app |
| `BM_DUMP_FRAME=<path>` | Dump back buffers to `<path>.NNN.bmp`; the overlay is excluded from capture, so this is the only outside view of the render |
| `BM_DUMP_AFTER=<n>` | First frame to dump (default 60) |
| `BM_DUMP_COUNT=<n>` | How many to dump (default 1) |
| `BM_DUMP_EVERY=<n>` | Frames between dumps (default 30) |
| `BM_DUMP_MONITOR=<n>` | Which monitor to dump (default 0) |
| `BM_PANEL=1` | Open the control panel at startup |
| `BM_POINTER_BREAKOUT_PX=<n>` | Hand travel a shove at a spent edge must spend before the monitor lock opens (default 150; 0 makes the lock nominal) |

Command lines: `--self-check` (pure-logic assertions, then exit),
`--dump-cursors`, `--dump-osd`. All three are exempt from the single-instance
mutex, so they run while the app is running — but **not** from elevation, which
is a property of the binary rather than the mode. From an ordinary shell they
fail with "requires elevation" before `main` is reached. `.\bm.ps1 check` builds,
elevates once, runs all three and reports from the log.

**These have to be set persistently.** Elevation breaks the obvious way of using
them: a process elevated through UAC gets a fresh environment built from the
user's registry, not the launching process' in-memory one, so
`$env:BM_X = '1'; Start-Process ...` silently has no effect. Use:

```powershell
[Environment]::SetEnvironmentVariable('BM_PANEL', '1', 'User')
```

and clear it the same way with `$null` afterwards.

## Remaining work

**Comment pass: done.** Source comments are English throughout — the table that
used to live here is gone because there is nothing left to count. `docs/` is
still partly Turkish: [`PANEL-BLANK.md`](PANEL-BLANK.md) is the panel diagnosis
log and is deliberately left as written.

**Not built, roughly in order of what a low-vision user would notice:**

- **Colour filters** — invert, greyscale, high contrast. The shader pipeline is
  already a single fullscreen pass, so this is close to free, and it is the
  largest accessibility feature Windows Magnifier has that this does not.
- **Smoothing choice** — bilinear is right for photos and wrong for small text
  at low zoom, where nearest is sharper.
- **Per-monitor zoom limits** — one set is shared today, and applying it writes
  every monitor's entry.
- **Smooth zoom interpolation** — `targetZoom` exists for it and nothing uses it.

## Decisions waiting on the user

1. Which popup mode becomes the default
2. Whether `Win+Plus` still double-triggers under elevation, which decides
   whether the OS Magnifier shortcut has to be turned off
3. Whether to spike the `WC_MAGNIFIER` path, given how much the current
   architecture is fighting the OS — noting that it would cost the cursor
   compositing that now works
4. `targetZoom`: build smooth zoom, or delete the field

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
and the hotkeys on `WTS_SESSION_UNLOCK`. The hidden pointer is also restored on
`WTS_SESSION_LOCK`, so `Hide()` cannot early-out on return and leave the real
pointer visible under our sprite.
