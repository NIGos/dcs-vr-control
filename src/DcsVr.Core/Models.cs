using System.Text.Json;
using System.Text.Json.Serialization;

namespace DcsVr.Core;

public enum RuntimeKind { Pimax, SboysSteamVr }
public enum QuadProvider { None, PimaxNative, QuadViewsFoveated }
public enum FrameGeneration { Off, Nvidia, FidelityFx }
public enum GazeMode { Fixed, EyeTracked }
public enum TrackingKind { Unknown, Slam, Lighthouse }
public enum NeuralDepthMode { Game, Normal, Reversed }
/// <summary>DLSS-NR model style (DLSSNR.Style 0..2), the same three modes RenoDX's DLSS 5 add-on offers.</summary>
public enum NeuralStyle { Standard, Natural, Cinematic }
public enum NvidiaFlowPreset { Fast, Medium, Slow }
public enum FpsLimitMode { Preserve, RuntimeHeadroom, MatchRefresh, Custom }
public enum ExternalPacingState { Unknown, Off, Active }
public enum IssueSeverity { Info, Warning, Error }
/// <summary>Where the bundled Quad Views focus area comes from: Pimax Play's own Quad Views settings, read (never
/// written) at preview, apply and launch, or the values stored in the profile.</summary>
public enum FoveaSource { PimaxPlay, Profile }
public enum BoostPriority { Normal, AboveNormal, High }
/// <summary>DCS's repeated PrefetchVirtualMemory calls (edterrain/edCore workers): leave them, only count them, or
/// skip a range already prefetched within the last second.</summary>
public enum PrefetchFix { Off, Observe, Skip }

public sealed record VrProfile
{
    public int SchemaVersion { get; init; } = 1;
    public string Id { get; init; } = "pimax-baseline";
    public string Name { get; init; } = "Pimax · original configuration";
    public RuntimeKind Runtime { get; init; } = RuntimeKind.Pimax;
    public QuadProvider QuadViews { get; init; } = QuadProvider.PimaxNative;
    public FrameGeneration FrameGen { get; init; }
    /// <summary>Output frames per rendered frame: 2 (one generated frame), 3 (OFXR fork triple_frame_gen: two generated
    /// frames, renders at a third of the refresh rate, more latency and more artifacts in sideways motion) or
    /// <see cref="FrameGenAuto"/> (OFXR picks 2 while DCS can hold half the refresh rate, 3 otherwise).</summary>
    public int FrameGenFactor { get; init; } = FrameGenAuto;
    public const int FrameGenAuto = 0;
    /// <summary>OFXR deep_pipeline ("Prefer FPS over latency" in its tray): one more frame in flight ahead of the headset,
    /// which absorbs DCS frame-time spikes at the cost of about one refresh period of latency (11 ms at 90 Hz). Applies
    /// to 2×, 3× and Auto (fork patch 0007); in 3× the extra slot is taken only under sustained lateness.</summary>
    public bool FrameGenDeepPipeline { get; init; } = true;
    /// <summary>OFXR smooth_cursor (fork patch 0010): generated frames find DCS's mouse cursor in the two real frames
    /// and draw it at its in-between position, so it moves at the headset's rate instead of fading between two places.
    /// Only the generated frames change; where a click lands does not. Off by default until flown.</summary>
    public bool SmoothCursor { get; init; }
    public GazeMode Gaze { get; init; } = GazeMode.EyeTracked;
    /// <summary>Foveated Super Resolution (Cheeky's own foveated DLSS SR), for stereo without Quad Views only. With Quad Views
    /// it would make Cheeky redo DCS's DLSS of the focus views privately instead of building DLSS 5 on it, so it is ignored
    /// there (<see cref="UsesFoveatedDlss"/>) and cleared by <see cref="ProfileValidation.ResolveFeatures"/>.</summary>
    public bool FoveatedDlss { get; init; }
    public bool NeuralRendering { get; init; }
    public bool QuadFocusAdapter { get; init; }
    // Used when the focus area source is this profile (or Pimax Play's settings cannot be read). The defaults are
    // Pimax Play's Fine values on a Crystal Super (0.66 / 0.20 per side shares left out, gaze scale 0.25, periphery
    // 0.1919) converted the same way PimaxFovea converts them: 0.57 x 0.34 focus at 1.125x, periphery 0.19.
    public double FoveaWidth { get; init; } = 0.57;
    public double FoveaHeight { get; init; } = 0.34;
    public double PeripheralScale { get; init; } = 0.19;
    public double QuadFocusScale { get; init; } = 1.125;
    public double QuadSharpening { get; init; } = 0.7;
    public double QuadEdgeBlend { get; init; } = 0.2;
    /// <summary>Quad Views turbo: DCS starts the next frame while the runtime still holds the previous one. Quad Views itself
    /// enables it on most runtimes and disables it on SteamVR; off by default until measured in the headset.</summary>
    public bool QuadTurbo { get; init; }
    public double NeuralWorkingScale { get; init; } = 0.75;
    public double NeuralIntensity { get; init; } = 1;
    public bool NeuralBeforeUpscaling { get; init; } = true;
    public double NeuralLocalTone { get; init; } = 1;
    public double NeuralLocalStructure { get; init; } = 1;
    public double NeuralSkinStructure { get; init; } = 1;
    public bool NeuralAutomaticMask { get; init; }
    public bool NeuralUiCorrection { get; init; }
    public double NeuralColorStrength { get; init; } = 1;
    public double NeuralTransferStrength { get; init; } = 1;
    public double NeuralPaperWhiteScale { get; init; } = 1;
    public NeuralDepthMode NeuralDepth { get; init; }
    public NeuralStyle NeuralStyle { get; init; }
    /// <summary>With the Quad Views focus adapter: DLSS 5 covers the central part of each focus view, in percent per axis
    /// (100 = whole focus view, 80, 70 or 50). The NR edge fades out over <see cref="NeuralAreaFeather"/>. One of
    /// <see cref="NeuralAreas"/>.</summary>
    public int NeuralFocusArea { get; init; } = 100;
    public static IReadOnlyList<int> NeuralAreas { get; } = [100, 80, 70, 50];
    /// <summary>Cheeky NrTransitionWidth for a central DLSS 5 area: the fade from DLSS 5 to the plain DLSS image spans
    /// half of this fraction of the focus view on each side (6%), independent of the area size.</summary>
    public const double NeuralAreaFeather = 0.12;
    /// <summary>In-flight DLSS 5 toggle: "Off", "virtual-key:modifiers" (recorded by the key capture field) or a label of
    /// earlier versions (<see cref="NeuralHotkeys.All"/>).</summary>
    public string NeuralToggleKey { get; init; } = NeuralHotkeys.Default;
    /// <summary>Shows or hides the in-headset diagnostic panel (FPS, DLSS 5, framegen); drawn by OFXR, so framegen profiles only.</summary>
    public string DiagnosticOverlayKey { get; init; } = NeuralHotkeys.DiagnosticDefault;
    public bool DiagnosticOverlayAtStart { get; init; }
    /// <summary>OFXR's in-headset VRAM counter (diag_vram in its ini); off by default.</summary>
    public bool DiagnosticVram { get; init; }
    public double NeuralMotionScaleX { get; init; } = 1;
    public double NeuralMotionScaleY { get; init; } = 1;
    public int NvidiaFlowScale { get; init; } = 50;
    public NvidiaFlowPreset FlowPreset { get; init; } = NvidiaFlowPreset.Medium;
    public bool BidirectionalFlow { get; init; }
    public bool DiagnosticRecorder { get; init; } = true;
    /// <summary>OFXR's always-on FPS counter; the diagnostic panel (its own key) shows FPS on demand.</summary>
    public bool ShowOverlay { get; init; }
    public FpsLimitMode FpsLimit { get; init; }
    public double HeadsetRefreshHz { get; init; } = 90;
    public double RenderedFpsCap { get; init; } = 45;
    public bool DisableDcsVSync { get; init; }
    /// <summary>Keep the DCS launcher window when launching from the app. Off: the profile sets
    /// miscellaneous.launcher = false (restored with the profile) and DCS goes straight into the game.</summary>
    public bool KeepDcsLauncher { get; init; }
    /// <summary>No longer asked for (the app cannot read or verify other limiters or motion smoothing; preflight lists
    /// them as checks you do yourself); kept so profiles saved by earlier versions still load.</summary>
    public ExternalPacingState ExternalLimiter { get; init; }
    /// <summary>No longer used; kept so profiles saved by earlier versions still load.</summary>
    public double ExternalLimiterFps { get; init; } = 45;
    /// <summary>No longer used; kept so profiles saved by earlier versions still load.</summary>
    public ExternalPacingState RuntimeReprojection { get; init; }
    /// <summary>No longer required; kept so profiles saved by earlier versions still load.</summary>
    public bool ExperimentalAcknowledged { get; init; }
    public string? NeuralRuntimePath { get; init; }
    public string? RuntimeManifestPath { get; init; }
    public string? QuadViewsLayerDirectory { get; init; }
    /// <summary>Never used by deployment (the registered Sboys driver is used); kept so profiles saved by earlier versions still load.</summary>
    public string? SboysDriverDirectory { get; init; }
    public TrackingKind Tracking { get; init; }
    /// <summary>New profiles (presets, the checklist, Detect my setup) follow Pimax Play. A saved profile without this
    /// property predates it and always used its own values, so <see cref="JsonData"/> loads it as <see cref="FoveaSource.Profile"/>.</summary>
    public FoveaSource FoveaSource { get; init; } = FoveaSource.PimaxPlay;

    /// <summary>CPU Boost while DCS runs (started with the launch, undone when DCS exits).</summary>
    public bool CpuBoost { get; init; }
    public BoostPriority BoostDcsPriority { get; init; } = BoostPriority.AboveNormal;
    /// <summary>Keep VR runtime and headset services off the CPU cores DCS uses for its main and render threads.</summary>
    public bool BoostMoveVrRuntime { get; init; } = true;
    /// <summary>Move <see cref="BoostBackgroundApps"/> to the slowest cores at below-normal priority.</summary>
    public bool BoostMoveBackgroundApps { get; init; } = true;
    public IReadOnlyList<string> BoostBackgroundApps { get => _boostBackgroundApps; init => _boostBackgroundApps = new(value); }
    private readonly NameList _boostBackgroundApps = new(["msedge", "chrome", "firefox", "brave", "opera", "OneDrive", "Teams", "ms-teams", "Spotify", "steamwebhelper"]);
    /// <summary>Replaced by <see cref="FreeVramApps"/>: a profile saved with this list is loaded with Free VRAM before
    /// flight on (when CPU Boost was on), the same names, ending apps that do not close and no reopening, which is what
    /// the list did. Kept so those profiles still load; always empty after loading.</summary>
    public IReadOnlyList<string> BoostCloseApps { get => _boostCloseApps; init => _boostCloseApps = new(value); }
    private readonly NameList _boostCloseApps = new([]);
    public PrefetchFix BoostPrefetch { get; init; } = PrefetchFix.Skip;
    /// <summary>Ask for administrator rights (UAC) so services running under other accounts can be moved too.</summary>
    public bool BoostElevated { get; init; }

    /// <summary>Free VRAM before flight: the boost helper closes <see cref="FreeVramApps"/> when DCS starts.</summary>
    public bool FreeVram { get; init; }
    /// <summary>Process names (without .exe, * and ? allowed) closed by <see cref="FreeVram"/>. The defaults are programs
    /// seen holding video memory on a Windows gaming PC; voice chat and recording programs are deliberately absent.</summary>
    public IReadOnlyList<string> FreeVramApps { get => _freeVramApps; init => _freeVramApps = new(value); }
    private readonly NameList _freeVramApps = new(DefaultFreeVramApps);
    public static IReadOnlyList<string> DefaultFreeVramApps { get; } =
        ["NVIDIA Overlay", "msedge", "ChatGPT", "HueSync", "RazerCortex", "RazerAppEngine", "wallpaper64", "wallpaper32"];
    /// <summary>Start the programs Free VRAM closed again, as the signed-in user, when DCS exits.</summary>
    public bool FreeVramReopen { get; init; } = true;
    /// <summary>End a program that is still running 5 s after the close request. Off: it is left running.</summary>
    public bool FreeVramForce { get; init; }
    /// <summary>The profile sets DCS's desktop window to 1280×720, windowed (options.lua, restored with the profile).</summary>
    public bool SmallDcsWindow { get; init; }
    public const int SmallWindowWidth = 1280, SmallWindowHeight = 720;
    /// <summary>The boost helper switches the main monitor to <see cref="FlightDisplayWidth"/> ×
    /// <see cref="FlightDisplayHeight"/> at <see cref="FlightDisplayRefresh"/> Hz while DCS runs; never saved as a Windows setting.</summary>
    public bool LowerMonitor { get; init; }
    public int FlightDisplayWidth { get; init; } = 1920;
    public int FlightDisplayHeight { get; init; } = 1080;
    public int FlightDisplayRefresh { get; init; } = 60;

    /// <summary>The prefetch fix is deployed (bin\dxgi2.dll) and enabled through the launch environment.</summary>
    public bool UsesPrefetchFix => CpuBoost && BoostPrefetch != PrefetchFix.Off;
    /// <summary>The boost helper is started with DCS: for CPU Boost, Free VRAM before flight or the monitor mode.</summary>
    public bool UsesBoostHelper => CpuBoost || FreeVram || LowerMonitor;
    /// <summary>Foveated Super Resolution actually runs: only in stereo, without Quad Views.</summary>
    public bool UsesFoveatedDlss => FoveatedDlss && QuadViews == QuadProvider.None;
    public bool UsesCheeky => UsesFoveatedDlss || NeuralRendering;
    public bool UsesQuadFocus => UsesCheeky && QuadViews == QuadProvider.QuadViewsFoveated && QuadFocusAdapter;
    /// <summary>Central DLSS 5 area actually written to Cheeky: only with the focus adapter and DLSS 5 on.</summary>
    public bool UsesCentralNeuralArea => UsesQuadFocus && NeuralRendering && NeuralFocusArea < 100;
    public override string ToString() => Name;
}

/// <summary>A read-only list of names that compares by its items, so profiles holding equal lists stay equal records
/// (after a JSON round trip, a copy or a fresh default).</summary>
public sealed class NameList(IEnumerable<string>? names) : IReadOnlyList<string>, IEquatable<NameList>
{
    private readonly string[] _names = names?.ToArray() ?? [];
    public string this[int index] => _names[index];
    public int Count => _names.Length;
    public IEnumerator<string> GetEnumerator() => ((IEnumerable<string>)_names).GetEnumerator();
    System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() => GetEnumerator();
    public bool Equals(NameList? other) => other is not null && _names.SequenceEqual(other._names, StringComparer.Ordinal);
    public override bool Equals(object? obj) => obj is NameList other && Equals(other);
    public override int GetHashCode() { var hash = new HashCode(); foreach (var name in _names) hash.Add(name, StringComparer.Ordinal); return hash.ToHashCode(); }
}
public sealed record ValidationIssue(string Code, IssueSeverity Severity, string Message);
/// <param name="ExcludedEntries">Archive entries never extracted. Cheeky's version.dll fallback loader is excluded:
/// Microsoft Defender flags it (Trojan:Win32/Posilod.CA!cl, cloud heuristic) and dxgi.dll alone is sufficient.</param>
public sealed record ComponentPackage(string Id, string Version, string ArchiveName, string Url, string Sha256, string License, string SourceUrl, IReadOnlyList<string>? ExcludedEntries = null);
public sealed record FileFact(string Path, bool Exists, string? Version, string? Sha256);
public sealed record LayerFact(string Scope, string ManifestPath, bool Enabled, string? Name, string? LibraryPath, bool LibraryExists);
public sealed record DriverFact(string Name, string Directory, bool LibraryExists, bool Blocked, string? Version = null);
public sealed record InventorySnapshot
{
    public DateTimeOffset CapturedAt { get; init; } = DateTimeOffset.UtcNow;
    public string? DcsDirectory { get; init; }
    public string? DcsExecutable { get; init; }
    public string? OptionsPath { get; init; }
    public string? ActiveRuntime { get; init; }
    public string? PimaxRuntime { get; init; }
    public string? SteamVrRuntime { get; init; }
    public string? PimaxVersion { get; init; }
    public string? SboysDirectory { get; init; }
    public string? SboysConfigPath { get; init; }
    public bool DcsRunning { get; init; }
    public TrackingKind Tracking { get; init; }
    public IReadOnlyList<FileFact> Files { get; init; } = [];
    public IReadOnlyList<LayerFact> Layers { get; init; } = [];
    public IReadOnlyList<DriverFact> Drivers { get; init; } = [];
    public IReadOnlyDictionary<string, string> DcsSettings { get; init; } = new Dictionary<string, string>();
    public IReadOnlyList<string> Observations { get; init; } = [];
    public IReadOnlyList<string> LimiterProcesses { get; init; } = [];
    public string? AutoexecPath { get; init; }
    public string? AutoexecMaxFps { get; init; }
    public bool AutoexecPacingUnknown { get; init; }
}

public static class JsonData
{
    public static JsonSerializerOptions Options { get; } = Create(withProfileConverter: true);

    private static JsonSerializerOptions Create(bool withProfileConverter)
    {
        var options = new JsonSerializerOptions
        {
            WriteIndented = true,
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
            Converters = { new JsonStringEnumConverter(allowIntegerValues: false) }
        };
        if (withProfileConverter) options.Converters.Add(new VrProfileConverter(Create(withProfileConverter: false)));
        return options;
    }

    /// <summary>Reads a profile with the plain options, then marks a profile saved without "foveaSource" (written before
    /// the setting existed) as using its own focus values instead of silently switching it to Pimax Play.</summary>
    private sealed class VrProfileConverter(JsonSerializerOptions plain) : JsonConverter<VrProfile>
    {
        public override VrProfile Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
        {
            using var document = JsonDocument.ParseValue(ref reader);
            var profile = document.Deserialize<VrProfile>(plain) ?? throw new JsonException("The profile is empty.");
            if (document.RootElement.ValueKind == JsonValueKind.Object && !document.RootElement.TryGetProperty("foveaSource", out _))
                profile = profile with { FoveaSource = FoveaSource.Profile };
            return MigrateCloseApps(profile, document.RootElement.ValueKind == JsonValueKind.Object && document.RootElement.TryGetProperty("freeVram", out _));
        }
        public override void Write(Utf8JsonWriter writer, VrProfile value, JsonSerializerOptions options) => JsonSerializer.Serialize(writer, value, plain);
    }

    /// <summary>The CPU Boost "Close when DCS starts" list became Free VRAM before flight. A profile saved before
    /// (<paramref name="hasFreeVram"/> false) keeps doing what it did: the same names are closed when CPU Boost was on,
    /// apps that do not close are ended, nothing is reopened. Names left in the old list of a newer profile join the
    /// Free VRAM list. The old list is always empty afterwards.</summary>
    public static VrProfile MigrateCloseApps(VrProfile profile, bool hasFreeVram)
    {
        var old = profile.BoostCloseApps.Where(n => !string.IsNullOrWhiteSpace(n)).Select(n => n.Trim()).ToArray();
        if (old.Length == 0) return profile.BoostCloseApps.Count == 0 ? profile : profile with { BoostCloseApps = [] };
        if (!hasFreeVram)
            return profile with { FreeVram = profile.CpuBoost, FreeVramApps = old.Distinct(StringComparer.OrdinalIgnoreCase).ToArray(), FreeVramForce = true, FreeVramReopen = false, BoostCloseApps = [] };
        return profile with { FreeVramApps = profile.FreeVramApps.Concat(old).Distinct(StringComparer.OrdinalIgnoreCase).ToArray(), BoostCloseApps = [] };
    }

    public static string Serialize<T>(T value) => JsonSerializer.Serialize(value, Options);
    public static T Deserialize<T>(string text) => JsonSerializer.Deserialize<T>(text, Options)
        ?? throw new InvalidDataException("The JSON document is empty.");
}

public static class ProfilePresets
{
    public static IReadOnlyList<VrProfile> All { get; } = [
        new(),
        new() { Id = "pimax-framegen", Name = "Pimax · Quad Views + framegen", FrameGen = FrameGeneration.Nvidia },
        new() { Id = "sboys-framegen", Name = "Sboys · Quad Views + framegen", Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.QuadViewsFoveated, FrameGen = FrameGeneration.Nvidia },
        new() { Id = "pimax-neural", Name = "Pimax · DLSS 5 + framegen (no Quad Views)", QuadViews = QuadProvider.None, FrameGen = FrameGeneration.Nvidia, FoveatedDlss = true, NeuralRendering = true },
        new() { Id = "sboys-neural", Name = "Sboys · DLSS 5 + framegen (no Quad Views)", Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.None, FrameGen = FrameGeneration.Nvidia, FoveatedDlss = true, NeuralRendering = true },
        new() { Id = "pimax-combined", Name = "Pimax · DLSS 5 + framegen + Quad Views", QuadViews = QuadProvider.QuadViewsFoveated, QuadFocusAdapter = true, FrameGen = FrameGeneration.Nvidia, NeuralRendering = true },
        new() { Id = "sboys-combined", Name = "Sboys · DLSS 5 + framegen + Quad Views", Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.QuadViewsFoveated, QuadFocusAdapter = true, FrameGen = FrameGeneration.Nvidia, NeuralRendering = true }
    ];
}
