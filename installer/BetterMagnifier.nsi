; =============================================================================
; BetterMagnifier installer
; =============================================================================
; Per-machine by necessity: the manifest requires administrator rights, so a
; per-user install would buy nothing.
;
; Build with:
;   makensis /DVERSION=0.2.0 BetterMagnifier.nsi
;   /DRUNTIME_EXE=..\build\WindowsAppRuntimeInstall-x64.exe   (CI only)
;
; RUNTIME_EXE is optional so the local loop need not download 100 MB. Without
; it the setup works only where the Windows App Runtime is already installed.
; =============================================================================

Unicode true
SetCompressor /SOLID lzma

!ifndef VERSION
  !error "VERSION is required: makensis /DVERSION=0.2.0 BetterMagnifier.nsi"
!endif

!ifndef SOURCE_DIR
  !define SOURCE_DIR "..\bin\Release-x64"
!endif

!define APP_NAME   "BetterMagnifier"
!define APP_EXE    "BetterMagnifier.exe"
!define MSG_CLASS  "BetterMagnifierMsg"
!define PUBLISHER  "mertemr"
!define APP_URL    "https://github.com/mertemr/BetterMagnifier"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

Name "${APP_NAME} ${VERSION}"
OutFile "out\${APP_NAME}-${VERSION}-x64-setup.exe"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "Software\${APP_NAME}" "InstallDir"
RequestExecutionLevel admin

VIProductVersion "${VERSION}.0"
VIAddVersionKey  "ProductName"     "${APP_NAME}"
VIAddVersionKey  "FileDescription" "${APP_NAME} installer"
VIAddVersionKey  "FileVersion"     "${VERSION}.0"
VIAddVersionKey  "ProductVersion"  "${VERSION}.0"
VIAddVersionKey  "LegalCopyright"  "Apache License 2.0"
VIAddVersionKey  "CompanyName"     "${PUBLISHER}"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "WinMessages.nsh"

!define MUI_ICON   "..\res\BetterMagnifier.ico"
!define MUI_UNICON "..\res\BetterMagnifier.ico"
!define MUI_ABORTWARNING

!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP_NAME}"

!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES

; English first: it is the default when the user's language is neither.
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Turkish"

;; CloseRunningInstance
;;
;; The hidden message window answers WM_CLOSE with PostQuitMessage, so the
;; graceful path needs nothing on the application side. taskkill is the
;; fallback: a hard kill loses no settings but skips the cursor restore.
;;
;; OMIT FindWindow's window-name argument, never pass "". The window's title is
;; "BetterMagnifier", so an empty string searches for a window with an empty
;; title, finds nothing, and reports the application as not running.
!macro CloseRunningInstanceBody un
Function ${un}CloseRunningInstance
    Push $0
    Push $1

    StrCpy $1 0

    ${Do}
        FindWindow $0 "${MSG_CLASS}"
        ${If} $0 == 0
            ${ExitDo}
        ${EndIf}

        DetailPrint "Closing ${APP_NAME}..."
        SendMessage $0 ${WM_CLOSE} 0 0 /TIMEOUT=1000
        Sleep 500

        IntOp $1 $1 + 1
        ${If} $1 >= 10
            ${ExitDo}
        ${EndIf}
    ${Loop}

    FindWindow $0 "${MSG_CLASS}"
    ${If} $0 != 0
        DetailPrint "It did not close; ending it the hard way."
        nsExec::Exec 'taskkill /IM ${APP_EXE} /F'
        Pop $0
        Sleep 1000
    ${EndIf}

    Pop $1
    Pop $0
FunctionEnd
!macroend

; The same body is needed in the installer and the uninstaller, and NSIS keeps
; those in separate namespaces - hence the macro rather than two copies.
!insertmacro CloseRunningInstanceBody ""
!insertmacro CloseRunningInstanceBody "un."

; =============================================================================
; Install
; =============================================================================
Section "Install"
    ; Both are load-bearing on a 64-bit machine.
    ;
    ; SetRegView 64: makensis is 32-bit, so HKLM\Software writes land in
    ; Wow6432Node, where IsInstalledCopy - which reads the 64-bit view - will
    ; never find them, and the updater refuses to install on every installed
    ; copy with nothing logged to say why.
    ;
    ; SetShellVarContext all: $SMPROGRAMS is the CURRENT USER's Start Menu by
    ; default, even for an elevated per-machine install.
    SetRegView 64
    SetShellVarContext all

    Call CloseRunningInstance

    SetOutPath "$INSTDIR"

    File "${SOURCE_DIR}\${APP_EXE}"
    File "${SOURCE_DIR}\Microsoft.WindowsAppRuntime.Bootstrap.dll"
    File "${SOURCE_DIR}\Microsoft.Web.WebView2.Core.dll"
    File "${SOURCE_DIR}\BetterMagnifier.pri"
    File "..\LICENSE"
    File "..\README.md"

    ; The application is framework-dependent: the SDK auto-initializer resolves
    ; Microsoft.WindowsAppRuntime.1.8 at module load, and that package is
    ; machine-wide state the build output does not contain. Without it the
    ; process dies before wWinMain.
    ;
    ; Run unconditionally rather than probed - it is idempotent and cheap when
    ; already present, which beats a registry check that must stay correct
    ; across versions.
!ifdef RUNTIME_EXE
    DetailPrint "Installing the Windows App Runtime..."
    InitPluginsDir
    SetOutPath "$PLUGINSDIR"
    File "${RUNTIME_EXE}"
    ${GetFileName} "${RUNTIME_EXE}" $R0
    nsExec::ExecToLog '"$PLUGINSDIR\$R0" --quiet'
    Pop $0
    ; 0 is success. 0x80073D06 is "already installed", which is the common case
    ; on any machine that has run a WinUI application before.
    ${If} $0 != 0
    ${AndIf} $0 != "0x80073D06"
        DetailPrint "The runtime installer returned $0."
        DetailPrint "The control panel may not open; magnification, hotkeys and the tray icon do not depend on it."
    ${EndIf}
    SetOutPath "$INSTDIR"
!endif

    ; IsInstalledCopy reads this to decide whether the updater may run. A
    ; portable copy has no such key and offers no self-install.
    WriteRegStr HKLM "Software\${APP_NAME}" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "Software\${APP_NAME}" "Version"    "${VERSION}"

    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegStr   HKLM "${UNINST_KEY}" "DisplayName"          "${APP_NAME}"
    WriteRegStr   HKLM "${UNINST_KEY}" "DisplayVersion"       "${VERSION}"
    WriteRegStr   HKLM "${UNINST_KEY}" "DisplayIcon"          "$INSTDIR\${APP_EXE}"
    WriteRegStr   HKLM "${UNINST_KEY}" "Publisher"            "${PUBLISHER}"
    WriteRegStr   HKLM "${UNINST_KEY}" "URLInfoAbout"         "${APP_URL}"
    WriteRegStr   HKLM "${UNINST_KEY}" "InstallLocation"      "$INSTDIR"
    WriteRegStr   HKLM "${UNINST_KEY}" "UninstallString"      '"$INSTDIR\Uninstall.exe"'
    WriteRegStr   HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize"        "$0"
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify"             1
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair"             1

    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortcut  "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
    CreateShortcut  "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\Uninstall.exe"

    WriteUninstaller "$INSTDIR\Uninstall.exe"

    ; /RELAUNCH: passed by the updater so the new build comes back up. The MUI
    ; finish page covers the interactive case, so this only fires under /S.
    ${GetParameters} $R1
    ClearErrors
    ${GetOptions} $R1 "/RELAUNCH" $R2
    ${IfNot} ${Errors}
        DetailPrint "Restarting ${APP_NAME}..."
        Exec '"$INSTDIR\${APP_EXE}"'
    ${EndIf}
SectionEnd

; =============================================================================
; Uninstall
; =============================================================================
Section "un.${APP_NAME}"
    ; Must match the installer exactly, or the uninstaller looks for the key and
    ; the shortcuts somewhere they were never written.
    SetRegView 64
    SetShellVarContext all

    Call un.CloseRunningInstance

    Delete "$INSTDIR\${APP_EXE}"
    Delete "$INSTDIR\Microsoft.WindowsAppRuntime.Bootstrap.dll"
    Delete "$INSTDIR\Microsoft.Web.WebView2.Core.dll"
    Delete "$INSTDIR\BetterMagnifier.pri"
    Delete "$INSTDIR\LICENSE"
    Delete "$INSTDIR\README.md"
    Delete "$INSTDIR\Uninstall.exe"

    ; The application writes its log next to the exe, by design - see main.cpp.
    RMDir /r "$INSTDIR\logs"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
    RMDir  "$SMPROGRAMS\${APP_NAME}"

    DeleteRegKey HKLM "Software\${APP_NAME}"
    DeleteRegKey HKLM "${UNINST_KEY}"

    ; Written by the "Start with Windows" setting. Left behind it would point at
    ; a binary that no longer exists.
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "${APP_NAME}"
SectionEnd

; Unticked by default: %APPDATA% holds the settings and the remembered
; per-monitor zoom levels, and an update runs this installer too, so removing
; them without asking would discard a user's configuration on every bump.
Section /o "un.Settings and preferences"
    ; Deliberate: SetShellVarContext is global, so the "all" above carries in
    ; and $APPDATA would mean ProgramData rather than the user's own.
    SetShellVarContext current
    RMDir /r "$APPDATA\${APP_NAME}"
SectionEnd
