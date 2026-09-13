; Hi5Central Viewer - per-user installer
; Installs to: %LOCALAPPDATA%\Hi5Central\Viewer
; Registers protocol handlers under HKCU, no admin required:
;   hi5tech://connect?session_id=...&token=...&mode=console
;   hi5tech://session?session_id=...&token=...&mode=backstage
;   hi5viewer://connect?session_id=...&token=...&mode=console
;   hi5viewer://session?session_id=...&token=...&mode=backstage

#define MyAppName "Hi5Central Viewer"
#define MyAppPublisher "Hi5Central"
#define MyAppExeName "Hi5CentralViewer.exe"
#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#define DistDir "..\..\dist\viewer"

[Setup]
AppId={{8A1B18F7-5C9D-42E6-A889-3A0C13A3280E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Hi5Central\Viewer
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=..\..\dist\installer
OutputBaseFilename=Hi5CentralViewerSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
UninstallDisplayName={#MyAppName}
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#DistDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\web\*"; DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#DistDir}\webview.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#DistDir}\WebView2Loader.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Registry]

; Hi5Central Viewer deep link protocol
Root: HKCU; Subkey: "Software\Classes\hi5central-viewer"; ValueType: string; ValueName: ""; ValueData: "URL:Hi5Central Viewer Protocol"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\hi5central-viewer"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\hi5central-viewer\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Hi5CentralViewer.exe,0"
Root: HKCU; Subkey: "Software\Classes\hi5central-viewer\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Hi5CentralViewer.exe"" ""%1"""

; Backwards-compatible alias while migrating old links
Root: HKCU; Subkey: "Software\Classes\hi5viewer"; ValueType: string; ValueName: ""; ValueData: "URL:Hi5Central Viewer Protocol"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\hi5viewer"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\hi5viewer\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Hi5CentralViewer.exe,0"
Root: HKCU; Subkey: "Software\Classes\hi5viewer\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Hi5CentralViewer.exe"" ""%1"""
; Current portal protocol.
Root: HKCU; Subkey: "Software\Classes\hi5tech"; ValueType: string; ValueData: "URL:Hi5Central Viewer Protocol"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\hi5tech"; ValueName: "URL Protocol"; ValueType: string; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\hi5tech\DefaultIcon"; ValueType: string; ValueData: """{app}\{#MyAppExeName}"",0"
Root: HKCU; Subkey: "Software\Classes\hi5tech\shell\open\command"; ValueType: string; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

; Clearer forward-compatible viewer protocol.
Root: HKCU; Subkey: "Software\Classes\hi5viewer"; ValueType: string; ValueData: "URL:Hi5Central Viewer Protocol"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\hi5viewer"; ValueName: "URL Protocol"; ValueType: string; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\hi5viewer\DefaultIcon"; ValueType: string; ValueData: """{app}\{#MyAppExeName}"",0"
Root: HKCU; Subkey: "Software\Classes\hi5viewer\shell\open\command"; ValueType: string; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

[UninstallDelete]
Type: filesandordirs; Name: "{app}\web"
Type: files; Name: "{app}\webview.dll"
Type: files; Name: "{app}\WebView2Loader.dll"
Type: files; Name: "{app}\{#MyAppExeName}"
Type: dirifempty; Name: "{app}"
Type: dirifempty; Name: "{localappdata}\Hi5Central"
