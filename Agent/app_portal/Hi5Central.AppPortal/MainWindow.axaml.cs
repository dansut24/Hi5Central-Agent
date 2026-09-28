using System.Collections.ObjectModel;
using System.Text.Json;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Threading;
using Hi5Central.AppPortal.Broker;
using Hi5Central.AppPortal.Models;

namespace Hi5Central.AppPortal;

public sealed partial class MainWindow : Window
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        PropertyNameCaseInsensitive = true,
    };

    private readonly IAppPortalBroker _broker;
    private readonly ObservableCollection<AppCard> _visibleApps = [];
    private readonly DispatcherTimer _pollTimer;
    private List<PortalApp> _apps = [];
    private List<PortalInstallation> _installations = [];
    private string _view = "home";
    private bool _busy;

    public MainWindow()
    {
        InitializeComponent();
        _broker = AppPortalBrokerFactory.Create();
        AppsItems.ItemsSource = _visibleApps;

        _pollTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromSeconds(6),
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
        if (_busy || !HasInstallingApps()) return;
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
            var response = payload.RootElement.Deserialize<PortalCatalogueResponse>(JsonOptions)
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
            RebuildCategories();
            ApplyFilters();

            AgentStatusText.Text = "Connected through Hi5Central Agent";
            AgentStatusText.Foreground = Avalonia.Media.Brushes.MediumSeaGreen;

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

    private void RebuildCategories()
    {
        var categories = _apps
            .Select(app => string.IsNullOrWhiteSpace(app.Category) ? "Company software" : app.Category)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(value => value, StringComparer.OrdinalIgnoreCase)
            .ToList();

        var selected = CategoryCombo.SelectedItem as string ?? "All categories";
        var values = new List<string> { "All categories" };
        values.AddRange(categories);
        CategoryCombo.ItemsSource = values;
        CategoryCombo.SelectedItem = values.Contains(selected, StringComparer.OrdinalIgnoreCase)
            ? values.First(value => string.Equals(value, selected, StringComparison.OrdinalIgnoreCase))
            : "All categories";
    }

    private void ApplyFilters()
    {
        var query = SearchBox.Text?.Trim() ?? string.Empty;
        var category = CategoryCombo.SelectedItem as string ?? "All categories";
        var cards = BuildCards();

        IEnumerable<AppCard> filtered = cards;

        if (!string.IsNullOrWhiteSpace(query))
        {
            filtered = filtered.Where(card =>
                card.Name.Contains(query, StringComparison.OrdinalIgnoreCase)
                || card.Publisher.Contains(query, StringComparison.OrdinalIgnoreCase)
                || card.Description.Contains(query, StringComparison.OrdinalIgnoreCase));
        }

        if (!string.Equals(category, "All categories", StringComparison.OrdinalIgnoreCase))
        {
            filtered = filtered.Where(card =>
                string.Equals(card.Category, category, StringComparison.OrdinalIgnoreCase));
        }

        filtered = _view switch
        {
            "installed" => filtered.Where(card => card.Status == AppPortalStatus.Installed),
            "requests" => filtered.Where(card => card.Status == AppPortalStatus.ApprovalRequired),
            _ => filtered,
        };

        var result = filtered.OrderBy(card => card.Name, StringComparer.OrdinalIgnoreCase).ToList();

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
            else
            {
                EmptyTitle.Text = "No matching applications";
                EmptyDescription.Text =
                    "Try a different search term, category or navigation filter.";
            }
        }

        UpdateHeroText();
        UpdatePollState();
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

        return _apps
            .Select(app => new AppCard
            {
                App = app,
                Status = ResolveStatus(
                    app,
                    latestByApp.TryGetValue(app.Id, out var install) ? install : null),
            })
            .ToList();
    }

    private static AppPortalStatus ResolveStatus(PortalApp app, PortalInstallation? installation)
    {
        if (installation is not null)
        {
            var lifecycle = installation.InstallationStatus.Trim().ToLowerInvariant();
            var job = installation.JobStatus.Trim().ToLowerInvariant();

            if (lifecycle is "queued" or "running" or "requested"
                || job is "queued" or "claimed")
            {
                return AppPortalStatus.Installing;
            }

            if (lifecycle == "succeeded" || job == "completed")
            {
                return AppPortalStatus.Installed;
            }

            if (lifecycle is "failed" or "cancelled"
                || job is "failed" or "cancelled")
            {
                return AppPortalStatus.Failed;
            }
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
            var response = payload.RootElement.Deserialize<PortalInstallResponse>(JsonOptions)
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

        foreach (var nav in new[] { NavHome, NavAll, NavInstalled, NavRequests })
        {
            nav.Classes.Remove("active");
            if (ReferenceEquals(nav, button))
            {
                nav.Classes.Add("active");
            }
        }

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
                HeroEyebrow.Text = "ALL APPLICATIONS";
                HeroTitle.Text = "Everything available to you.";
                HeroDescription.Text =
                    "Browse software approved for your user, device, site, group or organisation.";
                break;
            default:
                HeroEyebrow.Text = "COMPANY SOFTWARE";
                HeroTitle.Text = "Company software, ready when you need it.";
                HeroDescription.Text =
                    "Install applications approved for you or this device. Hi5Central handles elevation securely in the background.";
                break;
        }
    }

    private bool HasInstallingApps()
        => BuildCards().Any(card => card.Status == AppPortalStatus.Installing);

    private void UpdatePollState()
    {
        if (HasInstallingApps())
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
            OperationCanceledException => "The Hi5Central Agent did not respond in time. Please try again.",
            PlatformNotSupportedException => exception.Message,
            _ when !string.IsNullOrWhiteSpace(exception.Message) => exception.Message,
            _ => "Please try again. If the problem continues, contact your IT team.",
        };
    }
}