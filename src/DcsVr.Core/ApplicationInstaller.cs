using System.Diagnostics;
using System.Text;

namespace DcsVr.Core;

public sealed record ReleaseFile(string Path, string Sha256, long Bytes);
public sealed record ReleaseManifest(int SchemaVersion, string Product, string Version, List<ReleaseFile> Files);

/// <summary>Installs only application payload files. Never changes DCS, registry, or VR drivers.</summary>
public sealed class ApplicationInstaller(string destination)
{
    public string Root { get; } = ValidateRoot(destination);
    public TransactionStore Transactions => new(Path.Combine(Root, ".installer"));
    public static string DefaultDestination => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs/DcsVrControl");
    public static ReleaseManifest VerifySource(string source)
    {
        var root = Path.GetFullPath(source);
        var manifest = JsonData.Deserialize<ReleaseManifest>(File.ReadAllText(Path.Combine(root, "release-manifest.json")));
        if (manifest.Product != "DcsVrControl" || manifest.SchemaVersion != 1 || manifest.Files.Count == 0 || manifest.Files.Count > 10000)
            throw new InvalidDataException("Invalid release manifest.");
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var file in manifest.Files)
        {
            var path = PathPolicy.UnderRoot(root, file.Path);
            if (!names.Add(path) || file.Path.StartsWith(".installer", StringComparison.OrdinalIgnoreCase) || file.Path.Equals("installation.json", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Invalid or duplicate payload path.");
            if (new FileInfo(path).Length != file.Bytes || Hashing.FileSha256(path) != file.Sha256)
                throw new InvalidDataException("Release integrity failed: " + file.Path);
        }
        return manifest;
    }
    public TransactionJournal Install(string source, Action<int>? beforeWrite = null)
    {
        EnsureAppClosed();
        var sourceRoot = Path.GetFullPath(source).TrimEnd(Path.DirectorySeparatorChar);
        if (Root.Equals(sourceRoot, StringComparison.OrdinalIgnoreCase) || Root.StartsWith(sourceRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || sourceRoot.StartsWith(Root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Source and destination must be separate application folders.");
        var manifest = VerifySource(sourceRoot);
        var journals = Transactions.List();
        ValidateJournalScope(journals);
        if (journals.Any(j => j.Status is not ("applied" or "restored"))) throw new InvalidOperationException("Restore the interrupted application installation before updating.");
        var owned = CurrentOwned(journals);
        foreach (var entry in owned.Values) VerifyOwned(entry, Root);
        var changes = new List<FileMutation>();
        foreach (var file in manifest.Files)
        {
            var path = PathPolicy.UnderRoot(Root, file.Path);
            var old = owned.GetValueOrDefault(path);
            if (File.Exists(path) && old is null) throw new IOException("Existing unowned file retained: " + path);
            changes.Add(new(path, old is null || old.Deleted || !File.Exists(path) ? null : old.InstalledSha256, File.ReadAllBytes(PathPolicy.UnderRoot(sourceRoot, file.Path)), "Application payload", Root: Root));
        }
        var ledger = PathPolicy.UnderRoot(Root, "installation.json");
        var retainedPaths = changes.Select(c => c.Path).Append(ledger).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var obsolete in owned.Values.Where(e => !e.Deleted && !retainedPaths.Contains(e.Path) && File.Exists(e.Path)))
            changes.Add(new(obsolete.Path, obsolete.InstalledSha256, [], "Remove obsolete owned application file", Delete: true, Root: Root));
        var previousLedger = owned.GetValueOrDefault(ledger);
        if (File.Exists(ledger) && previousLedger is null) throw new IOException("Unowned installation ledger retained.");
        changes.Add(new(ledger, previousLedger?.InstalledSha256, Encoding.UTF8.GetBytes(JsonData.Serialize(manifest)), "Application installation ledger", Root: Root));
        return Transactions.Apply(new("application", "DCS VR Control " + manifest.Version, changes, new Dictionary<string, string>(), ""), beforeWrite);
    }
    public RestoreResult Uninstall()
    {
        EnsureAppClosed();
        var journals = Transactions.List();
        ValidateJournalScope(journals);
        if (!journals.Any(j => j.Status != "restored")) throw new InvalidOperationException("No application installation to restore.");
        foreach (var entry in CurrentOwned(journals).Values) VerifyOwned(entry, Root);
        foreach (var journal in journals.Where(j => j.Status != "restored"))
        {
            var result = Transactions.Restore(journal.Id);
            if (!result.Complete) return result;
        }
        return new(true, []);
    }
    private static Dictionary<string, JournalEntry> CurrentOwned(IReadOnlyList<TransactionJournal> journals)
    {
        var result = new Dictionary<string, JournalEntry>(StringComparer.OrdinalIgnoreCase);
        foreach (var journal in journals.Where(j => j.Status != "restored"))
        foreach (var entry in journal.Entries.Where(e => e.Applied && !e.Reverted)) result.TryAdd(entry.Path, entry);
        return result;
    }
    private void ValidateJournalScope(IReadOnlyList<TransactionJournal> journals)
    {
        foreach (var entry in journals.SelectMany(j => j.Entries))
            if (!Path.GetFullPath(entry.Path).StartsWith(Root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Application journal contains an out-of-scope destination.");
    }
    /// <summary>Owned files must be unmodified. A missing file is tolerated: antivirus may have quarantined it
    /// (Defender removes the version.dll that 0.4.1 and earlier installed), and there is nothing left to protect.</summary>
    private static void VerifyOwned(JournalEntry entry, string root)
    {
        PathPolicy.RejectReparsePoints(entry.Path, root);
        if (!entry.Deleted && !File.Exists(entry.Path) && !Directory.Exists(entry.Path)) return;
        if (entry.Deleted ? File.Exists(entry.Path) || Directory.Exists(entry.Path) : !File.Exists(entry.Path) || Hashing.FileSha256(entry.Path) != entry.InstalledSha256)
            throw new IOException("Modified or missing application file retained: " + entry.Path);
    }
    private static string ValidateRoot(string destination)
    {
        var root = Path.GetFullPath(destination).TrimEnd(Path.DirectorySeparatorChar);
        if (root.Length <= 3 || root.Equals(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Select a dedicated application folder.");
        PathPolicy.RejectReparsePoints(root); return root;
    }
    private static void EnsureAppClosed()
    {
        var processes = Process.GetProcessesByName("DcsVrControl");
        try { if (processes.Length > 0) throw new InvalidOperationException("Close DCS VR Control before installing or uninstalling it."); }
        finally { foreach (var p in processes) p.Dispose(); }
    }
}
