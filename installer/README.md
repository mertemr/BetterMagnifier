# installer

The NSIS script that produces `BetterMagnifier-<version>-x64-setup.exe`.

Build it with `.\bm.ps1 installer` from the repository root, which builds Release
x64 first and reads the version out of `src/Version.h` so there is nothing to
type. To run `makensis` by hand:

```
makensis /DVERSION=0.2.0 BetterMagnifier.nsi
```

| Define | Meaning |
|---|---|
| `VERSION` | **Required.** `0.2.0` — names the output and fills the version resource |
| `SOURCE_DIR` | Where the built binaries are. Defaults to `..\bin\Release-x64` |
| `RUNTIME_EXE` | Path to `WindowsAppRuntimeInstall-x64.exe`. Optional locally, **required for a release** |

`RUNTIME_EXE` is optional so the local loop does not have to download 40 MB on
every iteration. A setup built without it works on a machine that already has
the Windows App Runtime and cannot open the control panel on one that does not,
which is why CI always passes it.

Output lands in `out/`, which is gitignored.

## Things this script cannot omit

Both were found by installing and looking, not by reading, and both are silent
failures rather than errors:

- **`SetRegView 64`.** `makensis` produces a 32-bit binary, so every
  `HKLM\Software` write is redirected into `Wow6432Node` without it. That is
  not cosmetic here: `IsInstalledCopy` in `src/UpdateChecker.cpp` reads the
  64-bit view, so the key would never be found and the in-app updater would
  refuse to install on every installed copy — the whole feature, off, with
  nothing logged to say why.
- **`SetShellVarContext all`.** `$SMPROGRAMS` means the *current user's* Start
  Menu by default, even under an elevated per-machine install. Without it the
  shortcut lands in one profile, nobody else on the machine sees it, and the
  uninstaller looks somewhere else to remove it.

  It is global state, so the optional settings-removal section sets it back to
  `current` — otherwise `$APPDATA` there would mean `ProgramData` and the
  `RMDir /r` would be aimed at the wrong directory entirely.

## Uninstall keeps your settings

`%APPDATA%\BetterMagnifier` survives an uninstall unless the user ticks the
optional component. Settings and the remembered per-monitor zoom levels live
there, and an *update* runs this installer too — so removing them by default
would quietly discard a user's configuration on every version bump.

## Verified by hand

Install, silent install, the `HKLM` gate the updater reads, uninstall while the
application is running, and that the settings survive. See
`docs/superpowers/plans/2026-08-15-installer-autoupdate.md` for the design and
`docs/superpowers/specs/2026-08-15-installer-autoupdate-design.md` for why the
runtime is carried rather than the build going self-contained.
