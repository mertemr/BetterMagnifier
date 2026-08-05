#pragma once

// =============================================================================
// AppMessages.h — Thread'ler arasi mesaj sabitleri
// =============================================================================
// GUI thread ve input thread, render thread'e SADECE bu mesajlarla konusur.
// Paylasilan mutable state yok, kilit yok — Win32 mesaj kuyrugu bizim
// thread-safe kuyrugumuz.
//
// Python analojisi: queue.Queue() ile thread'ler arasi is gecirmek.
// Win32'de mesaj kuyrugu zaten her thread'de var, ayrica kurmaya gerek yok.
//
// NEDEN PostMessage, SendMessage DEGIL:
//   SendMessage hedef thread'in mesaji ISLEMESINI BEKLER. Render thread
//   Present(vSync) ile bloklu oldugu icin bu, gonderen thread'i bir frame
//   boyunca kilitler. Low-level hook'ta bu LowLevelHooksTimeout'a takilir.
// =============================================================================

#ifndef BETTER_MAGNIFIER_APP_MESSAGES_H
#define BETTER_MAGNIFIER_APP_MESSAGES_H

#include <windows.h>

namespace BetterMagnifier {

// Tray icon callback'i — TrayIcon::Create bu degeri uCallbackMessage'a koyar
inline constexpr UINT WM_APP_TRAY             = WM_APP + 1;

// GUI -> motor: ayarlar degisti, SettingsStore'dan yeniden oku
inline constexpr UINT WM_APP_SETTINGS_CHANGED = WM_APP + 2;

// GUI -> motor: zoom seviyesini ayarla
//   wParam = monitor indeksi (size_t)
//   lParam = zoom * 1000 (int)   ornek: 2.50x -> 2500
inline constexpr UINT WM_APP_SET_ZOOM         = WM_APP + 3;

// GUI/hotkey -> motor: zoom ac-kapa
//   wParam = monitor indeksi, veya kFocusedMonitor = fare neredeyse orada
inline constexpr UINT WM_APP_TOGGLE_ZOOM      = WM_APP + 4;

// GUI/hotkey -> motor: freeze ac-kapa (wParam ayni)
inline constexpr UINT WM_APP_TOGGLE_FREEZE    = WM_APP + 5;

// Input thread -> motor: zoom'u bir adim degistir
//   wParam = yon: kZoomIn (+1) veya kZoomOut (-1)
//   lParam = kullanilmiyor. Render thread GetCursorPos() ile konumu kendisi
//   okur — olay ile isleme arasi birkac ms, fare ayni monitorde kalir.
//
// Kaynaklari: Ctrl+Alt+tekerlek, Win+arti, Win+eksi.
//
// TOGGLE_ZOOM'dan farki: bu mesaj zoom KAPALIYSA da acabilir (Windows
// Magnifier'in Win+arti davranisi). Cikarken minZoom'a inilirse kapatir.
inline constexpr UINT WM_APP_ZOOM_STEP        = WM_APP + 6;

// WM_APP_ZOOM_STEP wParam degerleri
inline constexpr WPARAM kZoomIn  = 1;
inline constexpr WPARAM kZoomOut = static_cast<WPARAM>(-1);

// Input thread -> motor: klavye odagi degisti, focal point'i oraya kaydir
//   wParam = kullanilmiyor
//   lParam = odaklanan pencerenin HWND'si
inline constexpr UINT WM_APP_FOCUS_CHANGED    = WM_APP + 7;

// Tray -> motor: kontrol panelini goster
inline constexpr UINT WM_APP_SHOW_PANEL       = WM_APP + 8;

// Input thread -> motor: yeni bir popup/menu dogdu, topmost'u ANINDA yeniden
// iddia et.
//
// Neden gerekli: menuler ve dropdown'lar HWND_TOPMOST ile ve bizden SONRA
// olusturuluyor, yani uzerimize cikiyorlar. Periyodik yoklama (250 ms)
// dropdown'lar icin cok yavas — kullanici onu saniyenin altinda bir surede
// aciyor ve kullaniyor, o arada popup'i cift goruyor.
inline constexpr UINT WM_APP_ASSERT_TOPMOST   = WM_APP + 9;

// wParam sentinel'i: "belirli bir monitor degil, farenin uzerinde oldugu monitor"
inline constexpr WPARAM kFocusedMonitor = static_cast<WPARAM>(-1);

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_APP_MESSAGES_H
