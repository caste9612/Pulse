; Pulse installer (Inno Setup 6)
; Build: ISCC.exe pulse.iss
; Requires: dist/Pulse.exe + native DLLs (run dotnet publish first)
; MyAppVersion stays 1.0.0 here: the release workflow writes the tag version into it

#define MyAppName "Pulse"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Pulse Contributors"
#define MyAppExeName "Pulse.exe"
; Pulse looks up the logon task by this name too (TaskName in ResourceMonitor/Services/AdminStart.cs): change both together
#define MyAppTaskName "Pulse"
#define MyAppRestartTaskName "Pulse restart"
; PawnIO, the driver LibreHardwareMonitor reads the CPU's MSRs through (temperature, power, core clocks). Not bundled: its
; releases come with no licence to redistribute them. Downloaded from its official release and checked against this SHA256,
; the one of winget's manifest (namazso.PawnIO); the setup is signed by namazso. Bump all three together
#define PawnIOVersion "2.2.0"
#define PawnIOUrl "https://github.com/namazso/PawnIO.Setup/releases/download/" + PawnIOVersion + "/PawnIO_setup.exe"
#define PawnIOSha256 "1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032"

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
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
english.StartupTask=Start with Windows (normal user)
english.StartupAdminTask=Start with administrator privileges (for CPU temp/watt on supported systems)
english.PawnIOTask=Install the PawnIO driver, for CPU temperature and power (downloaded from its official release)
english.PawnIOInstalling=Installing the PawnIO driver...
english.PawnIODownloadFailed=The PawnIO driver could not be downloaded:%n%1%n%nPulse is installed without it: CPU temperature and power will read n/a. You can install PawnIO later with: winget install namazso.PawnIO
english.RestartTask=Restart Pulse if it closes because of an error
english.DesktopIconTask=Create desktop shortcut
english.OptionsGroup=Installation options:

[Tasks]
Name: "startup"; Description: "{cm:StartupTask}"; GroupDescription: "{cm:OptionsGroup}"; Flags: unchecked
Name: "startup_admin"; Description: "{cm:StartupAdminTask}"; GroupDescription: "{cm:OptionsGroup}"; Flags: unchecked
; under the administrator option: only an elevated Pulse can use the driver. Ticked with its parent, offered only when PawnIO
; isn't installed yet
Name: "startup_admin\pawnio"; Description: "{cm:PawnIOTask}"; GroupDescription: "{cm:OptionsGroup}"; Check: not IsPawnIOInstalled
Name: "restart"; Description: "{cm:RestartTask}"; GroupDescription: "{cm:OptionsGroup}"
Name: "desktopicon"; Description: "{cm:DesktopIconTask}"; GroupDescription: "{cm:OptionsGroup}"; Flags: unchecked

[Files]
Source: "..\dist\Pulse.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\HARDWARE-SUPPORT.md"; DestDir: "{app}\docs"; Flags: ignoreversion
; Scheduled Task templates and the script that registers them: needed only during setup
Source: "register-task.ps1"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "task-logon.xml"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "task-crash.xml"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

; Regular user-mode autostart (Run registry key) — only if startup but NOT startup_admin selected
[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#MyAppName}"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: startup AND NOT startup_admin

[Run]
; Scheduled Tasks from the templates (register-task.ps1). With "Install for me only" Setup has no administrator rights and
; Windows refuses a task with the highest privileges, or overwriting one: the script then asks for elevation, once per call,
; which is why the logon task and the restart task with the same rights are registered in one call.
; Elevated logon task if startup_admin selected (task-logon.xml: no 72-hour run limit, starts on battery too), with the restart
; after a crash (task-crash.xml) at the same level
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{tmp}\register-task.ps1"" -Template ""{tmp}\task-logon.xml"" -Name ""{#MyAppTaskName}"" -Template2 ""{tmp}\task-crash.xml"" -Name2 ""{#MyAppRestartTaskName}"" -Exe ""{app}\{#MyAppExeName}"" -RunLevel HighestAvailable"; \
  Tasks: startup_admin and restart; Flags: runhidden waituntilterminated
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{tmp}\register-task.ps1"" -Template ""{tmp}\task-logon.xml"" -Name ""{#MyAppTaskName}"" -Exe ""{app}\{#MyAppExeName}"" -RunLevel HighestAvailable"; \
  Tasks: startup_admin and not restart; Flags: runhidden waituntilterminated

; Restart after a crash without startup_admin: with the rights Pulse normally starts with
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{tmp}\register-task.ps1"" -Template ""{tmp}\task-crash.xml"" -Name ""{#MyAppRestartTaskName}"" -Exe ""{app}\{#MyAppExeName}"" -RunLevel LeastPrivilege"; \
  Tasks: restart and not startup_admin; Flags: runhidden waituntilterminated

; An option unticked in an update stops acting: the task an earlier install registered for it goes (register-task.ps1 -Remove:
; nothing to do if it isn't there, one elevation if Windows refuses, as above). A leftover logon task would keep starting Pulse
; elevated, at logon and through AdminStart at every other start. The Run value of "Start with Windows" is left alone: the tray
; menu sets it too, and an update without that tick would take it away
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{tmp}\register-task.ps1"" -Remove -Name ""{#MyAppTaskName}"" -Name2 ""{#MyAppRestartTaskName}"""; \
  Tasks: not startup_admin and not restart; Flags: runhidden waituntilterminated
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{tmp}\register-task.ps1"" -Remove -Name ""{#MyAppTaskName}"""; \
  Tasks: not startup_admin and restart; Flags: runhidden waituntilterminated
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{tmp}\register-task.ps1"" -Remove -Name ""{#MyAppRestartTaskName}"""; \
  Tasks: startup_admin and not restart; Flags: runhidden waituntilterminated

; PawnIO from its official setup, downloaded before the installation started (NextButtonClick) and only if the download
; worked. Started elevated (runas: one UAC prompt with "Install for me only"), so waiting for it waits for the real install and
; Pulse, launched below, already finds the driver. Uninstalling Pulse leaves it: other programs use it too
Filename: "{tmp}\PawnIO_setup.exe"; Parameters: "-install -silent"; StatusMsg: "{cm:PawnIOInstalling}"; \
  Verb: "runas"; Tasks: startup_admin\pawnio; Check: PawnIODownloaded; Flags: shellexec waituntilterminated

; Launch the app at end of install
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Remove the Scheduled Tasks on uninstall (if they exist)
Filename: "schtasks.exe"; Parameters: "/Delete /TN ""{#MyAppTaskName}"" /F"; Flags: runhidden; RunOnceId: "DelPulseTask"
Filename: "schtasks.exe"; Parameters: "/Delete /TN ""{#MyAppRestartTaskName}"" /F"; Flags: runhidden; RunOnceId: "DelPulseRestartTask"
; With "Install for me only" the uninstaller has no administrator rights either, and Windows refuses to delete the tasks
; registered elevated (access denied): one elevation for both. Tasks and Check are evaluated when Setup records the entry, so it
; is there only for such installs with startup_admin
Filename: "{cmd}"; Parameters: "/c schtasks /Delete /TN ""{#MyAppTaskName}"" /F & schtasks /Delete /TN ""{#MyAppRestartTaskName}"" /F"; \
  Verb: "runas"; Tasks: startup_admin; Check: not IsAdminInstallMode; Flags: shellexec runhidden waituntilterminated; RunOnceId: "DelPulseTasksElevated"

[Code]
var
  DownloadPage: TDownloadWizardPage;
  PawnIOReady: Boolean;

// PawnIO is installed when its uninstall entry is there: LibreHardwareMonitor and Pulse look for it in the same place
function IsPawnIOInstalled: Boolean;
begin
  Result := RegKeyExists(HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO');
end;

// its setup was downloaded and matched the SHA256: the [Run] entry can start it
function PawnIODownloaded: Boolean;
begin
  Result := PawnIOReady;
end;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  Result := True;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), SetupMessage(msgPreparingDesc), @OnDownloadProgress);
end;

// With the PawnIO option, its setup is downloaded once the user confirms on the Ready page, into {tmp}, and checked against
// its SHA256. A failed download doesn't stop Pulse's installation: a message says so and how to install PawnIO later.
// Aborted by the user, the wizard stays on the Ready page, where the option can be unticked
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID <> wpReady) or not WizardIsTaskSelected('startup_admin\pawnio') then
    Exit;
  DownloadPage.Clear;
  DownloadPage.Add('{#PawnIOUrl}', 'PawnIO_setup.exe', '{#PawnIOSha256}');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
      PawnIOReady := True;
    except
      if DownloadPage.AbortedByUser then
        Result := False
      else
        SuppressibleMsgBox(FmtMessage(CustomMessage('PawnIODownloadFailed'), [GetExceptionMessage]), mbError, MB_OK, IDOK);
    end;
  finally
    DownloadPage.Hide;
  end;
end;

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
