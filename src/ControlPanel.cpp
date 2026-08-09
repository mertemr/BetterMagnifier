// WinUI 3 control panel. See ControlPanel.h for why it owns a thread, and
// spike/xaml-island-thread/FINDINGS.md for why the startup sequence looks the
// way it does - three of the lines below are there because the obvious version
// crashes or fails.

#include "pch.h"
#include "ControlPanel.h"
#include "AppMessages.h"
#include "SettingsStore.h"
#include "StatusSnapshot.h"
#include "Logger.h"

// windows.h defines GetCurrentTime as a macro and
// Microsoft.UI.Xaml.Media.Animation has a method with that name. The macro wins
// and the generated header warns C4002, which is an error in this project.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
// IVector<T>::Append and friends are fully defined here, not in
// Windows.Foundation.h. Missing it produces C3779, "a function that returns
// auto cannot be used before it is defined".
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
// ButtonBase::Click and RangeBase::ValueChanged live in Primitives; the same
// C3779 trap as Collections.h above.
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include <dwmapi.h>
#include <future>

namespace WUX  = winrt::Microsoft::UI::Xaml;
namespace WUXC = winrt::Microsoft::UI::Xaml::Controls;
namespace WUXP = winrt::Microsoft::UI::Xaml::Controls::Primitives;

namespace BetterMagnifier {

namespace {

constexpr wchar_t kHostClassName[] = L"BetterMagnifierPanelHost";
constexpr int kPanelWidth  = 560;
constexpr int kPanelHeight = 680;
constexpr int kPanelMinW   = 440;
constexpr int kPanelMinH   = 480;

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CLOSE:
        // Hide, do not destroy. Reopening then costs nothing and the XAML tree
        // does not have to be rebuilt.
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_GETMINMAXINFO:
    {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = kPanelMinW;
        mmi->ptMinTrackSize.y = kPanelMinH;
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void ApplyDarkTitleBar(HWND hwnd)
{
    BOOL dark = TRUE;
    const HRESULT hr = DwmSetWindowAttribute(
        hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    if (FAILED(hr))
        LOG_DEBUG("Dark title bar unavailable: 0x{:08X}", static_cast<unsigned long>(hr));
}

WUX::Media::SolidColorBrush Brush(uint8_t r, uint8_t g, uint8_t b)
{
    return WUX::Media::SolidColorBrush{ winrt::Windows::UI::Color{ 255, r, g, b } };
}

WUXC::TextBlock MakeHint(std::wstring_view text)
{
    WUXC::TextBlock t{};
    t.Text(winrt::hstring{ text });
    t.FontSize(12.0);
    t.Opacity(0.65);
    t.TextWrapping(WUX::TextWrapping::Wrap);
    return t;
}

WUXC::TextBlock MakeHeader(std::wstring_view text)
{
    WUXC::TextBlock h{};
    h.Text(winrt::hstring{ text });
    h.FontSize(15.0);
    // Aggregate init rather than FontWeights::SemiBold(): the statics moved
    // namespace between WinUI 2 and 3, the struct did not.
    h.FontWeight(winrt::Windows::UI::Text::FontWeight{ 600 });
    h.Margin(WUX::ThicknessHelper::FromLengths(0, 14, 0, 0));
    return h;
}

// ToggleButton rather than CheckBox: RadioButton derives from the same base and
// IsChecked is nullable on both.
bool IsCheckedTrue(WUXP::ToggleButton const& button)
{
    const auto v = button.IsChecked();
    return v && v.Value();
}

} // anonymous namespace

// =============================================================================
// Impl - the XAML objects
// =============================================================================
// Kept out of the header so that including ControlPanel.h does not drag the
// WinUI projection into every translation unit that touches App.h.
// =============================================================================
struct MonitorCard
{
    WUXC::TextBlock    title{ nullptr };
    WUXC::TextBlock    details{ nullptr };
    WUXC::ToggleSwitch activeToggle{ nullptr };
    WUXC::Slider       zoomSlider{ nullptr };
    WUXC::TextBlock    zoomLabel{ nullptr };
    WUXC::TextBlock    fpsLabel{ nullptr };
    WUXC::TextBlock    captureLabel{ nullptr };
    WUXP::ToggleButton freezeButton{ nullptr };

    // Set while the timer writes the engine's current state into the controls.
    // Without it: timer sets the slider -> ValueChanged fires -> a zoom message
    // goes back to the engine -> loop.
    bool suppressEvents = false;
};

struct ControlPanel::Impl
{
    HWND                                 host{ nullptr };
    WUX::Hosting::DesktopWindowXamlSource source{ nullptr };
    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue{ nullptr };

    WUXC::StackPanel  statusPanel{ nullptr };
    WUXC::StackPanel  settingsPanel{ nullptr };
    WUX::DispatcherTimer liveTimer{ nullptr };

    std::vector<MonitorCard> cards;

    // Settings tab
    WUXC::TextBlock   hotkeyWarning{ nullptr };
    WUXC::CheckBox    hijackBox{ nullptr };
    WUXC::RadioButton followMouseRadio{ nullptr };
    WUXC::RadioButton followFocusRadio{ nullptr };
    // Sliders, not NumberBox: a NumberBox embeds a TextBox internally and kills
    // the process the same way a bare TextBox does. See docs/PANEL-BLANK.md.
    WUXC::Slider      minZoomSlider{ nullptr };
    WUXC::Slider      maxZoomSlider{ nullptr };
    WUXC::Slider      zoomStepSlider{ nullptr };
    WUXC::TextBlock   minZoomLabel{ nullptr };
    WUXC::TextBlock   maxZoomLabel{ nullptr };
    WUXC::TextBlock   zoomStepLabel{ nullptr };
    WUXC::CheckBox    startWithWindowsBox{ nullptr };
    WUXC::CheckBox    rememberZoomBox{ nullptr };

    bool suppressSettingsEvents = false;
};

ControlPanel::ControlPanel()
    : m_impl(std::make_unique<Impl>())
{
}

ControlPanel::~ControlPanel()
{
    Stop();
}

// =============================================================================
// Show
// =============================================================================
void ControlPanel::Show(HWND engineHwnd, SettingsStore* settings, StatusSnapshot* status)
{
    if (m_running.load(std::memory_order_acquire))
    {
        // Already up: hand the work to the GUI thread. PostThreadMessage is not
        // an option, because Application::Start runs the message loop itself and
        // does not forward thread-only messages to us.
        auto queue = m_impl->queue;
        if (queue)
        {
            queue.TryEnqueue([this]()
            {
                if (m_impl->host)
                {
                    ShowWindow(m_impl->host, SW_SHOW);
                    SetForegroundWindow(m_impl->host);
                }
            });
        }
        return;
    }

    if (m_startAttempted.load(std::memory_order_acquire))
    {
        // Either still coming up, or it failed. Application::Start is a
        // once-per-process call, so a failed panel cannot be retried.
        LOG_DEBUG("Control panel start already attempted, not starting again");
        return;
    }
    m_startAttempted.store(true, std::memory_order_release);

    m_engineHwnd = engineHwnd;
    m_settings   = settings;
    m_status     = status;

    m_exitedFuture = m_exited.get_future();
    m_thread = std::thread([this]() { ThreadMain(); });
}

// =============================================================================
// ThreadMain - STA apartment, XAML runtime, then Application::Start's own loop
// =============================================================================
void ControlPanel::ThreadMain()
{
    m_threadId.store(GetCurrentThreadId(), std::memory_order_release);

    bool cameUp = false;

    try
    {
        // XAML requires STA. The main thread stays MTA: two apartments in one
        // process, which is exactly what the spike had to prove was allowed.
        winrt::init_apartment(winrt::apartment_type::single_threaded);

        // The bootstrapper is NOT called here. WindowsPackageType=None puts the
        // SDK's auto-initializer in the binary, and it runs at module load in
        // the right order; calling MddBootstrapInitialize as well resolves the
        // framework package but leaves WinUI half-initialised.

        // The DispatcherQueue has to exist before the XAML runtime, and it binds
        // itself to this thread's message loop - which is why TryEnqueue works
        // below without any extra plumbing.
        auto controller =
            winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnCurrentThread();

        // THIS is what brings XAML up on this thread, and its absence is why the
        // island used to render nothing: Application::Start ran a message loop
        // but never gave this thread a XAML runtime, so Content() was accepted
        // and then simply never laid out or composited.
        //
        // XamlControlsResources is still deliberately not assigned: the default
        // theme dictionary is already loaded and assigning that object replaces
        // it, wiping the brushes it references itself.
        auto xamlManager = WUX::Hosting::WindowsXamlManager::InitializeForCurrentThread();

        BuildUi();

        // Our own loop, so the panel is reachable with PostThreadMessage again
        // and there is no once-per-process restriction to work around.
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            // No PreTranslateMessage call: WinUI 3 does not expose one, the
            // island routes its own keyboard input through the content site.
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        cameUp = true;
        LOG_INFO("Control panel closed");

        if (m_impl->liveTimer)
            m_impl->liveTimer.Stop();
        if (m_impl->source)
            m_impl->source.Close();
        if (m_impl->host)
        {
            DestroyWindow(m_impl->host);
            m_impl->host = nullptr;
        }
        xamlManager.Close();
        controller.ShutdownQueueAsync();
    }
    catch (winrt::hresult_error const& e)
    {
        LOG_ERROR("Control panel thread failed: 0x{:08X} - {}",
            static_cast<unsigned long>(e.code().value),
            ToUtf8(e.message()));
    }
    catch (...)
    {
        LOG_ERROR("Control panel thread failed with an unknown exception");
    }

    const bool everRan = m_running.exchange(false, std::memory_order_acq_rel);

    // Never reached the UI at all: almost always a missing Windows App Runtime.
    // The core keeps working, so this is a warning, not a fatal error.
    if (!everRan && !cameUp)
    {
        MessageBoxW(nullptr,
            L"The settings panel could not open.\n\n"
            L"It needs the Windows App Runtime. Magnification, hotkeys and the "
            L"tray menu keep working.\n\nDetails are in the logs folder.",
            L"BetterMagnifier", MB_ICONWARNING | MB_OK);
    }

    m_exited.set_value();
}

// =============================================================================
// BuildUi - host window, island, tabs. GUI thread only.
// =============================================================================
void ControlPanel::BuildUi()
{
    m_impl->queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = HostWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kHostClassName;
    RegisterClassExW(&wc);

    // No WS_MAXIMIZEBOX: a settings window has no business going fullscreen.
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;

    HWND host = CreateWindowExW(
        0, kHostClassName, L"BetterMagnifier",
        style, CW_USEDEFAULT, CW_USEDEFAULT, kPanelWidth, kPanelHeight,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!host)
    {
        LOG_ERROR("Panel host window could not be created: {}", GetLastError());
        return;
    }

    // The process is per-monitor DPI aware, so the size above is physical
    // pixels. On a scaled display that would come out tiny.
    const UINT dpi = GetDpiForWindow(host);
    if (dpi != 0 && dpi != 96)
    {
        SetWindowPos(host, nullptr, 0, 0,
            MulDiv(kPanelWidth, static_cast<int>(dpi), 96),
            MulDiv(kPanelHeight, static_cast<int>(dpi), 96),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ApplyDarkTitleBar(host);

    m_impl->host = host;

    m_impl->source = WUX::Hosting::DesktopWindowXamlSource{};
    m_impl->source.Initialize(winrt::Microsoft::UI::GetWindowIdFromWindow(host));
    m_impl->source.SiteBridge().ResizePolicy(
        winrt::Microsoft::UI::Content::ContentSizePolicy::ResizeContentToParentWindow);

    // One scrolling page, not a TabView. Two tabs were the original plan, but a
    // TabView is a heavy templated control and the panel showed nothing while it
    // was in the tree - the XAML dispatcher stopped ticking as soon as it was
    // there, while the same island rendered fine with plain panels. A single
    // page carries the same information for a window this small.
    WUXC::ScrollViewer scroller{};
    WUXC::StackPanel page{};
    page.Padding(WUX::ThicknessHelper::FromUniformLength(16));
    page.Spacing(12);

    m_impl->statusPanel = WUXC::StackPanel{};
    m_impl->statusPanel.Spacing(12);

    m_impl->settingsPanel = WUXC::StackPanel{};
    m_impl->settingsPanel.Spacing(8);

    page.Children().Append(MakeHeader(L"Monitors"));
    page.Children().Append(m_impl->statusPanel);
    page.Children().Append(m_impl->settingsPanel);

    scroller.Content(page);

    WUXC::Grid root{};

    // The theme is pinned to dark to match the title bar above. Left on the
    // default, light text landed on the host window's white background and the
    // panel looked empty.
    root.RequestedTheme(WUX::ElementTheme::Dark);
    root.Background(Brush(32, 32, 32));
    root.Children().Append(scroller);

    // Loaded proves the tree is attached to a live island; SizeChanged proves it
    // got a non-zero layout slot. Blank window with neither firing means the
    // island is not connected at all.
    root.Loaded([](winrt::Windows::Foundation::IInspectable const&,
                   WUX::RoutedEventArgs const&)
    {
        LOG_INFO("Panel root Loaded");
    });
    root.SizeChanged([](winrt::Windows::Foundation::IInspectable const&,
                        WUX::SizeChangedEventArgs const& e)
    {
        LOG_INFO("Panel root SizeChanged: {}x{}",
            static_cast<int>(e.NewSize().Width), static_cast<int>(e.NewSize().Height));
    });

    m_impl->source.Content(root);

    m_impl->source.SiteBridge().Show();

    RebuildMonitorCards();
    BuildSettingsTab();
    StartLiveTimer();

    ShowWindow(host, SW_SHOW);
    SetForegroundWindow(host);

    // XamlRoot is the decisive check: an element attached to a live island has
    // one, with a size. Null means Content() was accepted but nothing hosts it.
    if (auto xr = root.XamlRoot())
    {
        LOG_INFO("Panel XamlRoot present, size {}x{}",
            static_cast<int>(xr.Size().Width), static_cast<int>(xr.Size().Height));
    }
    else
    {
        LOG_ERROR("Panel XamlRoot is NULL - the island is not hosting the tree");
    }

    m_running.store(true, std::memory_order_release);
    LOG_INFO("Control panel opened");
}

// =============================================================================
// Status tab
// =============================================================================
void ControlPanel::RebuildMonitorCards()
{
    if (!m_impl->statusPanel || !m_status || m_stopping.load(std::memory_order_acquire))
        return;

    m_impl->statusPanel.Children().Clear();
    m_impl->cards.clear();

    const size_t count = m_status->monitorCount.load(std::memory_order_acquire);

    if (count == 0)
    {
        WUXC::TextBlock empty{};
        empty.Text(L"No monitors found.");
        m_impl->statusPanel.Children().Append(empty);
        return;
    }

    m_impl->cards.reserve(count);

    for (size_t i = 0; i < count && i < StatusSnapshot::kMaxMonitors; ++i)
    {
        const auto& st = m_status->Monitor(i);

        // Slider bounds come from that monitor's settings.
        const std::wstring device(st.deviceName);
        const MonitorSettings ms = m_settings ? m_settings->Monitor(device) : MonitorSettings{};

        MonitorCard card;

        WUXC::Border border{};
        border.CornerRadius(WUX::CornerRadiusHelper::FromUniformRadius(8));
        border.Padding(WUX::ThicknessHelper::FromUniformLength(12));
        border.Background(Brush(45, 45, 48));

        WUXC::StackPanel body{};
        body.Spacing(6);

        card.title = WUXC::TextBlock{};
        card.title.FontWeight(winrt::Windows::UI::Text::FontWeight{ 600 });
        body.Children().Append(card.title);

        card.details = WUXC::TextBlock{};
        card.details.FontSize(12.0);
        card.details.Opacity(0.65);
        body.Children().Append(card.details);

        WUXC::Grid controls{};
        {
            WUXC::ColumnDefinition c0{}; c0.Width(WUX::GridLengthHelper::Auto());
            WUXC::ColumnDefinition c1{};
            c1.Width(WUX::GridLengthHelper::FromValueAndType(1.0, WUX::GridUnitType::Star));
            WUXC::ColumnDefinition c2{}; c2.Width(WUX::GridLengthHelper::Auto());
            controls.ColumnDefinitions().Append(c0);
            controls.ColumnDefinitions().Append(c1);
            controls.ColumnDefinitions().Append(c2);
            controls.ColumnSpacing(8);
        }

        card.activeToggle = WUXC::ToggleSwitch{};
        card.activeToggle.OnContent(winrt::box_value(L"On"));
        card.activeToggle.OffContent(winrt::box_value(L"Off"));
        WUXC::Grid::SetColumn(card.activeToggle, 0);
        controls.Children().Append(card.activeToggle);

        card.zoomSlider = WUXC::Slider{};
        card.zoomSlider.Minimum(ms.minZoom);
        card.zoomSlider.Maximum(ms.maxZoom);
        card.zoomSlider.StepFrequency(ms.zoomStep);
        card.zoomSlider.Value(ms.minZoom);
        WUXC::Grid::SetColumn(card.zoomSlider, 1);
        controls.Children().Append(card.zoomSlider);

        card.zoomLabel = WUXC::TextBlock{};
        card.zoomLabel.MinWidth(56.0);
        card.zoomLabel.VerticalAlignment(WUX::VerticalAlignment::Center);
        WUXC::Grid::SetColumn(card.zoomLabel, 2);
        controls.Children().Append(card.zoomLabel);

        body.Children().Append(controls);

        WUXC::Grid footer{};
        {
            WUXC::ColumnDefinition f0{};
            f0.Width(WUX::GridLengthHelper::FromValueAndType(1.0, WUX::GridUnitType::Star));
            WUXC::ColumnDefinition f1{}; f1.Width(WUX::GridLengthHelper::Auto());
            WUXC::ColumnDefinition f2{}; f2.Width(WUX::GridLengthHelper::Auto());
            footer.ColumnDefinitions().Append(f0);
            footer.ColumnDefinitions().Append(f1);
            footer.ColumnDefinitions().Append(f2);
            footer.ColumnSpacing(8);
        }

        card.captureLabel = WUXC::TextBlock{};
        card.captureLabel.FontSize(12.0);
        card.captureLabel.Opacity(0.65);
        card.captureLabel.VerticalAlignment(WUX::VerticalAlignment::Center);
        WUXC::Grid::SetColumn(card.captureLabel, 0);
        footer.Children().Append(card.captureLabel);

        card.fpsLabel = WUXC::TextBlock{};
        card.fpsLabel.FontSize(12.0);
        card.fpsLabel.Opacity(0.65);
        card.fpsLabel.VerticalAlignment(WUX::VerticalAlignment::Center);
        WUXC::Grid::SetColumn(card.fpsLabel, 1);
        footer.Children().Append(card.fpsLabel);

        card.freezeButton = WUXP::ToggleButton{};
        card.freezeButton.Content(winrt::box_value(L"Freeze"));
        WUXC::Grid::SetColumn(card.freezeButton, 2);
        footer.Children().Append(card.freezeButton);

        body.Children().Append(footer);
        border.Child(body);
        m_impl->statusPanel.Children().Append(border);

        // Events capture the INDEX, never a pointer into the vector: the vector
        // is rebuilt on display changes and pointers would dangle.
        const HWND   engine = m_engineHwnd;
        const size_t index  = i;

        card.activeToggle.Toggled(
            [this, engine, index](winrt::Windows::Foundation::IInspectable const&,
                                  WUX::RoutedEventArgs const&)
            {
                if (index >= m_impl->cards.size() || m_impl->cards[index].suppressEvents)
                    return;
                PostMessageW(engine, WM_APP_TOGGLE_ZOOM, static_cast<WPARAM>(index), 0);
            });

        card.freezeButton.Click(
            [this, engine, index](winrt::Windows::Foundation::IInspectable const&,
                                  WUX::RoutedEventArgs const&)
            {
                if (index >= m_impl->cards.size() || m_impl->cards[index].suppressEvents)
                    return;
                PostMessageW(engine, WM_APP_TOGGLE_FREEZE, static_cast<WPARAM>(index), 0);
            });

        card.zoomSlider.ValueChanged(
            [this, engine, index](winrt::Windows::Foundation::IInspectable const&,
                                  WUXP::RangeBaseValueChangedEventArgs const& args)
            {
                if (index >= m_impl->cards.size() || m_impl->cards[index].suppressEvents)
                    return;

                const int scaled = static_cast<int>(args.NewValue() * 1000.0);
                PostMessageW(engine, WM_APP_SET_ZOOM,
                             static_cast<WPARAM>(index), static_cast<LPARAM>(scaled));
            });

        m_impl->cards.push_back(card);
    }

    // Fill the labels now instead of waiting for the first timer tick.
    UpdateLiveValues();
}

// =============================================================================
// StartLiveTimer - 10 Hz
// =============================================================================
// Nobody studies a 60 Hz readout in a settings window. At 10 Hz the contention
// on the snapshot is nil and the eye cannot tell.
// =============================================================================
void ControlPanel::StartLiveTimer()
{
    m_impl->liveTimer = WUX::DispatcherTimer{};
    m_impl->liveTimer.Interval(std::chrono::milliseconds(100));
    m_impl->liveTimer.Tick(
        [this](winrt::Windows::Foundation::IInspectable const&,
               winrt::Windows::Foundation::IInspectable const&)
        {
            // One-shot: proves the XAML dispatcher on this thread is pumping.
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                LOG_INFO("Panel dispatcher is ticking");

            UpdateLiveValues();
        });
    m_impl->liveTimer.Start();
}

void ControlPanel::UpdateLiveValues()
{
    if (!m_status || m_stopping.load(std::memory_order_acquire))
        return;

    // Hidden window: nothing to update, and the engine keeps running.
    if (m_impl->host && !IsWindowVisible(m_impl->host))
        return;

    const size_t count = m_status->monitorCount.load(std::memory_order_acquire);
    const size_t n = (count < m_impl->cards.size()) ? count : m_impl->cards.size();

    for (size_t i = 0; i < n; ++i)
    {
        const auto& st = m_status->Monitor(i);
        auto& card = m_impl->cards[i];

        std::wstring title(st.deviceName);
        if (st.isPrimary.load(std::memory_order_relaxed))
            title += L"   PRIMARY";
        card.title.Text(winrt::hstring{ title });

        card.details.Text(winrt::hstring{ std::format(L"{}x{} - {} Hz - {}%",
            st.width.load(std::memory_order_relaxed),
            st.height.load(std::memory_order_relaxed),
            st.refreshRate.load(std::memory_order_relaxed),
            st.dpiPercent.load(std::memory_order_relaxed)) });

        const bool  active = st.isActive.load(std::memory_order_relaxed);
        const bool  frozen = st.isFrozen.load(std::memory_order_relaxed);
        const float zoom   = st.zoomLevel.load(std::memory_order_relaxed);

        card.zoomLabel.Text(winrt::hstring{ std::format(L"{:.2f}x", zoom) });

        // Everything below writes the engine's state into a control, which
        // raises the control's own event. suppressEvents stops that from being
        // posted straight back as a user action.
        card.suppressEvents = true;

        if (card.activeToggle.IsOn() != active)
            card.activeToggle.IsOn(active);

        // Do not fight the user while they drag: only correct a real difference.
        if (std::abs(card.zoomSlider.Value() - static_cast<double>(zoom)) > 0.005)
            card.zoomSlider.Value(static_cast<double>(zoom));

        const auto checked = card.freezeButton.IsChecked();
        if (!checked || checked.Value() != frozen)
            card.freezeButton.IsChecked(winrt::Windows::Foundation::IReference<bool>{ frozen });

        card.suppressEvents = false;

        const float fps = st.fps.load(std::memory_order_relaxed);
        card.fpsLabel.Text(fps > 0.0f
            ? winrt::hstring{ std::format(L"{:.0f} FPS", fps) }
            : winrt::hstring{ L"- FPS" });

        if (!st.captureOk.load(std::memory_order_relaxed))
            card.captureLabel.Text(L"Capture: reconnecting");
        else if (!st.captureExcluded.load(std::memory_order_relaxed))
            card.captureLabel.Text(L"Capture: feedback risk");
        else
            card.captureLabel.Text(L"Capture: OK");
    }

    // A hotkey that RegisterHotKey rejected has to be visible here. Logging it
    // only leaves the user pressing a key and wondering why nothing happens.
    if (m_impl->hotkeyWarning && m_settings)
    {
        const unsigned mask = m_status->hotkeyFailedMask.load(std::memory_order_acquire);

        if (mask != 0)
        {
            const auto& g = m_settings->General();
            std::wstring msg;

            if (mask & 0x1u)
                msg += L"\"" + FormatHotkey(g.toggleModifiers, g.toggleVk)
                     + L"\" could not be registered (Windows or another app holds it). ";
            if (mask & 0x2u)
                msg += L"\"" + FormatHotkey(g.freezeModifiers, g.freezeVk)
                     + L"\" could not be registered. ";

            m_impl->hotkeyWarning.Text(winrt::hstring{ msg });
            m_impl->hotkeyWarning.Visibility(WUX::Visibility::Visible);
        }
        else
        {
            m_impl->hotkeyWarning.Visibility(WUX::Visibility::Collapsed);
        }
    }
}

// =============================================================================
// Settings tab
// =============================================================================
// Hotkeys are typed as text and validated by ParseHotkey. Capturing real key
// presses would be nicer, but mapping XAML KeyRoutedEventArgs onto Win32 hotkey
// semantics (extended keys, left/right modifiers) is a job of its own, and
// ParseHotkey is the one piece of this codebase that already has tests.
// =============================================================================
void ControlPanel::BuildSettingsTab()
{
    if (!m_impl->settingsPanel || !m_settings || m_stopping.load(std::memory_order_acquire))
        return;

    auto panel = m_impl->settingsPanel;

    m_impl->suppressSettingsEvents = true;
    panel.Children().Clear();

    const auto& g = m_settings->General();

    // ── Hotkeys ──
    panel.Children().Append(MakeHeader(L"Hotkeys"));

    // Read-only on purpose. A XAML TextBox kills this process: put one in the
    // island and the first layout pass ends in a stowed exception (0xC000027B),
    // bisected down to exactly this control while every other control here is
    // fine. Text input services do not come up for an island on a secondary STA
    // thread in an unpackaged, elevated process.
    //
    // Hotkeys are edited in settings.ini and picked up with Reload below. Key
    // capture through the low-level hook we already own would be nicer, and is
    // the upgrade path if typing a hotkey in the file gets tiring.
    {
        WUXC::TextBlock bindings{};
        bindings.Text(winrt::hstring{
            L"Toggle zoom:  " + FormatHotkey(g.toggleModifiers, g.toggleVk) +
            L"\nFreeze:       " + FormatHotkey(g.freezeModifiers, g.freezeVk) });
        bindings.FontFamily(WUX::Media::FontFamily{ L"Consolas" });
        bindings.IsTextSelectionEnabled(true);
        panel.Children().Append(bindings);
    }

    panel.Children().Append(MakeHint(
        L"Edit these in settings.ini and press Reload below. "
        L"Format: Ctrl+Alt+Z. Modifiers: Ctrl, Alt, Shift, Win. Keys: A-Z, 0-9, F1-F24."));

    m_impl->hotkeyWarning = WUXC::TextBlock{};
    m_impl->hotkeyWarning.FontSize(12.0);
    m_impl->hotkeyWarning.TextWrapping(WUX::TextWrapping::Wrap);
    m_impl->hotkeyWarning.Foreground(Brush(255, 99, 99));
    m_impl->hotkeyWarning.Visibility(WUX::Visibility::Collapsed);
    panel.Children().Append(m_impl->hotkeyWarning);

    m_impl->hijackBox = WUXC::CheckBox{};
    m_impl->hijackBox.Content(winrt::box_value(L"Take over the Windows Magnifier shortcuts"));
    m_impl->hijackBox.IsChecked(
        winrt::Windows::Foundation::IReference<bool>{ g.hijackMagnifierKeys });
    panel.Children().Append(m_impl->hijackBox);

    panel.Children().Append(MakeHint(
        L"Win+Plus / Win+Minus step zoom, Ctrl+Alt+wheel steps and is swallowed so the "
        L"page does not scroll, Win+middle-click freezes. Win+Plus may still reach the "
        L"Windows Magnifier as well, because that process runs with UIAccess and a hook "
        L"cannot swallow its shortcut."));

    // ── Follow mode ──
    panel.Children().Append(MakeHeader(L"Follow mode"));

    m_impl->followMouseRadio = WUXC::RadioButton{};
    m_impl->followMouseRadio.Content(winrt::box_value(L"Mouse only"));
    m_impl->followMouseRadio.GroupName(L"FollowMode");
    m_impl->followMouseRadio.IsChecked(
        winrt::Windows::Foundation::IReference<bool>{ g.followMode == FollowMode::Mouse });
    panel.Children().Append(m_impl->followMouseRadio);

    m_impl->followFocusRadio = WUXC::RadioButton{};
    m_impl->followFocusRadio.Content(winrt::box_value(L"Mouse and keyboard focus"));
    m_impl->followFocusRadio.GroupName(L"FollowMode");
    m_impl->followFocusRadio.IsChecked(
        winrt::Windows::Foundation::IReference<bool>{ g.followMode == FollowMode::MouseAndFocus });
    panel.Children().Append(m_impl->followFocusRadio);

    panel.Children().Append(MakeHint(
        L"Following focus moves the zoom region on Tab, but it detaches the anchor from "
        L"the cursor, and then clicks no longer land where they appear."));

    // ── Zoom limits ──
    panel.Children().Append(MakeHeader(L"Zoom limits"));

    // Slider, not NumberBox: a NumberBox embeds a TextBox internally, and that
    // kills this process on first layout the same way a bare TextBox does (see
    // the Hotkeys section above and docs/PANEL-BLANK.md). Sliders are plain
    // controls with no text-input service dependency and are already proven
    // safe by the per-monitor zoom slider in the Status section.
    auto makeLimitRow = [&panel](std::wstring_view label, double value, double lo, double hi,
                                 double step, WUXC::Slider& sliderOut, WUXC::TextBlock& labelOut)
    {
        WUXC::Grid row{};
        WUXC::ColumnDefinition c0{};
        c0.Width(WUX::GridLengthHelper::FromValueAndType(64.0, WUX::GridUnitType::Pixel));
        WUXC::ColumnDefinition c1{};
        c1.Width(WUX::GridLengthHelper::FromValueAndType(1.0, WUX::GridUnitType::Star));
        WUXC::ColumnDefinition c2{}; c2.Width(WUX::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(c0);
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);
        row.ColumnSpacing(8);

        WUXC::TextBlock name{};
        name.Text(winrt::hstring{ label });
        name.VerticalAlignment(WUX::VerticalAlignment::Center);
        WUXC::Grid::SetColumn(name, 0);
        row.Children().Append(name);

        sliderOut = WUXC::Slider{};
        sliderOut.Minimum(lo);
        sliderOut.Maximum(hi);
        sliderOut.StepFrequency(step);
        sliderOut.Value(value);
        WUXC::Grid::SetColumn(sliderOut, 1);
        row.Children().Append(sliderOut);

        labelOut = WUXC::TextBlock{};
        labelOut.MinWidth(48.0);
        labelOut.VerticalAlignment(WUX::VerticalAlignment::Center);
        labelOut.Text(winrt::hstring{ std::format(L"{:.2f}", value) });
        WUXC::Grid::SetColumn(labelOut, 2);
        row.Children().Append(labelOut);

        panel.Children().Append(row);
    };

    // The first monitor's values stand in for all of them; writing applies to
    // every monitor (see PushSettings). Per-monitor limits would need a picker
    // here, and nobody has asked for that.
    MonitorSettings representative{};
    if (m_status)
    {
        const std::wstring device(m_status->Monitor(0).deviceName);
        if (!device.empty())
            representative = m_settings->Monitor(device);
    }

    makeLimitRow(L"Minimum", representative.minZoom, 1.0, 20.0, 0.25,
                 m_impl->minZoomSlider, m_impl->minZoomLabel);
    makeLimitRow(L"Maximum", representative.maxZoom, 1.0, 20.0, 0.25,
                 m_impl->maxZoomSlider, m_impl->maxZoomLabel);
    makeLimitRow(L"Step", representative.zoomStep, 0.05, 2.0, 0.05,
                 m_impl->zoomStepSlider, m_impl->zoomStepLabel);

    panel.Children().Append(MakeHint(L"Applied to every monitor."));

    // ── Other ──
    panel.Children().Append(MakeHeader(L"Other"));

    m_impl->startWithWindowsBox = WUXC::CheckBox{};
    m_impl->startWithWindowsBox.Content(winrt::box_value(L"Start with Windows"));
    m_impl->startWithWindowsBox.IsChecked(
        winrt::Windows::Foundation::IReference<bool>{ g.startWithWindows });
    panel.Children().Append(m_impl->startWithWindowsBox);

    panel.Children().Append(MakeHint(
        L"Writes an HKCU Run entry. This app requires administrator rights, so Windows "
        L"may refuse to launch it from Run at logon; a Task Scheduler task with highest "
        L"privileges is the reliable route."));

    m_impl->rememberZoomBox = WUXC::CheckBox{};
    m_impl->rememberZoomBox.Content(winrt::box_value(L"Remember the zoom level"));
    m_impl->rememberZoomBox.IsChecked(
        winrt::Windows::Foundation::IReference<bool>{ g.rememberZoomLevel });
    panel.Children().Append(m_impl->rememberZoomBox);

    // ── Settings file ──
    panel.Children().Append(MakeHeader(L"Settings file"));

    WUXC::TextBlock pathText{};
    pathText.Text(winrt::hstring{ SettingsStore::FilePath().wstring() });
    pathText.FontSize(12.0);
    pathText.Opacity(0.65);
    pathText.TextWrapping(WUX::TextWrapping::Wrap);
    pathText.IsTextSelectionEnabled(true);
    panel.Children().Append(pathText);

    WUXC::Button reload{};
    reload.Content(winrt::box_value(L"Reload from disk"));
    reload.HorizontalAlignment(WUX::HorizontalAlignment::Left);
    reload.Click([this](winrt::Windows::Foundation::IInspectable const&,
                        WUX::RoutedEventArgs const&)
    {
        ReloadFromDisk();
    });
    panel.Children().Append(reload);

    panel.Children().Append(MakeHint(
        L"Everything on this tab is stored in that file and is applied without a restart. "
        L"Use Reload after editing it by hand."));

    // ── Wiring ──
    auto onChanged = [this](winrt::Windows::Foundation::IInspectable const&,
                            WUX::RoutedEventArgs const&)
    {
        if (!m_impl->suppressSettingsEvents)
            PushSettings();
    };

    m_impl->hijackBox.Checked(onChanged);
    m_impl->hijackBox.Unchecked(onChanged);
    m_impl->followMouseRadio.Checked(onChanged);
    m_impl->followFocusRadio.Checked(onChanged);
    m_impl->startWithWindowsBox.Checked(onChanged);
    m_impl->startWithWindowsBox.Unchecked(onChanged);
    m_impl->rememberZoomBox.Checked(onChanged);
    m_impl->rememberZoomBox.Unchecked(onChanged);

    auto makeLimitChanged = [this](WUXC::TextBlock label)
    {
        return [this, label](winrt::Windows::Foundation::IInspectable const&,
                             WUXP::RangeBaseValueChangedEventArgs const& args)
        {
            label.Text(winrt::hstring{ std::format(L"{:.2f}", args.NewValue()) });
            if (!m_impl->suppressSettingsEvents)
                PushSettings();
        };
    };
    m_impl->minZoomSlider.ValueChanged(makeLimitChanged(m_impl->minZoomLabel));
    m_impl->maxZoomSlider.ValueChanged(makeLimitChanged(m_impl->maxZoomLabel));
    m_impl->zoomStepSlider.ValueChanged(makeLimitChanged(m_impl->zoomStepLabel));

    m_impl->suppressSettingsEvents = false;
}

// =============================================================================
// PushSettings - controls -> store -> disk -> engine
// =============================================================================
void ControlPanel::PushSettings()
{
    if (!m_settings || m_stopping.load(std::memory_order_acquire))
        return;

    auto& g = m_settings->MutableGeneral();

    // Hotkeys are not edited here; they come from settings.ini untouched.
    g.hijackMagnifierKeys = IsCheckedTrue(m_impl->hijackBox);
    g.startWithWindows    = IsCheckedTrue(m_impl->startWithWindowsBox);
    g.rememberZoomLevel   = IsCheckedTrue(m_impl->rememberZoomBox);
    g.followMode          = IsCheckedTrue(m_impl->followMouseRadio)
                          ? FollowMode::Mouse : FollowMode::MouseAndFocus;

    {
        double lo   = m_impl->minZoomSlider.Value();
        double hi   = m_impl->maxZoomSlider.Value();
        double step = m_impl->zoomStepSlider.Value();

        if (hi <= lo)
            hi = lo + 1.0;

        const size_t count = m_status
            ? m_status->monitorCount.load(std::memory_order_acquire) : 0;

        for (size_t i = 0; i < count && i < StatusSnapshot::kMaxMonitors; ++i)
        {
            const std::wstring device(m_status->Monitor(i).deviceName);
            if (device.empty())
                continue;

            auto ms = m_settings->Monitor(device);
            ms.minZoom  = static_cast<float>(lo);
            ms.maxZoom  = static_cast<float>(hi);
            ms.zoomStep = static_cast<float>(step);
            ms.lastZoom = std::clamp(ms.lastZoom, ms.minZoom, ms.maxZoom);
            m_settings->SetMonitor(device, ms);
        }
    }

    m_settings->Save();

    if (m_engineHwnd)
        PostMessageW(m_engineHwnd, WM_APP_SETTINGS_CHANGED, 0, 0);

    // Slider bounds moved with the limits.
    RebuildMonitorCards();
}

void ControlPanel::ReloadFromDisk()
{
    if (!m_settings || m_stopping.load(std::memory_order_acquire))
        return;

    if (!m_settings->Load())
    {
        LOG_WARN("Settings could not be reloaded from disk");
        return;
    }

    LOG_INFO("Settings reloaded from disk");

    if (m_engineHwnd)
        PostMessageW(m_engineHwnd, WM_APP_SETTINGS_CHANGED, 0, 0);

    // Deferred: this runs from the Click handler of a button that
    // BuildSettingsTab is about to remove from the tree. Rebuilding on the next
    // dispatcher turn keeps that out of the handler's own stack frame.
    auto queue = m_impl->queue;
    if (queue)
    {
        queue.TryEnqueue([this]()
        {
            BuildSettingsTab();
            RebuildMonitorCards();
        });
    }
}

// =============================================================================
// NotifyDisplayChange
// =============================================================================
void ControlPanel::NotifyDisplayChange()
{
    if (!m_running.load(std::memory_order_acquire))
        return;

    auto queue = m_impl->queue;
    if (queue)
        queue.TryEnqueue([this]() { RebuildMonitorCards(); });
}

// =============================================================================
// Stop
// =============================================================================
void ControlPanel::Stop()
{
    if (!m_thread.joinable())
        return;

    m_stopping.store(true, std::memory_order_release);

    // The loop is ours now, so WM_QUIT ends it. Cleanup happens on that thread
    // right after the loop, where the XAML objects belong.
    const DWORD tid = m_threadId.load(std::memory_order_acquire);
    if (tid != 0)
        PostThreadMessageW(tid, WM_QUIT, 0, 0);

    // ponytail: waiting on the thread's own promise rather than joining flat. If
    // Exit ever fails to end the XAML loop, a plain join would hang shutdown -
    // the exact failure that already cost this project a trip to Task Manager.
    // The thread is abandoned instead; the process is on its way out either way.
    if (m_exitedFuture.valid()
        && m_exitedFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready)
    {
        m_thread.join();
    }
    else
    {
        LOG_WARN("Control panel thread did not exit in time, abandoning it");
        m_thread.detach();
    }

    m_running.store(false, std::memory_order_release);
}

} // namespace BetterMagnifier
