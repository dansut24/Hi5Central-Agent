#define MyAppName "Hi5Central Agent"
#define MyAppPublisher "Hi5Central"
#define MyAppExeName "Hi5CentralAgent.exe"
#define MyPatchHostExeName "Hi5CentralPatchHost.exe"
#define MyServiceName "Hi5CentralAgent"
#define MyAppVersion GetEnv("HI5_AGENT_VERSION")
#if MyAppVersion == ""
  #define MyAppVersion "1.0.0"
#endif

#ifndef SourceDir
  #define SourceDir "..\..\build\Release"
#endif

#ifndef OutputDir
  #define OutputDir "..\..\dist\installer"
#endif

#ifndef AgentExePath
  #define AgentExePath SourceDir + "\" + MyAppExeName
#endif
#ifndef PatchHostExePath
  #define PatchHostExePath SourceDir + "\" + MyPatchHostExeName
#endif


[Setup]
AppId={{1D8D35E5-54F5-4DB1-996D-BC97E2588D48}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\Hi5Central\Agent
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir={#OutputDir}
OutputBaseFilename=Hi5CentralAgentSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupLogging=yes
CloseApplications=yes
RestartApplications=no

[Dirs]
Name: "{commonappdata}\Hi5Central\Agent"; Permissions: users-readexec admins-full system-full
Name: "{commonappdata}\Hi5Central\Agent\Logs"; Permissions: users-readexec admins-full system-full
Name: "{commonappdata}\Hi5Central\Agent\ChatLogs"; Permissions: users-readexec admins-full system-full

[Files]
Source: "{#AgentExePath}"; DestDir: "{app}"; DestName: "{#MyAppExeName}"; Flags: ignoreversion
Source: "{#PatchHostExePath}"; DestDir: "{app}"; DestName: "{#MyPatchHostExeName}"; Flags: ignoreversion

[InstallDelete]
Type: files; Name: "{app}\hi5tech_cad_winlogon_helper.exe"
Type: files; Name: "{app}\hi5central_sas_helper.exe"
Type: files; Name: "{app}\hi5tech_sas_launcher.exe"

[Run]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--install-service"; StatusMsg: "Installing Hi5Central Agent service..."; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "start {#MyServiceName}"; StatusMsg: "Starting Hi5Central Agent service..."; Flags: runhidden waituntilterminated; Check: not IsUpgradeStopOnly

[UninstallRun]
Filename: "{sys}\sc.exe"; Parameters: "stop {#MyServiceName}"; Flags: runhidden waituntilterminated; RunOnceId: "StopHi5CentralAgentService"
Filename: "{app}\{#MyAppExeName}"; Parameters: "--uninstall-service"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveHi5CentralAgentService"

[Code]
const
  ServiceName = '{#MyServiceName}';

function TrimSlashRight(Value: String): String;
begin
  Result := Value;
  while (Length(Result) > 0) and (Copy(Result, Length(Result), 1) = '/') do
    Delete(Result, Length(Result), 1);
end;

function ParamValue(Name: String; DefaultValue: String): String;
begin
  Result := ExpandConstant('{param:' + Name + '|' + DefaultValue + '}');
end;

function FirstNonEmpty(A: String; B: String): String;
begin
  if A <> '' then
    Result := A
  else
    Result := B;
end;

function JsonStringValue(JsonText: String; Name: String): String;
var
  Tail: String;
  P: Integer;
begin
  Result := '';
  P := Pos('"' + Name + '"', JsonText);
  if P = 0 then
    Exit;

  Tail := Copy(JsonText, P + Length(Name) + 2, Length(JsonText));
  P := Pos(':', Tail);
  if P = 0 then
    Exit;

  Tail := Copy(Tail, P + 1, Length(Tail));
  P := Pos('"', Tail);
  if P = 0 then
    Exit;

  Tail := Copy(Tail, P + 1, Length(Tail));
  P := Pos('"', Tail);
  if P = 0 then
    Exit;

  Result := Copy(Tail, 1, P - 1);
end;

procedure ApplyDeploymentJson(
  var ApiBaseUrl: String;
  var DeploymentId: String;
  var DeploymentSecret: String;
  var InstallSource: String);
var
  ConfigPath: String;
  JsonText: AnsiString;
  Value: String;
begin
  ConfigPath := ParamValue('DEPLOYMENT_CONFIG', '');

  if ConfigPath = '' then
    ConfigPath := ExpandConstant('{src}\Hi5CentralDeployment.json');

  if not FileExists(ConfigPath) then
    ConfigPath := ExpandConstant('{commonappdata}\Hi5Central\Deployment.json');

  if not FileExists(ConfigPath) then
    Exit;

  if not LoadStringFromFile(ConfigPath, JsonText) then
    RaiseException('Unable to read Hi5Central deployment configuration: ' + ConfigPath);

  if ApiBaseUrl = '' then
  begin
    Value := JsonStringValue(JsonText, 'apiBase');
    if Value <> '' then
      ApiBaseUrl := Value;
  end;

  if DeploymentId = '' then
  begin
    Value := JsonStringValue(JsonText, 'deploymentId');
    if Value <> '' then
      DeploymentId := Value;
  end;

  if DeploymentSecret = '' then
  begin
    Value := JsonStringValue(JsonText, 'deploymentSecret');
    if Value <> '' then
      DeploymentSecret := Value;
  end;

  if InstallSource = '' then
    InstallSource := 'deployment-json';
end;

function IsUpgradeStopOnly: Boolean;
begin
  Result := False;
end;

procedure StopExistingService;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop ' + ServiceName, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure HardenAgentConfig(ConfigPath: String);
var
  ResultCode: Integer;
  Args: String;
begin
  Args :=
    '"' + ConfigPath + '" ' +
    '/inheritance:r ' +
    '/grant:r *S-1-5-18:F *S-1-5-32-544:F';

  if not Exec(
    ExpandConstant('{sys}\icacls.exe'),
    Args,
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  ) then
    Log('Hi5Central warning: unable to launch icacls for Agent config.')
  else if ResultCode <> 0 then
    Log('Hi5Central warning: icacls returned ' + IntToStr(ResultCode) + ' for Agent config.');
end;

procedure WriteAgentConfig;
var
  ConfigPath: String;
  ApiBaseUrl: String;
  AgentWsBaseUrl: String;
  EnrollmentToken: String;
  TenantId: String;
  GroupId: String;
  PackageId: String;
  DeploymentId: String;
  DeploymentSecret: String;
  ProvisionBlob: String;
  InstallSource: String;
begin
  ConfigPath := ExpandConstant('{commonappdata}\Hi5Central\Agent\config.ini');

  ApiBaseUrl := TrimSlashRight(FirstNonEmpty(ParamValue('API_BASE_URL', ''), ParamValue('API_URL', '')));
  AgentWsBaseUrl := FirstNonEmpty(ParamValue('AGENT_WS_BASE_URL', ''), ParamValue('WSS_URL', 'wss://rmm.hi5central.com/agent/ws'));

  EnrollmentToken := FirstNonEmpty(ParamValue('ENROLLMENT_TOKEN', ''), ParamValue('ENROLL_TOKEN', ''));
  TenantId := ParamValue('TENANT_ID', '');
  GroupId := ParamValue('GROUP_ID', '');
  DeploymentId := ParamValue('DEPLOYMENT_ID', '');
  DeploymentSecret := ParamValue('DEPLOYMENT_SECRET', '');
  ProvisionBlob := ParamValue('PROVISION_BLOB', '');
  InstallSource := ParamValue('INSTALL_SOURCE', '');

  ApplyDeploymentJson(ApiBaseUrl, DeploymentId, DeploymentSecret, InstallSource);

  if ApiBaseUrl = '' then
    ApiBaseUrl := 'https://rmm.hi5central.com';

  ApiBaseUrl := TrimSlashRight(ApiBaseUrl);
  PackageId := FirstNonEmpty(ParamValue('PACKAGE_ID', ''), DeploymentId);

  if InstallSource = '' then
    InstallSource := 'manual-installer';

  SetIniString('agent', 'api_base_url', ApiBaseUrl, ConfigPath);
  SetIniString('agent', 'agent_ws_base_url', AgentWsBaseUrl, ConfigPath);
  SetIniString('agent', 'install_source', InstallSource, ConfigPath);
  SetIniString('agent', 'installer_version', '{#MyAppVersion}', ConfigPath);

  if EnrollmentToken <> '' then
    SetIniString('agent', 'enrollment_token', EnrollmentToken, ConfigPath);

  if TenantId <> '' then
    SetIniString('agent', 'tenant_id', TenantId, ConfigPath);

  if GroupId <> '' then
    SetIniString('agent', 'group_id', GroupId, ConfigPath);

  if PackageId <> '' then
    SetIniString('agent', 'package_id', PackageId, ConfigPath);

  if DeploymentId <> '' then
    SetIniString('agent', 'deployment_id', DeploymentId, ConfigPath);

  if DeploymentSecret <> '' then
    SetIniString('agent', 'deployment_secret', DeploymentSecret, ConfigPath);

  if ProvisionBlob <> '' then
    SetIniString('agent', 'provision_blob', ProvisionBlob, ConfigPath);

  HardenAgentConfig(ConfigPath);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    StopExistingService;
    ForceDirectories(ExpandConstant('{commonappdata}\Hi5Central\Agent'));
    ForceDirectories(ExpandConstant('{commonappdata}\Hi5Central\Agent\Logs'));
    ForceDirectories(ExpandConstant('{commonappdata}\Hi5Central\Agent\ChatLogs'));
    WriteAgentConfig;
  end;
end;
