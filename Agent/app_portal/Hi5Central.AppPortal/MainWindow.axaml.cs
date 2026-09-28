using System.Collections.ObjectModel;
using System.Text.Json;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Threading;
using Hi5Central.AppPortal.Broker;
using Hi5Central.AppPortal.Models;
using Hi5Central.AppPortal.Serialization;
using Hi5Central.AppPortal.Services;

namespace Hi5Central.AppPortal;

public sealed partial class MainWindow : Window
{
    private readonly IAppPortalBroker _broker;
    private readonly ObservableCollection<AppCard> _visibleApps = [];
    private readonly ObservableCollection<RequestSummaryItem> _recentRequests = [];
    private readonly ObservableCollection<RequestSummaryItem> _allRequestItems = [];
    private readonly ObservableCollection<DashboardUpdateItem> _dashboardUpdates = [];
    private readonly ObservableCollection<DashboardUpdateItem> _allUpdateItems = [];
    private readonly AppIconCache _iconCache = new();
    private readonly DispatcherTimer _pollTimer;
    private List<PortalApp> _apps = [];
    private List<PortalInstallation> _installations = [];
    private List<PortalRequest> _requests = [];
    private PortalDevice _device = new();
    private PortalUpdates _updates = new();
    private string _view = "home";
    private string _appSection = "all";
    private bool _busy;
    private bool _loadFailed;

    public MainWindow()
    {
        InitializeComponent();
        _broker = AppPortalBrokerFactory.Create();
        AppsItems.ItemsSource = _visibleApps;
        RecentRequestItems.ItemsSource = _recentRequests;
        RequestsPageItems.ItemsSource = _allRequestItems;
        DashboardUpdateItems.ItemsSource = _dashboardUpdates;
        UpdatesPageItems.ItemsSource = _allUpdateItems;

        _pollTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromSeconds(10),
        };
        _pollTimer.Tick += PollTimer_Tick;

        Opened += MainWindow_Opened;
        Closed += MainWindow_Closed;
    }

    private async void MainWindow_Opened(object? sender, EventArgs e)
    {
        await RefreshCatalogueAsync(showLoading: true);
    }

    private void MainWindow_Closed(object? sender, EventArgs e)
    {
        _pollTimer.Stop();
    }

    private async void PollTimer_Tick(object? sender, EventArgs e)
    {
        if (_busy || (!_loadFailed && !NeedsPolling())) return;
        await RefreshCatalogueAsync(showLoading: false, quiet: true);
    }

    private async void RefreshButton_Click(object? sender, RoutedEventArgs e)
    {
        await RefreshCatalogueAsync(showLoading: _apps.Count == 0);
    }

    private async Task RefreshCatalogueAsync(bool showLoading, bool quiet = false)
    {
        if (_busy) return;

        SetBusy(true, showLoading);
        try
        {
            using var payload = await _broker.GetCatalogueAsync();
            var response = payload.RootElement.Deserialize(PortalJsonContext.Default.PortalCatalogueResponse)
                ?? throw new InvalidDataException("Hi5Central returned an invalid App Portal response.");

            if (!response.Success)
            {
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(response.Error)
                        ? "Hi5Central could not load company software."
                        : response.Error);
            }

            _apps = response.Apps;
            _installations = response.Installations;
            _requests = response.Requests;
            _device = response.Device;
            _updates = response.Updates;
            var recoveredFromLoadFailure = _loadFailed;
            _loadFailed = false;
            RebuildCategories();
            RebuildDashboard();
            ApplyFilters();

            AgentStatusText.Text = "Connected through Hi5Central Agent";
            AgentStatusText.Foreground = Avalonia.Media.Brushes.MediumSeaGreen;

            if (recoveredFromLoadFailure)
            {
                StatusBanner.IsVisible = false;
            }

            if (!quiet)
            {
                ShowStatus(
                    "Catalogue refreshed",
                    _apps.Count == 0
                        ? "No applications have been assigned to you or this device yet."
                        : $"{_apps.Count} application{(_apps.Count == 1 ? string.Empty : "s")} available.",
                    isError: false,
                    autoHide: true);
            }
        }
        catch (Exception ex)
        {
            _loadFailed = true;
            AgentStatusText.Text = "Unable to contact Hi5Central Agent";
            AgentStatusText.Foreground = Avalonia.Media.Brushes.IndianRed;
            ShowStatus(
                "Unable to load company software",
                FriendlyError(ex),
                isError: true,
                autoHide: false);
            ShowEmptyStateForError(FriendlyError(ex));
        }
        finally
        {
            SetBusy(false, showLoading: false);
            UpdatePollState();
        }
    }

    private void SetBusy(bool busy, bool showLoading)
    {
        _busy = busy;
        RefreshButton.IsEnabled = !busy;
        if (showLoading)
        {
            LoadingOverlay.IsVisible = busy;
        }
        else if (!busy)
        {
            LoadingOverlay.IsVisible = false;
        }
    }

    private void RebuildDashboard()
    {
        var pendingRequests = _requests.Count(request =>
            string.Equals(request.Status, "pending", StringComparison.OrdinalIgnoreCase));

        UpdatesCountText.Text = _updates.Total.ToString();
        SidebarUpdateCountText.Text = _updates.Total.ToString();
        UpdatesSummaryText.Text =
            $"{_updates.Windows.Count} Windows  |  {_updates.Software.Count} Application";
        PendingRequestCountText.Text = pendingRequests.ToString();
        PendingRequestSummaryText.Text = pendingRequests == 1
            ? "Awaiting approval"
            : pendingRequests == 0 ? "No requests awaiting approval" : "Awaiting approval";
        AvailableCountText.Text = _apps.Count.ToString();

        var online = string.Equals(_device.WebsocketStatus, "Connected", StringComparison.OrdinalIgnoreCase);
        DeviceHealthTitle.Text = online ? "Device Healthy" : "Device Offline";
        DeviceHealthSubtitle.Text = online
            ? "Connected and managed by Hi5Central"
            : "Waiting for the Hi5Central Agent";
        HeaderDeviceNameText.Text = string.IsNullOrWhiteSpace(_device.Name)
            ? "This device"
            : _device.Name;
        HeaderDeviceStateText.Text = online ? "Online" : "Offline";

        var displayUser = FirstNonEmpty(
            _device.UserDisplayName,
            _device.UserPrincipalName,
            TrimDomainUser(_device.ActiveUser),
            "Signed-in user");
        HeaderUserText.Text = displayUser;
        HeaderUserInitialsText.Text = BuildInitials(displayUser);

        DeviceNameText.Text = string.IsNullOrWhiteSpace(_device.Name) ? "This device" : _device.Name;
        DeviceModelText.Text = FirstNonEmpty(
            string.Join(" ", new[] { _device.Manufacturer, _device.Model }
                .Where(value => !string.IsNullOrWhiteSpace(value))),
            "Managed endpoint");
        DeviceOsText.Text = FirstNonEmpty(
            string.Join(" ", new[] { _device.OperatingSystem, _device.OsVersion }
                .Where(value => !string.IsNullOrWhiteSpace(value))),
            "Windows");
        DeviceHealthText.Text = online ? "Connected" : "Offline";
        DeviceEncryptionText.Text = _device.IsEncrypted switch
        {
            true => "Encryption enabled",
            false => "Encryption not enabled",
            _ => "Encryption not reported",
        };
        DeviceLastSyncText.Text = _device.LastTelemetryAt?.ToLocalTime().ToString("dd MMM yyyy, HH:mm")
            ?? "Not reported";
        DeviceStorageText.Text = FormatStorage(_device.StorageTotalBytes, _device.StorageFreeBytes);
        DeviceUserText.Text = displayUser;

        _recentRequests.Clear();
        _allRequestItems.Clear();
        foreach (var request in _requests.OrderByDescending(item => item.CreatedAt))
        {
            var status = RequestDisplay(request.Status);
            var item = new RequestSummaryItem
            {
                Name = _apps.FirstOrDefault(app =>
                    string.Equals(app.Id, request.AppId, StringComparison.OrdinalIgnoreCase))?.Name
                    ?? "Software request",
                StatusText = status.Text,
                DateText = request.CreatedAt?.ToLocalTime().ToString("dd MMM yyyy") ?? string.Empty,
                StatusBackground = status.Background,
                StatusForeground = status.Foreground,
            };
            _allRequestItems.Add(item);
            if (_recentRequests.Count < 5)
            {
                _recentRequests.Add(item);
            }
        }
        NoRequestsText.IsVisible = _recentRequests.Count == 0;
        RequestsPageEmpty.IsVisible = _allRequestItems.Count == 0;

        var updateItems = new List<DashboardUpdateItem>();
        updateItems.AddRange(_updates.Windows.Select(update => new DashboardUpdateItem
        {
            IconText = "W",
            Title = update.Title,
            Detail = string.IsNullOrWhiteSpace(update.UpdateClass)
                ? "Windows update"
                : $"{ToTitleCase(update.UpdateClass)} update",
            StatusText = update.RebootRequired
                ? "Pending restart"
                : update.Downloaded ? "Ready to install" : "Available",
            StatusBackground = update.RebootRequired ? "#FFF4DE" : "#EAF3FF",
            StatusForeground = update.RebootRequired ? "#A56400" : "#1769D2",
            UpdatedAt = update.UpdatedAt,
        }));
        updateItems.AddRange(_updates.Software.Select(update => new DashboardUpdateItem
        {
            IconText = "A",
            Title = update.Title,
            Detail = string.IsNullOrWhiteSpace(update.AvailableVersion)
                ? "Application update"
                : $"{update.InstalledVersion}  →  {update.AvailableVersion}",
            StatusText = "Update available",
            StatusBackground = "#EAF3FF",
            StatusForeground = "#1769D2",
            UpdatedAt = update.UpdatedAt,
        }));

        _dashboardUpdates.Clear();
        _allUpdateItems.Clear();
        foreach (var update in updateItems.OrderByDescending(item => item.UpdatedAt))
        {
            _allUpdateItems.Add(update);
            if (_dashboardUpdates.Count < 5)
            {
                _dashboardUpdates.Add(update);
            }
        }
        NoUpdatesText.IsVisible = _dashboardUpdates.Count == 0;
        UpdatesPageEmpty.IsVisible = _allUpdateItems.Count == 0;
        UpdatesPageWindowsCountText.Text = _updates.Windows.Count.ToString();
        UpdatesPageSoftwareCountText.Text = _updates.Software.Count.ToString();

        DevicePageNameText.Text = DeviceNameText.Text;
        DevicePageModelText.Text = DeviceModelText.Text;
        DevicePageOsText.Text = DeviceOsText.Text;
        DevicePageHealthText.Text = DeviceHealthText.Text;
        DevicePageEncryptionText.Text = DeviceEncryptionText.Text;
        DevicePageLastSyncText.Text = DeviceLastSyncText.Text;
        DevicePageStorageText.Text = DeviceStorageText.Text;
        DevicePageUserText.Text = DeviceUserText.Text;
        DevicePageAgentText.Text = string.IsNullOrWhiteSpace(_device.AgentVersion)
            ? "Not reported"
            : _device.AgentVersion;

        SettingsAgentVersionText.Text = string.IsNullOrWhiteSpace(_device.AgentVersion)
            ? "Not reported"
            : _device.AgentVersion;
        SettingsPortalVersionText.Text =
            typeof(MainWindow).Assembly.GetName().Version?.ToString(3) ?? "Development";
    }

    private static (string Text, string Background, string Foreground) RequestDisplay(string value)
        => value.Trim().ToLowerInvariant() switch
        {
            "pending" => ("Awaiting approval", "#FFF4DE", "#A56400"),
            "approved" => ("Approved", "#E9F8F1", "#147A59"),
            "fulfilled" => ("Installed", "#E9F8F1", "#147A59"),
            "rejected" => ("Request declined", "#FFF0F2", "#B63D4C"),
            "cancelled" => ("Cancelled", "#F2F4F7", "#64748B"),
            _ => ("Requested", "#EAF3FF", "#1769D2"),
        };

    private static string FirstNonEmpty(params string[] values)
        => values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? string.Empty;

    private static string TrimDomainUser(string value)
    {
        if (string.IsNullOrWhiteSpace(value)) return string.Empty;
        var slash = value.LastIndexOf('\\');
        return slash >= 0 && slash + 1 < value.Length ? value[(slash + 1)..] : value;
    }

    private static string BuildInitials(string value)
    {
        var parts = value.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length == 0) return "H5";
        return string.Concat(parts.Take(2).Select(part => char.ToUpperInvariant(part[0])));
    }

    private static string ToTitleCase(string value)
        => string.IsNullOrWhiteSpace(value)
            ? string.Empty
            : char.ToUpperInvariant(value[0]) + value[1..].ToLowerInvariant();

    private static string FormatStorage(long? totalBytes, long? freeBytes)
    {
        if (totalBytes is null or <= 0) return "Not reported";
        var totalGb = totalBytes.Value / 1024d / 1024d / 1024d;
        if (freeBytes is null or < 0) return $"{totalGb:0} GB";
        var freeGb = freeBytes.Value / 1024d / 1024d / 1024d;
        return $"{totalGb:0} GB ({freeGb:0} GB free)";
    }

    private void RebuildCategories()
    {
        var categories = _apps
            .Select(app => string.IsNullOrWhiteSpace(app.Category) ? "Company software" : app.Category)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(value => value, StringComparer.OrdinalIgnoreCase)
            .ToList();

        var selected = CategoryCombo.SelectedItem as string ?? "Categories";
        var values = new List<string> { "Categories" };
        values.AddRange(categories);
        CategoryCombo.ItemsSource = values;
        CategoryCombo.SelectedItem = values.Contains(selected, StringComparer.OrdinalIgnoreCase)
            ? values.First(value => string.Equals(value, selected, StringComparison.OrdinalIgnoreCase))
            : "Categories";
    }

    private void ApplyFilters()
    {
        var query = SearchBox.Text?.Trim() ?? string.Empty;
        var category = CategoryCombo.SelectedItem as string ?? "Categories";
        var cards = BuildCards();

        IEnumerable<AppCard> filtered = cards;

        if (!string.IsNullOrWhiteSpace(query))
        {
            filtered = filtered.Where(card =>
                card.Name.Contains(query, StringComparison.OrdinalIgnoreCase)
                || card.Publisher.Contains(query, StringComparison.OrdinalIgnoreCase)
                || card.Description.Contains(query, StringComparison.OrdinalIgnoreCase));
        }

        if (!string.Equals(category, "Categories", StringComparison.OrdinalIgnoreCase)
            && !string.Equals(category, "All categories", StringComparison.OrdinalIgnoreCase))
        {
            filtered = filtered.Where(card =>
                string.Equals(card.Category, category, StringComparison.OrdinalIgnoreCase));
        }

        if (_appSection == "company")
        {
            filtered = filtered.Where(card =>
                string.Equals(card.App.SourceType, "custom", StringComparison.OrdinalIgnoreCase));
        }

        filtered = _view switch
        {
            "installed" => filtered.Where(card => card.Status == AppPortalStatus.Installed),
            "requests" => filtered.Where(card => card.HasRequest),
            _ => filtered,
        };

        var result = (_appSection == "popular"
                ? filtered.OrderByDescending(card => card.App.InstallCount)
                    .ThenBy(card => card.Name, StringComparer.OrdinalIgnoreCase)
                : filtered.OrderBy(card => card.Name, StringComparer.OrdinalIgnoreCase))
            .ToList();

        _visibleApps.Clear();
        foreach (var card in result)
        {
            _visibleApps.Add(card);
        }

        AvailableCountText.Text = _apps.Count.ToString();
        ResultCountText.Text = $"{result.Count} of {_apps.Count} apps";
        EmptyState.IsVisible = result.Count == 0;
        AppsScroll.IsVisible = result.Count > 0;

        if (result.Count == 0)
        {
            if (_apps.Count == 0)
            {
                EmptyTitle.Text = "No software has been assigned yet";
                EmptyDescription.Text =
                    "Your IT team can publish Hi5Central catalogue applications or company-specific software to this portal.";
            }
            else if (_view == "requests")
            {
                EmptyTitle.Text = "No software requests";
                EmptyDescription.Text =
                    "Applications you request for approval will appear here with their current decision and install status.";
            }
            else
            {
                EmptyTitle.Text = "No matching applications";
                EmptyDescription.Text =
                    "Try a different search term, category or navigation filter.";
            }
        }

        UpdateHeroText();
        UpdatePageVisibility();
        UpdatePollState();
        _ = LoadVisibleIconsAsync();
    }

    private void UpdatePageVisibility()
    {
        var isHome = _view == "home";
        var isApps = _view is "all" or "installed";

        HomeSummaryPanel.IsVisible = isHome;
        AppCataloguePanel.IsVisible = isHome || isApps;
        HomeOverviewPanel.IsVisible = isHome;
        HomeSupportPanel.IsVisible = isHome;

        UpdatesPagePanel.IsVisible = _view == "updates";
        RequestsPagePanel.IsVisible = _view == "requests";
        DevicePagePanel.IsVisible = _view == "device";
        SupportPagePanel.IsVisible = _view == "support";
        SettingsPagePanel.IsVisible = _view == "settings";

        AppsSectionTitle.Text = _view switch
        {
            "installed" => "Installed Applications",
            "all" => "Applications",
            _ => "Featured Apps",
        };
        AppTabsPanel.IsVisible = _view != "installed";
    }

    private async Task LoadVisibleIconsAsync()
    {
        var candidates = _visibleApps
            .Where(card => !string.IsNullOrWhiteSpace(card.App.IconUrl) && !card.HasIcon)
            .ToArray();

        await Task.WhenAll(candidates.Select(async card =>
        {
            var bitmap = await _iconCache.GetAsync(card.App.IconUrl).ConfigureAwait(false);
            if (bitmap is null) return;

            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                if (_visibleApps.Contains(card))
                {
                    card.IconImage = bitmap;
                }
            });
        }));
    }

    private List<AppCard> BuildCards()
    {
        var latestByApp = new Dictionary<string, PortalInstallation>(StringComparer.OrdinalIgnoreCase);
        foreach (var installation in _installations)
        {
            if (string.IsNullOrWhiteSpace(installation.AppId)) continue;
            if (!latestByApp.ContainsKey(installation.AppId))
            {
                latestByApp[installation.AppId] = installation;
            }
        }

        var latestRequestByApp = new Dictionary<string, PortalRequest>(StringComparer.OrdinalIgnoreCase);
        foreach (var request in _requests)
        {
            if (string.IsNullOrWhiteSpace(request.AppId)) continue;
            if (!latestRequestByApp.ContainsKey(request.AppId))
            {
                latestRequestByApp[request.AppId] = request;
            }
        }

        return _apps
            .Select(app =>
            {
                latestByApp.TryGetValue(app.Id, out var installation);
                latestRequestByApp.TryGetValue(app.Id, out var request);
                return new AppCard
                {
                    App = app,
                    Request = request,
                    Status = ResolveStatus(app, installation, request),
                };
            })
            .ToList();
    }

    private static AppPortalStatus ResolveStatus(
        PortalApp app,
        PortalInstallation? installation,
        PortalRequest? request)
    {
        var requestIsCurrent = request is not null
            && (installation is null
                || request.CreatedAt is null
                || installation.CreatedAt is null
                || request.CreatedAt >= installation.CreatedAt);

        if (requestIsCurrent)
        {
            var requestStatus = request!.Status.Trim().ToLowerInvariant();
            if (requestStatus == "pending")
            {
                return AppPortalStatus.ApprovalPending;
            }

            if (requestStatus == "rejected")
            {
                return AppPortalStatus.ApprovalRejected;
            }

            if (requestStatus == "fulfilled" && installation is null)
            {
                return AppPortalStatus.Installing;
            }
        }

        if (installation is not null)
        {
            var lifecycle = installation.InstallationStatus.Trim().ToLowerInvariant();
            var job = installation.JobStatus.Trim().ToLowerInvariant();

            if (lifecycle is "queued" or "running" or "requested"
                || job is "queued" or "claimed")
            {
                return AppPortalStatus.Installing;
            }

            if (app.Installed || lifecycle == "succeeded" || job == "completed")
            {
                return AppPortalStatus.Installed;
            }

            if (lifecycle is "failed" or "cancelled"
                || job is "failed" or "cancelled")
            {
                return app.Installed ? AppPortalStatus.Installed : AppPortalStatus.Failed;
            }
        }

        if (app.Installed)
        {
            return AppPortalStatus.Installed;
        }

        return string.Equals(app.Intent, "approval_required", StringComparison.OrdinalIgnoreCase)
            ? AppPortalStatus.ApprovalRequired
            : AppPortalStatus.Available;
    }

    private async void InstallButton_Click(object? sender, RoutedEventArgs e)
    {
        if (_busy || sender is not Button { DataContext: AppCard card } || !card.CanInstall)
        {
            return;
        }

        SetBusy(true, showLoading: false);
        try
        {
            using var payload = await _broker.InstallAsync(card.Id);
            var response = payload.RootElement.Deserialize(PortalJsonContext.Default.PortalInstallResponse)
                ?? throw new InvalidDataException("Hi5Central returned an invalid install response.");

            if (!response.Success)
            {
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(response.Error)
                        ? "Hi5Central could not start this installation."
                        : response.Error);
            }

            ShowStatus(
                response.ApprovalRequired ? "Request sent" : "Installation started",
                response.ApprovalRequired
                    ? $"{card.Name} has been sent to IT for approval."
                    : $"{card.Name} is being installed securely in the background.",
                isError: false,
                autoHide: true);

            SetBusy(false, showLoading: false);
            await RefreshCatalogueAsync(showLoading: false, quiet: true);
        }
        catch (Exception ex)
        {
            ShowStatus(
                $"Could not install {card.Name}",
                FriendlyError(ex),
                isError: true,
                autoHide: false);
        }
        finally
        {
            SetBusy(false, showLoading: false);
            UpdatePollState();
        }
    }

    private void NavButton_Click(object? sender, RoutedEventArgs e)
    {
        if (sender is not Button button) return;

        _view = button.Tag?.ToString() ?? "home";

        foreach (var nav in new[]
                 { NavHome, NavAll, NavUpdates, NavInstalled, NavRequests, NavDevice, NavSupport, NavSettings })
        {
            nav.Classes.Remove("active");
            if (ReferenceEquals(nav, button))
            {
                nav.Classes.Add("active");
            }
        }

        ApplyFilters();
    }

    private void AppSectionButton_Click(object? sender, RoutedEventArgs e)
    {
        if (sender is not Button button) return;

        _appSection = button.Tag?.ToString() ?? "all";
        foreach (var tab in new[] { AppTabAll, AppTabCompany, AppTabPopular })
        {
            tab.Classes.Remove("active");
            if (ReferenceEquals(tab, button))
            {
                tab.Classes.Add("active");
            }
        }

        ApplyFilters();
    }

    private void ViewAllApps_Click(object? sender, RoutedEventArgs e)
    {
        _view = "all";
        foreach (var nav in new[]
                 { NavHome, NavAll, NavUpdates, NavInstalled, NavRequests, NavDevice, NavSupport, NavSettings })
        {
            nav.Classes.Remove("active");
        }
        NavAll.Classes.Add("active");
        ApplyFilters();
    }

    private void SearchBox_TextChanged(object? sender, TextChangedEventArgs e)
    {
        ApplyFilters();
    }

    private void CategoryCombo_SelectionChanged(object? sender, SelectionChangedEventArgs e)
    {
        ApplyFilters();
    }

    private void UpdateHeroText()
    {
        switch (_view)
        {
            case "installed":
                HeroEyebrow.Text = "INSTALLED SOFTWARE";
                HeroTitle.Text = "Software already on this device.";
                HeroDescription.Text =
                    "View applications installed through Hi5Central. Installation status refreshes automatically.";
                break;
            case "requests":
                HeroEyebrow.Text = "APPROVAL REQUESTS";
                HeroTitle.Text = "Software that needs approval.";
                HeroDescription.Text =
                    "Request access to approved applications. Your IT team can review and approve them centrally.";
                break;
            case "all":
                HeroEyebrow.Text = "APPLICATIONS";
                HeroTitle.Text = "Apps";
                HeroDescription.Text =
                    "Browse software approved for your user, device, site, group or organisation.";
                break;
            case "updates":
                HeroEyebrow.Text = "UPDATES";
                HeroTitle.Text = "Updates";
                HeroDescription.Text =
                    "Review Windows and application updates reported by this device.";
                break;
            case "device":
                HeroEyebrow.Text = "MY DEVICE";
                HeroTitle.Text = "My Device";
                HeroDescription.Text =
                    "View device health, storage, encryption and management information.";
                break;
            case "support":
                HeroEyebrow.Text = "HELP & SUPPORT";
                HeroTitle.Text = "Help & Support";
                HeroDescription.Text =
                    "Request assistance, open a ticket or connect with your IT team.";
                break;
            case "settings":
                HeroEyebrow.Text = "SETTINGS";
                HeroTitle.Text = "Settings";
                HeroDescription.Text =
                    "Manage Self Service preferences for this user.";
                break;
            default:
                HeroEyebrow.Text = "SELF SERVICE";
                HeroTitle.Text = "Hi5 Self Service";
                HeroDescription.Text =
                    "Your one stop for apps, updates, device management and IT support.";
                break;
        }
    }

    private bool NeedsPolling()
        => BuildCards().Any(card => card.Status is AppPortalStatus.Installing
            or AppPortalStatus.ApprovalPending);

    private void UpdatePollState()
    {
        if (_loadFailed || NeedsPolling())
        {
            if (!_pollTimer.IsEnabled) _pollTimer.Start();
        }
        else
        {
            _pollTimer.Stop();
        }
    }

    private void ShowEmptyStateForError(string message)
    {
        _visibleApps.Clear();
        AppsScroll.IsVisible = false;
        EmptyState.IsVisible = true;
        EmptyTitle.Text = "Unable to load company software";
        EmptyDescription.Text = message;
        ResultCountText.Text = "Catalogue unavailable";
    }

    private async void ShowStatus(
        string title,
        string message,
        bool isError,
        bool autoHide)
    {
        StatusTitle.Text = title;
        StatusMessage.Text = message;
        StatusBanner.Background = new Avalonia.Media.SolidColorBrush(
            isError ? Avalonia.Media.Color.FromRgb(255, 240, 242) : Avalonia.Media.Color.FromRgb(234, 248, 242));
        StatusBanner.BorderBrush = new Avalonia.Media.SolidColorBrush(
            isError ? Avalonia.Media.Color.FromRgb(240, 201, 207) : Avalonia.Media.Color.FromRgb(197, 235, 221));
        StatusTitle.Foreground = new Avalonia.Media.SolidColorBrush(
            isError ? Avalonia.Media.Color.FromRgb(182, 61, 76) : Avalonia.Media.Color.FromRgb(20, 107, 80));
        StatusBanner.IsVisible = true;

        if (autoHide)
        {
            await Task.Delay(TimeSpan.FromSeconds(5));
            StatusBanner.IsVisible = false;
        }
    }

    private void DismissStatus_Click(object? sender, RoutedEventArgs e)
    {
        StatusBanner.IsVisible = false;
    }

    private static string FriendlyError(Exception exception)
    {
        return exception switch
        {
            OperationCanceledException =>
                "Hi5Central did not respond in time. Self Service will retry automatically.",
            JsonException =>
                "Hi5Central returned device data in an unexpected format. Self Service will retry automatically.",
            PlatformNotSupportedException => exception.Message,
            _ when !string.IsNullOrWhiteSpace(exception.Message) => exception.Message,
            _ => "Please try again. If the problem continues, contact your IT team.",
        };
    }
}