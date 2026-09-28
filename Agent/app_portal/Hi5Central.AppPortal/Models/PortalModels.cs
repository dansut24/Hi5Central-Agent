using System.Text.Json.Serialization;

namespace Hi5Central.AppPortal.Models;

public sealed class PortalCatalogueResponse
{
    [JsonPropertyName("success")]
    public bool Success { get; init; }

    [JsonPropertyName("error")]
    public string Error { get; init; } = string.Empty;

    [JsonPropertyName("apps")]
    public List<PortalApp> Apps { get; init; } = [];

    [JsonPropertyName("installations")]
    public List<PortalInstallation> Installations { get; init; } = [];
}

public sealed class PortalApp
{
    [JsonPropertyName("id")]
    public string Id { get; init; } = string.Empty;

    [JsonPropertyName("name")]
    public string Name { get; init; } = string.Empty;

    [JsonPropertyName("publisher")]
    public string Publisher { get; init; } = string.Empty;

    [JsonPropertyName("description")]
    public string Description { get; init; } = string.Empty;

    [JsonPropertyName("category")]
    public string Category { get; init; } = string.Empty;

    [JsonPropertyName("iconUrl")]
    public string IconUrl { get; init; } = string.Empty;

    [JsonPropertyName("sourceType")]
    public string SourceType { get; init; } = string.Empty;

    [JsonPropertyName("version")]
    public string Version { get; init; } = string.Empty;

    [JsonPropertyName("intent")]
    public string Intent { get; init; } = string.Empty;

    [JsonPropertyName("scope")]
    public PortalScope Scope { get; init; } = new();
}

public sealed class PortalScope
{
    [JsonPropertyName("type")]
    public string Type { get; init; } = string.Empty;

    [JsonPropertyName("id")]
    public string Id { get; init; } = string.Empty;

    [JsonPropertyName("name")]
    public string Name { get; init; } = string.Empty;
}

public sealed class PortalInstallation
{
    [JsonPropertyName("app_id")]
    public string AppId { get; init; } = string.Empty;

    [JsonPropertyName("installation_status")]
    public string InstallationStatus { get; init; } = string.Empty;

    [JsonPropertyName("job_status")]
    public string JobStatus { get; init; } = string.Empty;

    [JsonPropertyName("error_message")]
    public string ErrorMessage { get; init; } = string.Empty;

    [JsonPropertyName("created_at")]
    public DateTimeOffset? CreatedAt { get; init; }

    [JsonPropertyName("completed_at")]
    public DateTimeOffset? CompletedAt { get; init; }
}

public sealed class PortalInstallResponse
{
    [JsonPropertyName("success")]
    public bool Success { get; init; }

    [JsonPropertyName("approvalRequired")]
    public bool ApprovalRequired { get; init; }

    [JsonPropertyName("error")]
    public string Error { get; init; } = string.Empty;
}

public enum AppPortalStatus
{
    Available,
    ApprovalRequired,
    Installing,
    Installed,
    Failed,
}

public sealed class AppCard
{
    public required PortalApp App { get; init; }
    public required AppPortalStatus Status { get; init; }

    public string Id => App.Id;
    public string Name => string.IsNullOrWhiteSpace(App.Name) ? "Application" : App.Name;
    public string Publisher => string.IsNullOrWhiteSpace(App.Publisher)
        ? "Company application"
        : App.Publisher;
    public string Description => string.IsNullOrWhiteSpace(App.Description)
        ? "Approved company software available through Hi5Central."
        : App.Description;
    public string Category => string.IsNullOrWhiteSpace(App.Category)
        ? "Company software"
        : App.Category;
    public string Version => string.IsNullOrWhiteSpace(App.Version)
        ? "Current release"
        : App.Version;
    public string Initials => BuildInitials(Name);

    public string StatusText => Status switch
    {
        AppPortalStatus.ApprovalRequired => "Approval required",
        AppPortalStatus.Installing => "Installing",
        AppPortalStatus.Installed => "Installed",
        AppPortalStatus.Failed => "Failed",
        _ => "Available",
    };

    public string ActionText => Status switch
    {
        AppPortalStatus.ApprovalRequired => "Request approval",
        AppPortalStatus.Installing => "Installing...",
        AppPortalStatus.Installed => "Installed",
        AppPortalStatus.Failed => "Retry",
        _ => "Install",
    };

    public bool CanInstall => Status is AppPortalStatus.Available
        or AppPortalStatus.ApprovalRequired
        or AppPortalStatus.Failed;

    public string ScopeText => App.Scope.Type switch
    {
        "Estate" => "Available to your organisation",
        "" => "Assigned by your organisation",
        _ => $"Assigned via {App.Scope.Type}"
             + (string.IsNullOrWhiteSpace(App.Scope.Name)
                 ? string.Empty
                 : $" - {App.Scope.Name}"),
    };

    private static string BuildInitials(string value)
    {
        var parts = value.Split(
            ' ',
            StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        if (parts.Length == 0) return "AP";

        return string.Concat(
            parts.Take(2).Select(part => char.ToUpperInvariant(part[0])));
    }
}