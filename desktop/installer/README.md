# SideLantern Windows installer

The installer stages an AMD64 Electron distribution and a Sunshine engine into
a new directory chosen during Setup. The default is `%ProgramFiles%\SideLantern`.
Use a Windows build of this fork for the release
candidate. A copy of upstream Sunshine is useful for verifying the package
layout and lacks the fork's live session and display endpoints.
The Electron interface uses the engine's local management API. The installer
packages the engine's capture resources and default app entries.

## Build

Build on Windows x64 with the .NET Framework compiler available. Supply the
portable Electron version pinned in [package.json](../package.json), the
engine installation staging directory containing `sunshine.exe` and
`assets\apps.json`, and the NSIS compiler path. By default the script builds
the service from [desktop/native/service](../native/service). Supply
[rcedit 2.0.0](https://github.com/electron/rcedit/releases/tag/v2.0.0) to embed the SideLantern icon in the Electron executable.

```powershell
.\desktop\installer\build.ps1 `
    -ElectronDirectory E:\tools\electron-win32-x64 `
    -EngineDirectory E:\build\sunshine-stage `
    -Makensis E:\tools\nsis\makensis.exe `
    -ResourceEditor E:\tools\rcedit-x64.exe `
    -OutputDirectory E:\build\side-lantern `
    -ViGEmBusInstaller E:\tools\ViGEmBus_1.22.0_x64_x86_arm64.exe `
    -VirtualDisplayArchive E:\tools\VirtualDisplayDriver-x86.Driver.Only.zip `
    -VirtualDisplayLicense E:\tools\MttVDD-25.7.23-LICENSE
```

`-ServiceExecutable` can supply a prebuilt AMD64 service. `-KeepStaging`
keeps the extracted staging tree for package inspection.
`-WithoutViGEmBus` produces a gamepad-free packaging test. The default
installer option installs the verified official ViGEmBus 1.22.0 installer if
the driver is absent. Compatible installed ViGEmBus 1.17 or later is reused.
Setup detects the driver before displaying the component list. Only a missing
driver is selected by default. A compatible copy is marked for reuse. An
existing driver of unknown or incompatible version is left in place.
[ViGEmBus release](https://github.com/nefarius/ViGEmBus/releases/tag/v1.22.0) ·
[Published hash and installer switches](https://github.com/microsoft/winget-pkgs/blob/master/manifests/v/ViGEm/ViGEmBus/1.22.0/ViGEm.ViGEmBus.installer.yaml)

Pass the official `VirtualDisplayDriver-x86.Driver.Only.zip` archive and the
license from **25.7.23** to include the optional AMD64 MttVDD component. The
builder verifies both digests and the catalog signature. The component is
unchecked by default. Setup reuses the existing display adapter when found.
[MttVDD release](https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/tag/25.7.23) ·
[Driver license](https://github.com/VirtualDrivers/Virtual-Display-Driver/blob/25.7.23/LICENSE)

## Behavior and verification

- Start Setup from the desktop account that will use SideLantern. Setup registers
  demand-start `SideLanternService` and grants that account service start and
  status access. Setup leaves the service stopped, so SideLantern can start it
  under its own lifetime management at first launch.
- Choose a new folder on a fixed NTFS drive. Setup checks the parent folders for
  ownership, permissions and redirection because the service runs as LocalSystem.
  Program Files is the default protected location. A different drive is accepted
  if its folder chain also passes these checks.
- Setup allows inbound LAN and Internet traffic to the SideLantern engine on
  TCP 47984, 47989, 48010 and UDP 47998–48000, 48002 across Windows network
  profiles. Pairing is still required for streaming. The management port 47990
  stays local to the host. With a public IPv4 address on a NAT router, forward
  the streaming ports to the PC's LAN address. UPnP starts disabled.
- Setup owns the SideLantern service and firewall rules it creates. Exit
  Sunshine yourself when you intend to switch hosts on the default streaming
  ports.
- Before replacing this candidate, exit SideLantern from the tray, uninstall
  through Windows Apps, and install the new version. Uninstall checks the
  service and firewall ownership, removes only staged program paths and keeps
  the per-user profile and shared drivers.
- The virtual display component registers a Windows virtual monitor adapter.
  The probe identifies it once Windows provides an independent extended screen.
  A mirrored screen shares the primary desktop. An installed driver with no
  monitor available may require enabling a screen in the driver control panel.
  Windows owns this layout and preserves the driver after SideLantern uninstall.
- Inspect a different MttVDD candidate without bundling it by passing
  `-VirtualDisplayDirectory` and `-SignTool`.
- Build the candidate and inspect the staged programs and media assets.
  Installer compilation alone verifies the payload layout. Perform
  install, default component selection, standard-user start/stop, uninstall
  and reinstall checks on a disposable x64 Windows system before distribution.
  Release signing and distribution verification are separate release steps.
- To verify the default firewall rules without applying them, compile
  [SetupFirewallTest.cs](../test/SetupFirewallTest.cs) with the x64 .NET
  Framework compiler and reference `Microsoft.CSharp.dll`. Run the test with the
  staged `payload\resources\SetupSupport.exe` as its argument.
