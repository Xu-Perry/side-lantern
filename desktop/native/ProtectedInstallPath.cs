using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;

internal static class ProtectedInstallPath
{
    private static readonly SecurityIdentifier Administrators = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
    private static readonly SecurityIdentifier SystemAccount = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
    private static readonly SecurityIdentifier TrustedInstaller = new SecurityIdentifier("S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");
    private static readonly SecurityIdentifier Users = new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null);
    private const uint GenericWriteOrAll = 0x50000000;
    private const FileSystemRights RedirectRights = FileSystemRights.Delete | FileSystemRights.DeleteSubdirectoriesAndFiles |
        FileSystemRights.ChangePermissions | FileSystemRights.TakeOwnership | FileSystemRights.WriteAttributes | FileSystemRights.WriteExtendedAttributes;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint QueryDosDevice(string device, StringBuilder path, int size);

    private static bool Trusted(SecurityIdentifier sid)
    {
        return sid.Equals(Administrators) || sid.Equals(SystemAccount) || sid.Equals(TrustedInstaller);
    }

    private static string LocalPath(string path)
    {
        if (String.IsNullOrWhiteSpace(path) || path.Length < 4 || path[1] != ':' || path[2] != '\\' || path.IndexOf(':', 2) >= 0 || path.IndexOf('/') >= 0)
            throw new IOException("Choose a folder on a fixed local NTFS volume.");
        string full = Path.GetFullPath(path);
        if (!String.Equals(full, path, StringComparison.OrdinalIgnoreCase) || full.EndsWith("\\", StringComparison.Ordinal) || full.Length < 4 || full.Length > 160)
            throw new IOException("Choose a normal installation folder with a shorter, fully qualified path.");
        foreach (var component in full.Substring(3).Split('\\'))
        {
            if (component.Length == 0 || component.TrimEnd(' ', '.') != component)
                throw new IOException("Choose a folder name without trailing spaces or periods.");
        }
        string root = Path.GetPathRoot(full);
        var device = new StringBuilder(2048);
        var drive = new DriveInfo(root);
        if (drive.DriveType != DriveType.Fixed || !String.Equals(drive.DriveFormat, "NTFS", StringComparison.OrdinalIgnoreCase) ||
            QueryDosDevice(root.Substring(0, 2), device, device.Capacity) == 0 || !device.ToString().StartsWith(@"\Device\HarddiskVolume", StringComparison.OrdinalIgnoreCase))
            throw new IOException("Choose a folder on a fixed local NTFS volume.");
        string windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        if (String.Equals(full, windows, StringComparison.OrdinalIgnoreCase) || full.StartsWith(windows + "\\", StringComparison.OrdinalIgnoreCase))
            throw new IOException("Choose a folder outside the Windows directory.");
        return full;
    }

    private static void CheckDirectory(string path, bool protectContents)
    {
        var attributes = File.GetAttributes(path);
        if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != FileAttributes.Directory)
            throw new IOException("Installation paths cannot use junctions, links, or mount points.");

        var security = Directory.GetAccessControl(path, AccessControlSections.Access | AccessControlSections.Owner);
        if (!Trusted((SecurityIdentifier)security.GetOwner(typeof(SecurityIdentifier))))
            throw new IOException("A parent folder is owned by a regular user. Choose a system-protected location such as Program Files.");
        var forbidden = (uint)(protectContents ? RedirectRights | FileSystemRights.Write : RedirectRights) | GenericWriteOrAll;
        foreach (FileSystemAccessRule rule in security.GetAccessRules(true, true, typeof(SecurityIdentifier)))
        {
            if (rule.AccessControlType == AccessControlType.Allow && (rule.PropagationFlags & PropagationFlags.InheritOnly) == 0 &&
                !Trusted((SecurityIdentifier)rule.IdentityReference) && (unchecked((uint)rule.FileSystemRights) & forbidden) != 0)
                throw new IOException("A regular user can modify this location. Choose a system-protected folder such as Program Files.");
        }
    }

    private static void CheckParents(string path)
    {
        for (string parent = Path.GetDirectoryName(path); !String.IsNullOrEmpty(parent); parent = Path.GetDirectoryName(parent))
            CheckDirectory(parent, false);
    }

    internal static string ValidateNew(string path)
    {
        string full = LocalPath(path);
        CheckParents(full);
        try { File.GetAttributes(full); }
        catch (FileNotFoundException) { return full; }
        catch (DirectoryNotFoundException) { return full; }
        throw new IOException("Choose a new folder. To upgrade, uninstall SideLantern first. User settings are kept.");
    }

    internal static void Create(string path)
    {
        string full = ValidateNew(path);
        var security = new DirectorySecurity();
        security.SetAccessRuleProtection(true, false);
        security.SetOwner(Administrators);
        foreach (var account in new[] {Administrators, SystemAccount, Users})
            security.AddAccessRule(new FileSystemAccessRule(account, account.Equals(Users) ? FileSystemRights.ReadAndExecute : FileSystemRights.FullControl,
                InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
        Directory.CreateDirectory(full, security);
        Verify(full);
    }

    internal static void Verify(string path)
    {
        string full = LocalPath(path);
        CheckParents(full);
        CheckDirectory(full, true);
    }
}
