using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

internal static class InstallVirtualDisplay
{
    private const string HardwareId = @"Root\MttVDD";
    private const uint DigcfPresent = 2;
    private const uint GenerateInstanceId = 1;
    private const uint HardwareIdsProperty = 1;
    private const uint RegisterDevice = 0x19;
    private const uint RemoveDevice = 5;
    private static readonly IntPtr InvalidHandle = new IntPtr(-1);

    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceInfo
    {
        public uint Size;
        public Guid ClassGuid;
        public uint DeviceInstance;
        public IntPtr Reserved;
    }

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid classGuid, IntPtr parent);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiCreateDeviceInfo(IntPtr devices, string name, ref Guid classGuid,
        string description, IntPtr parent, uint flags, ref DeviceInfo device);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiSetDeviceRegistryProperty(IntPtr devices, ref DeviceInfo device, uint property, byte[] value, uint length);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiCallClassInstaller(uint operation, IntPtr devices, ref DeviceInfo device);

    [DllImport("setupapi.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr devices);

    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UpdateDriverForPlugAndPlayDevices(IntPtr parent, string hardwareId, string infPath, uint flags,
        [MarshalAs(UnmanagedType.Bool)] out bool reboot);

    internal static int Status()
    {
        if (ProbeVirtualDisplay.DriverIsPresent(DigcfPresent)) return 0;
        if (ProbeVirtualDisplay.DriverIsPresent(0)) return 21;
        return 10;
    }

    internal static int Install(string package)
    {
        int status = Status();
        if (status != 10) return status;

        var displayClass = new Guid("4d36e968-e325-11ce-bfc1-08002be10318");
        IntPtr devices = SetupDiCreateDeviceInfoList(ref displayClass, IntPtr.Zero);
        if (devices == InvalidHandle) throw new Win32Exception(Marshal.GetLastWin32Error());
        var device = new DeviceInfo { Size = (uint)Marshal.SizeOf(typeof(DeviceInfo)) };
        bool registered = false;
        try
        {
            byte[] ids = Encoding.Unicode.GetBytes(HardwareId + "\0\0");
            if (!SetupDiCreateDeviceInfo(devices, "MttVDD", ref displayClass, "Virtual Display Driver", IntPtr.Zero, GenerateInstanceId, ref device) ||
                !SetupDiSetDeviceRegistryProperty(devices, ref device, HardwareIdsProperty, ids, (uint)ids.Length) ||
                !SetupDiCallClassInstaller(RegisterDevice, devices, ref device))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            registered = true;

            bool reboot;
            // Windows SetupAPI enforces driver trust and can show its normal publisher dialog.
            if (!UpdateDriverForPlugAndPlayDevices(IntPtr.Zero, HardwareId, Path.Combine(package, "MttVDD.inf"), 0, out reboot))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            registered = false;
            return reboot ? 3010 : 0;
        }
        finally
        {
            if (registered && !SetupDiCallClassInstaller(RemoveDevice, devices, ref device))
                Console.Error.WriteLine("Could not remove the new virtual display device after installation failed.");
            SetupDiDestroyDeviceInfoList(devices);
        }
    }
}
