using System.Diagnostics;
using System.Net.Http;
using System.Windows.Forms;

namespace Hi5Central.AgentTenantInstaller;

internal static class Program
{
    private const string DeploymentId = "__DEPLOYMENT_ID__";
    private const string DeploymentSecret = "__DEPLOYMENT_SECRET__";
    private const string ApiBase = "__API_BASE__";
    private const string SetupUrl = "https://downloads.hi5central.com/agent/latest/Hi5CentralAgentSetup.exe";

    [STAThread]
    private static async Task Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        var quiet = args.Any(arg => string.Equals(arg, "--quiet", StringComparison.OrdinalIgnoreCase));
        var tempDir = Path.Combine(Path.GetTempPath(), "Hi5Central", "TenantInstaller", Guid.NewGuid().ToString("N"));
        var setupPath = Path.Combine(tempDir, "Hi5CentralAgentSetup.exe");

        try
        {
            Directory.CreateDirectory(tempDir);
            using var client = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
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
                    $"/DEPLOYMENT_ID=\"{DeploymentId}\" /DEPLOYMENT_SECRET=\"{DeploymentSecret}\" " +
                    $"/PACKAGE_ID=\"{DeploymentId}\" /API_BASE_URL=\"{ApiBase}\" " +
                    $"/INSTALL_SOURCE=\"tenant-native-installer\""
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
}
