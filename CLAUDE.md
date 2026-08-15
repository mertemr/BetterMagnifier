# BetterMagnifier

A live per-monitor screen magnifier for Windows. The one requirement Windows
Magnifier cannot meet — **independent zoom per monitor** — drives every hard
design decision in this codebase.

## Read this first

@docs/STATUS.md is the maintained source of truth: architecture, the three-thread
model and why it exists, the magnification transform, and current state. Read it
before changing anything under `src/`. @docs/PANEL-BLANK.md holds the control-panel
diagnosis notes (Turkish).

## Build

Use `.\bm.ps1` — never call MSBuild directly. It locates MSBuild via vswhere and
passes `/restore`, which the build now requires because the control panel pulls in
the Windows App SDK through `PackageReference`.

```
.\bm.ps1            # Debug
.\bm.ps1 run        # Debug + launch
.\bm.ps1 release    # Release
.\bm.ps1 errors     # WARN/ERROR lines from the newest log
.\bm.ps1 kill       # kill stray instances
```

`TreatWarningAsError` is on for both configurations. A warning is a build failure.

## Constraints that bite

- **Restore `RequireAdministrator` before shipping.** The manifest level was
  temporarily switched to `AsInvoker` (2026-08-07) so panel testing doesn't hit a
  UAC prompt every launch. It must be restored in **all four** `ItemDefinitionGroup`
  blocks in the vcxproj — a comment marks each site. Shipping `AsInvoker` silently
  breaks magnification over elevated windows.
- **Low-level hooks must stay on the input thread.** `WH_MOUSE_LL` and
  `WH_KEYBOARD_LL` run on the message queue of the thread that installed them. The
  render thread blocks in `Present`, so a hook there puts every mouse and key event
  in the system behind our frame — and exceeding `LowLevelHooksTimeout` (300 ms)
  makes Windows uninstall the hook without telling us. Hook callbacks may only
  `PostMessage` and return.
- **No shared mutable state on the hot path.** Render/input/GUI threads communicate
  by `PostMessage` (`WM_APP_*`) and lock-free atomic writes to `StatusSnapshot`.
  Adding a lock to the render path is a correctness regression, not an optimisation.
- The control panel is **off by default**; `BM_PANEL=1` enables it. The tray
  "Settings…" item only appears when that switch is on.
- The GUI thread runs its own `GetMessage` loop rather than `Application::Start`,
  so `PostThreadMessage(WM_QUIT)` can end it. Don't "simplify" it back.

## Conventions

- Comments are in English; commits follow Conventional Commits
  (`feat(input):`, `fix(render):`, `refactor:`).
- `spike/` holds throwaway experiments — not shipped code, don't refactor it.
- `docs/superpowers/` holds plans and specs written by earlier sessions.
