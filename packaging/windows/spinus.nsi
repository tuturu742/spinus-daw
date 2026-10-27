; The Spinus installer for Windows (NSIS 3). Made from the folder packaging/windows/stage.py fills:
;
;   python packaging/windows/stage.py --build build/windows --out build/windows-stage
;   makensis /DSTAGE=<full path of build\windows-stage> packaging\windows\spinus.nsi   (writes Spinus-<version>-Setup.exe)
;
; STAGE is a full path: makensis works in this script's folder, so a relative one is not found.
;
; It installs for the current user only (no administrator rights): into %LOCALAPPDATA%\Programs\Spinus DAW,
; with a Start menu entry, .spn projects opening in Spinus, and an uninstaller in Settings > Apps. Settings
; and downloads (%APPDATA%\Spinus DAW, %LOCALAPPDATA%\Spinus DAW) stay when Spinus is removed.

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel user

!ifndef STAGE
  !error "Pass /DSTAGE=<the folder stage.py made>"
!endif
!include "${STAGE}\version.nsh"
!include "MUI2.nsh"

!define NAME "Spinus DAW"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SpinusDAW"
!define PROG_ID "SpinusDAW.Project"

Name "${NAME}"
OutFile "${STAGE}\..\Spinus-${VERSION}-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\${NAME}"
InstallDirRegKey HKCU "Software\${NAME}" "InstallDir"
BrandingText "${NAME} ${VERSION}"

!define MUI_ICON "${STAGE}\spinus.ico"
!define MUI_UNICON "${STAGE}\spinus.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\spinus.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Spinus now"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\app\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${NUMERIC_VERSION}"
VIAddVersionKey "ProductName" "${NAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${NAME} installer"
VIAddVersionKey "LegalCopyright" "AGPL-3.0-or-later"

Section "Spinus"
  SetOutPath "$INSTDIR"
  ; A new version replaces the old one's files: nothing of an older AI service or Python is left over.
  RMDir /r "$INSTDIR\ai"
  RMDir /r "$INSTDIR\demo"
  RMDir /r "$INSTDIR\samples"
  File /r "${STAGE}\app\*.*"
  WriteUninstaller "$INSTDIR\Uninstall Spinus.exe"
  WriteRegStr HKCU "Software\${NAME}" "InstallDir" "$INSTDIR"

  CreateShortcut "$SMPROGRAMS\${NAME}.lnk" "$INSTDIR\spinus.exe"

  ; .spn projects open in Spinus (double-click in Explorer).
  WriteRegStr HKCU "Software\Classes\.spn" "" "${PROG_ID}"
  WriteRegStr HKCU "Software\Classes\${PROG_ID}" "" "Spinus project"
  WriteRegStr HKCU "Software\Classes\${PROG_ID}\DefaultIcon" "" "$INSTDIR\spinus.exe,0"
  WriteRegStr HKCU "Software\Classes\${PROG_ID}\shell\open\command" "" '"$INSTDIR\spinus.exe" "%1"'
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)' ; SHCNE_ASSOCCHANGED

  ; Settings > Apps.
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "Spinus DAW contributors"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\spinus.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall Spinus.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "https://github.com/tuturu742/spinus-daw"
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\${NAME}.lnk"
  DeleteRegKey HKCU "Software\Classes\${PROG_ID}"
  ReadRegStr $0 HKCU "Software\Classes\.spn" ""
  StrCmp $0 "${PROG_ID}" 0 +2
    DeleteRegKey HKCU "Software\Classes\.spn"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "Software\${NAME}"
  ; Only what was installed: the folder may have been one with other files in it.
  RMDir /r "$INSTDIR\ai"
  RMDir /r "$INSTDIR\docs"
  RMDir /r "$INSTDIR\demo"
  RMDir /r "$INSTDIR\samples"
  Delete "$INSTDIR\spinus.exe"
  Delete "$INSTDIR\spinus-plugin-scanner.exe"
  Delete "$INSTDIR\spinus-plugin-host.exe"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\THIRD_PARTY.md"
  Delete "$INSTDIR\Uninstall Spinus.exe"
  RMDir "$INSTDIR"
SectionEnd
