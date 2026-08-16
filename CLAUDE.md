# BetterMagnifier

A live per-monitor screen magnifier for Windows. The one requirement Windows
Magnifier cannot meet — **independent zoom per monitor** — drives every hard
design decision in this codebase.

## Read this first

@docs/ARCHITECTURE.md is the maintained source of truth: the three-thread
model and why it exists, the magnification transform, the control panel's
load-bearing gotchas, and known limitations. Read it before changing anything
under `src/`.

## Build

Use `.\bm.ps1` — never call MSBuild directly. It locates MSBuild via vswhere and
passes `/restore`, which the build now requires because the control panel pulls in
the Windows App SDK through `PackageReference`.

```
.\bm.ps1            # Debug
.\bm.ps1 run        # Debug + launch
.\bm.ps1 release    # Release
.\bm.ps1 check      # Debug + run the diagnostic modes (elevates, see below)
.\bm.ps1 errors     # WARN/ERROR lines from the newest log
.\bm.ps1 kill       # kill stray instances
```

`TreatWarningAsError` is on for both configurations. A warning is a build failure.

## Constraints that bite

- **`RequireAdministrator` lives in the vcxproj, in four places.** All four
  `ItemDefinitionGroup` blocks carry `UACExecutionLevel`, and they have to agree;
  `AsInvoker` silently breaks magnification over elevated windows. Not in
  `app.manifest`, because `mt.exe` cannot merge two different level values
  (c1010001). If you flip it to `AsInvoker` for a testing session, flip all four
  back — and note that every launch then costs a UAC prompt, so scripted checks
  want `--self-check` or `BM_PANEL=1` rather than driving the window.
- **Low-level hooks must stay on the input thread.** `WH_MOUSE_LL` and
  `WH_KEYBOARD_LL` run on the message queue of the thread that installed them. The
  render thread blocks in `Present`, so a hook there puts every mouse and key event
  in the system behind our frame — and exceeding `LowLevelHooksTimeout` (300 ms)
  makes Windows uninstall the hook without telling us. Hook callbacks may only
  `PostMessage` and return.
- **The captured texture is not in desktop orientation.** Desktop Duplication
  returns the display's *unrotated* mode image, so a 1080x1920 portrait monitor
  arrives as a 1920x1080 texture on its side. Everything outside `RenderFrame`
  works in desktop coordinates and must keep doing so; the rotation is absorbed
  by `ComputeSourceUv` alone. `DXGI_OUTDUPL_DESC.ModeDesc` is no help here — it
  reports the rotated size while the texture has the unrotated one, so trust
  `D3D11_TEXTURE2D_DESC`.
- **No shared mutable state on the hot path.** Render/input/GUI threads communicate
  by `PostMessage` (`WM_APP_*`) and lock-free atomic writes to `StatusSnapshot`.
  Adding a lock to the render path is a correctness regression, not an optimisation.
- The control panel is on, reachable from the tray's "Settings…". `BM_PANEL=1`
  now only makes it **open at startup**, which is a test affordance: UIPI stops a
  normal-integrity script from clicking the tray or posting the window a message,
  so having the app open it is the only way in from outside.
- **The panel's control list is empirical. Extend it by measurement, never by
  assumption.** `TextBox`, `NumberBox` and `ProgressBar` each take the process
  down with a stowed exception on first layout. Proven safe: `TextBlock`,
  `Button`, `CheckBox`, `RadioButton`, `Slider`, `ToggleSwitch`, `ToggleButton`,
  `StackPanel`, `Border`. Hotkeys are captured through the existing
  `WH_KEYBOARD_LL` hook rather than typed.

  The failure has no error attached to it: the log ends at "Control panel
  opened" and the process is simply gone. If that is what you are looking at,
  the last control you added is the one to remove.
- The GUI thread runs its own `GetMessage` loop rather than `Application::Start`,
  so `PostThreadMessage(WM_QUIT)` can end it. Don't "simplify" it back.

## Conventions

- Comments are in English; commits follow Conventional Commits
  (`feat(input):`, `fix(render):`, `refactor:`).
- `spike/` holds throwaway experiments — not shipped code, don't refactor it.
- `res/*.ico` is **generated** by `tools/make-icons.ps1` from `res/icon-source.png`.
  Edit the source PNG and rerun; editing the `.ico` files directly loses the
  source. `tray-off.ico` is a desaturated copy of the same artwork, not a
  separate asset — the script derives it.

## Verifying without hands

Four assertion suites run on every debug start and under `--self-check`:
`SettingsStore`, `ViewportController`, `PointerInput` and `D3DRenderer`. The
last one covers the rotated-output transform, which is pure math precisely so
that it can be asserted without a GPU or a portrait monitor to hand.

Three diagnostic modes cover everything checkable without looking at a screen —
`--self-check` (pure-logic assertions), `--dump-cursors` and `--dump-osd` (both
write BMPs for eyeballing). All three are exempt from the single-instance mutex,
so they run while the app is running.

They are **not** exempt from elevation: `RequireAdministrator` is a property of
the binary, so launching the exe from an ordinary shell fails outright with
"requires elevation". Use the wrapper, which elevates once and reads the results
back out of the log:

```bash
.\bm.ps1 check
```

`BM_DUMP_FRAME` plus `BM_ALLOW_INJECTED=1` gets an actual magnified frame out of
a running instance — the overlay is excluded from capture, so it is the only
outside view of the render. Note that driving the pointer with `SetCursorPos`
from a script produces no `WH_MOUSE_LL` event on this machine, so the hook
liveness check reads it as a dead hook and reinstalls; harmless, but it is why
those log lines appear during automated runs and not in real use.
