using System;
using System.Collections;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Principal;
using System.ServiceProcess;
using System.Text;
using Microsoft.Win32;

internal static class SetupSupport
{
    private const string ServiceName = "SideLanternService";
    private const string SetupKey = @"SOFTWARE\SideLantern";
    private const string SetupIdentifier = "SideLantern.Windows.x64";
    private const string FirewallGroup = "SideLantern Windows Host";
    private const string TcpRule = "SideLantern-Host-In-TCP";
    private const string UdpRule = "SideLantern-Host-In-UDP";
    private const string ViGemSha256 = "89220a7865076b342892f98865f3499fb7c4cfd673159e89d352c360fd014c6a";

    private static readonly string InstallPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "SideLantern");
    private static readonly string AppPath = Path.Combine(InstallPath, "SideLantern.exe");
    private static readonly string ServicePath = Path.Combine(InstallPath, "resources", "SideLanternService.exe");
    private static readonly string EnginePath = Path.Combine(InstallPath, "resources", "engine", "sunshine.exe");
    private static readonly string SetupPath = Path.Combine(InstallPath, "resources", "SetupSupport.exe");
    private static readonly string ViGemInstaller = Path.Combine(InstallPath, "resources", "ViGEmBusSetup.exe");

    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool WTSQuerySessionInformation(IntPtr server, int sessionId, int infoClass, out IntPtr buffer, out int bytes);

    [DllImport("wtsapi32.dll")]
    private static extern void WTSFreeMemory(IntPtr buffer);

    private static string SessionValue(int session, int infoClass)
    {
        IntPtr buffer;
        int bytes;
        if (!WTSQuerySessionInformation(IntPtr.Zero, session, infoClass, out buffer, out bytes))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try { return Marshal.PtrToStringUni(buffer) ?? ""; }
        finally { WTSFreeMemory(buffer); }
    }

    private static string OwnerSid()
    {
        int session = Process.GetCurrentProcess().SessionId;
        string name = SessionValue(session, 5); // WTSUserName
        string domain = SessionValue(session, 7); // WTSDomainName
        if (session == 0 || String.IsNullOrWhiteSpace(name))
            throw new InvalidOperationException("Run Setup from the desktop account that will use SideLantern.");
        return new NTAccount(domain, name).Translate(typeof(SecurityIdentifier)).Value;
    }

    private static string Quote(string argument)
    {
        var result = new StringBuilder("\"");
        int slashes = 0;
        foreach (char value in argument)
        {
            if (value == '\\') { slashes++; continue; }
            if (value == '"') result.Append('\\', slashes * 2 + 1);
            else result.Append('\\', slashes);
            result.Append(value);
            slashes = 0;
        }
        result.Append('\\', slashes * 2);
        return result.Append('"').ToString();
    }

    private static int Run(string executable, int timeoutMilliseconds, params string[] args)
    {
        using (var process = Process.Start(new ProcessStartInfo(executable, String.Join(" ", args.Select(Quote)))
        {
            UseShellExecute = false, CreateNoWindow = true,
            WorkingDirectory = Environment.SystemDirectory
        }))
        {
            if (!process.WaitForExit(timeoutMilliseconds))
                throw new System.TimeoutException(Path.GetFileName(executable) + " did not finish.");
            return process.ExitCode;
        }
    }

    private static void Sc(params string[] args)
    {
        int code = Run(Path.Combine(Environment.SystemDirectory, "sc.exe"), 15000, args);
        if (code != 0) throw new Win32Exception(code, "Windows service command " + args[0] + " failed (" + code + ").");
    }

    private static string ServiceCommand(string sid)
    {
        return Quote(ServicePath) + " --owner-sid " + new SecurityIdentifier(sid).Value;
    }

    private static string ServiceImage()
    {
        using (var key = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\" + ServiceName))
        {
            if (key == null) return null;
            return (key.GetValue("ImagePath", "", RegistryValueOptions.DoNotExpandEnvironmentNames) as string) ?? "";
        }
    }

    private static void RequirePath(string path)
    {
        if (!File.Exists(path) && !Directory.Exists(path))
            throw new FileNotFoundException("SideLantern Setup input is missing.", path);
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidOperationException("SideLantern Setup cannot use a redirected installation path.");
    }

    private static void VerifyPackage(bool requirePayload)
    {
        if (!String.Equals(Path.GetFullPath(typeof(SetupSupport).Assembly.Location), SetupPath, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Unexpected path for the SideLantern Setup helper.");
        RequirePath(InstallPath);
        RequirePath(Path.Combine(InstallPath, "resources"));
        if (requirePayload)
        {
            RequirePath(AppPath);
            RequirePath(ServicePath);
            RequirePath(Path.GetDirectoryName(EnginePath));
            RequirePath(EnginePath);
        }
    }

    private static dynamic Rules()
    {
        dynamic policy = Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FwPolicy2", true));
        return policy.Rules;
    }

    private static bool VerifyRule(dynamic rules, string name, bool allowOwned)
    {
        bool found = false;
        foreach (dynamic rule in (IEnumerable)rules)
        {
            if (!String.Equals((string)rule.Name, name, StringComparison.OrdinalIgnoreCase)) continue;
            if (!allowOwned || !String.Equals((string)rule.ApplicationName, EnginePath, StringComparison.OrdinalIgnoreCase) ||
                !String.Equals((string)rule.Grouping, FirewallGroup, StringComparison.Ordinal))
                throw new InvalidOperationException("A different application owns the firewall rule " + name + ".");
            found = true;
        }
        return found;
    }

    private static void AddRule(dynamic rules, string name, int protocol, string ports)
    {
        dynamic rule = Activator.CreateInstance(Type.GetTypeFromProgID("HNetCfg.FWRule", true));
        rule.Name = name;
        rule.Grouping = FirewallGroup;
        rule.Description = "Allow SideLantern streaming from the local private network.";
        rule.ApplicationName = EnginePath;
        rule.Protocol = protocol;
        rule.LocalPorts = ports;
        rule.RemoteAddresses = "LocalSubnet";
        rule.Direction = 1; // inbound
        rule.Action = 1; // allow
        rule.Profiles = 2; // private network
        rule.Enabled = true;
        rules.Add(rule);
    }

    private static void Install()
    {
        VerifyPackage(true);
        using (var marker = Registry.LocalMachine.OpenSubKey(SetupKey))
        {
            if (marker != null || ServiceImage() != null)
                throw new InvalidOperationException("An installation already uses the SideLantern service or Setup registration.");
        }
        string owner = OwnerSid();
        dynamic rules = Rules();
        VerifyRule(rules, TcpRule, false);
        VerifyRule(rules, UdpRule, false);
        bool service = false, tcp = false, udp = false, createdMarker = false;
        try
        {
            using (var marker = Registry.LocalMachine.CreateSubKey(SetupKey))
            {
                createdMarker = true;
                marker.SetValue("SetupIdentifier", SetupIdentifier);
                marker.SetValue("OwnerSid", owner);
            }
            Sc("create", ServiceName, "binPath=", ServiceCommand(owner), "start=", "demand",
                "obj=", "LocalSystem", "DisplayName=", "SideLantern Streaming Service");
            service = true;
            // LC is QUERY_STATUS and RP is START. Only SYSTEM and Administrators have full control.
            Sc("sdset", ServiceName, "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;LCRP;;;" + owner + ")");
            AddRule(rules, TcpRule, 6, "47984,47989,48010");
            tcp = true;
            AddRule(rules, UdpRule, 17, "47998-48000,48002");
            udp = true;
        }
        catch (Exception cause)
        {
            bool rolledBack = true;
            if (udp) rolledBack &= TryCleanup(() => rules.Remove(UdpRule));
            if (tcp) rolledBack &= TryCleanup(() => rules.Remove(TcpRule));
            if (service) rolledBack &= TryCleanup(() => Sc("delete", ServiceName));
            if (rolledBack && createdMarker)
                rolledBack = TryCleanup(() => Registry.LocalMachine.DeleteSubKey(SetupKey, false));
            if (!rolledBack)
                throw new RecoveryRequiredException("Setup could not roll back all components. Run SideLantern Uninstall before retrying.", cause);
            throw;
        }
    }

    private sealed class RecoveryRequiredException : Exception
    {
        public RecoveryRequiredException(string message, Exception inner) : base(message, inner) {}
    }

    private static bool TryCleanup(Action cleanup)
    {
        try { cleanup(); return true; }
        catch (Exception error) { Console.Error.WriteLine(error.Message); return false; }
    }

    private static void Uninstall()
    {
        VerifyPackage(false);
        foreach (var process in Process.GetProcessesByName("SideLantern"))
        {
            using (process)
            {
                if (String.Equals(process.MainModule.FileName, AppPath, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("Exit SideLantern from the tray before uninstalling.");
            }
        }
        string owner;
        using (var marker = Registry.LocalMachine.OpenSubKey(SetupKey))
        {
            if (marker == null || !String.Equals(marker.GetValue("SetupIdentifier") as string, SetupIdentifier, StringComparison.Ordinal))
                throw new InvalidOperationException("SideLantern setup registration could not be verified.");
            owner = marker.GetValue("OwnerSid") as string;
        }
        string image = ServiceImage();
        if (image != null && !String.Equals(image, ServiceCommand(owner), StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The registered Windows service is not owned by this SideLantern installation.");
        dynamic rules = Rules();
        bool hasTcpRule = VerifyRule(rules, TcpRule, true);
        bool hasUdpRule = VerifyRule(rules, UdpRule, true);
        if (image != null)
        {
            using (var service = new ServiceController(ServiceName))
            {
                if (service.Status != ServiceControllerStatus.Stopped)
                {
                    if (service.Status != ServiceControllerStatus.StopPending) service.Stop();
                    service.WaitForStatus(ServiceControllerStatus.Stopped, TimeSpan.FromSeconds(20));
                }
            }
            Sc("delete", ServiceName);
        }
        if (hasTcpRule) rules.Remove(TcpRule);
        if (hasUdpRule) rules.Remove(UdpRule);
        Registry.LocalMachine.DeleteSubKey(SetupKey, false);
    }

    private static int InstallViGemBus()
    {
        VerifyPackage(true);
        string driver = Path.Combine(Environment.SystemDirectory, "drivers", "ViGEmBus.sys");
        using (var existing = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\ViGEmBus"))
        {
            if (existing != null || File.Exists(driver))
            {
                if (existing != null && File.Exists(driver))
                {
                    var version = FileVersionInfo.GetVersionInfo(driver);
                    if (version.FileMajorPart > 1 || (version.FileMajorPart == 1 && version.FileMinorPart >= 17))
                        return 0;
                }
                return 21; // No unattended replacement of an existing shared driver.
            }
        }
        RequirePath(ViGemInstaller);
        using (var file = File.OpenRead(ViGemInstaller))
        using (var sha = SHA256.Create())
        {
            string digest = BitConverter.ToString(sha.ComputeHash(file)).Replace("-", "");
            if (!String.Equals(digest, ViGemSha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("The bundled ViGEmBus installer did not pass verification.");
        }
        // ViGEmBus 1.22.0 deployment switches published in its Windows Package Manager manifest.
        return Run(ViGemInstaller, 150000, "/exenoui", "/qn", "/norestart");
    }

    private static int Main(string[] args)
    {
        if (args.Length != 1) return 2;
        try
        {
            switch (args[0])
            {
                case "install": Install(); return 0;
                case "uninstall": Uninstall(); return 0;
                case "vigem": return InstallViGemBus();
                default: return 2;
            }
        }
        catch (RecoveryRequiredException error)
        {
            Console.Error.WriteLine(error.InnerException.Message);
            Console.Error.WriteLine(error.Message);
            return 3;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error.Message);
            return 1;
        }
    }
}
