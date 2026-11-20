# SideLantern Windows development build

This development build uses a private Sunshine process and a separate profile.
It reads the executable, DLLs and assets from the supplied Sunshine installation.
It uses base port 48989 by default and can run while the original service uses
its normal ports.

The development runner disables Sunshine's UPnP requests, system-wide NVIDIA
profile changes, and automatic installation of Steam audio drivers.

## Start on Windows

From this directory with Node.js 24 and npm installed:

```powershell
npm install
$env:SIDELANTERN_ENGINE_DIR = 'C:\Program Files\Sunshine'
npm start
```

Set the engine directory to the location containing `sunshine.exe` and
`assets`. A development launcher can also use the official Electron 45.4.1
executable directly by passing this directory as its app argument.

SideLantern shows the address and test port for manual entry in Moonlight. If the
supplied Sunshine build uses the older pairing interface, enter the Moonlight
PIN with the manual pairing control. Builds with the current API show each
pending request individually.

Closing or minimizing the window hides it to the tray. The tray menu can
reopen SideLantern or exit and stop the private engine. Application settings
include the theme and the private engine log directory.

## Test

```powershell
npm test
npm start -- --smoke-test
```

Set `SIDELANTERN_DATA_DIR` to a new directory to exercise a fresh profile.
The smoke test opens the renderer, starts the private engine, checks the
authenticated management interface and verifies that a theme preference is
persisted. It then shuts down the private engine. Use a standard Windows
desktop session for tests that launch child processes.

For a fresh profile in each run, test both graceful and forced desktop exits:

```powershell
.\test\windows-smoke.ps1 -ElectronPath .\node_modules\electron\dist\electron.exe -EngineDirectory $env:SIDELANTERN_ENGINE_DIR -OutputDirectory .\smoke-output
```

The development runner builds its shutdown helper with the Windows x64
.NET Framework C# compiler.

## Migrate an existing profile

The export produces an encrypted local backup tied to the current Windows
account. Supply the original Sunshine `config` directory and a new backup
path. Quit SideLantern through the tray before migrating. Import with a fresh
SideLantern profile before first launch.

```powershell
npm start -- --export-profile 'C:\Program Files\Sunshine\config' 'C:\Users\me\Documents\sunshine.slbak'
$env:SIDELANTERN_DATA_DIR = 'C:\Users\me\AppData\Roaming\SideLantern-New'
npm start -- --import-profile 'C:\Users\me\Documents\sunshine.slbak'
```

Export reads the old installation and reports missing application paths. The
backup includes the host identity, paired clients, application list, and
supported PNG covers. SideLantern uses its own management credentials and
stream settings. An existing destination profile is left in place.

## Native engine and installer

Build the fork with its Windows UCRT64 toolchain. The following turns off the
upstream WiX configuration and tray when building for SideLantern's installer:

```bash
cmake -S . -B cmake-build-sidelantern -G Ninja -DBUILD_DOCS=OFF -DBUILD_TESTS=ON \
  -DSUNSHINE_BUILD_WIX_PACKAGE=OFF -DSUNSHINE_ENABLE_TRAY=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-sidelantern --target sunshine sunshinesvc dxgi-info audio-info web-ui test_sunshine
cmake --install cmake-build-sidelantern --prefix path/to/engine-stage
```

Run these commands from the repository root in UCRT64. The install command
copies files into the staging directory. Use that directory as the engine input
for the [SideLantern installer builder](installer/README.md).

On Windows, run `node desktop/test/moonlight-smoke.cjs <engine-stage> <Moonlight.exe> <output-dir>`
with the original portable Moonlight distribution. The test uses two temporary
client profiles and a separate host on port 49189. It checks that each client
receives desktop video and that unpairing one leaves the other session active.

The fork reports active clients, actual capture output and Windows display IDs
through authenticated management endpoints. SideLantern can route to an
already-extended MttVDD output when it can resolve the output's engine ID.
Enabling and changing a shared MttVDD display remains in Windows or the driver
control panel. Old Sunshine builds still support the basic management and PIN
flows.

Application Settings supports English and Simplified Chinese. The desktop
interface and tray switch immediately and retain the selected language.
Installed-service validation and ownership of a bundled virtual display are
tracked in the [implementation plan](../docs/superpowers/plans/2026-11-19-side-lantern-mvp.md).
