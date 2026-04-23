[Setup]
AppName=OBS AirPlay Receiver
AppVersion=2.0.1
AppPublisher=aomkoyo
AppPublisherURL=https://github.com/aomkoyo/obs-airplay-receiver
DefaultDirName={commonappdata}\obs-studio\plugins\obs-airplay-receiver
DirExistsWarning=no
OutputDir=.
OutputBaseFilename=OBS-AirPlay-Receiver-Setup-v2.0.1
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
UninstallDisplayName=OBS AirPlay Receiver
WizardStyle=modern
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "artifact\obs-airplay-receiver.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "artifact\libcrypto-*-x64.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "artifact\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Messages]
WelcomeLabel2=This will install the OBS AirPlay Receiver plugin.%n%nThis plugin lets you receive AirPlay screen mirroring from iPhone, iPad, and Mac directly as an OBS source.%n%nRequirements:%n- OBS Studio 30+%n- Apple Bonjour (install iTunes or Bonjour Print Services)
SelectDirLabel3=Select the OBS Studio plugins folder. The default should be correct for most installations.

[Code]
function FindOBSPluginDir: String;
var
  InstallPath: String;
begin
  // Try registry first
  if RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', InstallPath) then
  begin
    Result := ExpandConstant('{commonappdata}') + '\obs-studio\plugins\obs-airplay-receiver';
    Exit;
  end;
  // Check common paths
  if DirExists(ExpandConstant('{commonpf}\obs-studio')) then
  begin
    Result := ExpandConstant('{commonappdata}') + '\obs-studio\plugins\obs-airplay-receiver';
    Exit;
  end;
  // Default
  Result := ExpandConstant('{commonappdata}') + '\obs-studio\plugins\obs-airplay-receiver';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
end;

procedure InitializeWizard;
begin
  WizardForm.DirEdit.Text := FindOBSPluginDir;
end;

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
