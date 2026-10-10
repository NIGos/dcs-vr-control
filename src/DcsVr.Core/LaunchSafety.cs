using Microsoft.Win32;

namespace DcsVr.Core;

/// <summary>
/// Conditions under which the private launch environment cannot work. The OpenXR loader ignores
/// XR_RUNTIME_JSON and XR_API_LAYER_PATH in elevated processes but still honours XR_ENABLE_API_LAYERS,
/// so an elevated DCS asks for layers it cannot find and xrCreateInstance fails.
/// </summary>
public static class LaunchSafety
{
    public static bool CurrentProcessElevated => Environment.IsPrivilegedProcess;

    /// <summary>Returns the compatibility flag that forces elevation for this executable, or null.</summary>
    public static string? ForcedElevation(string executable)
    {
        var full = Path.GetFullPath(executable);
        foreach (var hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            try
            {
                using var root = RegistryKey.OpenBaseKey(hive, view);
                using var layers = root.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers");
                if (layers is null || layers.GetValueNames().FirstOrDefault(n => string.Equals(n, full, StringComparison.OrdinalIgnoreCase)) is not { } name) continue;
                var flags = (layers.GetValue(name) as string ?? "").Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (flags.FirstOrDefault(f => f is "RUNASADMIN" or "RUNASHIGHEST") is { } flag) return $"{hive}: {flag}";
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException or ArgumentException or NotSupportedException) { }
        }
        return null;
    }

    /// <summary>Throws when DCS would start elevated and silently lose the profile environment.</summary>
    public static void EnsureNotElevated(string executable)
    {
        if (CurrentProcessElevated)
            throw new InvalidOperationException("DCS Control is running as administrator. The OpenXR loader ignores the profile runtime and layers in elevated processes. Reopen the app without administrator rights.");
        if (ForcedElevation(executable) is { } flag)
            throw new InvalidOperationException($"DCS.exe is set to run as administrator ({flag}). Elevated DCS ignores the profile runtime and layers. Clear 'Run this program as an administrator' in DCS.exe Properties → Compatibility.");
    }

    /// <summary>
    /// The DCS launcher starts the game by running <c>DCS.exe --restarted</c> with CREATE_BREAKAWAY_FROM_JOB. When this
    /// app (and so DCS, which inherits it) runs inside a Windows job object that allows no breakaway, that call fails
    /// with access denied: dcs.log reads "Launching process … failed with error code 5" and the game never opens. This
    /// happens when the app is started by a tool that runs its children in a job (a sandbox, a script runner, some
    /// launchers), never when it is started from the Start menu or Explorer. Replaceable for tests.
    /// </summary>
    public static Func<JobState> CurrentJob { get; set; } = ReadCurrentJob;

    /// <param name="InJob">The process belongs to a job object.</param>
    /// <param name="LimitFlags">JOBOBJECT_BASIC_LIMIT_INFORMATION.LimitFlags of the innermost job.</param>
    /// <param name="Unknown">The process is in a job whose limits could not be read: nothing is blocked, the user is warned.</param>
    public readonly record struct JobState(bool InJob, uint LimitFlags, bool Unknown = false)
    {
        public const uint BreakawayOk = 0x800, SilentBreakawayOk = 0x1000;
        /// <summary>A child that asks to break away from the job (the DCS launcher's restart) is refused.</summary>
        public bool BlocksBreakaway => InJob && !Unknown && (LimitFlags & (BreakawayOk | SilentBreakawayOk)) == 0;
    }

    public const string LauncherUnknownMessage = "DCS Control runs inside a Windows job whose limits could not be read, so it is unknown whether the DCS launcher can start the game here. If DCS does not open after the launcher, close DCS Control and open it from the Start menu or Explorer, or turn off Show the DCS launcher.";

    /// <summary>True when the app runs in a job whose breakaway limit could not be read (a warning, never a block).</summary>
    public static bool LauncherRestartUnknown
    {
        get { try { return CurrentJob() is { InJob: true, Unknown: true }; } catch (Exception e) when (e is System.ComponentModel.Win32Exception or EntryPointNotFoundException or DllNotFoundException) { return true; } }
    }

    public const string LauncherBlockedMessage = "DCS Control was started inside a Windows job (by another program, such as a script runner or sandbox), so the DCS launcher cannot start the game: Windows refuses its restart with access denied (error 5). Close DCS Control and open it from the Start menu or Explorer, or turn off Show the DCS launcher.";

    /// <summary>True when the DCS launcher's restart into the game would be refused (see <see cref="CurrentJob"/>).</summary>
    public static bool LauncherRestartBlocked
    {
        get { try { return CurrentJob().BlocksBreakaway; } catch (Exception e) when (e is System.ComponentModel.Win32Exception or EntryPointNotFoundException or DllNotFoundException) { return false; } }
    }

    private static JobState ReadCurrentJob()
    {
        if (!OperatingSystem.IsWindows()) return default;
        if (!IsProcessInJob(GetCurrentProcess(), IntPtr.Zero, out var inJob) || !inJob) return new(false, 0);
        // A null job handle queries the job of the calling process; 9 = JobObjectExtendedLimitInformation.
        var info = new byte[144];
        return QueryInformationJobObject(IntPtr.Zero, 9, info, info.Length, IntPtr.Zero)
            ? new(true, BitConverter.ToUInt32(info, 16)) : new(true, 0, Unknown: true);
    }
    [System.Runtime.InteropServices.DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
    [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)] private static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);
    [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)] private static extern bool QueryInformationJobObject(IntPtr job, int infoClass, byte[] info, int length, IntPtr returnLength);

    /// <summary>Creates and immediately deletes a probe file, which is the only reliable answer under UAC virtualization and ACLs.</summary>
    public static bool CanWrite(string directory)
    {
        try
        {
            if (!Directory.Exists(directory)) return false;
            using var probe = new FileStream(Path.Combine(directory, ".dcs-vr-probe-" + Guid.NewGuid().ToString("N")), FileMode.CreateNew, FileAccess.Write, FileShare.None, 1, FileOptions.DeleteOnClose);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }
}
