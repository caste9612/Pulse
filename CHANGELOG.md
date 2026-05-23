# Changelog

All notable changes to Pulse will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial public release: WPF widget with CPU/RAM/GPU/Net/Disk live metrics
- Sparklines for 60-second history per metric
- Top processes by CPU and RAM (updated every 5 s)
- Network details: ping, local + public IP, Wi-Fi SSID/signal
- Per-drive free space + total disk I/O
- System uptime + estimated total PC power draw
- Tray icon: show/hide, pin (topmost), auto-start, restart as admin, exit
- Persistent window position/size between sessions (JSON in `%APPDATA%\ResourceMonitor`)
- Inno Setup installer with optional admin-via-Scheduled-Task autostart
- GitHub Actions release workflow

### Hardware support
- AMD Ryzen (Zen 1-5): CPU temp/power/clock via Ryzen Master driver
- NVIDIA / AMD / Intel GPUs: usage/temp/power/VRAM via LibreHardwareMonitorLib
- SSD temp via SMART (no admin needed)
- Intel: documented path via HWiNFO64 (integration not yet implemented)

### Performance
- ~1.5-2% CPU idle (down from initial 6.77% after optimization)
- ~150 MB RAM (WPF + WinForms + LHM lib floor)
- ~270 ms cold start (with ReadyToRun)
- Single-file 9 MB exe + 1.5 MB native deps
