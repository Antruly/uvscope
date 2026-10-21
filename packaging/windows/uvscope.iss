; uvscope Windows installer (Inno Setup 6+)
; Build with: iscc packaging\windows\uvscope.iss
; Expects the CMake install tree under stage\ (bin/, share/uvscope/web/).

#define MyAppName "uvscope"
#define MyAppVersion "0.4.4"
#define MyAppPublisher "Antruly"
#define MyAppURL "https://github.com/Antruly/uvscope"
#define MyAppExeName "uvscope.exe"

[Setup]
AppId={{7C6E07C3-6C90-4FB8-9B5A-7B8A8F7D2E41}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\uvscope
DefaultGroupName={#MyAppName}
LicenseFile=..\..\LICENSE
OutputDir=..\..\packages
OutputBaseFilename=uvscope-{#MyAppVersion}-setup
SetupIconFile=..\..\packaging\icons\uvscope.ico
UninstallDisplayIcon={app}\bin\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "addtopath"; Description: "Add uvscope to the user PATH"; GroupDescription: "Environment:"; Flags: unchecked

[Files]
Source: "..\..\stage\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\stage\share\uvscope\web\*"; DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "cacert.pem"; DestDir: "{app}\bin"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\uvscope"; Filename: "{app}\bin\{#MyAppExeName}"; IconFilename: "{app}\bin\{#MyAppExeName}"; Parameters: "--open"
Name: "{group}\Uninstall uvscope"; Filename: "{uninstallexe}"; IconFilename: "{app}\bin\{#MyAppExeName}"
Name: "{autodesktop}\uvscope"; Filename: "{app}\bin\{#MyAppExeName}"; IconFilename: "{app}\bin\{#MyAppExeName}"; Parameters: "--open"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; Tasks: addtopath; Check: NeedsAddPath('{app}\bin')

[Run]
Filename: "{app}\bin\{#MyAppExeName}"; Parameters: "--open"; Description: "Launch uvscope"; Flags: nowait postinstall skipifsilent

[Code]
function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;
