# Linux Build Environment

Date: 2026-04-26

This project builds the native GTK target on Ubuntu through WSL from the Windows repository checkout. The expected distro for current verification is `Ubuntu-24.04`.

## Required Host Setup

Install WSL 2 with Ubuntu 24.04 from an elevated PowerShell prompt if it is not already installed:

```powershell
wsl --install -d Ubuntu-24.04
```

Restart Windows if the WSL installer requests it, then open Ubuntu once and finish the initial Linux user setup.

## Required Ubuntu Packages

From an Ubuntu shell:

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config libgtk-4-dev libspelling-1-dev hunspell-en-gb python3
```

Package purpose:

- `build-essential`: C++ compiler and standard build tools.
- `cmake`: configures the shared core and GTK targets.
- `ninja-build`: preferred fast single-config generator.
- `pkg-config`: lets CMake discover GTK4.
- `libgtk-4-dev`: GTK4 headers and libraries for `ClassicNotepadGtk`.
- `libspelling-1-dev`: optional GTK4 spelling integration, backed by Enchant.
- `hunspell-en-gb`: British English dictionary used by the Linux spelling provider.
- `python3`: runs the cross-platform automation suite on Linux.

## Build And Test

From PowerShell in the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-ubuntu.ps1 -Distro Ubuntu-24.04
```

The script:

- maps the Windows checkout to `/mnt/c/...` for WSL,
- auto-selects Ninja when installed,
- configures `build-ubuntu/`,
- builds `ClassicNotepadCore`, `TextConversionTests`, `ClassicNotepadGtk`, and `LinuxSpellingProbe` when `libspelling-1` is available,
- runs Ubuntu CTest unless `-SkipTests` is passed.

For a clean reconfigure:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-ubuntu.ps1 -Distro Ubuntu-24.04 -Fresh
```

For a Release-style Linux build in a separate directory:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-ubuntu.ps1 -Distro Ubuntu-24.04 -BuildDir build-ubuntu-release -Configuration Release
```

## Automation Suite

After the Debug Ubuntu build:

```powershell
wsl.exe -d Ubuntu-24.04 -- bash -lc "cd /mnt/c/vibe/classicNotepad && python3 tests/automation/run_automation_tests.py --binary build-ubuntu/ClassicNotepadGtk --platform linux"
```

The same suite runs against Windows by changing the binary and platform:

```powershell
python .\tests\automation\run_automation_tests.py --binary .\build\Debug\ClassicNotepad.exe --platform windows
```

## Run The GTK App

From an Ubuntu shell:

```bash
cd /mnt/c/vibe/classicNotepad
./build-ubuntu/ClassicNotepadGtk
./build-ubuntu/ClassicNotepadGtk README.md
```

WSLg is required to display the GTK window from WSL. Automation mode can run headless enough for the semantic suite, but manual UI checks need a working Linux desktop display.

To force a deterministic app appearance without changing WSL or desktop theme configuration:

```bash
CLASSIC_NOTEPAD_THEME=system ./build-ubuntu/ClassicNotepadGtk
CLASSIC_NOTEPAD_THEME=light ./build-ubuntu/ClassicNotepadGtk
CLASSIC_NOTEPAD_THEME=dark ./build-ubuntu/ClassicNotepadGtk
```

## Current Linux Capability Notes

- File/edit/find/replace/go-to/format/view/status/printing workflows are covered by the shared automation suite.
- Linux spell checking is available when CMake finds `libspelling-1` and Ubuntu has the GB Hunspell dictionary installed. If the backend or dictionary is absent, `getCapabilities` reports a non-available `spellCapability`, and spelling commands return graceful unavailable responses.
- Linux dark mode is available through the shared appearance state. `getCapabilities` reports `appearanceTheme`, `effectiveAppearance`, `darkMode`, and `highContrast`; high-contrast GTK themes suppress custom light/dark colors.

## Troubleshooting

If CMake says GTK4 is missing, confirm:

```bash
pkg-config --modversion gtk4
```

If that command fails, install `libgtk-4-dev` and `pkg-config` again.

If CMake says Linux spelling is disabled, confirm:

```bash
pkg-config --modversion libspelling-1
enchant-2 -d en_GB -l
```

The first command should print the installed `libspelling` version. The second reads words from stdin and reports only misspellings for the GB dictionary.

The `LinuxSpellingProbe` CTest target validates the configured dictionary with `teh colour centre recieve` and records US variant behavior. On the current Ubuntu-24.04 WSL environment, `color` and `center` are reported as misspellings by the selected `en_GB` dictionary.

If `wsl.exe` reports access denied from the Codex desktop sandbox, rerun the same PowerShell build command with approval. The build script itself does not need network access after Ubuntu packages are installed.
