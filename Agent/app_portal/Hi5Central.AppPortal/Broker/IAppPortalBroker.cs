using System.Text.Json;

namespace Hi5Central.AppPortal.Broker;

public interface IAppPortalBroker
{
    Task<JsonDocument> GetCatalogueAsync(CancellationToken cancellationToken = default);
    Task<JsonDocument> InstallAsync(string appId, CancellationToken cancellationToken = default);
}