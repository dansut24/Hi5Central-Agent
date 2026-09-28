using System.Text.Json.Serialization;
using Hi5Central.AppPortal.Models;

namespace Hi5Central.AppPortal.Serialization;

[JsonSerializable(typeof(PortalCatalogueResponse))]
[JsonSerializable(typeof(PortalInstallResponse))]
internal sealed partial class PortalJsonContext : JsonSerializerContext
{
}
