# Installer

What `Setup-Pulse-X.Y.Z.exe` does and how it works.

## User experience

1. Double-click `Setup-Pulse-X.Y.Z.exe`
2. (No UAC prompt yet — installer runs as current user, installs into `%LocalAppData%\Programs\Pulse`)
3. Choose language (English / Italian)
4. Accept license (MIT)
5. Choose options:
   - ☐ Start with Windows (regular user privileges)
   - ☐ **Start with administrator privileges** (uses Scheduled Task — no UAC at login, enables CPU temp/power on systems with signed MSR driver)
   - ☐ Create desktop shortcut
6. Click Install → files copied
7. Optionally launches Pulse at end of install

Uninstall via Settings → Apps removes everything including the Scheduled Task.

## Per-user vs system-wide

Default: **per-user install** (no UAC).

- Files go to `%LocalAppData%\Programs\Pulse\`
- Shortcuts in user Start Menu
- Registry entries under `HKCU\Software\Pulse`
- Scheduled Task created under the current user's account

If the user selects "Install for all users" in the wizard, UAC prompts and install goes to `%ProgramFiles%\Pulse\`.

## The "admin via Scheduled Task" trick

This is the standout feature. Normally a program that needs admin rights triggers a UAC prompt every time you launch it (annoying), or has to be manually pinned to Run as Admin. Neither survives a Windows login.

The **Scheduled Task with `RunLevel = Highest`** workaround:

1. Installer creates a Windows Task (under Task Scheduler → `\Pulse`)
2. Trigger: `On Logon` for the current user
3. Action: launch `ResourceMonitor.exe`
4. Setting: `Run with highest privileges` ✅

Result: at every Windows login, Windows itself launches Pulse with admin token, no UAC. This is the standard trick used by HWiNFO64, ThrottleStop, MSI Afterburner, and others.

Created via the Inno Setup `[Run]` section calling `schtasks.exe`:

```inno
[Run]
Filename: "schtasks.exe"; \
  Parameters: "/Create /TN ""Pulse"" /TR ""\""{app}\ResourceMonitor.exe\"""" /SC ONLOGON /RL HIGHEST /F"; \
  Tasks: startup_admin; Flags: runhidden
```

Removed on uninstall via `[UninstallRun]` calling `schtasks /Delete`.

## What's in the installer

```
Setup-Pulse-X.Y.Z.exe (~6-8 MB)
├── ResourceMonitor.exe           (~9 MB — Pulse main binary, R2R precompiled)
├── libMonoPosixHelper.dll        (~1.5 MB — native LHM deps)
├── MonoPosixHelper.dll           (~0.1 MB — native LHM deps)
├── LICENSE
├── docs/HARDWARE-SUPPORT.md      (so users know about Ryzen Master / HWiNFO64)
└── unins000.exe                  (Inno Setup uninstaller)
```

Total install footprint: ~12 MB.

## .NET runtime dependency

Pulse is built framework-dependent → **requires .NET 9 Desktop Runtime** on the target machine.

The installer checks for it. If missing:
- Shows a friendly dialog with a direct link to https://dotnet.microsoft.com/download/dotnet/9.0
- Optionally offers to download + run the official Microsoft installer

(For self-contained build, drop the `--self-contained false` flag and the resulting exe is ~80 MB but needs no .NET install.)

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

Output: `installer/Output/Setup-Pulse-X.Y.Z.exe`.

## CI-built installers

Every `vX.Y.Z` git tag triggers `.github/workflows/release.yml`, which:

1. Sets up .NET 9 SDK
2. `dotnet publish` → `dist/`
3. Installs Inno Setup via Chocolatey on the runner
4. Compiles `installer/pulse.iss`
5. Also zips `dist/` as a portable build
6. Creates a draft GitHub Release with both `Setup-Pulse-vX.Y.Z.exe` and `Pulse-vX.Y.Z-portable.zip`

Manual step: edit release notes + publish from GitHub UI.
