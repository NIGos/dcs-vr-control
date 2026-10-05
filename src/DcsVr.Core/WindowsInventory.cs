using System.Diagnostics;
using Microsoft.Win32;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Runtime.InteropServices;

namespace DcsVr.Core;

public sealed class WindowsInventory
{
    public InventorySnapshot Capture(string? executableOverride = null, string? optionsOverride = null)
    {
        var observations = new List<string>();
        var installed = InstalledPrograms(observations);
        // Exact product names only; other uninstall entries (mods, tools) can contain "DCS World". Empty locations are ignored.
        var dcsDirectory = installed.Where(p => p.Name.Equals("DCS World", StringComparison.OrdinalIgnoreCase) || p.Name.StartsWith("DCS World ", StringComparison.OrdinalIgnoreCase) || p.Name.StartsWith("DCS World_", StringComparison.OrdinalIgnoreCase))
            .Select(p => p.Directory).FirstOrDefault(d => !string.IsNullOrWhiteSpace(d) && HasDcsExecutable(d));
        var steamRoot = ReadString(RegistryHive.CurrentUser, @"SOFTWARE\Valve\Steam", "SteamPath", observations)
            ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam");
        var libraries = SteamLibraries(steamRoot);
        dcsDirectory ??= libraries.Select(p => Path.Combine(p, "steamapps/common/DCSWorld")).FirstOrDefault(HasDcsExecutable);
        string? executable = executableOverride is not null ? Path.GetFullPath(executableOverride) : null;
        if (executable is null && dcsDirectory is not null)
            // Current releases keep identical copies in bin and bin-mt; bin is what Steam and the launcher start.
            executable = new[] { "bin/DCS.exe", "bin-mt/DCS.exe" }.Select(p => Path.Combine(dcsDirectory, p)).FirstOrDefault(File.Exists);
        if (executable is not null && !File.Exists(executable)) { observations.Add("The selected DCS executable does not exist."); executable = null; }
        if (executable is not null) dcsDirectory ??= Path.GetDirectoryName(Path.GetDirectoryName(executable));
        var savedGames = new[] { SavedGamesDirectory(), Path.Combine(Environment.GetEnvironmentVariable("USERPROFILE") ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Saved Games") }
            .Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        string? options = optionsOverride is not null ? Path.GetFullPath(optionsOverride) : savedGames.SelectMany(folder => new[] { "DCS", "DCS.openbeta" }
            .Select(p => Path.Combine(folder, p, "Config/options.lua"))).FirstOrDefault(File.Exists);
        if (options is null) observations.Add("Saved Games settings not detected. Searched: " + string.Join("; ", savedGames) + ". Select the matching options.lua manually.");
        if (options is not null && !File.Exists(options)) { observations.Add("The selected options.lua file does not exist."); options = null; }
        var activeRuntime = ReadString(RegistryHive.LocalMachine, @"SOFTWARE\Khronos\OpenXR\1", "ActiveRuntime", observations);
        var pimaxManifest = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Pimax/Runtime/PiOpenXR_64.json");
        var steamDirectory = installed.FirstOrDefault(p => p.Name.Equals("SteamVR", StringComparison.OrdinalIgnoreCase)).Directory;
        steamDirectory ??= libraries.Select(p => Path.Combine(p, "steamapps/common/SteamVR")).FirstOrDefault(p => File.Exists(Path.Combine(p, "steamxr_win64.json")));
        var steamManifest = steamDirectory is not null ? Path.Combine(steamDirectory, "steamxr_win64.json") : null;
        var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        var sboysConfig = Path.Combine(appData, "CustomHeadset/settings.json");
        var files = new List<FileFact>();
        if (executable is not null)
        {
            files.Add(Fact(executable));
            var bin = Path.GetDirectoryName(executable)!;
            foreach (var file in new[] { "nvngx_dlss.dll", "nvngx_dlssnr.dll", "dxgi.dll", "version.dll", "openxr_loader.dll" }) files.Add(Fact(Path.Combine(bin, file)));
        }
        var settings = new Dictionary<string, string>();
        if (options is not null)
        {
            try
            {
                var lua = new LuaOptions(File.ReadAllText(options));
                foreach (var path in new[] { "VR.openxr_quadView", "VR.openxr_eyeGaze", "VR.pixel_density", "graphics.Upscaling", "graphics.DLSS_PerfQuality", "graphics.maxFPS", "graphics.sync" })
                    if (lua.Get(path.Split('.')) is { } raw) settings[path] = raw;
            }
            catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException) { observations.Add(e.Message); }
        }
        string? autoexecPath = options is null ? null : Path.Combine(Path.GetDirectoryName(options)!, "autoexec.cfg");
        string? autoexecFps = null; var autoexecUnknown = false;
        if (File.Exists(autoexecPath))
        {
            try { (autoexecFps, autoexecUnknown) = FramePacing.ReadAutoexec(File.ReadAllText(autoexecPath)); files.Add(Fact(autoexecPath!)); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { autoexecUnknown = true; observations.Add("Cannot inspect autoexec.cfg: " + e.Message); }
        }
        else autoexecPath = null;
        var limiterProcesses = new List<string>();
        foreach (var name in new[] { "RTSS", "RTSSHooksLoader", "RTSSHooksLoader64" })
        {
            var processes = Process.GetProcessesByName(name);
            if (processes.Length > 0) limiterProcesses.Add(name);
            foreach (var process in processes) process.Dispose();
        }
        var pimax = installed.FirstOrDefault(p => p.Name.Contains("Pimax", StringComparison.OrdinalIgnoreCase));
        var layers = ReadLayers(observations);
        var drivers = ReadDrivers(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "openvr/openvrpaths.vrpath"), steamDirectory, observations);
        var running = Process.GetProcessesByName("DCS");
        var dcsRunning = running.Length > 0;
        foreach (var process in running) process.Dispose();
        return new()
        {
            DcsDirectory = dcsDirectory, DcsExecutable = executable, OptionsPath = options,
            ActiveRuntime = activeRuntime, PimaxRuntime = File.Exists(pimaxManifest) ? pimaxManifest : null,
            SteamVrRuntime = steamManifest is not null && File.Exists(steamManifest) ? steamManifest : null,
            PimaxVersion = pimax.Version, SboysConfigPath = File.Exists(sboysConfig) ? sboysConfig : null,
            SboysDirectory = drivers.FirstOrDefault(d => d.Name.Contains("customheadset", StringComparison.OrdinalIgnoreCase))?.Directory, DcsRunning = dcsRunning,
            Files = files, Layers = layers, Drivers = drivers, DcsSettings = settings, Observations = observations,
            AutoexecPath = autoexecPath, AutoexecMaxFps = autoexecFps, AutoexecPacingUnknown = autoexecUnknown, LimiterProcesses = limiterProcesses
        };
    }

    private static FileFact Fact(string path)
    {
        if (!File.Exists(path)) return new(path, false, null, null);
        try { return new(path, true, FileVersionInfo.GetVersionInfo(path).FileVersion, Hashing.FileSha256(path)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return new(path, true, null, null); }
    }

    private static bool HasDcsExecutable(string directory) =>
        File.Exists(Path.Combine(directory, "bin/DCS.exe")) || File.Exists(Path.Combine(directory, "bin-mt/DCS.exe"));

    private static List<(string Name, string? Directory, string? Version)> InstalledPrograms(List<string> observations)
    {
        var result = new List<(string, string?, string?)>();
        foreach (var hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            try
            {
                using var root = RegistryKey.OpenBaseKey(hive, view);
                using var uninstall = root.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall");
                if (uninstall is null) continue;
                foreach (var key in uninstall.GetSubKeyNames())
                {
                    using var app = uninstall.OpenSubKey(key);
                    if (app?.GetValue("DisplayName") is string name)
                        result.Add((name, app.GetValue("InstallLocation") as string, app.GetValue("DisplayVersion") as string));
                }
            }
            catch (Exception e) when (e is UnauthorizedAccessException or System.Security.SecurityException or IOException) { observations.Add($"Registry inventory: {e.Message}"); }
        }
        return result;
    }

    private static string? ReadString(RegistryHive hive, string key, string name, List<string> observations)
    {
        try
        {
            using var root = RegistryKey.OpenBaseKey(hive, RegistryView.Registry64);
            using var value = root.OpenSubKey(key);
            return value?.GetValue(name) as string;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { observations.Add(e.Message); return null; }
    }

    private static List<LayerFact> ReadLayers(List<string> observations)
    {
        var result = new List<LayerFact>();
        foreach (var hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
        {
            try
            {
                using var root = RegistryKey.OpenBaseKey(hive, RegistryView.Registry64);
                using var layers = root.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit");
                if (layers is null) continue;
                foreach (var path in layers.GetValueNames())
                {
                    string? name = null, dll = null;
                    try
                    {
                        using var manifest = System.Text.Json.JsonDocument.Parse(File.ReadAllText(path));
                        var api = manifest.RootElement.GetProperty("api_layer");
                        name = api.GetProperty("name").GetString();
                        if (api.TryGetProperty("library_path", out var library) && library.GetString() is { } relative)
                            dll = Path.GetFullPath(relative, Path.GetDirectoryName(path)!);
                    }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or KeyNotFoundException or ArgumentException or InvalidOperationException)
                    { observations.Add($"Cannot read layer manifest: {Path.GetFileName(path)}."); }
                    result.Add(new(hive.ToString(), path, layers.GetValue(path) is int v && v == 0, name, dll, dll is not null && File.Exists(dll)));
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { observations.Add(e.Message); }
        }
        return result;
    }

    public static IReadOnlyList<string> SteamLibraries(string root)
    {
        var paths = new List<string> { Path.GetFullPath(root) };
        try
        {
            var vdf = File.ReadAllText(Path.Combine(root, "steamapps/libraryfolders.vdf"));
            foreach (Match match in Regex.Matches(vdf, "\"(?:path|[0-9]+)\"\\s+\"([^\"]+)\""))
            {
                var value = match.Groups[1].Value.Replace("\\\\", "\\");
                if (Path.IsPathFullyQualified(value)) paths.Add(Path.GetFullPath(value));
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException) { }
        return paths.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    public static IReadOnlyList<DriverFact> ReadDrivers(string registration, string? steamDirectory, List<string> observations)
    {
        var paths = new List<string>(); var configPaths = new List<string>();
        try
        {
            using var json = JsonDocument.Parse(File.ReadAllText(registration));
            if (json.RootElement.TryGetProperty("external_drivers", out var drivers))
                paths.AddRange(drivers.EnumerateArray().Select(d => d.GetString()).OfType<string>());
            if (json.RootElement.TryGetProperty("config", out var configs))
                configPaths.AddRange(configs.EnumerateArray().Select(d => d.GetString()).OfType<string>());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException) { observations.Add("SteamVR driver registration could not be read: " + e.Message); }
        if (steamDirectory is not null && Directory.Exists(Path.Combine(steamDirectory, "drivers")))
            paths.AddRange(Directory.EnumerateDirectories(Path.Combine(steamDirectory, "drivers")));
        var blocked = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var config in configPaths)
        {
            try
            {
                using var settings = JsonDocument.Parse(File.ReadAllText(Path.Combine(config, "steamvr.vrsettings")));
                foreach (var item in settings.RootElement.EnumerateObject())
                    if (item.Name.StartsWith("driver_", StringComparison.Ordinal) &&
                        ((item.Value.TryGetProperty("blocked_by_safe_mode", out var block) && block.ValueKind == JsonValueKind.True) ||
                         (item.Value.TryGetProperty("enable", out var enabled) && enabled.ValueKind == JsonValueKind.False))) blocked.Add(item.Name[7..]);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException) { }
        }
        var result = new List<DriverFact>();
        foreach (var path in paths.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            try
            {
                using var manifest = JsonDocument.Parse(File.ReadAllText(Path.Combine(path, "driver.vrdrivermanifest")));
                var name = manifest.RootElement.GetProperty("name").GetString()!;
                result.Add(new(name, Path.GetFullPath(path), NativeBinary.IsX64(Path.Combine(path, "bin/win64/driver_" + name + ".dll")), blocked.Contains(name),
                    manifest.RootElement.TryGetProperty("version", out var version) ? version.GetString() : null));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or ArgumentException or KeyNotFoundException or InvalidOperationException) { observations.Add("SteamVR driver manifest could not be read: " + path); }
        }
        return result;
    }

    private static string SavedGamesDirectory()
    {
        var id = new Guid("4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4");
        var pointer = IntPtr.Zero;
        try { if (SHGetKnownFolderPath(ref id, 0x4000, IntPtr.Zero, out pointer) == 0) return Marshal.PtrToStringUni(pointer)!; }
        finally { if (pointer != IntPtr.Zero) Marshal.FreeCoTaskMem(pointer); }
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Saved Games");
    }
    [DllImport("shell32.dll")] private static extern int SHGetKnownFolderPath(ref Guid id, uint flags, IntPtr token, out IntPtr path);
}
