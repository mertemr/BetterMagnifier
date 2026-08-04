// =============================================================================
// spike.cpp — DesktopWindowXamlSource ikincil STA thread'de calisiyor mu?
// =============================================================================
//
// SORU: BetterMagnifier'in ana thread'i MTA (COINIT_MULTITHREADED, main.cpp:75).
// XAML ise UI thread'inde STA istiyor. Ana thread'i STA'ya cevirmek secenek
// degil (render loop'u Present(vSync) ile blokluyor, XAML dispatcher'i ac kalir).
//
// Bu spike sunu kanitliyor: ana thread MTA kalirken, AYRI bir STA thread'de
// XAML island ayaga kalkabilir mi?
//
// Cevap "hayir" cikarsa kontrol paneli tasarimi degisir (ayri process + IPC).
// =============================================================================

#include <windows.h>
#include <objbase.h>
#include <thread>
#include <atomic>
#include <cstdio>
#include <string_view>

// windows.h GetCurrentTime'i makro olarak tanimliyor; XAML'in
// Microsoft.UI.Xaml.Media.Animation'inda ayni isimde bir metot var.
// Makro once geliyor ve uretilmis header'da C4002 uyarisi cikariyor.
// Ana projede TreatWarningAsError acik oldugu icin bu bir hata olur.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
// IVector<T>::Append gibi koleksiyon metotlarinin TAM tanimi burada.
// Sadece Windows.Foundation.h yeterli degil — "auto donduren islev
// tanimlanmadan kullanilamaz" (C3779) hatasi bundan cikiyor.
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <MddBootstrap.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Hosting;
using namespace winrt::Microsoft::UI::Dispatching;

// -1 = henuz sonuc yok, 0 = basarili, digerleri = HRESULT
static std::atomic<int> g_result{ -1 };
static std::atomic<bool> g_windowUp{ false };

static LRESULT CALLBACK HostWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void GuiThreadMain()
{
    try
    {
        // ── 1. STA apartment ──
        // XAML bunu sart kosuyor. Ana thread MTA, bu thread STA — ayni
        // process'te iki farkli apartment. Sorunun cekirdegi bu.
        winrt::init_apartment(winrt::apartment_type::single_threaded);

        // ── 2. Bootstrapper ──
        // WindowsPackageType=None ile SDK'nin auto-initializer'i bunu modul
        // yuklenirken zaten yapiyor. Elle cagirmiyoruz — cift ilklendirme
        // yapmayalim.
        std::printf("  [1/5] Bootstrapper auto-init (WindowsPackageType=None)\n");

        // ── 3. XAML runtime + mesaj loop'u: Application::Start ──
        //
        // ILK DENEME BASARISIZ OLDU: DispatcherQueueController::CreateOnCurrentThread
        // + dogrudan "Application app{}" insaasi RPC_E_WRONG_THREAD (0x8001010E)
        // veriyor. WinUI 3'te Application dogrudan aktive edilemiyor.
        //
        // Dogru desen: Application::Start(callback). Bu cagri
        //   - XAML runtime'ini bu thread'de kuruyor
        //   - DispatcherQueue'yu kendisi olusturuyor
        //   - callback dondukten sonra mesaj loop'unu KENDISI isletiyor
        //   - uygulama kapanana kadar DONMUYOR
        //
        // Bizim icin sorun degil: bu thread'in tek isi panel. Ana thread
        // (render) kendi loop'unu isletmeye devam ediyor.
        //
        // Bedeli: GUI thread'e is gecirmek icin PostThreadMessage yerine
        // DispatcherQueue.TryEnqueue kullanmak gerekiyor — Application::Start'in
        // loop'u thread-only mesajlari bize dagitmayabilir.
        std::printf("  [2/5] Application::Start cagriliyor...\n");

        Application::Start([](ApplicationInitializationCallbackParams const&)
        {
            try
            {
                // Bu lambda XAML runtime hazir olduktan SONRA, dogru thread'de
                // cagriliyor. Butun XAML kurulumu buraya ait.
                auto app = Application::Current();
                app.DispatcherShutdownMode(DispatcherShutdownMode::OnExplicitShutdown);
                app.Resources(XamlControlsResources{});
                std::printf("  [3/5] XAML runtime OK\n");

                // ── Host Win32 penceresi ──
                WNDCLASSEXW wc{};
                wc.cbSize = sizeof(wc);
                wc.lpfnWndProc = HostWndProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"SpikeIslandHost";
                RegisterClassExW(&wc);

                HWND host = CreateWindowExW(0, L"SpikeIslandHost", L"Spike: XAML Island",
                    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 460, 260,
                    nullptr, nullptr, wc.hInstance, nullptr);

                if (!host)
                {
                    const HRESULT whr = HRESULT_FROM_WIN32(GetLastError());
                    std::printf("  Host pencere olusturulamadi: 0x%08X\n",
                        static_cast<unsigned>(whr));
                    g_result = static_cast<int>(whr);
                    return;
                }
                std::printf("  [4/5] Host HWND OK\n");

                // ── Island'i host'a bagla ──
                // static: Start'in loop'u boyunca yasamasi lazim, lambda
                // donunce yikilmasin.
                static DesktopWindowXamlSource source{};
                source.Initialize(winrt::Microsoft::UI::GetWindowIdFromWindow(host));
                source.SiteBridge().ResizePolicy(
                    winrt::Microsoft::UI::Content::ContentSizePolicy::ResizeContentToParentWindow);

                StackPanel panel{};
                panel.Padding(ThicknessHelper::FromUniformLength(24));
                panel.Spacing(8);

                TextBlock title{};
                title.Text(L"Island ikincil STA thread'de ayakta");
                title.FontSize(18.0);
                panel.Children().Append(title);

                TextBlock detail{};
                detail.Text(L"Ana thread MTA, bu thread STA. Tasarim gecerli.");
                detail.Opacity(0.7);
                panel.Children().Append(detail);

                source.Content(panel);
                source.SiteBridge().Show();

                ShowWindow(host, SW_SHOW);
                std::printf("  [5/5] DesktopWindowXamlSource OK\n");

                g_result = 0;
                g_windowUp = true;
            }
            catch (winrt::hresult_error const& e)
            {
                std::printf("  Callback ici istisna: 0x%08X — %ls\n",
                    static_cast<unsigned>(e.code().value), e.message().c_str());
                g_result = static_cast<int>(e.code().value);
            }
        });

        // Application::Start burada donuyor — uygulama kapandi.
        MddBootstrapShutdown();
    }
    catch (winrt::hresult_error const& e)
    {
        std::printf("  Istisna: 0x%08X — %ls\n",
            static_cast<unsigned>(e.code().value), e.message().c_str());
        g_result = static_cast<int>(e.code().value);
    }
    catch (...)
    {
        std::printf("  Bilinmeyen istisna\n");
        g_result = 1;
    }
}

// =============================================================================
// Varyantlar — hangi degisken cokmeye sebep oluyor?
// =============================================================================
// mta      : ana thread MTA + ayri STA thread (BetterMagnifier'in mevcut hali)
// none     : ana thread hic CoInitializeEx cagirmaz + ayri STA thread
// mainsta  : Application::Start ANA thread'de (kontrol deneyi — calismasi beklenir)
//
// "none" calisip "mta" cokerse: sucu process'te MTA bulunmasi, main.cpp'yi
// STA'ya cevirmek cozer.
// "none" da cokerse: sucu ikincil thread, tasarim gecersiz, IPC'ye gecmek lazim.
// =============================================================================
int main(int argc, char** argv)
{
    // Buffer'siz stdout — cokme durumunda son satiri kaybetmemek icin.
    // Access violation'da buffer flush edilmiyor, ilerleme izini yitiriyoruz.
    setvbuf(stdout, nullptr, _IONBF, 0);

    const char* variant = (argc > 1) ? argv[1] : "mta";
    std::printf("=== Varyant: %s ===\n", variant);

    if (std::string_view(variant) == "mainsta")
    {
        // Kontrol deneyi: Application::Start ana thread'de, ikincil thread yok.
        std::printf("Ana thread: STA, Application::Start burada calisacak\n");
        GuiThreadMain();
        const int r = g_result.load();
        std::printf("\n%s (HRESULT 0x%08X)\n",
            (r == 0 ? "KONTROL DENEYI BASARILI" : "KONTROL DENEYI BASARISIZ"),
            static_cast<unsigned>(r));
        return (r == 0) ? 0 : 1;
    }

    if (std::string_view(variant) == "mta")
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr))
        {
            std::printf("Ana thread MTA init basarisiz: 0x%08X\n", static_cast<unsigned>(hr));
            return 1;
        }
        std::printf("Ana thread: MTA (COINIT_MULTITHREADED)\n");
    }
    else
    {
        std::printf("Ana thread: apartment init YOK\n");
    }

    std::printf("GUI thread baslatiliyor (STA)...\n");

    std::thread gui(GuiThreadMain);

    // Sonucu bekle (en fazla 20 saniye)
    for (int i = 0; i < 200 && g_result.load() == -1; ++i)
        Sleep(100);

    const int r = g_result.load();

    std::printf("\n");
    if (r == 0)
    {
        std::printf("SPIKE BASARILI: island ikincil STA thread'de ayakta, ana thread MTA\n");
        std::printf("Pencereyi kapatinca cikilacak.\n");
    }
    else if (r == -1)
    {
        std::printf("SPIKE ZAMAN ASIMI: 20 saniyede sonuc gelmedi\n");
    }
    else
    {
        std::printf("SPIKE BASARISIZ: HRESULT 0x%08X\n", static_cast<unsigned>(r));
    }

    if (r == 0 && g_windowUp.load())
    {
        // Pencere acik; kullanici kapatinca GUI thread cikar.
        if (gui.joinable())
            gui.join();
    }
    else if (gui.joinable())
    {
        gui.detach();
    }

    if (std::string_view(variant) == "mta")
        CoUninitialize();

    return (r == 0) ? 0 : 1;
}
