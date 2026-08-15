# Architecture

A live screen magnifier for Windows. Not a screenshot tool: it magnifies the
desktop continuously, like Windows Magnifier, but with the one thing Windows
Magnifier cannot do — **an independent zoom level per monitor**.

That single requirement drives every hard decision in this codebase, and this
document exists to write those decisions down before they have to be
rediscovered. Read it before changing anything under `src/`.

## Three threads, no shared mutable state on the hot path

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

Render, input and GUI threads communicate by `PostMessage` (`WM_APP_*`) and
lock-free atomic writes to `StatusSnapshot` / `ViewportSnapshot`. Adding a lock
to the render path is a correctness regression, not an optimisation.

## The magnification transform

`ViewportController` owns the source rectangle and is pure math — no Windows
calls, no logging, no allocation. It is the only part of the transform that
can be asserted from a script, and every geometry bug this project has had
lived there.

Three pan modes, selected by `FollowMode` in the settings:

| `PanMode` | `srcOrigin` behaviour | Reached by |
|---|---|---|
| `EdgePush` | holds still, pushes only inside an edge band | `FollowMode::EdgePush` (default) |
| `Anchored` | `local * (1 - 1/zoom)` — tracks the pointer | `FollowMode::Mouse`, `MouseAndFocus` |
| `Fixed` | never moves | not currently selectable |

The anchored identity is worth stating because it looks arbitrary: the
pointer's screen position comes out as `(local - srcOrigin) * zoom = local`,
so screen position `C` shows source pixel `C`, and the sprite drawn there
lands exactly on the real OS cursor. Click alignment is free, with no
coordinate remapping. `SetZoom` preserves the pointer's screen position, which
turns out to preserve this identity exactly — so a zoom step needs no
re-anchoring.

Under `EdgePush` that identity does not hold, and the sprite is what recovers
alignment: the real cursor sits at `round(V)`, the sprite is drawn at
`(V - srcOrigin) * zoom`, and clicks land on what the pointer appears to point
at.

## Edge-push panning and the composite cursor

The view holds still while the cursor moves inside it and scrolls once the
cursor reaches a band at the edge — the default, and the whole point of
holding still rather than tracking the pointer on every move.

The conflict this creates with click alignment is resolved rather than
avoided: the app draws its own cursor. `ViewportController` decides where the
source rect sits, `PointerInput` keeps the real OS cursor at `round(V)`, and
the sprite is drawn at `(V - srcOrigin) * zoom`. Clicks go to the real cursor,
which is under the sprite, so they land on what the sprite points at.

Hiding the real pointer is the delicate part. `MagShowSystemCursor` is the
safe route and is probed at startup; the `SetSystemCursor` fallback, whose
effect outlives the process, is a dead path behind `allowUnsafeCursorHide` and
should stay one. When the safe hide is unavailable the whole feature turns
itself off and the pointer behaves natively, and the view still pans in that
state — panning does not depend on the sprite being drawn.

Pointer motion is scaled by `1/zoom` (with a tunable compensation exponent)
rather than left at native speed, because at any real zoom level the OS
cursor would spend nearly all its time outside the magnified source window.
`PointerInput` also confines the pointer to the monitor it is magnifying by
default, with a deliberate way out: keep pushing into an already-saturated
edge and a hand-travel threshold (`EdgeBreakout`, not a timer — a held press
generates no further events to time) opens the clamp and lets the pointer
cross onto the next display.

A machine-dependent detail worth knowing if you go looking for it:
`SetCursorPos` does not generate a `WH_MOUSE_LL` event on every system. Where
it doesn't, an injected-move counter in `PointerInput` reads `echo=0`, and any
script that drives the pointer programmatically will trip the hook-liveness
check (it sees the cursor move with no corresponding hook event and assumes
the hook died). Harmless — it reinstalls and carries on — but it is why "Mouse
hook appears dead" shows up in scripted testing and not in real use.

## The central tension

Three separate walls, all with the same root:

- `Win+Plus` cannot be swallowed (it belongs to the Windows Magnifier, which
  runs with UIAccess, and a hook from a normal-integrity process cannot block
  a UIAccess process's shortcut)
- Click-through requires `WS_EX_LAYERED`, which forbids the flip swap-chain
  model
- Popups draw over us

Windows provides the right primitive for all of it: `MagSetFullscreenTransform`
magnifies the composed desktop at the composition level, so menus, popups and
the cursor all behave, and the system resolves input coordinates. **But it
applies to the whole desktop, not per monitor**, which destroys the one
feature this project exists for.

So the overlay-over-desktop architecture is not a preference. It is the price
of per-monitor zoom, and the fights below are the instalments.

**Popups cannot be magnified correctly** as a consequence. Two switchable
modes, both flawed:

| Mode | Popup | Problem |
|---|---|---|
| Fight z-order (default) | Magnified | The magnified copy sits under the real popup, so it appears twice |
| `BM_NO_TOPMOST_FIGHT=1` | Live and correct | Not magnified |

Desktop Duplication does compose an occluded popup live even while the
overlay covers it — confirmed with `BM_DUMP_FRAME`, watching a context menu
render correctly across a dozen consecutive frames while covered. So the
capture pipeline is not the obstacle here; the remaining problem is strictly
z-order, and the fix for it is UIAccess, which requires a signed binary
installed in a trusted location.

**A middle path exists and is unexplored:** host a `WC_MAGNIFIER` control per
monitor in its own window (`MagSetWindowSource` + `MagSetWindowFilterList`).
That is what Windows Magnifier's lens and docked modes use, it shows menus
correctly, and it does not need UIAccess — per-monitor survives because each
monitor gets its own control. The cost is the DXGI capture and shader
pipeline, and the cursor compositing this document describes above.

## The on-screen readout

`OsdRenderer` draws a short string — `2.50x`, `Frozen`, `Live` — into a
premultiplied BGRA bitmap with GDI, uploads it as one texture, and the
existing `D3DRenderer::RenderSprite` path composites it at the bottom centre
of the monitor whose state changed. Cached by (text, pixel height), so a zoom
ramp revisiting `2.00x` costs a hash lookup.

Two things in it are easy to get wrong:

- **`DrawTextW` does not write alpha.** A 32-bit DIB comes back zeroed, so
  text drawn onto it is fully transparent, with no error anywhere. The alpha
  channel is built by hand: the pill's coverage geometrically, the glyph's
  from the luminance of white-on-black text, composited together.
- **The readout has to be part of the nothing-changed test.** It appears and
  expires without anything else on screen moving, so without that it is
  either never drawn or drawn once and never cleared. Same reasoning as the
  cursor sprite, which learned it the hard way when edge push started
  holding the source rect still.

Freeze is treated as a state rather than an event: the readout stays up for
as long as the freeze lasts, because a view that has silently stopped
following the pointer looks exactly like one that is broken.

## Control panel

WinUI 3, built in code, hosted as a XAML island on its own STA thread. One
scrolling page — a `TabView` was tried and dropped: it is a heavy templated
control and the same information fits a single page for a window this small.

**Monitors** — one card per monitor, refreshed at 10 Hz from `StatusSnapshot`:
device name, resolution, refresh rate, DPI, an on/off switch, a zoom slider,
the live zoom factor, FPS, capture health and a freeze button.

**Settings** — hotkeys (captured by pressing the combination), the
Magnifier-shortcut takeover, follow mode, zoom limits, pointer speed / zoom
drag / size, the edge-push band, start-with-Windows, remember-zoom, and a
*Reload from disk* button so the INI can be hand-edited and picked up live.

Order matters on the way out: the panel writes `SettingsStore`, saves it, and
only then posts `WM_APP_SETTINGS_CHANGED`. Reversed, the engine would read
values that had not been written yet.

Three things about the XAML island are load-bearing and not obvious:

- `WindowsXamlManager::InitializeForCurrentThread()` is what brings XAML up on
  the thread. Without it `Content()` is accepted, a `XamlRoot` even appears,
  and nothing is ever laid out or composited — a blank white window with no
  error.
- `XamlControlsResources` is **not** assigned. WinUI 3 already has the default
  theme dictionary; assigning that object replaces it and wipes the brushes
  it references itself.
- `MddBootstrapInitialize` is **not** called. `WindowsPackageType=None` puts
  the SDK's auto-initializer in the binary and it runs at module load.

See `spike/xaml-island-thread/FINDINGS.md` for how these were found — both
were access violations and an `E_FAIL` with no obvious cause until isolated.

**No control that embeds a `TextBox` may go in this tree.** `TextBox` itself
and `NumberBox` both kill the process on first layout with
`STATUS_STOWED_EXCEPTION` (0xC000027B), bisected to exactly those controls
while `CheckBox`, `RadioButton`, `Slider`, `Button`, `ToggleSwitch` and
`ToggleButton` in the same tree are fine. Text input services do not come up
for an island on a secondary STA thread in an unpackaged process. That is why
zoom limits are sliders and why hotkeys are captured through the keyboard
hook instead of typed — which turns out to be the better UI anyway: nothing to
mistype, no format to explain, and the extended-key and left/right-modifier
questions never arise.

`ControlPanel::Stop()` waits on the GUI thread's own promise with a
three-second deadline instead of joining flat, and abandons the thread if
that expires. A stuck XAML loop then costs a three-second delay on exit rather
than a hang.

## Lock and unlock recovery

Locking the workstation switches to the secure desktop, where Desktop
Duplication is unavailable and low-level hooks are detached.

**Capture recovers automatically.** A recoverable `DXGI_ERROR_ACCESS_LOST`
drops only the duplication session rather than the whole device, and retries
are throttled to twice a second so a locked machine does not spam the log.

**Hooks do not come back on their own** — Windows detaches low-level hooks on
the secure desktop and never restores them, so `Win+Plus` and
`Ctrl+Alt+wheel` go quiet while a `RegisterHotKey` binding like `Ctrl+Alt+Z`
keeps working. The app takes `WTSRegisterSessionNotification` and reinstalls
the input thread and the hotkeys on `WTS_SESSION_UNLOCK`. The hidden pointer
is also restored on `WTS_SESSION_LOCK`, so a later `Hide()` cannot early-out
on "already hidden" and leave the real pointer visible under the sprite.

## Known limitations

- **Popups double up or go unmagnified** — see [The central tension](#the-central-tension).
  There is no good default yet; it is a per-user tradeoff exposed as
  `BM_NO_TOPMOST_FIGHT`.
- **`Win+Plus` may double-trigger** the Windows Magnifier as well as this app,
  because that process runs with UIAccess and a hook here cannot block its
  shortcut. The panel detects the clash and offers to close the OS Magnifier.
- **Click alignment while zoomed** and the exact interaction feel of the
  control panel (slider drags, hotkey capture, restart persistence) need a
  hand on the mouse to verify and are not covered by the self-check suite.
- **Zoom limits are one set for all monitors.** Per-monitor limits would need
  a picker in the panel.
- `ZoomState::targetZoom` is currently write-only — a marker for smooth zoom
  interpolation that was never built.

## Environment variables

Kept deliberately, for testing across machines and configurations.

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
`--dump-cursors`, `--dump-osd` (both write BMPs for eyeballing). All three are
exempt from the single-instance mutex, so they run while the app is running —
but **not** from elevation, which is a property of the binary rather than the
mode. From an ordinary shell they fail with "requires elevation" before
`main` is reached. `.\bm.ps1 check` builds, elevates once, runs all three and
reports from the log.

**Environment variables have to be set persistently.** Elevation breaks the
obvious way of using them: a process elevated through UAC gets a fresh
environment built from the user's registry, not the launching process'
in-memory one, so `$env:BM_X = '1'; Start-Process ...` silently has no effect.
Use:

```powershell
[Environment]::SetEnvironmentVariable('BM_PANEL', '1', 'User')
```

and clear it the same way with `$null` afterwards.

## Lessons learned

Recorded because the reasons matter more than the outcomes.

**`WS_EX_LAYERED` cannot be dropped for the flip swap-chain model.** A
fullscreen magnifier visually doesn't need transparency, but removing the
layered style made the overlay swallow every click.
`WS_EX_TRANSPARENT | HTTRANSPARENT` alone is not enough for click-through;
`LAYERED | TRANSPARENT` is the reliable combination, and it forces the swap
chain to the blt model since flip refuses layered windows.

**Moving the OS cursor to follow keyboard focus creates a feedback loop.**
Moving the pointer triggers hover and focus in whatever is now under it,
which raises another focus event and moves it again — observed as the cursor
being flung off screen while walking down a context menu. Guarding with "skip
if the cursor is already inside the focused window" does not save it, because
a menu can leave focus on its owner window while the cursor sits in the menu.
Focus following now moves the view and never the pointer; the accepted cost
is that while focus is driving, the real cursor can end up outside the source
window and the sprite is simply not on screen until the next mouse move
re-anchors it.

**A synthetic-input test harness can report success while injecting nothing.**
A PowerShell script driving `SendInput` reported "Start menu did not open"
while its nested struct writes were being silently discarded by the marshaling
layer, so no key was ever actually injected — the failure looked like an app
bug and was a test-harness bug. The hooks now ignore `LLKHF_INJECTED` /
`LLMHF_INJECTED` input by default so automation cannot drive the app at all;
`BM_ALLOW_INJECTED=1` re-enables it deliberately, for verification.

**Write-only state is a bug with a grace period, not a harmless leftover.**
A follow mode wrote a field that nothing had read for some time, and stayed
that way because the panel still offered the mode and the code still looked
like it was doing something. If a field is only ever assigned, that is worth
treating as a bug report rather than dead-code cleanup.

## Roadmap

Roughly in order of what a low-vision user would notice first:

- **Colour filters** — invert, greyscale, high contrast. The shader pipeline
  is already a single fullscreen pass, so this is close to free, and it is
  the largest accessibility feature Windows Magnifier has that this app does
  not.
- **Smoothing choice** — bilinear is right for photos and wrong for small
  text at low zoom, where nearest-neighbour is sharper.
- **Per-monitor zoom limits** — one set is shared today, and applying it
  writes every monitor's entry.
- **Smooth zoom interpolation** — the `targetZoom` field exists for it and
  nothing uses it yet.
- A decision on the popup z-order default, and whether to spike the
  `WC_MAGNIFIER` per-monitor path described above.
