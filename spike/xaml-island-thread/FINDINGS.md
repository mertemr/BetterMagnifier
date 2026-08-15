# Spike result: WinUI 3 XAML Islands — SUCCESS

**Question:** With the main thread staying MTA, can a `DesktopWindowXamlSource`
come up on a separate STA thread?
**Answer:** **YES.** The island runs on a secondary STA thread, the main
thread stays MTA, theme styling applies, and controls take input.

This closed the open risk in the control panel design. The fallback paths
considered for it — a separate process with IPC, or Dear ImGui — were not
needed.

## Verified configuration

| What | Value |
|---|---|
| Windows App SDK | `1.8.250916003` (foundation `1.8.250906002`, WinUI `1.8.250906003`) |
| Runtime on the dev machine | `Microsoft.WindowsAppRuntime.1.8` `8000.921.1539.0` |
| Bootstrapper | `WindowsPackageType=None` auto-initializer (no manual `MddBootstrapInitialize`) |
| Main thread | MTA — `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` |
| GUI thread | STA — `winrt::init_apartment(apartment_type::single_threaded)` |
| Toolset | `$(DefaultPlatformToolset)` (resolves per machine) |

Output:

```
=== Variant: mta ===
Main thread: MTA (COINIT_MULTITHREADED)
Starting GUI thread (STA)...
  [1/5] Bootstrapper auto-init (WindowsPackageType=None)
  [2/5] Calling Application::Start...
  [2.5/5] ENTERED the Start callback
  [2.6/5] Application instance created
  [3/5] XAML runtime OK (default theme dictionary)
  [4/5] Host HWND OK
  [5/5] DesktopWindowXamlSource OK

SPIKE SUCCEEDED: island is up on the secondary STA thread, main thread stays MTA
```

The window shows a properly styled WinUI button with rounded corners and
correctly coloured text — no colour was set by hand; all of it came from the
theme dictionary.

## Two bugs that sank the first attempt

The previous run hit `0xC0000005` (access violation), which pointed the
investigation at MTA/STA as the cause. It was not. There were two separate
bugs, both in the spike's own code:

### 1. The `Application::Start` callback must CREATE an `Application` instance

`Application::Start(cb)` does not create an `Application` itself — it expects
the callback to do that. The earlier code called `Application::Current()`
directly inside the callback; with no instance yet, that returned `null`, and
calling `.DispatcherShutdownMode()` on a null reference is an access
violation.

In C++/WinRT, calling a method on a null projection object fails silently
rather than throwing. With no exception raised, it looked like the callback
was never entered at all — it was; it just fell over on the first three
lines.

Fix — a markup-less `App` using the `ApplicationT<D>` template the projection
already provides:

```cpp
struct SpikeApp : winrt::Microsoft::UI::Xaml::ApplicationT<SpikeApp> {};

Application::Start([](auto&&) {
    auto app = winrt::make<SpikeApp>();   // THIS LINE IS REQUIRED
    auto current = Application::Current(); // now populated
    ...
});
```

### 2. `XamlControlsResources` must NOT be assigned by hand

Once the access violation was fixed, a clean `E_FAIL` came out instead:
`Cannot find a resource with the given key: AcrylicBackgroundFillColorDefaultBrush`.

This is **not** a missing-file problem. WinUI 2 / UWP needed the theme
dictionary merged by hand; WinUI 3 does not — the default dictionary is
already loaded. `current.Resources(XamlControlsResources{})` replaces the
existing dictionary outright, and `XamlControlsResources`'s own internal
references (including that exact Acrylic brush) are gone at that point. That
line was not the fix — it was the cause.

**Elimination log** — a suspected missing PRI file was tested and ruled out
step by step:

| Attempt | Result |
|---|---|
| Just `spike.pri` | E_FAIL |
| Added `resources.pri` (the app's own PRI) | E_FAIL |
| The framework's `Microsoft.UI.Xaml.Controls.pri` next to the exe | E_FAIL |
| `resources.pri` = the framework's Controls.pri | E_FAIL |
| **Removed the `XamlControlsResources` line** | **WORKED** |

`makepri merge` was also tried — the `merge` command does not exist in
Windows SDK 10.0.26100, and turned out to be unnecessary anyway.

## Diagnostic tool: a vectored exception handler

A bare `0xC0000005` says nothing on its own. `CrashProbe` in `spike.cpp` runs
ahead of SEH and prints the faulting address, the module, and the address that
was being accessed. An accessed address of `0x0` names the culprit
immediately: a virtual call through a null pointer.

This function is not specific to the spike — the same pattern is useful for a
similar crash in the main project.

## Build issues resolved along the way

These will come up again wherever WinUI is added to the main project:

1. **`ResolveNuGetPackageAssets` crashes.** Using `PackageReference` in a C++
   vcxproj makes the legacy task inside `Microsoft.NuGet.targets` fail with
   "Sequence contains no elements". Fix:
   `<ResolveNuGetPackages>false</ResolveNuGetPackages>`.

2. **`Microsoft.WindowsAppRuntime.Bootstrap.lib` cannot be found.** The SDK is
   split across sub-packages. The lib lives under
   `microsoft.windowsappsdk.foundation/<version>/lib/native/x64/`; that
   package's `.props` adds the include path but **not** the lib path. A manual
   `AdditionalLibraryDirectories` entry is needed.

   Note: the foundation package's version differs from the main package's and
   is resolved transitively (`1.8.250916003` → `1.8.250906002`). It sits in
   `spike.vcxproj` as a `WasdkFoundationVersion` property; if the main
   package's version changes, this has to be updated too. (Automating it with
   a glob was tried and rejected: an item list inside `ItemDefinitionGroup` is
   not allowed — MSB4164.)

3. **`UseWinUI=true`** is required; it turns on the WinUI build targets and
   `.pri` generation.

4. **C3779 — missing includes.** In C++/WinRT, "a function that returns auto
   cannot be used before it is defined" always means a missing header:
   - `IVector<T>::Append` → `winrt/Windows.Foundation.Collections.h`
   - `Button::Click` → `winrt/Microsoft.UI.Xaml.Controls.Primitives.h`

5. **`C4002` on `GetCurrentTime`.** `windows.h` defines it as a macro, and
   `Microsoft.UI.Xaml.Media.Animation` has a method with the same name. With
   `TreatWarningAsError` on in the main project this becomes a hard **error**.
   `#undef GetCurrentTime` before including the XAML headers.

6. **`MSB8027` / `LNK4042`.** `WindowsAppRuntimeAutoInitializer.cpp` is
   contributed by two separate sub-packages and compiles twice. Warnings only,
   output is correct. Moving this into the main project may need it
   suppressed because of `TreatWarningAsError`.

## Effect on the design

The three-thread architecture holds up **exactly as planned**:

- Render thread (main, MTA) — D3D11/DXGI/overlay
- Input thread — low-level hooks
- GUI thread (STA) — a XAML island via `DesktopWindowXamlSource`

One constraint became clear: `Application::Start` runs the message loop
itself and does not return until the application closes. So handing work to
the GUI thread needs `DispatcherQueue.TryEnqueue` rather than
`PostThreadMessage`. The GUI→engine direction is unaffected — `PostMessage` to
the message window, as originally designed.
