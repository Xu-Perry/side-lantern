# SideLantern Windows installer

The installer stages an AMD64 Electron distribution and a Sunshine engine into
`%ProgramFiles%\SideLantern`. Use a Windows build of this fork for the release
candidate. A copy of upstream Sunshine is useful for verifying the package
layout and lacks the fork's live session and display endpoints.
The Electron interface uses the engine's local management API. The installer
packages the engine's capture resources and default app entries.

## Build

Build on Windows x64 with the .NET Framework compiler available. Supply the
portable Electron version pinned in [package.json](../package.json), the
engine installation staging directory containing `sunshine.exe` and
`assets\apps.json`, and the NSIS compiler path. By default the script builds
the service from [desktop/native/service](../native/service).

```powershell
.\desktop\installer\build.ps1 `
    -ElectronDirectory E:\tools\electron-win32-x64 `
    -EngineDirectory E:\build\sunshine-stage `
    -Makensis E:\tools\nsis\makensis.exe `
    -OutputDirectory E:\build\side-lantern `
    -ViGEmBusInstaller E:\tools\ViGEmBus_1.22.0_x64_x86_arm64.exe
```

`-ServiceExecutable` can supply a prebuilt AMD64 service. `-KeepStaging`
keeps the extracted staging tree for package inspection.
`-WithoutViGEmBus` produces a gamepad-free packaging test. The default
installer option installs the verified official ViGEmBus 1.22.0 installer if
the driver is absent. Compatible installed ViGEmBus 1.17 or later is reused.
An existing driver of unknown or incompatible version is left in place for
manual troubleshooting.
[ViGEmBus release](https://github.com/nefarius/ViGEmBus/releases/tag/v1.22.0) ·
[Published hash and installer switches](https://github.com/microsoft/winget-pkgs/blob/master/manifests/v/ViGEm/ViGEmBus/1.22.0/ViGEm.ViGEmBus.installer.yaml)

## Behavior and verification

- Start Setup from the desktop account that will use SideLantern. Setup registers
  demand-start `SideLanternService` and grants that account service start and
  status access. Setup leaves the service stopped, so SideLantern can start it
  under its own lifetime management at first launch.
- Setup adds SideLantern-only inbound rules on the private network profile for
  the local subnet. The Windows management endpoint is excluded. The existing
  `SunshineService`, its firewall rules and its files are outside Setup's
  ownership. Exit Sunshine yourself when you intend to switch hosts on the
  default streaming ports.
- Before replacing this candidate, exit SideLantern from the tray, uninstall
  through Windows Apps, and install the new version. Uninstall checks the
  service and firewall ownership, removes only staged program paths and keeps
  the per-user profile and shared drivers.
- The package includes the read-only MttVDD probe. It can identify an existing
  Windows-attached extended display and expose its actual modes. Use
  `-VirtualDisplayDirectory` and `-SignTool` to validate an optional driver
  candidate's catalog and exact files without packaging it. A future
  app-owned virtual display needs a distinct install/control boundary before
  Setup can safely add and remove a display.
  [MttVDD control protocol](https://github.com/VirtualDrivers/Virtual-Driver-Control/tree/main/VirtualDriverControl)
- Build the candidate and inspect the staged programs and media assets.
  Installer compilation alone verifies the payload layout. Perform
  install, default component selection, standard-user start/stop, uninstall
  and reinstall checks on a disposable x64 Windows system before distribution.
  Release signing and distribution verification are separate release steps.
