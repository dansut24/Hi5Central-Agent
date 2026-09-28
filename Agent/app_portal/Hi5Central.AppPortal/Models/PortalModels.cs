using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text.Json.Serialization;
using Avalonia.Media.Imaging;

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

    [JsonPropertyName("requests")]
    public List<PortalRequest> Requests { get; init; } = [];

    [JsonPropertyName("device")]
    public PortalDevice Device { get; init; } = new();

    [JsonPropertyName("updates")]
    public PortalUpdates Updates { get; init; } = new();
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

    [JsonPropertyName("catalogueId")]
    public string CatalogueId { get; init; } = string.Empty;

    [JsonPropertyName("installCount")]
    public int InstallCount { get; init; }

    [JsonPropertyName("installed")]
    public bool Installed { get; init; }

    [JsonPropertyName("version")]
    public string Version { get; init; } = string.Empty;

    [JsonPropertyName("intent")]
    public string Intent { get; init; } = string.Empty;

    [JsonPropertyName("scope")]
    public PortalScope Scope { get; init; } = new();
}

public sealed class PortalDevice
{
    [JsonPropertyName("name")]
    public string Name { get; init; } = string.Empty;

    [JsonPropertyName("manufacturer")]
    public string Manufacturer { get; init; } = string.Empty;

    [JsonPropertyName("model")]
    public string Model { get; init; } = string.Empty;

    [JsonPropertyName("operating_system")]
    public string OperatingSystem { get; init; } = string.Empty;

    [JsonPropertyName("os_version")]
    public string OsVersion { get; init; } = string.Empty;

    [JsonPropertyName("is_encrypted")]
    public bool? IsEncrypted { get; init; }

    [JsonPropertyName("storage_total_bytes")]
    public long? StorageTotalBytes { get; init; }

    [JsonPropertyName("storage_free_bytes")]
    public long? StorageFreeBytes { get; init; }

    [JsonPropertyName("user_display_name")]
    public string UserDisplayName { get; init; } = string.Empty;

    [JsonPropertyName("user_principal_name")]
    public string UserPrincipalName { get; init; } = string.Empty;

    [JsonPropertyName("compliance_state")]
    public string ComplianceState { get; init; } = string.Empty;

    [JsonPropertyName("agent_version")]
    public string AgentVersion { get; init; } = string.Empty;

    [JsonPropertyName("websocket_status")]
    public string WebsocketStatus { get; init; } = string.Empty;

    [JsonPropertyName("active_user")]
    public string ActiveUser { get; init; } = string.Empty;

    [JsonPropertyName("last_telemetry_at")]
    public DateTimeOffset? LastTelemetryAt { get; init; }
}

public sealed class PortalUpdates
{
    [JsonPropertyName("total")]
    public int Total { get; init; }

    [JsonPropertyName("software")]
    public List<PortalSoftwareUpdate> Software { get; init; } = [];

    [JsonPropertyName("windows")]
    public List<PortalWindowsUpdate> Windows { get; init; } = [];
}

public sealed class PortalSoftwareUpdate
{
    [JsonPropertyName("title")]
    public string Title { get; init; } = string.Empty;

    [JsonPropertyName("installed_version")]
    public string InstalledVersion { get; init; } = string.Empty;

    [JsonPropertyName("available_version")]
    public string AvailableVersion { get; init; } = string.Empty;

    [JsonPropertyName("status")]
    public string Status { get; init; } = string.Empty;

    [JsonPropertyName("updated_at")]
    public DateTimeOffset? UpdatedAt { get; init; }
}

public sealed class PortalWindowsUpdate
{
    [JsonPropertyName("title")]
    public string Title { get; init; } = string.Empty;

    [JsonPropertyName("update_class")]
    public string UpdateClass { get; init; } = string.Empty;

    [JsonPropertyName("severity")]
    public string Severity { get; init; } = string.Empty;

    [JsonPropertyName("downloaded")]
    public bool Downloaded { get; init; }

    [JsonPropertyName("reboot_required")]
    public bool RebootRequired { get; init; }

    [JsonPropertyName("updated_at")]
    public DateTimeOffset? UpdatedAt { get; init; }
}

public sealed class RequestSummaryItem
{
    public required string Name { get; init; }
    public required string StatusText { get; init; }
    public required string DateText { get; init; }
    public required string StatusBackground { get; init; }
    public required string StatusForeground { get; init; }
}

public sealed class DashboardUpdateItem
{
    public required string IconText { get; init; }
    public required string Title { get; init; }
    public required string Detail { get; init; }
    public required string StatusText { get; init; }
    public required string StatusBackground { get; init; }
    public required string StatusForeground { get; init; }
    public DateTimeOffset? UpdatedAt { get; init; }
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

public sealed class PortalRequest
{
    [JsonPropertyName("id")]
    public string Id { get; init; } = string.Empty;

    [JsonPropertyName("app_id")]
    public string AppId { get; init; } = string.Empty;

    [JsonPropertyName("revision_id")]
    public string RevisionId { get; init; } = string.Empty;

    [JsonPropertyName("status")]
    public string Status { get; init; } = string.Empty;

    [JsonPropertyName("created_at")]
    public DateTimeOffset? CreatedAt { get; init; }

    [JsonPropertyName("decided_at")]
    public DateTimeOffset? DecidedAt { get; init; }

    [JsonPropertyName("decision_note")]
    public string DecisionNote { get; init; } = string.Empty;
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
    ApprovalPending,
    ApprovalRejected,
    Installing,
    Installed,
    Failed,
}

public sealed class AppCard : INotifyPropertyChanged
{
    private Bitmap? _iconImage;

    public required PortalApp App { get; init; }
    public required AppPortalStatus Status { get; init; }
    public PortalRequest? Request { get; init; }

    public event PropertyChangedEventHandler? PropertyChanged;

    public string Id => App.Id;
    public bool HasRequest => Request is not null;
    public Bitmap? IconImage
    {
        get => _iconImage;
        set
        {
            if (ReferenceEquals(_iconImage, value)) return;
            _iconImage = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(HasIcon));
            OnPropertyChanged(nameof(ShowFallbackIcon));
        }
    }
    public bool HasIcon => IconImage is not null;
    public bool ShowFallbackIcon => IconImage is null;
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
        AppPortalStatus.ApprovalPending => "Awaiting approval",
        AppPortalStatus.ApprovalRejected => "Request declined",
        AppPortalStatus.Installing => "Installing",
        AppPortalStatus.Installed => "Installed",
        AppPortalStatus.Failed => "Failed",
        _ => "Available",
    };

    public string ActionText => Status switch
    {
        AppPortalStatus.ApprovalRequired => "Request approval",
        AppPortalStatus.ApprovalPending => "Awaiting approval",
        AppPortalStatus.ApprovalRejected => "Request again",
        AppPortalStatus.Installing => "Installing...",
        AppPortalStatus.Installed => "Installed",
        AppPortalStatus.Failed => "Retry",
        _ => "Install",
    };

    public bool CanInstall => Status is AppPortalStatus.Available
        or AppPortalStatus.ApprovalRequired
        or AppPortalStatus.ApprovalRejected
        or AppPortalStatus.Failed;

    public string StatusBackground => Status switch
    {
        AppPortalStatus.Installed => "#E9F8F1",
        AppPortalStatus.Installing => "#EEF4FF",
        AppPortalStatus.ApprovalPending => "#FFF6E5",
        AppPortalStatus.ApprovalRequired => "#FFF6E5",
        AppPortalStatus.ApprovalRejected => "#FFF0F2",
        AppPortalStatus.Failed => "#FFF0F2",
        _ => "#EEF4FF",
    };

    public string StatusForeground => Status switch
    {
        AppPortalStatus.Installed => "#146B50",
        AppPortalStatus.ApprovalPending => "#9A6400",
        AppPortalStatus.ApprovalRequired => "#9A6400",
        AppPortalStatus.ApprovalRejected => "#B63D4C",
        AppPortalStatus.Failed => "#B63D4C",
        _ => "#2E5CCC",
    };

    public string ScopeText => App.Scope.Type switch
    {
        "Estate" => "Available to your organisation",
        "" => "Assigned by your organisation",
        _ => $"Assigned via {App.Scope.Type}"
             + (string.IsNullOrWhiteSpace(App.Scope.Name)
                 ? string.Empty
                 : $" - {App.Scope.Name}"),
    };

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

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