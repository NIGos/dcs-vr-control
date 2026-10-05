using System.Security.Cryptography;
using System.Text;

namespace DcsVr.Core;

public static class Hashing
{
    public static string BytesSha256(ReadOnlySpan<byte> bytes) => Convert.ToHexStringLower(SHA256.HashData(bytes));
    public static string FileSha256(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(stream));
    }
}

public static class AtomicFile
{
    public static void Write(string path, byte[] bytes)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(path))!;
        Directory.CreateDirectory(directory);
        var temporary = Path.Combine(directory, ".dcs-vr-" + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536, FileOptions.WriteThrough))
            { stream.Write(bytes); stream.Flush(flushToDisk: true); }
            File.Move(temporary, path, overwrite: true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    public static void WriteText(string path, string content) => Write(path, new UTF8Encoding(false).GetBytes(content));
}

public static class PathPolicy
{
    public static string UnderRoot(string root, string relative)
    {
        if (Path.IsPathFullyQualified(relative) || relative.Contains(':')) throw new InvalidDataException("The path must be relative and contain no NTFS streams.");
        foreach (var segment in relative.Replace('/', '\\').Split('\\'))
        {
            if (segment is "" or "." or ".." || segment.EndsWith('.') || segment.EndsWith(' ') || segment.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
                throw new InvalidDataException("The path contains an invalid name.");
            var stem = segment.Split('.')[0].ToUpperInvariant();
            if (stem is "CON" or "PRN" or "AUX" or "NUL" || (stem.Length == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) && stem[3] is >= '1' and <= '9'))
                throw new InvalidDataException("The path contains a reserved Windows device name.");
        }
        var canonicalRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var full = Path.GetFullPath(Path.Combine(canonicalRoot, relative));
        if (!full.StartsWith(canonicalRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The path escapes the authorized folder.");
        RejectReparsePoints(full, canonicalRoot);
        return full;
    }

    /// <summary>
    /// Rejects a symlink or junction at the destination and at every folder between it and <paramref name="root"/>.
    /// Links above the root are allowed: moving DCS or Saved Games to another drive with a junction is common,
    /// and the user selected that location explicitly.
    /// </summary>
    public static void RejectReparsePoints(string fullPath, string? root = null)
    {
        var current = Path.GetFullPath(fullPath).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var stop = root is null ? null : Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        while (!string.IsNullOrEmpty(current))
        {
            if ((File.Exists(current) || Directory.Exists(current)) && (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("Symlink and junction destinations are unsupported: " + current);
            if (stop is null) return;
            current = Path.GetDirectoryName(current);
            if (current is null || current.Length <= stop.Length) return;
        }
    }
}
