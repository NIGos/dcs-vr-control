using System.Globalization;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

/// <summary>The headset's refresh rate as read from the headset software, and where it came from.</summary>
public sealed record HeadsetRefreshFact(double Hz, string Source);

/// <summary>
/// The refresh rate the headset runs at, read instead of asked. Pimax Play stores only an index of its display timing
/// (profile.json display_timing_selection, which varies by headset), so the rate is read from its logs: first the
/// compositor's live rate in the runtime log (%LOCALAPPDATA%\Pimax\runtime), then the end-of-session report in
/// %LOCALAPPDATA%\Pimax\PiService\Log, whose <c>"fps_target"</c> is the headset's rate (its <c>"refresh_rate"</c> is the
/// monitor's). On the Sboys route SteamVR's preferred refresh rate is read from steamvr.vrsettings. Nothing is ever written.
/// </summary>
public static partial class HeadsetRefresh
{
    /// <summary>Pimax Play's service logs; replaceable so tests never read the build machine's.</summary>
    public static string PimaxLogFolder { get; set; } = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Pimax", "PiService", "Log");
    /// <summary>The Pimax runtime's logs (the compositor's live rate); replaceable for tests.</summary>
    public static string PimaxRuntimeFolder { get; set; } = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Pimax", "runtime");
    /// <summary>SteamVR's settings; replaceable for tests.</summary>
    public static string SteamVrSettings { get; set; } = new DetectionSources().SteamVrSettings;

    private static (string Key, HeadsetRefreshFact? Fact)? _pimaxCache;

    [GeneratedRegex(@"rendering fps:\(a:[\d.]+,c:(\d+(?:\.\d+)?)\)")] private static partial Regex CompositorRate();
    [GeneratedRegex(@"""fps_target""\s*:\s*(\d+(?:\.\d+)?)")] private static partial Regex RefreshRate();

    /// <summary>The refresh rate of the profile's headset route, or null when it cannot be read.</summary>
    public static HeadsetRefreshFact? Detect(RuntimeKind runtime)
    {
        if (runtime == RuntimeKind.SboysSteamVr)
            return SetupDetection.ReadSteamVrRefresh(SteamVrSettings) is { } steam ? new(steam, "SteamVR") : null;
        return ReadPimaxRuntime(PimaxRuntimeFolder) ?? ReadPimax(PimaxLogFolder);
    }

    /// <summary>
    /// The rate the Pimax compositor runs at now: its server log (pvr_srv_log_*.txt) reports every second
    /// "rendering fps:(a:&lt;app&gt;,c:&lt;compositor&gt;)", and the compositor rate is the panel's refresh (89.997 at 90 Hz,
    /// 72.402 at 72 Hz). Follows a change in Pimax Play within a second, unlike the end-of-session report.
    /// </summary>
    public static HeadsetRefreshFact? ReadPimaxRuntime(string folder)
    {
        FileInfo? log;
        try { log = Directory.Exists(folder) ? new DirectoryInfo(folder).GetFiles("pvr_srv_log_*.txt").OrderByDescending(f => f.LastWriteTimeUtc).FirstOrDefault() : null; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        if (log is null) return null;
        string tail;
        try
        {
            // The log grows by megabytes a day; the last report is in its last few seconds.
            using var stream = new FileStream(log.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var length = Math.Min(stream.Length, 256 * 1024);
            stream.Seek(-length, SeekOrigin.End);
            var buffer = new byte[length];
            stream.ReadExactly(buffer);
            tail = System.Text.Encoding.UTF8.GetString(buffer);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        var matches = CompositorRate().Matches(tail);
        for (var i = matches.Count - 1; i >= 0; i--)
            if (double.TryParse(matches[i].Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var hz) && hz is >= 60 and <= 240)
                return new(Math.Round(hz, 1), "Pimax Play");
        return null;
    }

    /// <summary>The newest Pimax Play session report's refresh rate in <paramref name="folder"/>.</summary>
    public static HeadsetRefreshFact? ReadPimax(string folder)
    {
        FileInfo[] logs;
        try { logs = Directory.Exists(folder) ? new DirectoryInfo(folder).GetFiles("*.log").OrderByDescending(f => f.LastWriteTimeUtc).Take(20).ToArray() : []; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        // The interface asks on every refresh: read the logs again only when the newest one changed.
        var key = folder + "|" + string.Join("|", logs.Take(1).Select(f => f.FullName + ":" + f.Length + ":" + f.LastWriteTimeUtc.Ticks));
        if (_pimaxCache is { } cached && cached.Key == key) return cached.Fact;
        var fact = ReadPimax(logs);
        _pimaxCache = (key, fact);
        return fact;
    }

    private static HeadsetRefreshFact? ReadPimax(FileInfo[] logs)
    {
        foreach (var log in logs)
        {
            double? last = null;
            try
            {
                using var stream = new FileStream(log.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                using var reader = new StreamReader(stream);
                for (var line = reader.ReadLine(); line is not null; line = reader.ReadLine())
                {
                    if (!line.Contains("Performance body", StringComparison.Ordinal)) continue;
                    var match = RefreshRate().Match(line);
                    if (match.Success && double.TryParse(match.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var hz) && hz is >= 60 and <= 240) last = Math.Round(hz, 1);
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { continue; }
            if (last is { } found) return new(found, "Pimax Play, last session");
        }
        return null;
    }

    /// <summary>The rate read from the headset when the profile follows it and uses exactly that rate.</summary>
    public static HeadsetRefreshFact? ReadFor(VrProfile profile) =>
        profile.RefreshFromHeadset && !profile.Desktop && Detect(profile.Runtime) is { } fact && fact.Hz == profile.HeadsetRefreshHz ? fact : null;

    /// <summary>The profile with the headset's own refresh rate when it follows the headset and the rate can be read.</summary>
    public static VrProfile Resolve(VrProfile profile) =>
        profile.RefreshFromHeadset && !profile.Desktop && Detect(profile.Runtime) is { } fact ? profile with { HeadsetRefreshHz = fact.Hz } : profile;
}
