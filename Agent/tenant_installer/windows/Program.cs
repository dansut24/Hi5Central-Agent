using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Net.Http.Json;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Hi5Central.AgentTenantInstaller;

internal sealed record DeploymentConfig(
    int SchemaVersion,
    string ApiBase,
    string DeploymentId,
    string DeploymentSecret);

internal static class Program
{
    private const string SetupUrl = "https://downloads.hi5central.com/agent/latest/Hi5CentralAgentSetup.exe";
    private const string DefaultConfigName = "Hi5CentralDeployment.json";

    [STAThread]
    private static async Task Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        var quiet = HasArg(args, "--quiet");
        var tempDir = Path.Combine(Path.GetTempPath(), "Hi5Central", "DeploymentInstaller", Guid.NewGuid().ToString("N"));
        var setupPath = Path.Combine(tempDir, "Hi5CentralAgentSetup.exe");

        try
        {
            Directory.CreateDirectory(tempDir);
            using var client = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
            var config = await LoadConfigAsync(client, args);

            using var tokenResponse = await client.PostAsJsonAsync(
                $"{config.ApiBase.TrimEnd('/')}/api/v1/agent/deployments/{config.DeploymentId}/enrollment-token",
                new { deploymentSecret = config.DeploymentSecret });
            tokenResponse.EnsureSuccessStatusCode();

            using var tokenDocument = JsonDocument.Parse(await tokenResponse.Content.ReadAsStringAsync());
            var enrollmentToken = tokenDocument.RootElement.GetProperty("enrollmentToken").GetString();
            if (string.IsNullOrWhiteSpace(enrollmentToken))
                throw new InvalidOperationException("Hi5Central did not return an enrollment token.");

            using (var response = await client.GetAsync(SetupUrl, HttpCompletionOption.ResponseHeadersRead))
            {
                response.EnsureSuccessStatusCode();
                await using var source = await response.Content.ReadAsStreamAsync();
                await using var destination = File.Create(setupPath);
                await source.CopyToAsync(destination);
            }

            var process = Process.Start(new ProcessStartInfo
            {
                FileName = setupPath,
                UseShellExecute = false,
                CreateNoWindow = true,
                Arguments =
                    $"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART " +
                    $"/ENROLLMENT_TOKEN=\"{enrollmentToken}\" /API_BASE_URL=\"{config.ApiBase}\" " +
                    $"/INSTALL_SOURCE=\"deployment-json\""
            });

            if (process is null)
                throw new InvalidOperationException("The Hi5Central Agent setup process could not be started.");

            await process.WaitForExitAsync();
            if (process.ExitCode != 0)
                throw new InvalidOperationException($"Hi5Central Agent setup exited with code {process.ExitCode}.");

            if (!quiet)
            {
                MessageBox.Show(
                    "Hi5Central Agent installed and enrolled successfully.",
                    "Hi5Central Agent",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Information);
            }
        }
        catch (Exception ex)
        {
            if (!quiet)
            {
                MessageBox.Show(
                    "Hi5Central Agent could not be installed.\n\n" + ex.Message,
                    "Hi5Central Agent",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Error);
            }
            Environment.ExitCode = 1;
        }
        finally
        {
            try { Directory.Delete(tempDir, recursive: true); } catch { }
        }
    }

    private static bool HasArg(string[] args, string name) =>
        args.Any(arg => string.Equals(arg, name, StringComparison.OrdinalIgnoreCase));

    private static string? ArgValue(string[] args, string name)
    {
        for (var i = 0; i < args.Length - 1; i++)
        {
            if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
                return args[i + 1];
        }
        return null;
    }

    private static async Task<DeploymentConfig> LoadConfigAsync(HttpClient client, string[] args)
    {
        var source =
            ArgValue(args, "--config") ??
            ArgValue(args, "--config-url") ??
            Environment.GetEnvironmentVariable("HI5_DEPLOYMENT_CONFIG");

        if (string.IsNullOrWhiteSpace(source))
        {
            var adjacent = Path.Combine(AppContext.BaseDirectory, DefaultConfigName);
            if (File.Exists(adjacent)) source = adjacent;
        }

        if (string.IsNullOrWhiteSpace(source))
            throw new InvalidOperationException(
                $"Deployment configuration was not found. Place {DefaultConfigName} beside this installer or pass --config <path-or-url>.");

        string json;
        if (Uri.TryCreate(source, UriKind.Absolute, out var uri) &&
            (uri.Scheme == Uri.UriSchemeHttps || uri.Scheme == Uri.UriSchemeHttp))
        {
            if (uri.Scheme != Uri.UriSchemeHttps)
                throw new InvalidOperationException("Deployment configuration URLs must use HTTPS.");
            json = await client.GetStringAsync(uri);
        }
        else
        {
            json = await File.ReadAllTextAsync(Path.GetFullPath(source));
        }

        var config = JsonSerializer.Deserialize<DeploymentConfig>(
            json,
            new JsonSerializerOptions { PropertyNameCaseInsensitive = true });

        if (config is null || config.SchemaVersion != 1)
            throw new InvalidOperationException("Deployment configuration is invalid or unsupported.");
        if (!Uri.TryCreate(config.ApiBase, UriKind.Absolute, out var apiUri) || apiUri.Scheme != Uri.UriSchemeHttps)
            throw new InvalidOperationException("Deployment API base must use HTTPS.");
        if (!Guid.TryParse(config.DeploymentId, out _))
            throw new InvalidOperationException("Deployment ID is invalid.");
        if (string.IsNullOrWhiteSpace(config.DeploymentSecret))
            throw new InvalidOperationException("Deployment credential is missing.");

        return config;
    }
}