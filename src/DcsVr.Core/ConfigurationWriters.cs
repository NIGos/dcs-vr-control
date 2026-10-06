using System.Globalization;
using System.Text.Json.Nodes;

namespace DcsVr.Core;

public static class ConfigurationWriters
{
    private static string Number(double value) => value.ToString("G9", CultureInfo.InvariantCulture);
    // Cheeky clamps these ratios to 0.20..1 and rewrites its INI with the clamped value, which restore would then
    // report as a user edit. Write the value Cheeky will actually use (src/settings.cpp).
    private static double CheekyRange(double value) => Math.Clamp(value, 0.2, 1);
    // Central DLSS 5 area of each focus view (fraction per axis). Cheeky centres it in the focus view, which Quad Views
    // already centres on the gaze, keeps the game's own DX11 DLSS and feathers the NR edge over NrTransitionWidth.
    private static double NeuralArea(VrProfile p) => p.UsesCentralNeuralArea ? p.NeuralFocusArea / 100.0 : 1;

    public static string Cheeky(VrProfile p) => $"""
        [CheekyFoveatedDLSS]
        SchemaVersion=1
        Enabled={(p.UsesFoveatedDlss ? 1 : 0)}
        D3D11D3D12Transport={(p.NeuralRendering ? 1 : 0)}
        D3D12LowerHook=1
        PeripheralDlaa={(p.UsesQuadFocus ? 0 : 1)}
        PeripheralDlaaScale={Number(CheekyRange(p.PeripheralScale))}
        Width={Number(p.UsesQuadFocus ? 1 : CheekyRange(p.FoveaWidth))}
        Height={Number(p.UsesQuadFocus ? 1 : CheekyRange(p.FoveaHeight))}
        CenterMode={(p.Gaze == GazeMode.EyeTracked && !p.UsesQuadFocus ? 1 : 0)}
        AutoStereoAlignment={(p.UsesQuadFocus ? 0 : 1)}
        EyeCalibrationContinuous=0
        GazeSmoothingMs=20
        GazeHoldMs=400
        TransitionWidth=0.04
        NrEnabled={(p.NeuralRendering ? 1 : 0)}
        NrFoveated={(p.UsesQuadFocus ? (p.UsesCentralNeuralArea ? 1 : 0) : 1)}
        NrUseSrFoveation={(p.UsesQuadFocus ? 0 : 1)}
        NrWidth={Number(p.UsesQuadFocus ? NeuralArea(p) : CheekyRange(p.FoveaWidth))}
        NrHeight={Number(p.UsesQuadFocus ? NeuralArea(p) : CheekyRange(p.FoveaHeight))}
        NrRoundness=0
        NrTransitionWidth={Number(VrProfile.NeuralAreaFeather)}
        NrProcessingOrder={(p.NeuralBeforeUpscaling ? 1 : 0)}
        NrWorkingScale={Number(p.NeuralWorkingScale)}
        NrIntensity={Number(p.NeuralIntensity)}
        NrLocalToneStrength={Number(p.NeuralLocalTone)}
        NrLocalStructureStrength={Number(p.NeuralLocalStructure)}
        NrSkinStructureStrength={Number(p.NeuralSkinStructure)}
        NrUiCorrection={(p.NeuralUiCorrection ? 1 : 0)}
        NrColorStrength={Number(p.NeuralColorStrength)}
        NrHdrTransferStrength={Number(p.NeuralTransferStrength)}
        NrPaperWhiteScale={Number(p.NeuralPaperWhiteScale)}
        NrDepthConvention={(int)p.NeuralDepth}
        NrMotionScaleXMultiplier={Number(p.NeuralMotionScaleX)}
        NrMotionScaleYMultiplier={Number(p.NeuralMotionScaleY)}
        NrStyle={(int)p.NeuralStyle}
        NrAutomaticMask={(p.NeuralAutomaticMask ? 1 : 0)}

        """;

    public static string Ofxr(VrProfile p) => $"""
        [ofxr]
        enabled={(p.FrameGen == FrameGeneration.Off ? 0 : 1)}
        motion_vectors=off
        deep_pipeline={(p.FrameGenDeepPipeline ? 1 : 0)}
        triple_frame_gen={(p.FrameGenFactor == 3 ? 1 : 0)}
        adaptive_frame_gen={(p.FrameGenFactor == VrProfile.FrameGenAuto ? 1 : 0)}
        d3d11_bridge=1
        backend={(p.FrameGen == FrameGeneration.FidelityFx ? "fidelityfx" : "nvidia")}
        nvidia_preset={p.FlowPreset.ToString().ToLowerInvariant()}
        nvidia_input_scale={p.NvidiaFlowScale}
        nvidia_bidirectional={(p.BidirectionalFlow ? 1 : 0)}
        diag_vram={(p.DiagnosticVram ? 1 : 0)}

        [diagnostics]
        logging_enabled={(p.DiagnosticRecorder ? 1 : 0)}
        max_file_mb=128
        flush_each_event=0

        [overlay]
        position={(p.ShowOverlay ? "upper_right" : "off")}

        """;

    public static string LayerManifest(string name, string library, int implementationVersion, string description) => JsonData.Serialize(new
    {
        file_format_version = "1.0.0",
        api_layer = new
        {
            name, library_path = Path.GetFullPath(library), api_version = "1.0",
            implementation_version = implementationVersion.ToString(CultureInfo.InvariantCulture), description
        }
    });

    public static string QuadViews(VrProfile p) => $"""
        [exe:DCS]
        peripheral_multiplier={Number(p.PeripheralScale)}
        focus_multiplier={Number(p.QuadFocusScale)}
        stereo_output_multiplier=1
        sharpen_focus_view={Number(p.QuadSharpening)}
        smoothen_focus_view_edges={Number(p.QuadEdgeBlend)}
        horizontal_fixed_section={Number(Math.Min(p.FoveaWidth, .9))}
        vertical_fixed_section={Number(Math.Min(p.FoveaHeight, .9))}
        horizontal_focus_section={Number(Math.Min(p.FoveaWidth, .9))}
        vertical_focus_section={Number(Math.Min(p.FoveaHeight, .9))}
        force_no_eye_tracking={(p.Gaze == GazeMode.Fixed ? 1 : 0)}
        turbo_mode={(p.QuadTurbo ? 1 : 0)}

        """;

    public static string ExplicitManifest(string existing, string manifestPath)
    {
        var root = JsonNode.Parse(existing)?.AsObject() ?? throw new InvalidDataException("Invalid OpenXR manifest.");
        var layer = root["api_layer"]?.AsObject() ?? throw new InvalidDataException("Manifest is missing api_layer.");
        var library = layer["library_path"]?.GetValue<string>() ?? throw new InvalidDataException("Manifest is missing library_path.");
        var absoluteLibrary = Path.GetFullPath(library, Path.GetDirectoryName(Path.GetFullPath(manifestPath))!);
        if (!File.Exists(absoluteLibrary)) throw new InvalidDataException("The layer library does not exist.");
        layer["library_path"] = absoluteLibrary;
        layer.Remove("enable_environment"); layer.Remove("disable_environment");
        return root.ToJsonString(JsonData.Options);
    }
}
