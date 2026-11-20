Unicode true
RequestExecutionLevel admin

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"
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
Var SetupHelper
!define UNINSTALL_KEY "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\SideLantern"
!define SETUP_KEY "SOFTWARE\SideLantern"
!define MUI_CUSTOMFUNCTION_ABORT CleanSetupHelper
!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"

!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose a new folder in a protected location on a fixed NTFS drive. Windows system services require a directory that regular users cannot replace."
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE VerifyDirectory
!insertmacro MUI_PAGE_DIRECTORY
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

Function VerifyDirectory
  nsExec::ExecToStack '"$SetupHelper" validate-dir "$INSTDIR"'
  Pop $0
  Pop $1
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "$1"
    Abort
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
  ReadRegStr $0 HKLM "${SETUP_KEY}" "InstallLocation"
  ${If} $0 == ""
  ${OrIf} $INSTDIR != $0
    MessageBox MB_OK|MB_ICONSTOP "Run the uninstaller from the registered SideLantern installation folder." /SD IDOK
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
  nsExec::ExecToLog '"$SetupHelper" create-dir "$INSTDIR"'
  Pop $0
  ${If} $0 != 0
    SetErrorLevel 1
    Abort "The protected installation folder could not be created. See the Setup detail log."
  ${EndIf}
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

!ifdef VDD_PACKAGE
Section /o "Virtual display driver" VddComponent
  SetOutPath "$INSTDIR\resources\vdd-driver"
  File "${VDD_PACKAGE}\MttVDD.inf"
  File "${VDD_PACKAGE}\MttVDD.dll"
  File /oname=MttVDD.cat "${VDD_PACKAGE}\mttvdd.cat"
  File "${VDD_PACKAGE}\LICENSE.txt"
  nsExec::ExecToLog '"$INSTDIR\resources\SetupSupport.exe" vdd'
  Pop $0
  ${If} $0 == 3010
    SetRebootFlag true
  ${ElseIf} $0 != 0
    DetailPrint "Virtual display driver setup returned $0. Windows may have blocked the installation or found an existing driver."
    MessageBox MB_OK|MB_ICONEXCLAMATION "The virtual display driver could not be set up. SideLantern remains installed. See the Setup detail log." /SD IDOK
  ${EndIf}
  SetOutPath "$INSTDIR"
SectionEnd
!endif

Function .onInit
  ${IfNot} ${IsNativeAMD64}
    MessageBox MB_OK|MB_ICONSTOP "This SideLantern package requires x64 Windows." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
  !insertmacro AcquireSetupMutex
  ReadRegStr $0 HKLM "${SETUP_KEY}" "SetupIdentifier"
  ${If} $0 != ""
    MessageBox MB_OK|MB_ICONSTOP "An existing SideLantern installation was found. Exit and uninstall it before continuing. User settings are kept." /SD IDOK
    Abort
  ${EndIf}
  ; Keep elevated preflight code under the system-protected directory.
  System::Call 'kernel32::GetTempFileNameW(w "$PROGRAMFILES64", w "SLN", i 0, w .r0) i .r1'
  ${If} $1 == 0
    MessageBox MB_OK|MB_ICONSTOP "Setup could not create its protected preflight helper." /SD IDOK
    Abort
  ${EndIf}
  StrCpy $SetupHelper $0
  ClearErrors
  File /oname=$SetupHelper "${PAYLOAD}\resources\SetupSupport.exe"
  ${If} ${Errors}
    Delete "$SetupHelper"
    Abort "Setup could not extract its protected preflight helper."
  ${EndIf}
  !ifdef VIGEM_SETUP
    nsExec::ExecToLog '"$SetupHelper" vigem-status'
    Pop $0
    ${If} $0 == 0
      SectionSetText ${ViGemComponent} "ViGEmBus (already installed, will be reused)"
      SectionSetFlags ${ViGemComponent} ${SF_RO}
    ${ElseIf} $0 != 10
      SectionSetText ${ViGemComponent} "ViGEmBus (existing or unverified, preserved)"
      SectionSetFlags ${ViGemComponent} ${SF_RO}
    ${EndIf}
  !endif
  !ifdef VDD_PACKAGE
    nsExec::ExecToLog '"$SetupHelper" vdd-status'
    Pop $0
    ${If} $0 == 0
      SectionSetText ${VddComponent} "Virtual display driver (already installed, will be reused)"
      SectionSetFlags ${VddComponent} ${SF_RO}
    ${ElseIf} $0 != 10
      SectionSetText ${VddComponent} "Virtual display driver (existing or unverified, preserved)"
      SectionSetFlags ${VddComponent} ${SF_RO}
    ${EndIf}
  !endif
FunctionEnd

Function .onInstSuccess
  Delete "$SetupHelper"
FunctionEnd

Function .onInstFailed
  Delete "$SetupHelper"
FunctionEnd

Function CleanSetupHelper
  Delete "$SetupHelper"
FunctionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${HostComponent} "Installs the SideLantern app and Windows service for Moonlight streaming."
  !ifdef VIGEM_SETUP
    !insertmacro MUI_DESCRIPTION_TEXT ${ViGemComponent} "Enables virtual gamepad input from Moonlight. A compatible installed copy is reused."
  !endif
  !ifdef VDD_PACKAGE
    !insertmacro MUI_DESCRIPTION_TEXT ${VddComponent} "Adds a virtual Windows monitor for streaming a separate desktop, including wireless second-screen modes on supported clients."
  !endif
!insertmacro MUI_FUNCTION_DESCRIPTION_END

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
  Delete "$INSTDIR\resources\vdd-driver\MttVDD.inf"
  Delete "$INSTDIR\resources\vdd-driver\MttVDD.dll"
  Delete "$INSTDIR\resources\vdd-driver\MttVDD.cat"
  Delete "$INSTDIR\resources\vdd-driver\LICENSE.txt"
  RMDir "$INSTDIR\resources\vdd-driver"
  Delete "$INSTDIR\Uninstall.exe"
  !insertmacro DeletePayload
  RMDir "$INSTDIR"
SectionEnd
