#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #error SourceDir must point to a staged distribution directory.
#endif
#ifndef InstallerOutput
  #define InstallerOutput "..\out\installers"
#endif

[Setup]
AppId={{914F343C-4302-4BD7-83A4-385AF6D70448}
AppName=WindowsWidget · 随行组件
AppVersion={#AppVersion}
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\WindowsWidget
DefaultGroupName=WindowsWidget
DisableProgramGroupPage=auto
DisableDirPage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
OutputDir={#InstallerOutput}
OutputBaseFilename=WindowsWidget-{#AppVersion}-x64-Setup
SetupIconFile=..\assets\icons\app.ico
UninstallDisplayIcon={app}\WindowsWidget.exe
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
MergeDuplicateFiles=yes
CloseApplications=yes
CloseApplicationsFilter=*.exe,*.dll
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesesimplified.DesktopIcon=创建桌面快捷方式
english.DesktopIcon=Create a desktop shortcut
chinesesimplified.LaunchApp=启动随行组件
english.LaunchApp=Launch WindowsWidget
chinesesimplified.CloseFailed=随行组件仍在运行，请从托盘退出程序后重试。
english.CloseFailed=WindowsWidget is still running. Quit it from the tray and try again.

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\随行组件"; Filename: "{app}\WindowsWidget.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\随行组件"; Filename: "{app}\WindowsWidget.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\WindowsWidget.exe"; Description: "{cm:LaunchApp}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[Code]
const
  CompanionMutex = 'Local\WindowsWidget.CompanionDemo.Singleton';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValue = 'WindowsWidget.Companion';

function StopCompanion: Boolean;
var
  Window: HWND;
  Attempt: Integer;
begin
  Result := not CheckForMutexes(CompanionMutex);
  if Result then Exit;
  Window := FindWindowByClassName('WindowsWidget.CompanionDemo.Control');
  { Use the existing Quit hotkey handler. WM_CLOSE only hides the UI. }
  if Window <> 0 then PostMessage(Window, $0312, 2, 0);
  for Attempt := 1 to 100 do begin
    Sleep(100);
    if not CheckForMutexes(CompanionMutex) then begin
      Result := True;
      Exit;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not StopCompanion then Result := CustomMessage('CloseFailed');
end;

function InitializeUninstall: Boolean;
begin
  Result := StopCompanion;
  if not Result then
    SuppressibleMsgBox(CustomMessage('CloseFailed'), mbError, MB_OK, IDOK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Command, Executable, Tail: String;
begin
  if CurUninstallStep <> usUninstall then Exit;
  { Remove only an autostart entry belonging to this installation. }
  Executable := '"' + ExpandConstant('{app}\WindowsWidget.exe') + '"';
  if RegQueryStringValue(HKCU, RunKey, RunValue, Command) then begin
    if CompareText(Copy(Command, 1, Length(Executable)), Executable) = 0 then begin
      Tail := Copy(Command, Length(Executable) + 1, Length(Command));
      if (Tail = '') or (Copy(Tail, 1, 1) = ' ') then
        RegDeleteValue(HKCU, RunKey, RunValue);
    end;
  end;
  { User data and imported plugins in LocalAppData are never removed. }
end;
