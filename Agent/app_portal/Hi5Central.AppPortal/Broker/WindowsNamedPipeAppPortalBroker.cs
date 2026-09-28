using System.IO.Pipes;
using System.Text.Json;

namespace Hi5Central.AppPortal.Broker;

public sealed class WindowsNamedPipeAppPortalBroker : IAppPortalBroker
{
    private const string PipeName = "Hi5CentralAppPortal";

    public Task<JsonDocument> GetCatalogueAsync(CancellationToken cancellationToken = default)
        => SendAsync("catalogue", appId: null, cancellationToken);

    public Task<JsonDocument> InstallAsync(string appId, CancellationToken cancellationToken = default)
        => SendAsync("install", appId, cancellationToken);

    private static async Task<JsonDocument> SendAsync(
        string requestType,
        string? appId,
        CancellationToken cancellationToken)
    {
        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException(
                "The Windows App Portal broker requires the Hi5Central Windows Agent.");
        }

        await using var pipe = new NamedPipeClientStream(
            ".",
            PipeName,
            PipeDirection.InOut,
            PipeOptions.Asynchronous);

        using var connectTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        connectTimeout.CancelAfter(TimeSpan.FromSeconds(5));
        await pipe.ConnectAsync(connectTimeout.Token).ConfigureAwait(false);
        pipe.ReadMode = PipeTransmissionMode.Message;

        using var requestBuffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(requestBuffer))
        {
            writer.WriteStartObject();
            writer.WriteString("type", requestType);
            if (!string.IsNullOrWhiteSpace(appId))
            {
                writer.WriteString("appId", appId);
            }
            writer.WriteEndObject();
        }

        var requestBytes = requestBuffer.ToArray();
        await pipe.WriteAsync(requestBytes, cancellationToken).ConfigureAwait(false);
        await pipe.FlushAsync(cancellationToken).ConfigureAwait(false);

        using var response = new MemoryStream();
        var buffer = new byte[64 * 1024];

        do
        {
            var read = await pipe.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);
            if (read <= 0) break;

            response.Write(buffer, 0, read);
            if (response.Length > 2 * 1024 * 1024)
            {
                throw new InvalidDataException("The App Portal response exceeded the maximum size.");
            }
        }
        while (!pipe.IsMessageComplete);

        if (response.Length == 0)
        {
            throw new InvalidDataException("The Hi5Central Agent returned an empty response.");
        }

        response.Position = 0;
        var document = await JsonDocument.ParseAsync(response, cancellationToken: cancellationToken)
            .ConfigureAwait(false);

        if (document.RootElement.ValueKind != JsonValueKind.Object)
        {
            document.Dispose();
            throw new InvalidDataException("The Hi5Central Agent returned an invalid response.");
        }

        return document;
    }
}