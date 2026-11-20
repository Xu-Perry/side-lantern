#if HOST_SERVICE_SELF_TEST
using System;
using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Principal;
using Microsoft.Win32.SafeHandles;

namespace SideLantern.HostService
{
    internal static class SelfTest
    {
        [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CommandLineToArgvW(string command, out int count);

        [DllImport("kernel32.dll")]
        private static extern IntPtr LocalFree(IntPtr memory);

        private static void Require(bool result, string detail)
        {
            if (!result) throw new InvalidOperationException(detail);
        }

        private static void CheckArguments()
        {
            int parsed;
            Require(HostProtocol.TryPort("OPEN 47989", out parsed) && parsed == 47989, "valid port");
            foreach (string invalid in new[] { "OPEN 65515", "OPEN 1028", "OPEN -1", "OPEN 47989\nSTOP", "OPEN 47989 ", "STOP" })
                Require(!HostProtocol.TryPort(invalid, out parsed), "rejected port command");

            var input = new[] { @"C:\Program Files\SideLantern\SideLantern.exe", "", "space quotation \"here\"", @"C:\trailing\" };
            var quoted = Array.ConvertAll(input, HostProtocol.Quote);
            int count;
            IntPtr argv = CommandLineToArgvW(String.Join(" ", quoted), out count);
            Require(argv != IntPtr.Zero, "decode arguments");
            try
            {
                Require(count == input.Length, "argument count");
                for (int index = 0; index < count; index++)
                    Require(input[index] == Marshal.PtrToStringUni(Marshal.ReadIntPtr(argv, index * IntPtr.Size)), "quoted argument");
            }
            finally { LocalFree(argv); }
        }

        private static void CheckPipe(Process peer)
        {
            string name = "SideLantern.SelfTest." + Guid.NewGuid().ToString("N");
            using (var person = WindowsIdentity.GetCurrent())
            using (var server = Native.CreateControlPipe(person.User, @"\\.\pipe\" + name))
            using (var client = new NamedPipeClientStream(".", name, PipeDirection.InOut))
            {
                var wait = server.BeginWaitForConnection(null, null);
                client.Connect(2000);
                server.EndWaitForConnection(wait);
                uint pid;
                Require(Native.GetNamedPipeClientProcessId(server.SafePipeHandle, out pid) && pid == peer.Id, "kernel peer PID");
                uint session;
                Require(Native.GetNamedPipeClientSessionId(server.SafePipeHandle, out session) && session == peer.SessionId, "kernel peer session");
                client.WriteByte(23);
                Require(server.ReadByte() == 23, "read/write pipe access");
            }
        }

        private static void CheckDefaultPipe()
        {
            string name = "SideLantern.DefaultPipeTest." + Guid.NewGuid().ToString("N");
            using (var server = new NamedPipeServerStream(name, PipeDirection.InOut))
            using (var client = new NamedPipeClientStream(".", name, PipeDirection.InOut))
                client.Connect(2000);
        }

        private static void CheckJob(Process executable)
        {
            var start = new ProcessStartInfo(executable.MainModule.FileName, "--self-test-worker") {
                UseShellExecute = false, CreateNoWindow = true
            };
            using (var job = Native.CreateKillOnCloseJob())
            using (var child = Process.Start(start))
            {
                try
                {
                    using (var handle = new SafeFileHandle(child.Handle, false))
                        Require(Native.AssignProcessToJobObject(job, handle), "assign child to job");
                    job.Dispose();
                    Require(child.WaitForExit(2500), "job closure terminates child");
                }
                finally
                {
                    if (!child.HasExited) child.Kill();
                }
            }
        }

        internal static int Run(string type)
        {
            try
            {
                using (var self = Process.GetCurrentProcess())
                {
                    switch (type)
                    {
                        case "arguments": CheckArguments(); break;
                        case "pipe": CheckPipe(self); break;
                        case "default-pipe": CheckDefaultPipe(); break;
                        case "job": CheckJob(self); break;
                        default: return 87;
                    }
                }
                Console.WriteLine("SideLantern service self-test passed (" + type + ").");
                return 0;
            }
            catch (Exception error)
            {
                Console.Error.WriteLine(error.ToString());
                return 1;
            }
        }
    }
}
#endif
