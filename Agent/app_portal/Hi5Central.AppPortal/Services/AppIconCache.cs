using System.Collections.Concurrent;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using Avalonia.Media.Imaging;

namespace Hi5Central.AppPortal.Services;

public sealed class AppIconCache
{
    private const int MaxIconBytes = 512 * 1024;
    private static readonly HttpClient Http = CreateClient();
    private readonly ConcurrentDictionary<string, Task<Bitmap?>> _memory = new(StringComparer.OrdinalIgnoreCase);
    private readonly string _cacheDirectory;

    public AppIconCache()
    {
        _cacheDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Hi5Central",
            "SelfService",
            "IconCache");
    }

    public Task<Bitmap?> GetAsync(string iconUrl, CancellationToken cancellationToken = default)
    {
        if (!Uri.TryCreate(iconUrl, UriKind.Absolute, out var uri)
            || uri.Scheme != Uri.UriSchemeHttps)
        {
            return Task.FromResult<Bitmap?>(null);
        }

        return _memory.GetOrAdd(iconUrl, _ => LoadAsync(uri, cancellationToken));
    }
    private async Task<Bitmap?> LoadAsync(Uri uri, CancellationToken cancellationToken)
    {
        try
        {
            Directory.CreateDirectory(_cacheDirectory);
            var cachePath = Path.Combine(_cacheDirectory, CacheName(uri.AbsoluteUri));

            if (File.Exists(cachePath))
            {
                var info = new FileInfo(cachePath);
                if (info.Length is > 0 and <= MaxIconBytes)
                {
                    await using var cached = File.OpenRead(cachePath);
                    return new Bitmap(cached);
                }

                File.Delete(cachePath);
            }

            using var response = await Http.GetAsync(
                uri,
                HttpCompletionOption.ResponseHeadersRead,
                cancellationToken).ConfigureAwait(false);

            if (!response.IsSuccessStatusCode) return null;

            var length = response.Content.Headers.ContentLength;
            if (length is null or <= 0 or > MaxIconBytes) return null;

            var mediaType = response.Content.Headers.ContentType?.MediaType ?? string.Empty;
            if (!mediaType.StartsWith("image/", StringComparison.OrdinalIgnoreCase))
            {
                return null;
            }
            await using var input = await response.Content.ReadAsStreamAsync(cancellationToken)
                .ConfigureAwait(false);
            await using var buffer = new MemoryStream((int)length.Value);
            var block = new byte[16 * 1024];
            var total = 0;

            for (;;)
            {
                var read = await input.ReadAsync(block, cancellationToken).ConfigureAwait(false);
                if (read <= 0) break;

                total += read;
                if (total > MaxIconBytes) return null;
                await buffer.WriteAsync(block.AsMemory(0, read), cancellationToken)
                    .ConfigureAwait(false);
            }

            if (total == 0) return null;

            var bytes = buffer.ToArray();
            await File.WriteAllBytesAsync(cachePath, bytes, cancellationToken).ConfigureAwait(false);

            using var imageStream = new MemoryStream(bytes, writable: false);
            return new Bitmap(imageStream);
        }
        catch
        {
            return null;
        }
    }

    private static string CacheName(string value)
    {
        var digest = SHA256.HashData(Encoding.UTF8.GetBytes(value));
        return Convert.ToHexString(digest).ToLowerInvariant() + ".img";
    }
    private static HttpClient CreateClient()
    {
        var client = new HttpClient
        {
            Timeout = TimeSpan.FromSeconds(5),
        };

        client.DefaultRequestHeaders.UserAgent.Add(
            new ProductInfoHeaderValue("Hi5Central-SelfService", "1.0"));

        return client;
    }
}
