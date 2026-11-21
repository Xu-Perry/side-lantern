using System;
using System.IO;
using System.Reflection;

public static class SetupFirewallTest
{
    // Receive the real Windows firewall rule object without registering it with the firewall.
    public sealed class RecordingRules
    {
        public object Rule { get; private set; }
        public void Add(object rule) { Rule = rule; }
    }

    public static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("Usage: SetupFirewallTest.exe <path to SetupSupport.exe>");
            return 2;
        }
        try
        {
            var setup = Assembly.LoadFrom(Path.GetFullPath(args[0]));
            var add = setup.GetType("SetupSupport", true).GetMethod("AddRule", BindingFlags.Static | BindingFlags.NonPublic);
            string engine = Path.Combine(Path.GetDirectoryName(setup.Location), "engine", "sunshine.exe");
            Verify(add, engine, "SideLantern-Host-In-TCP", 6, "47984,47989,48010");
            Verify(add, engine, "SideLantern-Host-In-UDP", 17, "47998-48000,48002");
            Console.WriteLine("Both SideLantern rules allow direct streaming on all network profiles and scope access to the engine and streaming ports.");
            return 0;
        }
        catch (Exception error)
        {
            var cause = error.InnerException ?? error;
            Console.Error.WriteLine(cause.GetType().Name + ": " + cause.Message);
            return 1;
        }
    }

    private static void Verify(MethodInfo add, string engine, string name, int protocol, string ports)
    {
        var rules = new RecordingRules();
        add.Invoke(null, new object[] { rules, name, protocol, ports });
        dynamic rule = rules.Rule;
        if (rule.Name != name || rule.Grouping != "SideLantern Windows Host" || rule.ApplicationName != engine ||
            rule.Protocol != protocol || rule.LocalPorts != ports || rule.Direction != 1 || rule.Action != 1 ||
            rule.RemoteAddresses != "*" || rule.Profiles != 0x7fffffff || !rule.Enabled)
            throw new InvalidOperationException("Unexpected firewall defaults for " + name + " (remote addresses: " + rule.RemoteAddresses + ", profiles: " + rule.Profiles + ").");
    }
}
