# SideLantern Windows host service

Build the x64 .NET Framework executable with:

```powershell
.\desktop\native\service\build.ps1 -OutFile .\cmake-build-release\SideLanternService.exe
```

The package installs these executable paths under the same, administrator-writable Program Files directory.

| Component | Location |
| --- | --- |
| Electron main process | %ProgramFiles%\SideLantern\SideLantern.exe |
| Service process | %ProgramFiles%\SideLantern\resources\SideLanternService.exe |
| Sunshine engine | %ProgramFiles%\SideLantern\resources\engine\sunshine.exe |

Register SideLanternService as a demand-start LocalSystem service. Its binary path includes the persistent arguments "--owner-sid" and the intended desktop user's account SID. Retain the default SYSTEM and administrator rights. The owner needs SERVICE_QUERY_STATUS and SERVICE_START. The installer may create a SideLanternService source in the Application Event Log to capture lifecycle messages. Preserve Program Files ACLs and use a separate configuration from an existing Sunshine install.

Before launch, Electron prepares a private profile under %LOCALAPPDATA%\SideLantern\host\config and checks the engine ports. The service sets SIDELANTERN_ENGINE_CONFIG_DIR to that directory when it launches the packaged Sunshine engine. The engine must use the fork's configuration directory override.

Electron opens a single-owner local named pipe and requests the configured engine port. The service validates the kernel-supplied peer PID and active session ID, trusted Electron image path and account SID. It launches Sunshine using the interactive user's non-elevated token and places the engine and its descendants in a kill-on-close job before resuming it. Closing the desktop process or channel, leaving the session, or explicitly stopping the host ends that job. A bounded graceful shutdown runs in the engine's session first.

The desktop integration awaits launchServiceEngine({port}) from desktop/lib/windows-service.cjs. Its returned object provides exited, failure() and stop() like the development runner. A successful launch confirms that the child was created. The desktop still verifies the pinned management endpoint before reporting readiness.

## Tests without SCM registration

```powershell
node --test --test-isolation=none desktop/test/windows-service.test.cjs
.\desktop\native\service\build.ps1 -IncludeSelfTests -OutFile .\cmake-build-release\SideLanternService.Test.exe
.\cmake-build-release\SideLanternService.Test.exe --self-test arguments
.\cmake-build-release\SideLanternService.Test.exe --self-test pipe
.\cmake-build-release\SideLanternService.Test.exe --self-test job
```

The pipe test needs local Windows named-pipe client access. The test builds create a unique pipe and start a short-lived copy of the test executable to check kernel job cleanup. Installer acceptance must verify service permissions, startup as the intended user, stop and restart, and host cleanup after killing that user's Electron process.
