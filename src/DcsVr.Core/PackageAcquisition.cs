namespace DcsVr.Core;

public static class PackageAcquisition
{
    public static async Task<string> Acquire(ComponentPackage package, string cacheRoot, string? importPath = null, HttpClient? client = null)
    {
        var destination = PathPolicy.UnderRoot(cacheRoot, package.ArchiveName);
        if (importPath is not null)
        {
            var bytes = await File.ReadAllBytesAsync(importPath);
            if (Hashing.BytesSha256(bytes) != package.Sha256) throw new InvalidDataException("The archive does not match the pinned official " + package.Id + " release.");
            AtomicFile.Write(destination, bytes); return destination;
        }
        if (File.Exists(destination) && Hashing.FileSha256(destination) == package.Sha256) return destination;
        var ownsClient = client is null;
        client ??= new HttpClient { Timeout = TimeSpan.FromSeconds(90) };
        try
        {
            using var response = await client.GetAsync(package.Url, HttpCompletionOption.ResponseHeadersRead);
            response.EnsureSuccessStatusCode();
            if (response.Content.Headers.ContentLength > 64 * 1024 * 1024) throw new InvalidDataException("Official archive exceeds the size limit.");
            await using var input = await response.Content.ReadAsStreamAsync();
            using var buffer = new MemoryStream(); var block = new byte[81920]; int length;
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(90));
            while ((length = await input.ReadAsync(block, timeout.Token)) > 0)
            {
                if (buffer.Length + length > 64 * 1024 * 1024) throw new InvalidDataException("Official archive exceeds the size limit.");
                buffer.Write(block, 0, length);
            }
            var bytes = buffer.ToArray();
            if (Hashing.BytesSha256(bytes) != package.Sha256) throw new InvalidDataException("Downloaded archive hash does not match the pinned official release. Nothing was installed.");
            AtomicFile.Write(destination, bytes); return destination;
        }
        finally { if (ownsClient) client.Dispose(); }
    }
}
