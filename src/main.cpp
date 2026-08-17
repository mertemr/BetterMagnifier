// Process entry point and one-time setup: DPI mode, the single-instance mutex,
// COM, the logger, and the diagnostic command lines. Everything past that lives
// in App.

#include "pch.h"
#include "App.h"
#include "AppMessages.h"
#include "SettingsStore.h"
#include "ViewportController.h"
#include "PointerInput.h"
#include "SystemCursor.h"
#include "D3DRenderer.h"
#include "CursorRenderer.h"
#include "OsdRenderer.h"
#include "UpdateChecker.h"
#include "Version.h"
#include "Logger.h"

#include <cwchar>
#include <winver.h>
#pragma comment(lib, "version.lib")

static void SetupDpiAwareness();
static void AttachDebugConsole();

// A startup problem, said in whichever way the caller can actually receive.
//
// A message box is right for a user who double-clicked the exe and wrong for a
// script: nobody clears it, so the process waits forever holding the exit code
// the caller is blocking on. Diagnostic runs get stderr and the debugger
// channel instead, both of which a build agent captures.
static void ReportStartupFailure(bool diagnosticRun, const wchar_t* message, UINT icon)
{
    if (diagnosticRun)
    {
        std::fwprintf(stderr, L"BetterMagnifier: %ls\n", message);
        std::fflush(stderr);
        OutputDebugStringW(message);
        OutputDebugStringW(L"\n");
        return;
    }

    MessageBoxW(nullptr, message, L"BetterMagnifier", icon);
}

// Reads the version out of the exe's own VERSIONINFO resource (see
// BetterMagnifier.rc) instead of a string literal here, so the two can't
// drift apart the way FILEVERSION and the FileVersion string already have.
static std::wstring GetAppVersionString()
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(exePath, &handle);
    if (size == 0)
        return L"0.0.0.0";

    std::vector<BYTE> buffer(size);
    if (!GetFileVersionInfoW(exePath, handle, size, buffer.data()))
        return L"0.0.0.0";

    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoLen = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &infoLen) || !info)
        return L"0.0.0.0";

    return std::format(L"{}.{}.{}.{}",
        HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
        HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPWSTR /*lpCmdLine*/,
    _In_ int /*nCmdShow*/)
{
    // Per-Monitor V2. Without it Windows reports a flat 96 DPI and scales the
    // window itself, which on a magnifier means blurring the thing whose whole
    // job is to be legible.
    SetupDpiAwareness();

    // Worked out first, because everything below behaves differently for a
    // scripted run. A diagnostic mode exists to be driven from a script, so it
    // must never stop on something only a human can clear.
    const bool diagnosticRun =
        std::wcsstr(GetCommandLineW(), L"--self-check")   != nullptr ||
        std::wcsstr(GetCommandLineW(), L"--dump-cursors") != nullptr ||
        std::wcsstr(GetCommandLineW(), L"--dump-osd")     != nullptr ||
        std::wcsstr(GetCommandLineW(), L"--check-update") != nullptr;

#ifdef _DEBUG
    // Not for a diagnostic run. AttachDebugConsole reopens stdout and stderr on
    // CONOUT$, which points them at a console window nobody is reading - so on
    // a build agent every message the run produces disappears. Left alone, they
    // stay on whatever pipe the caller gave us.
    if (!diagnosticRun)
        AttachDebugConsole();
#endif

    // ── Single instance ──
    //
    // Two copies stack two fullscreen topmost overlays, install two global hook
    // chains, and open Desktop Duplication twice on the same output. The result
    // is stutter plus input nobody can reason about, because which overlay
    // swallowed a click is anyone's guess.
    //
    // A named mutex rather than a lock file: the kernel closes the handle when
    // the process dies, so a crash cannot leave the lock held.
    //
    // Diagnostic modes are exempt. They run pure logic and exit without
    // opening a window, installing a hook or touching Desktop Duplication, so
    // none of the reasons above apply to them. The exemption is not a nicety:
    // without it the self-check cannot run while the app is running, which is
    // exactly when you most want to check something — and worse, it blocks on
    // the "already running" message box, so a script waiting on it hangs
    // forever instead of failing.
    //
    // Which is exactly what happened to the two message boxes below, and the
    // reasoning above applies to them word for word: a build agent has nobody
    // to click OK, so the run sat there until the job timed out with no log and
    // no output to say why.
    HANDLE singleInstance = nullptr;
    if (!diagnosticRun)
    {
        singleInstance = CreateMutexW(nullptr, TRUE, L"BetterMagnifier_SingleInstance_Mutex");
        if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (singleInstance)
                CloseHandle(singleInstance);

            HWND existing = FindWindowW(L"BetterMagnifierMsg", nullptr);
            if (existing)
            {
                PostMessageW(existing, BetterMagnifier::WM_APP_SHOW_PANEL, 0, 0);
                return 0;
            }

            MessageBoxW(nullptr,
                L"BetterMagnifier is already running.\n\n"
                L"Look for the tray icon.",
                L"BetterMagnifier", MB_ICONINFORMATION | MB_OK);
            return 0;
        }
    }

    // ── COM ──
    // MTA because D3D11, DXGI and Desktop Duplication are touched from the
    // render thread while the tray and hotkeys live on this one. The panel
    // needs STA, which is why it gets a thread of its own.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        ReportStartupFailure(diagnosticRun, L"COM initialisation failed.", MB_ICONERROR);
        return 1;
    }

    // ── Logger ──
    // Next to the exe rather than in %APPDATA%: this is a development log, and
    // it should be where the binary is when someone goes looking for it.
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path logDir = std::filesystem::path(exePath).parent_path() / L"logs";

        auto& logger = BetterMagnifier::Logger::Instance();
        if (!logger.Initialize(logDir, BetterMagnifier::LogLevel::Debug))
        {
            // Not fatal. Running without a log beats not running.
            ReportStartupFailure(diagnosticRun, L"Logger initialisation failed.",
                                 MB_ICONWARNING);
        }
    }

    LOG_INFO("═══════════════════════════════════════════════════");
    LOG_INFO("  BetterMagnifier v{} starting", ToUtf8(GetAppVersionString()));
    LOG_INFO("═══════════════════════════════════════════════════");
    LOG_INFO("HINSTANCE: 0x{:X}", reinterpret_cast<uintptr_t>(hInstance));

    // A constant rather than #ifdef inside the macro call: preprocessor
    // directives cannot appear in a macro argument list.
#ifdef _DEBUG
    constexpr bool kIsDebugBuild = true;
#else
    constexpr bool kIsDebugBuild = false;
#endif
    LOG_INFO("Debug build: {}", kIsDebugBuild ? "yes" : "no");

    // ── Pointer-hiding capability ──
    //
    // Probed before anything else can want it, and before the self-check exit,
    // so `--self-check` reports the result too. Whether MagShowSystemCursor
    // works decides whether pointer compositing may be on by default: the
    // SetSystemCursor fallback outlives the process, so a kill from Task
    // Manager would leave the user with no pointer at all.
    BetterMagnifier::SystemCursor::Probe();
    BetterMagnifier::SystemCursor::InstallGuards();

    // ── Debug self-check ──
    // The two components with pure logic in them. Failing here beats failing as
    // strange behaviour halfway through a session.
#ifdef _DEBUG
    // Route CRT diagnostics to stderr rather than a dialog. This does NOT cover
    // <cassert> — _wassert opens a message box regardless in a GUI subsystem
    // build, which is why the suite uses BM_SELFCHECK instead.
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

    BetterMagnifier::SettingsStoreSelfCheck();
    BetterMagnifier::ViewportControllerSelfCheck();
    BetterMagnifier::PointerInputSelfCheck();
    BetterMagnifier::D3DRendererSelfCheck();
    BetterMagnifier::UpdateCheckerSelfCheck();

    // --self-check runs the pure-logic assertions and exits, so the suite is
    // scriptable. Without it the process would go on to open windows and never
    // return, and there would be no way to gate a commit on the asserts.
    if (std::wcsstr(GetCommandLineW(), L"--self-check") != nullptr)
    {
        LOG_INFO("Self-check complete, exiting (--self-check)");
        return 0;
    }

    // --dump-cursors decodes the standard cursors to BMP for eyeball checking.
    // Cursor decoding cannot be asserted: whether the monochrome AND/XOR
    // unpacking is right is a question about what the picture looks like, and
    // "some pixels are set" would pass for a solid black square too.
    if (std::wcsstr(GetCommandLineW(), L"--dump-cursors") != nullptr)
    {
        // const, not constexpr: IDC_* are MAKEINTRESOURCEW casts, which are
        // not constant expressions.
        struct Shape { const wchar_t* name; LPCWSTR id; };
        const Shape kShapes[] = {
            { L"arrow",   IDC_ARROW   },
            { L"ibeam",   IDC_IBEAM   },   // monochrome path
            { L"wait",    IDC_WAIT    },   // colour with alpha
            { L"hand",    IDC_HAND    },
            { L"sizeall", IDC_SIZEALL },
            { L"cross",   IDC_CROSS   },   // monochrome path
        };

        wchar_t dir[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"TEMP", dir, MAX_PATH) == 0)
            wcscpy_s(dir, L".");

        int failures = 0;
        for (const Shape& s : kShapes)
        {
            BetterMagnifier::CursorBitmap bmp;
            if (!BetterMagnifier::DecodeCursor(LoadCursorW(nullptr, s.id), bmp))
            {
                LOG_ERROR("DecodeCursor failed for {}", ToUtf8(s.name));
                ++failures;
                continue;
            }

            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%ls\\bm-cursor-%ls.bmp", dir, s.name);
            if (!BetterMagnifier::WriteCursorBitmapFile(bmp, path))
            {
                LOG_ERROR("Could not write {}", ToUtf8(path));
                ++failures;
                continue;
            }

            LOG_INFO("cursor {}: {}x{} hotspot ({},{}) -> {}",
                     ToUtf8(s.name), bmp.width, bmp.height,
                     bmp.hotspotX, bmp.hotspotY, ToUtf8(path));
        }

        LOG_INFO("Cursor dump complete, {} failure(s)", failures);
        return failures == 0 ? 0 : 3;
    }

    // --dump-osd writes the readout bitmaps for eyeball checking, for the same
    // reason --dump-cursors exists: whether the alpha compositing is right is a
    // question about what the picture looks like. An assertion that "some
    // pixels are opaque" passes just as happily for black text on a black pill,
    // which is precisely the failure the hand-built alpha channel invites.
    if (std::wcsstr(GetCommandLineW(), L"--dump-osd") != nullptr)
    {
        const struct { const wchar_t* text; int px; } kLabels[] = {
            { L"2.50x",  54 },
            { L"12.00x", 54 },
            { L"Frozen", 54 },
            { L"Live",   22 },
        };

        wchar_t dir[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"TEMP", dir, MAX_PATH) == 0)
            wcscpy_s(dir, L".");

        int failures = 0;
        for (const auto& l : kLabels)
        {
            BetterMagnifier::OsdBitmap osd;
            if (!BetterMagnifier::RenderOsdText(l.text, l.px, osd))
            {
                LOG_ERROR("RenderOsdText failed for {}", ToUtf8(l.text));
                ++failures;
                continue;
            }

            // Reuse the cursor BMP writer: both are premultiplied BGRA in a
            // top-down buffer, and the hotspot fields it wants are ignored.
            BetterMagnifier::CursorBitmap as;
            as.pixels = osd.pixels;
            as.width  = osd.width;
            as.height = osd.height;

            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%ls\\bm-osd-%ls-%d.bmp", dir, l.text, l.px);

            if (!BetterMagnifier::WriteCursorBitmapFile(as, path))
            {
                LOG_ERROR("Could not write {}", ToUtf8(path));
                ++failures;
                continue;
            }

            LOG_INFO("osd \"{}\" @{}px: {}x{} -> {}",
                     ToUtf8(l.text), l.px, osd.width, osd.height, ToUtf8(path));
        }

        LOG_INFO("OSD dump complete, {} failure(s)", failures);
        return failures == 0 ? 0 : 3;
    }
#endif

    // Outside the #ifdef _DEBUG above, unlike the other three: the binary worth
    // asking about its update state is the one that ships, and that is Release.
    // It opens no window and installs no hook, so the mutex exemption costs
    // nothing.
    //
    //   0  up to date        2  update available        3  the check failed
    if (std::wcsstr(GetCommandLineW(), L"--check-update") != nullptr)
    {
        const std::wstring feed = BetterMagnifier::UpdateFeedUrl();
        LOG_INFO("Update check against {}", ToUtf8(feed));

        BetterMagnifier::ReleaseInfo info;
        if (!BetterMagnifier::FetchLatestRelease(feed, info))
        {
            LOG_ERROR("Update check FAILED");
            return 3;
        }

        LOG_INFO("Update check: running {}, latest {}, setup {} ({} bytes)",
                 BM_VERSION_STRING, ToUtf8(info.version),
                 ToUtf8(info.setup.name), info.setup.size);

        if (BetterMagnifier::CompareVersion(info.version, BM_VERSION_STRING_W) > 0)
        {
            LOG_INFO("Update check: UPDATE AVAILABLE");
            return 2;
        }

        LOG_INFO("Update check: up to date");
        return 0;
    }

    // App owns everything past this point. The extra scope is load-bearing: its
    // destructor releases D3D and DXGI interfaces, and those must go while COM
    // is still initialised. Letting it run to the end of the function would put
    // the release after CoUninitialize.
    int exitCode = 0;
    {
        BetterMagnifier::App app;

        if (!app.Initialize(hInstance))
        {
            LOG_ERROR("App initialisation failed, exiting");
            MessageBoxW(nullptr,
                L"BetterMagnifier could not start.\nSee the logs folder for details.",
                L"BetterMagnifier", MB_ICONERROR);
            exitCode = 1;
        }
        else
        {
            exitCode = app.Run();
        }
    }

    LOG_INFO("BetterMagnifier shutting down");
    LOG_INFO("Exit code: {}", exitCode);

    BetterMagnifier::Logger::Instance().Shutdown();

    CoUninitialize();

    // Windows drops the handle on process exit anyway; releasing it explicitly
    // states the intent.
    if (singleInstance)
    {
        ReleaseMutex(singleInstance);
        CloseHandle(singleInstance);
    }

    return exitCode;
}

// A DPI-unaware process gets bitmap-stretched by the compositor on a scaled
// display. For a magnifier that is fatal: it would blur the one thing the
// application exists to keep sharp. Per-Monitor V2 also scales the non-client
// area and delivers WM_DPICHANGED, which matters with mixed-DPI displays.
static void SetupDpiAwareness()
{
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        // Windows falls back to system-DPI-aware on its own. Worth knowing
        // about, not worth failing over.
        OutputDebugStringW(L"[DPI] Per-Monitor V2 unavailable, using the system default\n");
    }
}

// A /SUBSYSTEM:WINDOWS binary has no console, so stdout and stderr go nowhere.
// Debug builds get one so CRT diagnostics and stream output are visible.
static void AttachDebugConsole()
{
    if (AllocConsole())
    {
        FILE* fp = nullptr;
        (void)freopen_s(&fp, "CONOUT$", "w", stdout);
        (void)freopen_s(&fp, "CONOUT$", "w", stderr);
        (void)freopen_s(&fp, "CONIN$", "r", stdin);

        // The iostream objects cached their old, failed state.
        std::cout.clear();
        std::cerr.clear();
        std::cin.clear();

        SetConsoleTitleW(L"BetterMagnifier — Debug Console");
        SetConsoleOutputCP(CP_UTF8);

        OutputDebugStringW(L"[Debug] Console attached successfully.\n");
    }
}
