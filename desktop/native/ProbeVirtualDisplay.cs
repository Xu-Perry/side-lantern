using System;
using System.ComponentModel;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

internal static class ProbeVirtualDisplay
{
    private const uint Active = 1;
    private const uint Primary = 4;
    private const uint GetMonitorInterface = 1;
    private const uint Present = 2;
    private const uint HardwareIdsProperty = 2;
    private const uint CurrentMode = UInt32.MaxValue;
    private const int InsufficientBuffer = 122;
    private const int NoMoreItems = 259;
    private static readonly IntPtr InvalidHandle = new IntPtr(-1);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DisplayDevice
    {
        public uint Size;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Interface;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Key;
    }

    // Only the DEVMODEW fields required for EnumDisplaySettingsW.
    [StructLayout(LayoutKind.Explicit, Size = 220)]
    private struct DisplayMode
    {
        [FieldOffset(68)] public ushort Size;
        [FieldOffset(172)] public uint Width;
        [FieldOffset(176)] public uint Height;
        [FieldOffset(184)] public uint RefreshRate;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceInfo
    {
        public uint Size;
        public Guid ClassId;
        public uint Node;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceInterface
    {
        public uint Size;
        public Guid ClassId;
        public uint Flags;
        public IntPtr Reserved;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumDisplayDevices(string name, uint index, ref DisplayDevice device, uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumDisplaySettings(string name, uint index, ref DisplayMode mode);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevs(ref Guid classId, string enumerator, IntPtr parent, uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiEnumDeviceInfo(IntPtr set, uint index, ref DeviceInfo info);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr classId, IntPtr parent);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiOpenDeviceInterface(IntPtr set, string path, uint flags, ref DeviceInterface data);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set, ref DeviceInterface data,
        IntPtr detail, uint detailBytes, out uint requiredBytes, ref DeviceInfo info);

    [DllImport("setupapi.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

    [DllImport("cfgmgr32.dll")]
    private static extern uint CM_Get_Parent(out uint parent, uint node, uint flags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    private static extern uint CM_Get_DevNode_Registry_Property(uint node, uint property,
        out uint propertyType, [Out] byte[] buffer, ref uint length, uint flags);

    private static bool IsMttAdapter(uint node)
    {
        var ids = new byte[4096];
        uint length = (uint)ids.Length;
        uint type;
        if (CM_Get_DevNode_Registry_Property(node, HardwareIdsProperty, out type, ids, ref length, 0) != 0 ||
            length > ids.Length)
            return false;

        foreach (string id in Encoding.Unicode.GetString(ids, 0, (int)length).Split('\0'))
        {
            if (String.Equals(id, @"Root\MttVDD", StringComparison.OrdinalIgnoreCase) ||
                String.Equals(id, "MttVDD", StringComparison.OrdinalIgnoreCase))
                return true;
        }
        return false;
    }

    internal static bool DriverIsPresent(uint flags)
    {
        var displayClass = new Guid("4d36e968-e325-11ce-bfc1-08002be10318");
        IntPtr set = SetupDiGetClassDevs(ref displayClass, null, IntPtr.Zero, flags);
        if (set == InvalidHandle)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            for (uint index = 0; ; index++)
            {
                var device = new DeviceInfo { Size = (uint)Marshal.SizeOf(typeof(DeviceInfo)) };
                if (!SetupDiEnumDeviceInfo(set, index, ref device))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error == NoMoreItems)
                        return false;
                    throw new Win32Exception(error);
                }
                if (IsMttAdapter(device.Node))
                    return true;
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
    }

    private static bool IsMttMonitor(IntPtr set, string name)
    {
        var data = new DeviceInterface { Size = (uint)Marshal.SizeOf(typeof(DeviceInterface)) };
        if (String.IsNullOrEmpty(name) || !SetupDiOpenDeviceInterface(set, name, 0, ref data))
            return false;

        var device = new DeviceInfo { Size = (uint)Marshal.SizeOf(typeof(DeviceInfo)) };
        uint requiredBytes;
        // A null detail buffer still fills the device info when the interface exists.
        SetupDiGetDeviceInterfaceDetail(set, ref data, IntPtr.Zero, 0, out requiredBytes, ref device);
        if (Marshal.GetLastWin32Error() != InsufficientBuffer || requiredBytes == 0)
            return false;

        uint parent;
        return CM_Get_Parent(out parent, device.Node, 0) == 0 && IsMttAdapter(parent);
    }

    private static bool IsExtendedMttOutput(IntPtr interfaces, string source)
    {
        bool found = false;
        for (uint index = 0; ; index++)
        {
            var target = new DisplayDevice { Size = (uint)Marshal.SizeOf(typeof(DisplayDevice)) };
            if (!EnumDisplayDevices(source, index, ref target, GetMonitorInterface))
                return found;
            if ((target.Flags & Active) == 0)
                continue;
            if (found || !IsMttMonitor(interfaces, target.Interface))
                return false;
            found = true;
        }
    }

    private static bool WriteMode(string tag, string source, uint index)
    {
        var mode = new DisplayMode { Size = (ushort)Marshal.SizeOf(typeof(DisplayMode)) };
        if (!EnumDisplaySettings(source, index, ref mode))
            return false;
        if (mode.Width > 0 && mode.Height > 0 && mode.RefreshRate > 1)
            Console.WriteLine("{0}\t{1}\t{2}\t{3}\t{4}", tag, source, mode.Width, mode.Height, mode.RefreshRate);
        return true;
    }

    private static int Main()
    {
        CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
        try
        {
            bool detected = DriverIsPresent(Present);
            Console.WriteLine("present\t{0}", detected ? "1" : "0");
            if (!detected)
                return 0;

            IntPtr interfaces = SetupDiCreateDeviceInfoList(IntPtr.Zero, IntPtr.Zero);
            if (interfaces == InvalidHandle)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            try
            {
                for (uint index = 0; ; index++)
                {
                    var source = new DisplayDevice { Size = (uint)Marshal.SizeOf(typeof(DisplayDevice)) };
                    if (!EnumDisplayDevices(null, index, ref source, 0))
                        return 0;
                    if ((source.Flags & (Active | Primary)) != Active || !IsExtendedMttOutput(interfaces, source.Name))
                        continue;
                    Console.WriteLine("display\t{0}", source.Name);
                    WriteMode("current", source.Name, CurrentMode);
                    for (uint mode = 0; WriteMode("mode", source.Name, mode); mode++) { }
                }
            }
            finally
            {
                SetupDiDestroyDeviceInfoList(interfaces);
            }
        }
        catch (Win32Exception error)
        {
            Console.Error.WriteLine(error.Message);
            return 1;
        }
    }
}
