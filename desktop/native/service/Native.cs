using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace SideLantern.HostService
{
    internal static class Native
    {
        internal const uint Running = 258;
        internal const uint Synchronize = 0x00100000;
        internal const uint QueryProcess = 0x1000;
        internal const uint TokenQuery = 0x0008;
        internal const uint NoWindow = 0x08000000;
        internal const uint Suspended = 0x00000004;
        internal const uint UnicodeEnvironment = 0x00000400;
        private const uint KeepJobLifetime = 0x00002000;
        private const int JobExtendedLimits = 9;
        private const int TokenElevation = 20;

        [StructLayout(LayoutKind.Sequential)]
        private struct SecurityAttributes
        {
            internal int Length;
            internal IntPtr Descriptor;
            internal int Inheritable;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct StartupInfo
        {
            internal int Size;
            internal string Reserved, Desktop, Title;
            internal int X, Y, Width, Height, CharsX, CharsY, Fill, Flags;
            internal short Show, ReservedSize;
            internal IntPtr ReservedPointer, Input, Output, Error;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessInformation
        {
            internal IntPtr Process, Thread;
            internal uint ProcessId, ThreadId;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct BasicLimits
        {
            internal long ProcessTime, JobTime;
            internal uint Flags;
            internal UIntPtr MinimumWorkingSet, MaximumWorkingSet;
            internal uint ActiveProcesses;
            internal UIntPtr Affinity;
            internal uint Priority, Scheduling;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct IoCounters
        {
            internal ulong ReadOperations, WriteOperations, OtherOperations;
            internal ulong ReadBytes, WriteBytes, OtherBytes;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JobLimits
        {
            internal BasicLimits Basic;
            internal IoCounters Counters;
            internal UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafePipeHandle CreateNamedPipe(string name, uint openMode, uint pipeMode,
            uint maxInstances, uint outputSize, uint inputSize, uint timeout, ref SecurityAttributes security);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool GetNamedPipeClientProcessId(SafePipeHandle pipe, out uint processId);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool GetNamedPipeClientSessionId(SafePipeHandle pipe, out uint sessionId);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern SafeFileHandle OpenProcess(uint access, bool inheritHandle, uint processId);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern bool QueryFullProcessImageName(SafeFileHandle process, uint flags, StringBuilder name, ref int size);

        [DllImport("advapi32.dll", SetLastError = true)]
        internal static extern bool OpenProcessToken(SafeFileHandle process, uint access, out SafeFileHandle token);

        [DllImport("wtsapi32.dll", SetLastError = true)]
        internal static extern bool WTSQueryUserToken(uint sessionId, out SafeFileHandle token);

        [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool WTSQuerySessionInformation(IntPtr server, uint sessionId, int information,
            out IntPtr buffer, out uint size);

        [DllImport("wtsapi32.dll")]
        private static extern void WTSFreeMemory(IntPtr buffer);

        [DllImport("advapi32.dll", SetLastError = true)]
        private static extern bool GetTokenInformation(SafeFileHandle token, int information, out int value, int size, out int required);

        [DllImport("userenv.dll", SetLastError = true)]
        private static extern bool CreateEnvironmentBlock(out IntPtr environment, SafeFileHandle token, bool inherit);

        [DllImport("userenv.dll")]
        private static extern bool DestroyEnvironmentBlock(IntPtr environment);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern SafeFileHandle CreateJobObject(IntPtr attributes, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetInformationJobObject(SafeFileHandle job, int kind, ref JobLimits information, int length);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool AssignProcessToJobObject(SafeFileHandle job, SafeFileHandle process);

        [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern bool CreateProcessAsUser(SafeFileHandle token, string application, StringBuilder commandLine,
            IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles, uint flags, IntPtr environment,
            string workingDirectory, ref StartupInfo startup, out ProcessInformation process);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern uint ResumeThread(SafeFileHandle thread);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern uint WaitForSingleObject(SafeFileHandle process, uint milliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool TerminateProcess(SafeFileHandle process, uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool GetExitCodeProcess(SafeFileHandle process, out uint exitCode);

        internal static NamedPipeServerStream CreateControlPipe(SecurityIdentifier owner,
            string name = @"\\.\pipe\SideLantern.Host.v1")
        {
            const uint duplex = 3;
            const uint overlapped = 0x40000000;
            const uint firstInstance = 0x00080000;
            const uint rejectRemote = 0x00000008;
            var security = new PipeSecurity();
            var system = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
            security.AddAccessRule(new PipeAccessRule(system, PipeAccessRights.FullControl, AccessControlType.Allow));
            // Windows GENERIC_WRITE includes FILE_CREATE_PIPE_INSTANCE. The first-instance
            // flag and single-instance limit keep it exclusive while the service is running.
            security.AddAccessRule(new PipeAccessRule(owner,
                PipeAccessRights.ReadWrite | PipeAccessRights.CreateNewInstance | PipeAccessRights.Synchronize,
                AccessControlType.Allow));
            var descriptor = GCHandle.Alloc(security.GetSecurityDescriptorBinaryForm(), GCHandleType.Pinned);
            try
            {
                var attrs = new SecurityAttributes {
                    Length = Marshal.SizeOf(typeof(SecurityAttributes)), Descriptor = descriptor.AddrOfPinnedObject()
                };
                var pipe = CreateNamedPipe(name, duplex | overlapped | firstInstance,
                    rejectRemote, 1, 128, 128, 0, ref attrs);
                if (pipe.IsInvalid)
                {
                    int error = Marshal.GetLastWin32Error();
                    pipe.Dispose();
                    throw new Win32Exception(error);
                }
                return new NamedPipeServerStream(PipeDirection.InOut, true, false, pipe);
            }
            finally { descriptor.Free(); }
        }

        internal static string ImagePath(SafeFileHandle process)
        {
            var name = new StringBuilder(32768);
            int length = name.Capacity;
            if (!QueryFullProcessImageName(process, 0, name, ref length)) ThrowLastError();
            return name.ToString();
        }

        internal static SecurityIdentifier User(SafeFileHandle token)
        {
            using (var identity = new WindowsIdentity(token.DangerousGetHandle()))
                return identity.User;
        }

        internal static bool IsElevated(SafeFileHandle token)
        {
            int elevated, required;
            if (!GetTokenInformation(token, TokenElevation, out elevated, sizeof(int), out required)) ThrowLastError();
            return elevated != 0;
        }

        internal static bool IsActiveSession(uint sessionId)
        {
            const int connectState = 8;
            IntPtr buffer;
            uint length;
            if (!WTSQuerySessionInformation(IntPtr.Zero, sessionId, connectState, out buffer, out length))
                return false;
            try { return length >= sizeof(int) && Marshal.ReadInt32(buffer) == 0; }
            finally { WTSFreeMemory(buffer); }
        }

        internal static SortedDictionary<string, string> EnvironmentFor(SafeFileHandle token)
        {
            IntPtr block;
            if (!CreateEnvironmentBlock(out block, token, false)) ThrowLastError();
            var values = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            try
            {
                int offset = 0;
                while (true)
                {
                    string entry = Marshal.PtrToStringUni(IntPtr.Add(block, offset));
                    if (entry.Length == 0) break;
                    int separator = entry.IndexOf('=', 1);
                    if (separator > 0) values[entry.Substring(0, separator)] = entry.Substring(separator + 1);
                    offset += (entry.Length + 1) * sizeof(char);
                }
            }
            finally { DestroyEnvironmentBlock(block); }
            return values;
        }

        internal static SafeFileHandle CreateKillOnCloseJob()
        {
            var job = CreateJobObject(IntPtr.Zero, null);
            try
            {
                if (job.IsInvalid) ThrowLastError();
                var limits = new JobLimits();
                limits.Basic.Flags = KeepJobLifetime;
                if (!SetInformationJobObject(job, JobExtendedLimits, ref limits, Marshal.SizeOf(typeof(JobLimits))))
                    ThrowLastError();
                return job;
            }
            catch
            {
                job.Dispose();
                throw;
            }
        }

        internal sealed class Child : IDisposable
        {
            internal readonly SafeFileHandle Process;
            internal readonly SafeFileHandle Thread;
            internal readonly uint Id;

            internal Child(IntPtr process, IntPtr thread, uint id)
            {
                Process = new SafeFileHandle(process, true);
                Thread = new SafeFileHandle(thread, true);
                Id = id;
            }

            public void Dispose()
            {
                Thread.Dispose();
                Process.Dispose();
            }
        }

        internal static Child Launch(SafeFileHandle token, string application, IEnumerable<string> args,
            SortedDictionary<string, string> environment, uint flags)
        {
            var block = new StringBuilder();
            foreach (var entry in environment)
                block.Append(entry.Key).Append('=').Append(entry.Value).Append('\0');
            block.Append('\0');
            var data = GCHandle.Alloc(block.ToString().ToCharArray(), GCHandleType.Pinned);
            try
            {
                var command = new StringBuilder(HostProtocol.Quote(application));
                foreach (string arg in args) command.Append(' ').Append(HostProtocol.Quote(arg));
                var startup = new StartupInfo {
                    Size = Marshal.SizeOf(typeof(StartupInfo)), Desktop = @"winsta0\default"
                };
                ProcessInformation process;
                if (!CreateProcessAsUser(token, application, command, IntPtr.Zero, IntPtr.Zero, false,
                    flags | UnicodeEnvironment, data.AddrOfPinnedObject(), Path.GetDirectoryName(application),
                    ref startup, out process)) ThrowLastError();
                return new Child(process.Process, process.Thread, process.ProcessId);
            }
            finally { data.Free(); }
        }

        internal static void ThrowLastError() { throw new Win32Exception(Marshal.GetLastWin32Error()); }
    }
}
