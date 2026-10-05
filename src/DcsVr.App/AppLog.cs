using System.IO;
using DcsVr.Core;

namespace DcsVr.App;

/// <summary>Unexpected errors the interface survived, appended to %LOCALAPPDATA%\DcsVrControl\app.log (kept under 1 MB).
/// Logging never throws.</summary>
internal static class AppLog
{
    private static readonly Lock Gate = new();
    public static string Path { get; set; } = System.IO.Path.Combine(ControlService.DefaultStateRoot, "app.log");

    public static void Error(string context, Exception error) => Write($"{context}: {error}");

    /// <summary>Something the app did on its own at start (such as setting a monitor mode back), for the record.</summary>
    public static void Info(string message) => Write(message);

    private static void Write(string text)
    {
        try
        {
            lock (Gate)
            {
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
                var info = new FileInfo(Path);
                if (info.Exists && info.Length > 1024 * 1024) File.Move(Path, Path + ".old", overwrite: true);
                File.AppendAllText(Path, $"{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss.fff} {ProductInfo.Version} {text}{Environment.NewLine}");
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or NotSupportedException or ArgumentException) { }
    }
}
