using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.ServiceProcess;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace SideLantern.HostService
{
    internal sealed class SideLanternService : ServiceBase
    {
        internal const string Name = "SideLanternService";
        private const uint InvalidEngineExit = 1067;
        private static readonly string InstallDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "SideLantern");
        private static readonly string ServiceBinary = Path.Combine(InstallDirectory, "resources", Name + ".exe");
        private static readonly string DesktopBinary = Path.Combine(InstallDirectory, "SideLantern.exe");
        private static readonly string EngineBinary = Path.Combine(InstallDirectory, "resources", "engine", "sunshine.exe");

        private readonly SecurityIdentifier owner;
        private readonly ManualResetEvent stop = new ManualResetEvent(false);
        private volatile uint sessionId;
        private volatile bool externalStop;
        private Thread worker;

        private SideLanternService(SecurityIdentifier user)
        {
            owner = user;
            ServiceName = Name;
            CanStop = true;
            CanShutdown = true;
            CanHandleSessionChangeEvent = true;
            AutoLog = false;
        }

        private static int Main(string[] args)
        {
            uint target;
#if HOST_SERVICE_SELF_TEST
            if (args.Length == 2 && args[0] == "--self-test")
                return SelfTest.Run(args[1]);
            if (args.Length == 1 && args[0] == "--self-test-worker")
            {
                Thread.Sleep(15000);
                return 0;
            }
#endif
            if (args.Length == 2 && args[0] == "--stop-engine" &&
                UInt32.TryParse(args[1], NumberStyles.None, CultureInfo.InvariantCulture, out target))
                return SignalEngine(target);

            if (args.Length != 2 || args[0] != "--owner-sid") return 87;
            try
            {
                var owner = new SecurityIdentifier(args[1]);
                if (!owner.IsAccountSid() || owner.IsWellKnown(WellKnownSidType.LocalSystemSid)) return 87;
                string ownPath;
                using (var current = Process.GetCurrentProcess())
                    ownPath = current.MainModule.FileName;
                if (!String.Equals(ServiceBinary, ownPath, StringComparison.OrdinalIgnoreCase) ||
                    !ProtectedFilesExist(ServiceBinary) || !ProtectedFilesExist(DesktopBinary) || !ProtectedFilesExist(EngineBinary))
                    return 5;
                using (var service = new SideLanternService(owner)) Run(service);
                return 0;
            }
            catch (ArgumentException) { return 87; }
            catch (IOException) { return 5; }
            catch (UnauthorizedAccessException) { return 5; }
        }

        private static bool ProtectedFilesExist(string file)
        {
            if (!File.Exists(file)) return false;
            for (string entry = file; !String.IsNullOrEmpty(entry); entry = Path.GetDirectoryName(entry))
                if ((File.GetAttributes(entry) & FileAttributes.ReparsePoint) != 0) return false;
            return true;
        }

        protected override void OnStart(string[] args)
        {
            worker = new Thread(RunOnce) { IsBackground = true };
            worker.Start();
        }

        protected override void OnStop()
        {
            externalStop = true;
            stop.Set();
            if (worker != null && worker != Thread.CurrentThread) worker.Join(18000);
        }

        protected override void OnShutdown() { OnStop(); }

        protected override void OnSessionChange(SessionChangeDescription change)
        {
            if (sessionId == 0 || change.SessionId != (int)sessionId) return;
            if (change.Reason == SessionChangeReason.SessionLogoff ||
                change.Reason == SessionChangeReason.ConsoleDisconnect ||
                change.Reason == SessionChangeReason.RemoteDisconnect)
                stop.Set();
        }

        private void RunOnce()
        {
            try
            {
                using (var pipe = Native.CreateControlPipe(owner))
                {
                    var pending = pipe.BeginWaitForConnection(null, null);
                    using (var connected = pending.AsyncWaitHandle)
                    {
                        if (WaitHandle.WaitAny(new WaitHandle[] { stop, connected }, 17000) != 1) return;
                        pipe.EndWaitForConnection(pending);
                    }
                    try { RunConnection(pipe); }
                    catch (Win32Exception error)
                    {
                        if (!stop.WaitOne(0))
                        {
                            Send(pipe, "ERROR " + error.NativeErrorCode.ToString(CultureInfo.InvariantCulture));
                            Log(error.Message);
                        }
                    }
                    catch (IOException error) { if (!stop.WaitOne(0)) Log(error.Message); }
                }
            }
            catch (Exception error)
            {
                ExitCode = 1067;
                Log(error.Message);
            }
            finally { if (!externalStop) Stop(); }
        }

        private void RunConnection(NamedPipeServerStream pipe)
        {
            uint processId, actualSession;
            if (!Native.GetNamedPipeClientProcessId(pipe.SafePipeHandle, out processId)) Native.ThrowLastError();
            if (!Native.GetNamedPipeClientSessionId(pipe.SafePipeHandle, out actualSession)) Native.ThrowLastError();
            if (actualSession == 0 || !Native.IsActiveSession(actualSession)) throw new Win32Exception(5);

            using (var desktop = Native.OpenProcess(Native.Synchronize | Native.QueryProcess, false, processId))
            {
                if (desktop.IsInvalid) Native.ThrowLastError();
                if (!String.Equals(DesktopBinary, Native.ImagePath(desktop), StringComparison.OrdinalIgnoreCase))
                    throw new Win32Exception(5);
                SafeFileHandle desktopToken;
                if (!Native.OpenProcessToken(desktop, Native.TokenQuery, out desktopToken)) Native.ThrowLastError();
                using (desktopToken)
                    if (!owner.Equals(Native.User(desktopToken)) || Native.IsElevated(desktopToken))
                        throw new Win32Exception(5);
                SafeFileHandle engineToken;
                if (!Native.WTSQueryUserToken(actualSession, out engineToken)) Native.ThrowLastError();
                using (engineToken)
                {
                    if (!owner.Equals(Native.User(engineToken)) || Native.IsElevated(engineToken))
                        throw new Win32Exception(5);
                    sessionId = actualSession;
                    int port;
                    if (!HostProtocol.TryPort(ReadLine(pipe, desktop, null, 4500), out port))
                        throw new Win32Exception(87);
                    if (!Native.IsActiveSession(actualSession) || !Alive(desktop) || stop.WaitOne(0))
                        return;
                    RunEngine(pipe, desktop, engineToken, port);
                }
            }
        }

        private void RunEngine(NamedPipeServerStream pipe, SafeFileHandle desktop, SafeFileHandle token, int port)
        {
            var environment = Native.EnvironmentFor(token);
            string localAppData;
            if (!environment.TryGetValue("LOCALAPPDATA", out localAppData) ||
                localAppData.Length < 3 || localAppData[1] != ':' || localAppData[2] != '\\')
                throw new Win32Exception(87);
            environment["SIDELANTERN_ENGINE_CONFIG_DIR"] = Path.Combine(localAppData, "SideLantern", "host", "config");
            environment.Remove("ProgramData");
            environment.Remove("ELECTRON_RUN_AS_NODE");
            string[] args = {
                "port=" + port.ToString(CultureInfo.InvariantCulture), "origin_web_ui_allowed=pc",
                "system_tray=disabled", "upnp=disabled", "credentials_file=sl-management.json",
                "file_state=sunshine_state.json", "file_apps=apps.json", "pkey=credentials/cakey.pem",
                "cert=credentials/cacert.pem", "log_path=sunshine.log", "install_steam_audio_drivers=disabled",
                "nvenc_opengl_vulkan_on_dxgi=disabled"
            };

            using (var job = Native.CreateKillOnCloseJob())
            using (var engine = Native.Launch(token, EngineBinary, args, environment, Native.Suspended | Native.NoWindow))
            {
                if (!Native.AssignProcessToJobObject(job, engine.Process))
                {
                    int error = Marshal.GetLastWin32Error();
                    Native.TerminateProcess(engine.Process, InvalidEngineExit);
                    throw new Win32Exception(error);
                }
                if (!Alive(desktop) || stop.WaitOne(0) || !Native.IsActiveSession(sessionId)) return;
                if (Native.ResumeThread(engine.Thread) == UInt32.MaxValue) Native.ThrowLastError();

                Log("Engine started in the authenticated desktop session.");
                try
                {
                    Send(pipe, "READY");
                    ReadLine(pipe, desktop, engine.Process, -1);
                }
                catch (IOException) { }
                uint code = 0;
                if (Alive(engine.Process))
                    RequestGracefulStop(engine, token, environment);
                else if (!Native.GetExitCodeProcess(engine.Process, out code))
                    code = InvalidEngineExit;

                job.Dispose();
                if (Native.WaitForSingleObject(engine.Process, 3500) != 0) code = InvalidEngineExit;
                Log("Engine exited (" + code.ToString(CultureInfo.InvariantCulture) + ").");
                Send(pipe, "EXIT " + code.ToString(CultureInfo.InvariantCulture));
            }
        }

        private static void RequestGracefulStop(Native.Child engine, SafeFileHandle token,
            SortedDictionary<string, string> environment)
        {
            try
            {
                using (var helper = Native.Launch(token, ServiceBinary,
                    new[] { "--stop-engine", engine.Id.ToString(CultureInfo.InvariantCulture) },
                    environment, Native.NoWindow))
                {
                    if (Native.WaitForSingleObject(helper.Process, 1300) != 0)
                        Native.TerminateProcess(helper.Process, InvalidEngineExit);
                    uint result;
                    if (Native.GetExitCodeProcess(helper.Process, out result) && result == 0)
                        Native.WaitForSingleObject(engine.Process, 7000);
                }
            }
            catch (Win32Exception error) { Log(error.Message); }
        }

        private string ReadLine(NamedPipeServerStream pipe, SafeFileHandle desktop,
            SafeFileHandle engine, int timeout)
        {
            var result = new StringBuilder();
            var clock = Stopwatch.StartNew();
            var oneByte = new byte[1];
            while (result.Length < 32)
            {
                Task<int> read = pipe.ReadAsync(oneByte, 0, 1);
                while (!read.IsCompleted)
                {
                    if (stop.WaitOne(90) || !Alive(desktop) || (engine != null && !Alive(engine))
                        || (timeout >= 0 && clock.ElapsedMilliseconds >= timeout)) return null;
                }
                if (read.GetAwaiter().GetResult() == 0) return null;
                if (oneByte[0] == 10) return result.ToString();
                if (oneByte[0] < 32 || oneByte[0] > 126) return null;
                result.Append((char)oneByte[0]);
            }
            return null;
        }

        private static bool Alive(SafeFileHandle process)
        {
            return Native.WaitForSingleObject(process, 0) == Native.Running;
        }

        private static void Send(NamedPipeServerStream pipe, string line)
        {
            try
            {
                var payload = Encoding.ASCII.GetBytes(line + "\n");
                pipe.WriteAsync(payload, 0, payload.Length).Wait(1000);
            }
            catch (IOException) { }
            catch (AggregateException) { }
        }

        private static void Log(string message)
        {
            try
            {
                if (EventLog.SourceExists(Name)) EventLog.WriteEntry(Name, message, EventLogEntryType.Information);
            }
            catch (InvalidOperationException) { }
            catch (UnauthorizedAccessException) { }
            catch (System.Security.SecurityException) { }
            catch (Win32Exception) { }
        }

        private delegate bool WindowCallback(IntPtr window, IntPtr unused);
        [DllImport("user32.dll")]
        private static extern bool EnumWindows(WindowCallback callback, IntPtr unused);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr window, StringBuilder name, int capacity);
        [DllImport("user32.dll")]
        private static extern bool PostMessage(IntPtr window, uint command, IntPtr wParam, IntPtr lParam);

        private static int SignalEngine(uint target)
        {
            using (var process = Native.OpenProcess(Native.QueryProcess | Native.Synchronize, false, target))
            {
                if (process.IsInvalid || !Alive(process)) return 3;
                if (!String.Equals(Native.ImagePath(process), EngineBinary, StringComparison.OrdinalIgnoreCase)) return 5;
                bool posted = false;
                EnumWindows(delegate(IntPtr window, IntPtr unused)
                {
                    uint processId;
                    if (GetWindowThreadProcessId(window, out processId) == 0 || processId != target) return true;
                    var name = new StringBuilder(128);
                    if (GetClassName(window, name, name.Capacity) == 0 || name.ToString() != "SunshineSessionMonitorClass")
                        return true;
                    posted = PostMessage(window, 0x0016, new IntPtr(1), IntPtr.Zero);
                    return false;
                }, IntPtr.Zero);
                return posted ? 0 : 3;
            }
        }
    }
}
