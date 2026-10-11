# Installer

What `Setup-Pulse-vX.Y.Z.exe` does and how it works.

## User experience

1. Double-click `Setup-Pulse-vX.Y.Z.exe`
2. Before the wizard: the .NET 9 Desktop Runtime check (see below), the install mode (*for me only*, the default: no UAC unless *Start with administrator privileges* is ticked, installs into `%LocalAppData%\Programs\Pulse`; or *for all users*, see below) in English only
3. Accept license (MIT)
4. Choose options:
   - ☐ Start with Windows (regular user privileges)
   - ☐ **Start with administrator privileges** (uses Scheduled Task — no UAC at login, enables CPU temp/power on systems with signed MSR driver)
   - ☑ **Restart Pulse if it closes because of an error** (ticked by default; a second Scheduled Task, see [Restart after a crash](#restart-after-a-crash))
   - ☐ Create desktop shortcut
5. Click Install → files copied
6. Optionally launches Pulse at end of install (as a normal user; with *Start with administrator privileges* Pulse then goes through its task and comes back elevated, see below)

With both startup options ticked, only the Scheduled Task is created: the `Run` value is written for *Start with Windows* alone.

Uninstall via Settings → Apps removes the program, the shortcuts, the autostart value written by the installer and both Scheduled Tasks. Settings (`%APPDATA%\Pulse`) and appearance (`%LOCALAPPDATA%\Pulse`) stay, so a reinstall finds them.

## Per-user vs system-wide

Default: **per-user install** (no UAC, unless *Start with administrator privileges* is ticked: see [below](#for-me-only-and-the-administrator-option)).

- Files go to `%LocalAppData%\Programs\Pulse\`
- Shortcuts in user Start Menu
- Uninstall entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall`; autostart, if chosen, as the `Pulse` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
- Scheduled Tasks created under the current user's account

The installer also offers *Install for all users* (`PrivilegesRequiredOverridesAllowed=dialog`): UAC prompts and the Start Menu shortcuts go to the All Users profile, but the default folder stays `{userpf}\Pulse`, the `%LocalAppData%\Programs\Pulse` of the account that elevates. Installing into `%ProgramFiles%\Pulse` would take `{autopf}` in `DefaultDirName`.

## The "admin via Scheduled Task" trick

This is the standout feature. Normally a program that needs admin rights triggers a UAC prompt every time you launch it (annoying), or has to be manually pinned to Run as Admin. Neither survives a Windows login.

The **Scheduled Task with `RunLevel = Highest`** workaround:

1. Installer creates a Windows Task (under Task Scheduler → `\Pulse`)
2. Trigger: `On Logon` for the current user
3. Action: launch `Pulse.exe`
4. Setting: `Run with highest privileges` ✅

Result: at every Windows login, Windows itself launches Pulse with admin token, no UAC. This is the standard trick used by HWiNFO64, ThrottleStop, MSI Afterburner, and others.

Every other start is a normal user's: *Launch Pulse* at the end of the installer (Inno Setup runs it as the user who started the setup, unelevated even from an elevated setup), the Start menu, the desktop icon. From v1.3.0 Pulse uses the task for those too: started without the rights, it runs the `Pulse` task and closes, and the elevated Pulse is up in about a second, still with no UAC prompt (*Starting elevated* in `docs/ARCHITECTURE.md`). So after an install or an update Pulse has its administrator rights at once, not from the next logon. The task name `Pulse` (`MyAppTaskName`) is also a constant in `Services/AdminStart.cs`: change both together.

The task comes from the template `installer/task-logon.xml`, registered by `installer/register-task.ps1` (Windows PowerShell, `Register-ScheduledTask`), which fills in the exe path, the installing user and the run level. Until v1.2.0 it was a `schtasks /Create /SC ONLOGON /RL HIGHEST` and inherited the Task Scheduler defaults: stop the task after **72 hours** (so Pulse disappeared after three days without a logoff), don't start on battery, stop when the laptop is unplugged. The template sets no time limit (`ExecutionTimeLimit` `PT0S`) and keeps running on battery:

```inno
[Run]
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{tmp}\register-task.ps1"" -Template ""{tmp}\task-logon.xml"" -Name ""{#MyAppTaskName}"" -Template2 ""{tmp}\task-crash.xml"" -Name2 ""{#MyAppRestartTaskName}"" -Exe ""{app}\{#MyAppExeName}"" -RunLevel HighestAvailable"; \
  Tasks: startup_admin and restart; Flags: runhidden waituntilterminated
```

With the restart option (see [below](#restart-after-a-crash)) the same call registers the restart task at the same level; without it, a second entry registers the logon task alone. The script and the templates go to `{tmp}` and are deleted after setup. Reinstalling over an older version replaces the tasks (`-Force`). Removed on uninstall via `[UninstallRun]` calling `schtasks /Delete`.

### For me only and the administrator option

Windows lets only an elevated process create a task with the highest privileges, overwrite a task registered elevated, or delete one: a non-elevated attempt gets *access denied*. With *Install for all users* Setup is elevated and that's all. With *Install for me only*, the default, it isn't, and until v1.3.0 ticking *Start with administrator privileges* created nothing: the refusal went unnoticed (the `ScheduledTasks` cmdlets ignore the script's `$ErrorActionPreference`), so there was no logon task and no restart task either.

Now `register-task.ps1` catches the refusal (a `CimException` whose error code is `E_ACCESSDENIED`, 0x80070005) and starts itself again elevated, with one UAC prompt for both tasks of the call; it passes the installing user's SID (`-User`), so the tasks stay that user's even if another account confirms the prompt. Declining the prompt leaves no task: that's the user's choice. The uninstaller of such an install has a third `[UninstallRun]` entry, recorded only for *for me only* installs with the option (`Tasks: startup_admin; Check: not IsAdminInstallMode`, evaluated when Setup records it): one elevated `cmd /c schtasks /Delete … & schtasks /Delete …` for both tasks.

Checked on test tasks from a non-elevated Windows PowerShell, as Setup runs the script: one UAC prompt registered both tasks with the highest privileges, another overwrote an elevated task with a non-elevated one, and the uninstaller's command deleted both with one prompt, where a non-elevated `schtasks /Delete` got *access denied*. Not checked: a real *for me only* install, since the installer is compiled only by CI.

### Unticking an option in an update

Inno Setup offers again the options of the previous install, but it only runs the entries of the ticked ones: until v1.3.0, unticking *Start with administrator privileges* or *Restart Pulse if it closes because of an error* left the task registered before, and Pulse kept starting elevated at logon (and, with the hand-off of v1.3.0, at every start) or kept coming back after a crash. Now `register-task.ps1 -Remove` takes away the task of each unticked option, in one call when both are unticked: nothing happens if the task isn't there, and a task registered elevated brings one UAC prompt when Setup isn't elevated, as above. The `Run` value of *Start with Windows* stays: the tray menu writes it too, and an update without that tick would remove an autostart set from there.

Checked on test tasks from a non-elevated Windows PowerShell: removing tasks that aren't there and a non-elevated task needed no prompt (under a second each), two tasks registered elevated went with one prompt. Not checked: a real update.

## Restart after a crash

On 10/10/2026 an NVIDIA driver update crashed a running Pulse four seconds later (a native access violation inside LibreHardwareMonitor's NVML call, which no .NET code can catch), and it stayed closed until it was reopened by hand, without the administrator rights. The *Restart Pulse if it closes because of an error* option registers a second task, **`Pulse restart`**, from `installer/task-crash.xml`:

- Trigger: the event Windows writes in the Application log when a program crashes, *Application Error* 1000, filtered on `Pulse.exe`, then a 10-second delay. Windows writes it even with Windows Error Reporting turned off.
- Action: `Pulse.exe --after-crash`. Pulse waits for the crashed process to be gone (single-instance mutex) and refuses a fourth restart within ten minutes, so a Pulse that crashes as soon as it starts doesn't loop: the refusal is noted in `%TEMP%\pulse-perf.log`.
- Run level: `HighestAvailable` when *Start with administrator privileges* is ticked, `LeastPrivilege` otherwise, so Pulse comes back with the rights it normally has.

Closing Pulse from the tray, or ending it from Task Manager, writes no crash event and restarts nothing. Two simpler mechanisms were tried and don't do this: the task setting *If the task fails, restart every…* only covers a task that fails to start, not a program that crashes later, and `RegisterApplicationRestart` relies on Windows Error Reporting, which is turned off on some machines (the reference PC among them).

## What's in the installer

```
Setup-Pulse-vX.Y.Z.exe (~6 MB)
├── Pulse.exe                     (~10 MB — Pulse main binary, R2R precompiled, Reticle and its fonts embedded)
├── libMonoPosixHelper.dll        (~1.5 MB — native LHM deps)
├── MonoPosixHelper.dll           (~0.1 MB — native LHM deps)
├── LICENSE
├── licenses/                     (SIL OFL of the embedded fonts + README)
├── README.md
├── docs/HARDWARE-SUPPORT.md      (so users know about Ryzen Master / HWiNFO64)
└── unins000.exe                  (Inno Setup uninstaller)
```

During setup only, `register-task.ps1`, `task-logon.xml` and `task-crash.xml` are extracted to `{tmp}` to register the Scheduled Tasks.

Total install footprint: ~12 MB plus the uninstaller.

## .NET runtime dependency

Pulse is built framework-dependent → **requires .NET 9 Desktop Runtime** on the target machine.

The installer checks for it: it runs `dotnet --list-runtimes` through PowerShell and looks for `Microsoft.WindowsDesktop.App 9.`. If missing, a dialog asks:
- **Yes**: opens https://dotnet.microsoft.com/download/dotnet/9.0 in the browser and closes the setup; run it again after installing the runtime
- **No**: installs anyway (Pulse won't start until the runtime is there)

(For a self-contained build, publish with `--self-contained true`: the exe grows by the whole runtime, ~70 MB, but needs no .NET install.)

## Code signing

The installer EXE is **not currently code-signed**. Windows SmartScreen will show "Windows protected your PC" the first time it's downloaded from the internet. User clicks `More info` → `Run anyway`.

To remove the SmartScreen warning, the installer needs to be signed with an EV (Extended Validation) code signing certificate. Costs ~$300/year. Until we have funding for that, users get the SmartScreen prompt.

## Localization

Pulse is in English only, and so is the installer: one language in the Inno Setup script.

```inno
[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
```

## Building locally

After installing Inno Setup (`winget install JRSoftware.InnoSetup`):

```powershell
# Build the app first
cd ResourceMonitor
dotnet publish -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true -o ../dist

# Compile installer
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" ../installer/pulse.iss
```

Output: `installer/Output/Setup-Pulse-vX.Y.Z.exe`, with the version of `#define MyAppVersion` in `pulse.iss`: 1.0.0 in the repository, because the release workflow overwrites it with the tag. Set it by hand for a local installer with the right number.

## CI-built installers

Every `vX.Y.Z` git tag triggers `.github/workflows/release.yml`, which:

1. Sets up .NET 9 SDK
2. `dotnet publish` → `dist/` (with the tag as `Version`)
3. Writes the tag version into `MyAppVersion` in `installer/pulse.iss`
4. Installs Inno Setup via Chocolatey on the runner
5. Compiles `installer/pulse.iss`
6. Also zips `dist/` with README, LICENSE and `licenses/` as a portable build
7. Creates a draft GitHub Release with both `Setup-Pulse-vX.Y.Z.exe` and `Pulse-vX.Y.Z-portable.zip`, with generated release notes

Manual step: edit release notes + publish from GitHub UI.
