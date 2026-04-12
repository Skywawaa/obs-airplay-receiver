[Setup]
AppName=OBS AirPlay Receiver
AppVersion=2.0.0
AppPublisher=aomkoyo
AppPublisherURL=https://github.com/aomkoyo/obs-airplay-receiver
DefaultDirName={commonpf}\obs-studio
DisableDirPage=yes
OutputDir=.
OutputBaseFilename=OBS-AirPlay-Receiver-Setup-v2.0.0
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
UninstallDisplayName=OBS AirPlay Receiver
SetupIconFile=
WizardStyle=modern
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "artifact\obs-airplay-receiver.dll"; DestDir: "{commonappdata}\obs-studio\plugins\obs-airplay-receiver\bin\64bit"; Flags: ignoreversion
Source: "artifact\libcrypto-3-x64.dll"; DestDir: "{commonappdata}\obs-studio\plugins\obs-airplay-receiver\bin\64bit"; Flags: ignoreversion
Source: "artifact\README.md"; DestDir: "{commonappdata}\obs-studio\plugins\obs-airplay-receiver"; Flags: ignoreversion

[Messages]
WelcomeLabel2=This will install the OBS AirPlay Receiver plugin.%n%nThis plugin lets you receive AirPlay screen mirroring from iPhone, iPad, and Mac directly as an OBS source.%n%nRequirements:%n- OBS Studio 30+%n- Apple Bonjour (install iTunes or Bonjour Print Services)

[Code]
function IsOBSInstalled: Boolean;
begin
  Result := DirExists(ExpandConstant('{commonpf}\obs-studio'));
end;

function InitializeSetup: Boolean;
begin
  if not IsOBSInstalled then
  begin
    MsgBox('OBS Studio was not found. Please install OBS Studio first.', mbError, MB_OK);
    Result := False;
  end
  else
    Result := True;
end;

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\obs-airplay-receiver"

[Run]
Filename: "notepad.exe"; Parameters: "{commonappdata}\obs-studio\plugins\obs-airplay-receiver\README.md"; Description: "View README"; Flags: postinstall skipifsilent nowait unchecked
