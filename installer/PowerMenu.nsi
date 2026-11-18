; PowerMenu installer. Build with:  makensis /DARCH=arm64 PowerMenu.nsi   (or /DARCH=x64)

Unicode true
ManifestDPIAware true

!ifndef ARCH
  !error "Pass /DARCH=arm64 or /DARCH=x64"
!endif

!define APP        "PowerMenu"
!define VERSION    "1.0.4"
!define PUBLISHER  "PowerMenu"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"
!define RUN_KEY    "Software\Microsoft\Windows\CurrentVersion\Run"

!if "${ARCH}" == "arm64"
  !define ARCH_NAME "ARM64"
!else if "${ARCH}" == "x64"
  !define ARCH_NAME "x64"
!else
  !error "ARCH must be arm64 or x64"
!endif

Name "${APP} ${VERSION} (${ARCH_NAME})"
OutFile "..\dist\${APP}-${VERSION}-${ARCH}-setup.exe"
InstallDir "$PROGRAMFILES64\${APP}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${APP} ${VERSION} ${ARCH_NAME}"

!include MUI2.nsh
!include x64.nsh
!include LogicLib.nsh

!define MUI_ICON   "..\res\PowerMenu.ico"
!define MUI_UNICON "..\res\PowerMenu.ico"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TEXT "Setup will install ${APP} (${ARCH_NAME}).$\r$\n$\r$\nWhen you press the power button, ${APP} shows a menu with Lock, Sleep, Power Off and Restart. Lock works with one click (or the L key); the others have to be held for 3 seconds before they run, on screen or with their key (S, O, R).$\r$\n$\r$\n${APP} starts with Windows as a small background service. It opens the menu app when you sign in, and at the sign-in screen the power button puts the PC to sleep.$\r$\n$\r$\nWindows' own power-button action is set to $\"Do nothing$\" while ${APP} is installed and restored when you uninstall it."
; No "run now" box: starting the service also starts the menu app for everyone signed in.

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP} ${ARCH_NAME} setup"
VIAddVersionKey "LegalCopyright" ""

Function .onInit
  SetRegView 64
!if "${ARCH}" == "arm64"
  ${IfNot} ${IsNativeARM64}
    MessageBox MB_ICONSTOP "This is the ARM64 installer. Use ${APP}-${VERSION}-x64-setup.exe on this PC." /SD IDOK
    Abort
  ${EndIf}
!else
  ${If} ${IsNativeARM64}
    MessageBox MB_YESNO|MB_ICONQUESTION "This PC runs ARM64 Windows, so ${APP}-${VERSION}-arm64-setup.exe is the better fit (it runs natively).$\r$\n$\r$\nInstall the x64 version anyway? It runs under emulation." /SD IDYES IDYES +2
    Abort
  ${ElseIfNot} ${IsNativeAMD64}
    MessageBox MB_ICONSTOP "${APP} needs 64-bit Windows." /SD IDOK
    Abort
  ${EndIf}
!endif
FunctionEnd

!macro StopApp
  nsExec::Exec '"$SYSDIR\sc.exe" stop ${APP}'
  Pop $0
  Sleep 1500
  IfFileExists "$INSTDIR\${APP}.exe" 0 +3
  nsExec::Exec '"$INSTDIR\${APP}.exe" --quit'
  Pop $0
  Sleep 700
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM ${APP}.exe'
  Pop $0
  Sleep 300
!macroend

Section "Install"
  SetShellVarContext all
  !insertmacro StopApp

  SetOutPath "$INSTDIR"
  File "..\build\${ARCH}\${APP}.exe"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Versions before 1.0.4 started at logon from the Run key; the service does that now.
  DeleteRegValue HKLM "${RUN_KEY}" "${APP}"

  CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\${APP}.exe" "--show" "$INSTDIR\${APP}.exe" 0 SW_SHOWNORMAL "" "Show the power menu"

  ; Windows' power-button action -> "Do nothing" on every power plan (old values are saved).
  DetailPrint "Setting the Windows power-button action to Do nothing"
  nsExec::ExecToLog '"$INSTDIR\${APP}.exe" --take-button'
  Pop $0
  ${If} $0 != 0
    DetailPrint "Warning: could not change every power plan (code $0)"
  ${EndIf}

  ; Automatic-start service; starting it also starts the menu app for everyone signed in.
  DetailPrint "Installing and starting the ${APP} service"
  nsExec::ExecToLog '"$INSTDIR\${APP}.exe" --install-service'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_ICONEXCLAMATION "The ${APP} service could not be installed or started (code $0)." /SD IDOK
  ${EndIf}

  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayName"          "${APP} (${ARCH_NAME})"
  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayVersion"       "${VERSION}"
  WriteRegStr   HKLM "${UNINST_KEY}" "Publisher"            "${PUBLISHER}"
  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayIcon"          '"$INSTDIR\${APP}.exe",0'
  WriteRegStr   HKLM "${UNINST_KEY}" "InstallLocation"      "$INSTDIR"
  WriteRegStr   HKLM "${UNINST_KEY}" "UninstallString"      '"$INSTDIR\Uninstall.exe"'
  WriteRegStr   HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" 600
SectionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section "Uninstall"
  SetShellVarContext all
  DetailPrint "Removing the ${APP} service"
  nsExec::ExecToLog '"$INSTDIR\${APP}.exe" --uninstall-service'
  Pop $0
  !insertmacro StopApp

  DetailPrint "Restoring the Windows power-button action"
  nsExec::ExecToLog '"$INSTDIR\${APP}.exe" --restore-button'
  Pop $0

  DeleteRegValue HKLM "${RUN_KEY}" "${APP}"
  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$INSTDIR\${APP}.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  RMDir /r "$APPDATA\${APP}"  ; ProgramData under SetShellVarContext all: the service log
  DeleteRegKey HKLM "${UNINST_KEY}"
SectionEnd
