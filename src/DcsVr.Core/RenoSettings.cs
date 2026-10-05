using System.Globalization;

namespace DcsVr.Core;

public sealed record RenoImportResult(VrProfile Profile, IReadOnlyList<string> Imported, IReadOnlyList<string> Unmapped);

/// <summary>Imports compatible tuning from the user's add-on configuration, without deploying another injector.</summary>
public static class RenoSettings
{
    public static VrProfile ResetAdvanced(VrProfile p) => p with
    {
        NeuralLocalTone = 1, NeuralLocalStructure = 1, NeuralSkinStructure = 1,
        NeuralAutomaticMask = false, NeuralUiCorrection = false,
        NeuralColorStrength = 1, NeuralTransferStrength = 1, NeuralPaperWhiteScale = 1,
        NeuralDepth = NeuralDepthMode.Game, NeuralMotionScaleX = 1, NeuralMotionScaleY = 1
    };

    public static RenoImportResult Import(string ini, VrProfile current)
    {
        var p = current; var imported = new List<string>(); var unmapped = new List<string>();
        var section = false; var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var line in ini.Split('\n'))
        {
            var text = line.Trim();
            if (text.StartsWith(';') || text.StartsWith('#') || text.Length == 0) continue;
            if (text.StartsWith('[')) { section = text.Equals("[RenoDX.DLSS5]", StringComparison.OrdinalIgnoreCase); continue; }
            if (!section) continue;
            var equal = text.IndexOf('='); if (equal < 1) throw new InvalidDataException("Invalid RenoDX setting: " + text);
            var key = text[..equal].Trim(); var value = text[(equal + 1)..].Split(';', '#')[0].Trim();
            if (!seen.Add(key)) throw new InvalidDataException("Duplicate RenoDX setting: " + key);
            double Number(double min, double max)
            {
                if (!double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var n) || !double.IsFinite(n) || n < min || n > max)
                    throw new InvalidDataException($"{key} must be between {min.ToString(CultureInfo.InvariantCulture)} and {max.ToString(CultureInfo.InvariantCulture)}.");
                return n;
            }
            bool Flag() => value.ToLowerInvariant() switch { "1" or "true" => true, "0" or "false" => false, _ => throw new InvalidDataException(key + " must be 0 or 1.") };
            switch (key.ToUpperInvariant())
            {
                case "NRINTENSITY": p = p with { NeuralIntensity = Number(0, 1) }; break;
                case "NRLOCALTONE": p = p with { NeuralLocalTone = Number(0, 2) }; break;
                case "NRLOCALSTRUCTURE": p = p with { NeuralLocalStructure = Number(0, 2) }; break;
                case "NRSKINSTRUCTURE": p = p with { NeuralSkinStructure = Number(0, 2) }; break;
                case "NRAUTOMASK": p = p with { NeuralAutomaticMask = Flag() }; break;
                case "NRUICORRECTION": p = p with { NeuralUiCorrection = Flag() }; break;
                case "NRCOLORSTRENGTH": p = p with { NeuralColorStrength = Number(0, 2) }; break;
                case "NRTRANSFERSTRENGTH": p = p with { NeuralTransferStrength = Number(0, 2) }; break;
                case "NRPAPERWHITESCALE": p = p with { NeuralPaperWhiteScale = Number(.01, 8) }; break;
                case "NRMVECSCALEX": p = p with { NeuralMotionScaleX = Number(-4, 4) }; break;
                case "NRMVECSCALEY": p = p with { NeuralMotionScaleY = Number(-4, 4) }; break;
                case "NRDEPTHMODE":
                    var depth = Number(0, 2); if (depth != Math.Truncate(depth)) throw new InvalidDataException("NRDepthMode must be 0, 1, or 2.");
                    p = p with { NeuralDepth = (NeuralDepthMode)(int)depth }; break;
                default: unmapped.Add(key); continue;
            }
            imported.Add(key);
        }
        if (imported.Count == 0) throw new InvalidDataException("No compatible settings found in [RenoDX.DLSS5].");
        return new(p, imported, unmapped);
    }
}
