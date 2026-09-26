$ErrorActionPreference = 'Stop'

function Require-Literal([string]$Text, [string]$Needle, [string]$Message) {
  if (-not $Text.Contains($Needle)) { throw $Message }
}

$inventory = Get-Content 'Agent/chatpass_agent/src/platform/windows/windows_inventory.cpp' -Raw
$service = Get-Content 'Agent/chatpass_agent/src/platform/windows/windows_service.cpp' -Raw
$patchHost = Get-Content 'Agent/chatpass_agent/src/patching/patch_host_main.cpp' -Raw
$logging = Get-Content 'Agent/chatpass_agent/src/util/log.h' -Raw
$h264 = Get-Content 'Agent/chatpass_agent/src/h264_mf_encoder.cpp' -Raw
$agentInstaller = Get-Content 'Agent/chatpass_agent/installer/windows/Hi5CentralAgentSetup.iss' -Raw
$agentBuild = Get-Content 'Agent/chatpass_agent/installer/windows/Build-AgentInstaller.ps1' -Raw
$windowsWorkflow = Get-Content '.github/workflows/windows-installers.yml' -Raw
$cmake = Get-Content 'Agent/chatpass_agent/CMakeLists.txt' -Raw

Require-Literal $inventory 'json LocalUsers()' 'Local-user inventory collector is missing.'
Require-Literal $inventory '{"local_users", localUsers}' 'Inventory snapshot no longer publishes local_users.'
Require-Literal $inventory 'Get-LocalGroupMember -Group ''Administrators''' 'Local administrator membership collection is missing.'
Require-Literal $inventory 'is_admin' 'Local-user administrator annotation is missing.'
Require-Literal $inventory 'saneLinkSpeed' 'Network inventory must suppress Windows unknown-speed sentinel values.'
Require-Literal $inventory 'Microsoft\Windows NT\CurrentVersion\ProfileList' 'Software inventory must enumerate real Windows user profiles instead of LocalSystem HKCU.'
Require-Literal $inventory '"user:" + sidUtf8' 'Per-user software inventory must retain the owning user SID.'
if ($inventory.Contains('HKEY_CURRENT_USER')) { throw 'Service software inventory must not treat LocalSystem HKCU/systemprofile as an end-user software source.' }
Require-Literal $patchHost 'hiveUtf8 == "S-1-5-18"' 'Patch verification must reject LocalSystem uninstall registrations.'
Require-Literal $patchHost 'hiveUtf8 == "S-1-5-19"' 'Patch verification must reject LocalService uninstall registrations.'
Require-Literal $patchHost 'hiveUtf8 == "S-1-5-20"' 'Patch verification must reject NetworkService uninstall registrations.'
Require-Literal $patchHost 'DetectInstallerTechnology(' 'PatchHost must inspect downloaded EXE installer technology before choosing silent arguments.'
Require-Literal $patchHost 'installerTechnologyAutoSelected' 'PatchHost must record when runtime installer-technology detection overrides a generic catalogue hint.'
Require-Literal $patchHost 'VendorInstallStrategies(executionManifest)' 'PatchHost must execute silent strategies from the runtime-detected installer technology.'
Require-Literal $patchHost 'msiManagedSourceCache' 'PatchHost must advertise managed MSI source caching.'
Require-Literal $patchHost 'office_c2r_registry' 'PatchHost must verify Microsoft 365 Apps through Click-to-Run registry state.'
Require-Literal $patchHost 'expectAbsent' 'PatchHost must support absence verification for Click-to-Run uninstall.'
Require-Literal $patchHost 'officeClickToRunUninstall' 'PatchHost must advertise Microsoft 365 Apps uninstall capability.'
Require-Literal $patchHost 'office_odt_sfx' 'PatchHost must support Microsoft Office Deployment Tool self-extracting packages.'
Require-Literal $patchHost 'office-odt-configure.log' 'PatchHost must execute extracted ODT setup.exe with a job-scoped configuration file.'
Require-Literal $patchHost 'ReadMsiProperty(installerPath, L"ProductCode")' 'PatchHost must identify MSI ProductCode before promotion into persistent source cache.'
Require-Literal $patchHost 'InstallerCache\Msi' 'Verified MSI packages must use the managed product-scoped source cache.'
Require-Literal $patchHost 'PurgeOrphanedManagedMsiSources();' 'PatchHost must remove managed MSI sources after their products are no longer registered.'
Require-Literal $service "'msi_cached_source'" 'Software uninstall must prefer a retained managed MSI source when available.'
Require-Literal $service 'Remove-Hi5MsiCache $productCode' 'Verified MSI uninstall must purge its managed source cache.'
Require-Literal $service "'nsis_inplace_silent'" 'Software uninstall must include the NSIS in-place silent strategy.'
Require-Literal $service "' /S _?='" 'NSIS uninstall must pin the install directory to avoid asynchronous temp-child cleanup.'

Require-Literal $service 'BuildNetworkStatsResponse' 'Native live network statistics response is missing.'
Require-Literal $service 'static std::string NowIsoUtc();' 'Network stats UTC helper must be declared before use.'
Require-Literal $service 'GetIfTable(' 'Live network statistics must use the broadly supported native Windows interface counters.'
Require-Literal $service 'network_stats_request' 'Agent no longer handles network_stats_request.'
Require-Literal $service 'network_stats_response' 'Agent no longer emits network_stats_response.'
Require-Literal $service 'receive_bytes' 'Live network receive counters are missing.'
Require-Literal $service 'send_bytes' 'Live network send counters are missing.'
Require-Literal $service 'receive_link_speed_bps' 'Live receive link speed is missing.'
Require-Literal $service 'transmit_link_speed_bps' 'Live transmit link speed is missing.'

# Inventory cadence and endpoint storage/log hygiene.
Require-Literal $service 'for (int i = 0; i < 30 && !stop_.load(); ++i)' 'Scheduled full inventory must run every 30 seconds.'
Require-Literal $service 'if (++housekeepingCycles >= 720)' 'Six-hour ProgramData housekeeping cadence must be preserved when inventory frequency changes.'
Require-Literal $service 'scheduled full inventory skipped while remote session is active' 'Full inventory must remain throttled during active remote sessions.'
Require-Literal $service 'PurgeProgramDataHousekeeping' 'ProgramData housekeeping must run on Agent startup and periodically.'
Require-Literal $service 'C:\ProgramData\Hi5CentralUpgrade' 'Legacy standalone updater storage must be scavenged safely.'
Require-Literal $service 'PatchHost" / L"jobs", 6' 'Abandoned PatchHost job payloads must age out after the safe grace period.'
Require-Literal $service 'remove_all(qualificationRoot' 'Production Agent housekeeping must remove legacy qualification-observer evidence.'
Require-Literal $service 'remove_all(legacyUpgrade' 'Retired Hi5CentralUpgrade storage must be removed completely.'
Require-Literal $service 'Hi5Central\Agent\Temp' 'Temporary uninstall diagnostics must use Agent Temp instead of permanent Logs.'
Require-Literal $service '[MSI failure context]' 'Failed MSI uninstall diagnostics must retain the meaningful failure context in the job result.'
Require-Literal $service '$isMsi = [string]$candidate.strategy -like ''msi_*''' 'MSI uninstall attempts must be identified explicitly.'
Require-Literal $service 'if (-not $isMsi -and -not (Test-Hi5StillInstalled))' 'MSI uninstall must not be terminated merely because registration disappears mid-transaction.'
Require-Literal $service 'Remove-Item -LiteralPath $script:hi5MsiLog' 'Temporary MSI uninstall logs must be removed after the attempt.'
if ($service.Contains('CadStatusPath()')) { throw 'CAD state must not maintain a duplicate unbounded CadStatus.txt log.' }
if ($service.Contains('Hi5Central\Agent\Logs\uninstall-')) { throw 'Verbose MSI uninstall logs must not be written to permanent Agent Logs.' }
Require-Literal $logging 'kDiagnosticMaxBytes = 4ull * 1024ull * 1024ull' 'Diagnostic log cap drifted from the bounded production policy.'
Require-Literal $logging 'kDiagnosticBackups = 2' 'Diagnostic log backup count must remain bounded.'
Require-Literal $logging 'kSupportBackups = 2' 'Support log backup count must remain bounded.'
Require-Literal $h264 'Hi5Central\\Agent\\Temp\\hi5-h264-dump.h264' 'Opt-in raw H.264 diagnostics must live in disposable Agent Temp.'
if ($h264.Contains('Hi5Central\\Agent\\Logs\\hi5-h264-dump.h264')) { throw 'Raw H.264 diagnostics must not pollute the bounded Agent Logs directory.' }

# Qualification Observer is lab-only and must not ship in the normal Agent.
Require-Literal $cmake 'option(HI5_BUILD_QUALIFICATION_OBSERVER "Build the lab-only Windows qualification observer" OFF)' 'Qualification Observer must default to excluded from production builds.'
if ($agentBuild.Contains('hi5central_qualification_observer') -or $agentBuild.Contains('Hi5CentralQualificationObserver.exe')) { throw 'Production Agent build script must not build or require the Qualification Observer.' }
if ($windowsWorkflow.Contains('Hi5CentralQualificationObserver.exe')) { throw 'Production Windows artifact must not publish the Qualification Observer.' }
if ($agentInstaller.Contains('Source: "{#QualificationObserverExePath}"')) { throw 'Production Agent installer must not install the Qualification Observer.' }
Require-Literal $agentInstaller 'Type: files; Name: "{app}\Hi5CentralQualificationObserver.exe"' 'Agent upgrade must delete Qualification Observer binaries left by older builds.'
Require-Literal $agentInstaller 'Type: filesandordirs; Name: "{commonappdata}\Hi5Central\Agent\Qualification"' 'Agent upgrade must remove legacy Qualification Observer evidence.'
Require-Literal $agentInstaller 'Type: filesandordirs; Name: "{commonappdata}\Hi5CentralUpgrade"' 'Agent upgrade must remove the retired standalone upgrade tree.'

Write-Host 'Device Details Agent contract check passed.'
