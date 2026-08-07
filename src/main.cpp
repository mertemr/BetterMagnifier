// =============================================================================
// main.cpp — BetterMagnifier Entry Point
// =============================================================================
//
// Python analojisi:
//   Python'da:  if __name__ == "__main__": main()
//   C++'ta:     WinMain() — Windows GUI uygulamalarının entry point'i
//
// Normal C++ programları main() ile başlar (Console uygulamaları).
// Windows GUI uygulamaları WinMain() ile başlar — fark:
//   - Console penceresi açılmaz
//   - Windows bize HINSTANCE (uygulama kimliği) verir
//   - Bu HINSTANCE'ı pencere oluşturmak için kullanırız
//
// Message Loop:
//   Python'da: asyncio.run() veya Qt'daki app.exec_() gibi event loop.
//   Windows'ta: GetMessage/PeekMessage + DispatchMessage döngüsü.
//   Her klavye tuşu, mouse hareketi, pencere olayı bu döngüden geçer.
//   Biz "hybrid" model kullanıyoruz:
//     - PeekMessage (non-blocking) + render loop = oyun motoru pattern'i
//     - Mesaj yoksa render/update yapabiliriz — boşa CPU harcamak yerine
//       DXGI frame capture ve render bu boşlukta çalışır
//
// =============================================================================

#include "pch.h"
#include "App.h"
#include "SettingsStore.h"
#include "ViewportController.h"
#include "SystemCursor.h"
#include "Logger.h"

#include <cwchar>

// ─────────────────────────────────────────────────────────────────────────────
// Forward Declarations (ileride App sınıfı buralara bağlanacak)
// ─────────────────────────────────────────────────────────────────────────────

// DPI Awareness ayarla — bu uygulama multi-monitor DPI farkını doğru handle edecek
static void SetupDpiAwareness();

// Konsol penceresi attach (Debug build'de log'ları görmek için)
static void AttachDebugConsole();

// ─────────────────────────────────────────────────────────────────────────────
// WinMain — Program Entry Point
// ─────────────────────────────────────────────────────────────────────────────
// Python'daki "if __name__ == '__main__':" karşılığı.
//
// Parametreler:
//   hInstance    — Bu EXE'nin kimlik numarası. Pencere oluştururken lazım.
//   hPrevInstance — Her zaman NULL (Win16 kalıntısı, ignore et).
//   lpCmdLine   — Komut satırı argümanları (Python'daki sys.argv gibi).
//   nCmdShow    — Pencere nasıl gösterilsin (minimize, maximize, normal).
// ─────────────────────────────────────────────────────────────────────────────
int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,  // Kullanılmıyor — isim vermeye gerek yok
    _In_ LPWSTR /*lpCmdLine*/,
    _In_ int /*nCmdShow*/)
{
    // ── 1. DPI Awareness ──
    // Multi-monitor setup'ta her monitörün farklı DPI'ı olabilir.
    // Bu çağrı olmadan Windows bize yalan söyler — 96 DPI varsayar
    // ve uygulamayı bulanık şekilde ölçekler (DPI Virtualization).
    // Per-Monitor DPI Aware V2 = en doğru ve modern mod.
    SetupDpiAwareness();

    // ── 2. Debug Console (sadece Debug build'de) ──
#ifdef _DEBUG
    AttachDebugConsole();
#endif

    // ── 2b. Tek instance kontrolü ──
    //
    // İki kopya birlikte çalışırsa: iki tam ekran topmost overlay üst üste
    // biner, iki ayrı global hook zinciri kurulur ve ikisi de aynı monitör
    // için Desktop Duplication açmaya çalışır. Sonuç kasma ve tahmin
    // edilemeyen girdi davranışı — hangi overlay'in tıklamayı yuttuğu
    // belirsiz hale gelir.
    //
    // Named mutex: process ölünce Windows handle'ı otomatik kapatır, yani
    // çökme sonrası kilitli kalma riski yok (dosya tabanlı kilidin aksine).
    //
    // Python analojisi: tek örnek için port bind etmek / lock file tutmak,
    // ama bu ikisinden de güvenli çünkü OS temizliğini garanti ediyor.
    //
    // Diagnostic modes are exempt. They run pure logic and exit without
    // opening a window, installing a hook or touching Desktop Duplication, so
    // none of the reasons above apply to them. The exemption is not a nicety:
    // without it the self-check cannot run while the app is running, which is
    // exactly when you most want to check something — and worse, it blocks on
    // the "already running" message box, so a script waiting on it hangs
    // forever instead of failing.
    const bool diagnosticRun =
        std::wcsstr(GetCommandLineW(), L"--self-check")   != nullptr ||
        std::wcsstr(GetCommandLineW(), L"--dump-cursors") != nullptr;

    HANDLE singleInstance = nullptr;
    if (!diagnosticRun)
    {
        singleInstance = CreateMutexW(nullptr, TRUE, L"BetterMagnifier_SingleInstance_Mutex");
        if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (singleInstance)
                CloseHandle(singleInstance);

            MessageBoxW(nullptr,
                L"BetterMagnifier zaten çalışıyor.\n\n"
                L"Tepsi (system tray) ikonuna bakın.",
                L"BetterMagnifier", MB_ICONINFORMATION | MB_OK);
            return 0;
        }
    }

    // ── 3. COM Initialize ──
    // COM = Component Object Model — Windows'un nesne paylaşım sistemi.
    // Python analojisi: Python'da import etmeden önce sys.path ayarlamak gibi.
    // DirectX, DXGI, Direct2D hepsi COM tabanlıdır — önce init etmeliyiz.
    //
    // COINIT_MULTITHREADED: Birden fazla thread COM objelerine erişebilir.
    // Magnifier'da capture thread + render thread + UI thread olacak.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"COM başlatılamadı!", L"BetterMagnifier", MB_ICONERROR);
        return 1;
    }

    // ── 4. Logger Initialize ──
    // Log dosyası EXE'nin yanındaki "logs" klasörüne yazılacak.
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path logDir = std::filesystem::path(exePath).parent_path() / L"logs";

        auto& logger = BetterMagnifier::Logger::Instance();
        if (!logger.Initialize(logDir, BetterMagnifier::LogLevel::Debug))
        {
            MessageBoxW(nullptr, L"Logger başlatılamadı!", L"BetterMagnifier", MB_ICONWARNING);
            // Kritik değil — devam edebiliriz, sadece log yazılmaz
        }
    }

    LOG_INFO("═══════════════════════════════════════════════════");
    LOG_INFO("  BetterMagnifier v0.1.0 başlatılıyor...");
    LOG_INFO("═══════════════════════════════════════════════════");
    LOG_INFO("HINSTANCE: 0x{:X}", reinterpret_cast<uintptr_t>(hInstance));

    // Preprocessor directive'ler (#ifdef) makro argümanı içinde kullanılamaz.
    // Python analojisi: f"Debug: {'yes' if __debug__ else 'no'}"
    // C++'ta: constexpr if veya compile-time sabit ile çözüyoruz.
#ifdef _DEBUG
    constexpr bool kIsDebugBuild = true;
#else
    constexpr bool kIsDebugBuild = false;
#endif
    LOG_INFO("Debug build: {}", kIsDebugBuild ? "EVET" : "HAYIR");

    // ── 5. Pointer-hiding capability ──
    //
    // Probed before anything else can want it, and before the self-check exit,
    // so `--self-check` reports the result too. Whether MagShowSystemCursor
    // works decides whether pointer compositing may be on by default: the
    // SetSystemCursor fallback outlives the process, so a kill from Task
    // Manager would leave the user with no pointer at all.
    BetterMagnifier::SystemCursor::Probe();
    BetterMagnifier::SystemCursor::InstallGuards();

    // ── 6. Debug self-check ──
    // Saf mantığı olan tek bileşenimiz SettingsStore. Bozulursa burada
    // düşüyoruz — uygulamanın ortasında tuhaf davranış olarak değil.
#ifdef _DEBUG
    // assert başarısız olunca MessageBox AÇMA — stderr'e yaz ve düş.
    // Otomatik doğrulamada dialog bekleyen bir process asılı kalır; log'da da
    // hiçbir iz bırakmaz. Bu üç çağrı self-check'i betikten koşulabilir yapıyor.
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

    BetterMagnifier::SettingsStoreSelfCheck();
    BetterMagnifier::ViewportControllerSelfCheck();

    // --self-check runs the pure-logic assertions and exits, so the suite is
    // scriptable. Without it the process would go on to open windows and never
    // return, and there would be no way to gate a commit on the asserts.
    if (std::wcsstr(GetCommandLineW(), L"--self-check") != nullptr)
    {
        LOG_INFO("Self-check complete, exiting (--self-check)");
        return 0;
    }
#endif

    // ── 6. App'i başlat ve çalıştır ──
    // Tüm iş App sınıfında: component init, message loop, capture/render, cleanup.
    // WinMain'in tek sorumluluğu process seviyesi kurulum (DPI, COM, Logger).
    //
    // Neden ayrı scope ({ }) içinde?
    //   App destructor'ı CoUninitialize'dan ÖNCE çalışmalı — içindeki tüm
    //   COM nesneleri (D3D device, DXGI swap chain) COM hâlâ ayaktayken
    //   serbest bırakılmalı. Scope bitince destructor garantili çalışır.
    //   Python analojisi: with App() as app: app.run()
    int exitCode = 0;
    {
        BetterMagnifier::App app;

        if (!app.Initialize(hInstance))
        {
            LOG_ERROR("App baslatilamadi — cikiliyor");
            MessageBoxW(nullptr,
                L"BetterMagnifier başlatılamadı.\nDetaylar için logs klasörüne bakın.",
                L"BetterMagnifier", MB_ICONERROR);
            exitCode = 1;
        }
        else
        {
            exitCode = app.Run();
        }
    }

    // ── 6. Cleanup (RAII sırası önemli!) ──
    // C++'ta "cleanup sırası" kritiktir. Genel kural:
    //   "Son oluşturulan → ilk yıkılır" (LIFO — stack gibi)
    //
    // DirectX/COM cleanup sırası:
    //   1. Render kaynakları (texture, shader, buffer)
    //   2. Swap chain
    //   3. Device context
    //   4. Device
    //   5. DXGI Factory
    //   6. COM Uninitialize
    //
    // Yanlış sıra = crash veya COM leak. ComPtr<T> RAII ile bu sıra
    // büyük ölçüde otomatik oluyor, ama dikkatli olmak lazım.

    LOG_INFO("BetterMagnifier kapatılıyor...");
    LOG_INFO("Exit code: {}", exitCode);

    BetterMagnifier::Logger::Instance().Shutdown();

    CoUninitialize();

    // Tek instance mutex'ini birak — process olurken Windows zaten kapatir
    // ama acikca birakmak niyeti belli ediyor.
    if (singleInstance)
    {
        ReleaseMutex(singleInstance);
        CloseHandle(singleInstance);
    }

    return exitCode;
}

// =============================================================================
// DPI Awareness Setup
// =============================================================================
// Neden önemli?
// Windows varsayılan olarak uygulamaları "DPI unaware" kabul eder.
// Bu durumda 4K monitörde (200% DPI) uygulama 1080p gibi render edilip
// bulanık şekilde büyütülür. Magnifier uygulaması MUTLAKA pixel-perfect olmalı.
//
// Per-Monitor DPI Aware V2 (en iyi mod):
//   - Her monitörde doğru DPI kullanılır
//   - Non-client area (title bar, scrollbar) da otomatik ölçeklenir
//   - WM_DPICHANGED mesajıyla DPI değişikliği bildirilir
// =============================================================================
static void SetupDpiAwareness()
{
    // Windows 10 1703+ API — en modern ve doğru yöntem
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        // Fallback: eski API (Windows 8.1+)
        // Bu da olmazsa Windows varsayılanı kullanır (system DPI aware)
        OutputDebugStringW(L"[DPI] Per-Monitor V2 ayarlanamadı, fallback deneniyor...\n");
    }
}

// =============================================================================
// Debug Console Attach
// =============================================================================
// GUI uygulamalarında (SubSystem: Windows) varsayılan olarak console penceresi
// açılmaz. Debug sırasında std::cout/cerr çıktılarını görmek için
// bir console penceresi oluşturup stdout/stderr'i yönlendiriyoruz.
//
// Release build'de bu fonksiyon çağrılmaz — #ifdef _DEBUG ile korunuyor.
// =============================================================================
static void AttachDebugConsole()
{
    // Konsol penceresi oluştur
    if (AllocConsole())
    {
        // stdout, stderr, stdin'i yeni konsola yönlendir
        FILE* fp = nullptr;
        (void)freopen_s(&fp, "CONOUT$", "w", stdout);
        (void)freopen_s(&fp, "CONOUT$", "w", stderr);
        (void)freopen_s(&fp, "CONIN$", "r", stdin);

        // C++ stream'leri de senkronize et
        std::cout.clear();
        std::cerr.clear();
        std::cin.clear();

        SetConsoleTitleW(L"BetterMagnifier — Debug Console");
        SetConsoleOutputCP(CP_UTF8);

        OutputDebugStringW(L"[Debug] Console attached successfully.\n");
    }
}
