# Installer

What `Setup-Pulse-vX.Y.Z.exe` does and how it works.

## User experience

1. Double-click `Setup-Pulse-vX.Y.Z.exe`
2. Before the wizard: the .NET 9 Desktop Runtime check (see below), the install mode (*for me only*, the default: no UAC, installs into `%LocalAppData%\Programs\Pulse`; or *for all users*, see below) and the language (English / Italian)
3. Accept license (MIT)
4. Choose options:
   - ☐ Start with Windows (regular user privileges)
   - ☐ **Start with administrator privileges** (uses Scheduled Task — no UAC at login, enables CPU temp/power on systems with signed MSR driver)
   - ☐ Create desktop shortcut
5. Click Install → files copied
6. Optionally launches Pulse at end of install

With both startup options ticked, only the Scheduled Task is created: the `Run` value is written for *Start with Windows* alone.

Uninstall via Settings → Apps removes the program, the shortcuts, the autostart value written by the installer and the Scheduled Task. Settings (`%APPDATA%\Pulse`) and appearance (`%LOCALAPPDATA%\Pulse`) stay, so a reinstall finds them.

## Per-user vs system-wide

Default: **per-user install** (no UAC).

- Files go to `%LocalAppData%\Programs\Pulse\`
- Shortcuts in user Start Menu
- Uninstall entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall`; autostart, if chosen, as the `Pulse` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
- Scheduled Task created under the current user's account

The installer also offers *Install for all users* (`PrivilegesRequiredOverridesAllowed=dialog`): UAC prompts and the Start Menu shortcuts go to the All Users profile, but the default folder stays `{userpf}\Pulse`, the `%LocalAppData%\Programs\Pulse` of the account that elevates. Installing into `%ProgramFiles%\Pulse` would take `{autopf}` in `DefaultDirName`.

## The "admin via Scheduled Task" trick

This is the standout feature. Normally a program that needs admin rights triggers a UAC prompt every time you launch it (annoying), or has to be manually pinned to Run as Admin. Neither survives a Windows login.

The **Scheduled Task with `RunLevel = Highest`** workaround:

1. Installer creates a Windows Task (under Task Scheduler → `\Pulse`)
2. Trigger: `On Logon` for the current user
3. Action: launch `Pulse.exe`
4. Setting: `Run with highest privileges` ✅

Result: at every Windows login, Windows itself launches Pulse with admin token, no UAC. This is the standard trick used by HWiNFO64, ThrottleStop, MSI Afterburner, and others.

Created via the Inno Setup `[Run]` section calling `schtasks.exe`:

```inno
[Run]
Filename: "schtasks.exe"; \
  Parameters: "/Create /TN ""{#MyAppTaskName}"" /TR ""\""{app}\{#MyAppExeName}\"""" /SC ONLOGON /RL HIGHEST /F"; \
  Tasks: startup_admin; Flags: runhidden waituntilterminated
```

Removed on uninstall via `[UninstallRun]` calling `schtasks /Delete`.

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

The Inno Setup script includes Italian + English language files:

```inno
[Languages]
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"
```

Wizard auto-detects from OS, user can override at first dialog.

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
