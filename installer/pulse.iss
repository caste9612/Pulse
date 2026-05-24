; Pulse installer (Inno Setup 6)
; Build: ISCC.exe pulse.iss
; Requires: dist/ResourceMonitor.exe + native DLLs (run dotnet publish first)

#define MyAppName "Pulse"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Pulse Contributors"
#define MyAppExeName "Pulse.exe"
#define MyAppTaskName "Pulse"

[Setup]
AppId={{A8D7E0B0-3F4C-4D6E-9A2B-1C3D5E6F7A8B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://github.com/caste9612/Pulse
AppSupportURL=https://github.com/caste9612/Pulse/issues
DefaultDirName={userpf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=Output
OutputBaseFilename=Setup-Pulse-v{#MyAppVersion}
Compression=lzma2/ultra
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
MinVersion=10.0.17763

[Languages]
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
italian.StartupTask=Avvia all'avvio di Windows (utente normale)
italian.StartupAdminTask=Avvia con privilegi amministratore (per temp/watt CPU su sistemi supportati)
italian.DesktopIconTask=Crea collegamento sul desktop
italian.OptionsGroup=Opzioni di installazione:
english.StartupTask=Start with Windows (normal user)
english.StartupAdminTask=Start with administrator privileges (for CPU temp/watt on supported systems)
english.DesktopIconTask=Create desktop shortcut
english.OptionsGroup=Installation options:

[Tasks]
Name: "startup"; Description: "{cm:StartupTask}"; GroupDescription: "{cm:OptionsGroup}"; Flags: unchecked
Name: "startup_admin"; Description: "{cm:StartupAdminTask}"; GroupDescription: "{cm:OptionsGroup}"; Flags: unchecked
Name: "desktopicon"; Description: "{cm:DesktopIconTask}"; GroupDescription: "{cm:OptionsGroup}"; Flags: unchecked

[Files]
Source: "..\dist\Pulse.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\HARDWARE-SUPPORT.md"; DestDir: "{app}\docs"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

; Regular user-mode autostart (Run registry key) — only if startup but NOT startup_admin selected
[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#MyAppName}"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: startup AND NOT startup_admin

[Run]
; Create elevated Scheduled Task at logon if startup_admin selected
Filename: "schtasks.exe"; \
  Parameters: "/Create /TN ""{#MyAppTaskName}"" /TR ""\""{app}\{#MyAppExeName}\"""" /SC ONLOGON /RL HIGHEST /F"; \
  Tasks: startup_admin; Flags: runhidden waituntilterminated

; Launch the app at end of install
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Remove the Scheduled Task on uninstall (if it exists)
Filename: "schtasks.exe"; Parameters: "/Delete /TN ""{#MyAppTaskName}"" /F"; Flags: runhidden; RunOnceId: "DelPulseTask"

[Code]
// Check if .NET 9 Desktop Runtime is installed
function IsDotNet9DesktopRuntimeInstalled: Boolean;
var
  ResultCode: Integer;
  TempFile: String;
  Output: AnsiString;
begin
  Result := False;
  TempFile := ExpandConstant('{tmp}\dotnet-list.txt');
  if Exec('powershell.exe', '-NoProfile -Command "& {dotnet --list-runtimes 2>$null | Out-File -Encoding ASCII ''' + TempFile + '''}"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    if LoadStringFromFile(TempFile, Output) then
    begin
      Result := Pos('Microsoft.WindowsDesktop.App 9.', Output) > 0;
    end;
  end;
end;

function InitializeSetup: Boolean;
var
  MsgResult: Integer;
begin
  Result := True;
  if not IsDotNet9DesktopRuntimeInstalled then
  begin
    MsgResult := MsgBox(
      'Pulse requires .NET 9 Desktop Runtime.' + #13#10 +
      'It does not appear to be installed.' + #13#10 + #13#10 +
      'Click YES to open the download page in your browser, then re-run this installer after installing the runtime.' + #13#10 +
      'Click NO to continue anyway (Pulse will fail to start if the runtime is missing).',
      mbConfirmation, MB_YESNO);
    if MsgResult = IDYES then
    begin
      ShellExec('open', 'https://dotnet.microsoft.com/download/dotnet/9.0', '', '', SW_SHOW, ewNoWait, MsgResult);
      Result := False;
    end;
  end;
end;
