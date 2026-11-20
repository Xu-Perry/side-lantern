using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

internal static class ShutdownHost
{
    private const uint WmEndSession = 0x0016;
    private const string MonitorWindow = "SunshineSessionMonitorClass";
    private delegate bool WindowCallback(IntPtr window, IntPtr unused);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(WindowCallback callback, IntPtr unused);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, StringBuilder name, int capacity);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    private static int Main(string[] args)
    {
        int processId;
        if (args.Length != 2 || !int.TryParse(args[0], out processId) || processId <= 0)
            return 1;

        try
        {
            using (Process engine = Process.GetProcessById(processId))
            {
                if (!String.Equals(engine.MainModule.FileName, Path.GetFullPath(args[1]), StringComparison.OrdinalIgnoreCase))
                    return 2;

                bool posted = false;
                EnumWindows(delegate(IntPtr window, IntPtr unused)
                {
                    uint owner;
                    if (GetWindowThreadProcessId(window, out owner) == 0 || owner != (uint)processId)
                        return true;

                    var name = new StringBuilder(128);
                    if (GetClassName(window, name, name.Capacity) == 0 || name.ToString() != MonitorWindow)
                        return true;

                    // Sunshine's session monitor handles this message by cleaning up and exiting.
                    posted = PostMessage(window, WmEndSession, new IntPtr(1), IntPtr.Zero);
                    return false;
                }, IntPtr.Zero);
                return posted ? 0 : 3;
            }
        }
        catch (System.ComponentModel.Win32Exception)
        {
            return 4;
        }
        catch (ArgumentException)
        {
            return 5;
        }
    }
}
