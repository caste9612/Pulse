# Reticle (binary)

Release build of **Reticle v0.5.0** (commit `5c65e03`), the author's WPF design system that Pulse's UI is built on. The Reticle source is not public; these two assemblies let Pulse build anywhere, CI included, without access to it.

| File | Contents |
|---|---|
| `Reticle.Wpf.dll` | ThemeManager, controls (ReticleWindow, TickBorder, Panel, AppearanceSettingsView…) and styles |
| `Reticle.Tokens.dll` | design tokens, theme dictionaries and the embedded fonts (licenses in `../../licenses/`) |

To move Pulse to another Reticle tag, with the Reticle repository cloned next to Pulse (`../Reticle`):

```powershell
./scripts/update-reticle.ps1 -Tag v0.5.0
```

The script builds Reticle at that tag in a temporary worktree, copies the two DLLs here and updates the version line above.
