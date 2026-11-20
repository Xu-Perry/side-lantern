Unicode true
RequestExecutionLevel admin

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "${PAYLOAD_CLEANUP}"

Name "SideLantern"
OutFile "${SETUP_EXE}"
InstallDir "$PROGRAMFILES64\SideLantern"
VIProductVersion "${VERSION_NUMBER}"
VIAddVersionKey /LANG=1033 "ProductName" "SideLantern"
VIAddVersionKey /LANG=1033 "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "FileDescription" "SideLantern Windows Setup"
SetCompressor /SOLID lzma

Var InstallMutex
!define UNINSTALL_KEY "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\SideLantern"

!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!macro AcquireSetupMutex
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Global\SideLantern.Windows.Setup") p .r0 ?e'
  Pop $1
  ${If} $0 == 0
  ${OrIf} $1 == 183
    MessageBox MB_OK|MB_ICONSTOP "Another SideLantern Setup operation is running." /SD IDOK
    Abort
  ${EndIf}
  StrCpy $InstallMutex $0
!macroend

!macro RegisterUninstaller
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "SideLantern"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\SideLantern.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
!macroend

Function .onInit
  ${IfNot} ${IsNativeAMD64}
    MessageBox MB_OK|MB_ICONSTOP "This SideLantern package requires x64 Windows." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
  StrCpy $INSTDIR "$PROGRAMFILES64\SideLantern"
  !insertmacro AcquireSetupMutex
  StrCpy $2 $INSTDIR
  System::Call 'kernel32::GetFileAttributesW(w r2) i .r0'
  ${If} $0 != -1
    MessageBox MB_OK|MB_ICONSTOP "The installation folder is already in use. Exit and uninstall the existing SideLantern version first. User settings are kept." /SD IDOK
    Abort
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
  ${If} $INSTDIR != "$PROGRAMFILES64\SideLantern"
    MessageBox MB_OK|MB_ICONSTOP "Run the uninstaller from the SideLantern folder in Program Files." /SD IDOK
    Abort
  ${EndIf}
  !insertmacro AcquireSetupMutex
  StrCpy $2 $INSTDIR
  System::Call 'kernel32::GetFileAttributesW(w r2) i .r0'
  IntOp $1 $0 & 0x400
  ${If} $0 == -1
  ${OrIf} $1 != 0
    MessageBox MB_OK|MB_ICONSTOP "The SideLantern installation folder could not be verified." /SD IDOK
    Abort
  ${EndIf}
  StrCpy $0 "$INSTDIR\resources"
  System::Call 'kernel32::GetFileAttributesW(w r0) i .r1'
  IntOp $2 $1 & 0x400
  ${If} $1 == -1
  ${OrIf} $2 != 0
    MessageBox MB_OK|MB_ICONSTOP "SideLantern resources could not be verified." /SD IDOK
    Abort
  ${EndIf}
  StrCpy $0 "$INSTDIR\resources\SetupSupport.exe"
  System::Call 'kernel32::GetFileAttributesW(w r0) i .r1'
  IntOp $2 $1 & 0x400
  ${If} $1 == -1
  ${OrIf} $2 != 0
    MessageBox MB_OK|MB_ICONSTOP "The SideLantern Setup helper could not be verified." /SD IDOK
    Abort
  ${EndIf}
FunctionEnd

Section "SideLantern host" HostComponent
  SectionIn RO
  SetOutPath "$INSTDIR"
  ClearErrors
  File /r "${PAYLOAD}\*"
  ${If} ${Errors}
    Goto installFailed
  ${EndIf}
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  ${If} ${Errors}
    Goto installFailed
  ${EndIf}
  nsExec::ExecToLog '"$INSTDIR\resources\SetupSupport.exe" install'
  Pop $0
  ${If} $0 == 3
    !insertmacro RegisterUninstaller
    SetErrorLevel 1
    Abort "Setup could not roll back all components. Run SideLantern Uninstall before retrying."
  ${ElseIf} $0 != 0
    Goto installFailed
  ${EndIf}

  CreateDirectory "$SMPROGRAMS\SideLantern"
  CreateShortCut "$SMPROGRAMS\SideLantern\SideLantern.lnk" "$INSTDIR\SideLantern.exe"
  !insertmacro RegisterUninstaller
  Goto installDone

installFailed:
  Delete "$INSTDIR\Uninstall.exe"
  !insertmacro DeletePayload
  RMDir "$INSTDIR"
  SetErrorLevel 1
  Abort "SideLantern could not be installed. See the Setup detail log."
installDone:
SectionEnd

!ifdef VIGEM_SETUP
Section "ViGEmBus virtual gamepad support" ViGemComponent
  SetOutPath "$INSTDIR\resources"
  File /oname=ViGEmBusSetup.exe "${VIGEM_SETUP}"
  nsExec::ExecToLog '"$INSTDIR\resources\SetupSupport.exe" vigem'
  Pop $0
  Delete "$INSTDIR\resources\ViGEmBusSetup.exe"
  ${If} $0 == 3010
    SetRebootFlag true
  ${ElseIf} $0 == 21
    DetailPrint "An existing ViGEmBus installation needs manual verification. Setup left it intact."
    MessageBox MB_OK|MB_ICONINFORMATION "An existing ViGEmBus installation was left intact. Verify it separately if gamepads are unavailable." /SD IDOK
  ${ElseIf} $0 != 0
    DetailPrint "ViGEmBus Setup returned $0."
    MessageBox MB_OK|MB_ICONEXCLAMATION "ViGEmBus Setup did not complete. SideLantern remains installed." /SD IDOK
  ${EndIf}
SectionEnd
!endif

Section "Uninstall"
  nsExec::ExecToLog '"$INSTDIR\resources\SetupSupport.exe" uninstall'
  Pop $0
  ${If} $0 != 0
    SetErrorLevel 1
    Abort "SideLantern could not be uninstalled. Exit SideLantern from the tray and see the Setup detail log."
  ${EndIf}
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  Delete "$SMPROGRAMS\SideLantern\SideLantern.lnk"
  RMDir "$SMPROGRAMS\SideLantern"
  Delete "$INSTDIR\resources\ViGEmBusSetup.exe"
  Delete "$INSTDIR\Uninstall.exe"
  !insertmacro DeletePayload
  RMDir "$INSTDIR"
SectionEnd
