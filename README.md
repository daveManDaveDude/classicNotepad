# Classic Notepad

<img width="931" height="300" alt="image" src="https://github.com/user-attachments/assets/2fa66227-88b2-4052-9a6d-071b25944fbf" />


Classic Notepad is a single-document native desktop recreation of classic Windows Notepad, with Win32, GTK4 Linux, and AppKit macOS targets in the same repository. It keeps the plain local editor feel: one window, one text document, native desktop dialogs, no tabs, no cloud features, no telemetry, and no editor extras that would pull it away from the classic experience.

Current version: `1.2.1`.

The project is built with C++17, native platform UI layers, and CMake. It is intended to be opened, built, tested, and debugged from Visual Studio Code or the platform build scripts.

## Platform Targets

| Platform | App | Status |
| --- | --- | --- |
| Windows | `ClassicNotepad.exe` | Full native Win32 target with automated parity coverage. |
| Linux | `ClassicNotepadGtk` | Native GTK4 target with automated parity coverage and optional `libspelling` British English spell checking. |
| macOS | `ClassicNotepadMac.app` | Native AppKit target with automated parity coverage, AppKit spelling/appearance integration, and universal Release build support. |

## Finished Feature Set

- Native Win32 desktop app with a classic menu bar and multiline `EDIT` control.
- Single-document workflow with the title format `Untitled - Classic Notepad`.
- Dirty-state tracking with a leading `*` in the window title.
- Save confirmation before New, Open, or Exit discards unsaved edits.
- Command-line file open through the first argument.
- Missing command-line files can be created after confirmation.
- `File > New`, `Open...`, `Save`, `Save As...`, `Page Setup...`, `Print...`, and `Exit`.
- `Edit > Undo`, `Cut`, `Copy`, `Paste`, `Delete`, `Find...`, `Find Next`, `Replace...`, `Go To...`, `Select All`, and `Time/Date`.
- `Format > Word Wrap` and `Font...`.
- `View > Status Bar`.
- `Help > About Classic Notepad`.
- Right-click editor context menu with edit commands and spelling actions.
- Find and Replace use the standard Windows modeless dialogs.
- Go To uses a small native dialog with range validation.
- Word Wrap recreates the editor while preserving text, selection, modified state, and scroll position where practical.
- Font selection uses the standard Windows font dialog.
- Status bar shows line, column, character count, detected line endings, and detected encoding.
- Page Setup stores margins and printer settings for the current app session.
- Print uses the standard Windows print dialog and sends plain Unicode text through GDI using the selected editor font.
- Automatic dark-mode matching for the main window, editor, status bar, menu bar, resize grip, and scrollbars.
- Custom dark-mode menu bar and scrollbars so the app remains readable when Windows is in dark mode.
- Shared `System` / `Light` / `Dark` appearance state with `CLASSIC_NOTEPAD_THEME` test override.
- Windows spell checking for British English (`en-GB`) when that language is installed.
- Spell checking runs over the visible editor region, draws red wavy underlines, and offers right-click suggestions.
- Spell check context actions include suggestions, Ignore Once, and Add to Dictionary.
- Keyboard accelerators: `Ctrl+N`, `Ctrl+O`, `Ctrl+S`, `Ctrl+Shift+S`, `Ctrl+F`, `F3`, `Ctrl+H`, `Ctrl+G`, `Ctrl+A`, `F5`, `Ctrl+Z`, `Ctrl+X`, `Ctrl+C`, `Ctrl+V`, and `Del`.
- Focused console tests for encoding, line-ending, document round-trip, metadata, new-file, and spelling text utility behavior.
- Shared JSON-lines automation suite for Windows, Linux, and macOS feature parity.

## Linux GTK Parity Target

The Linux target builds as `ClassicNotepadGtk` on Ubuntu/WSL with GTK4. It now covers the same automated feature groups as the Windows binary:

- File workflow, command-line open, missing command-line file creation, dirty title metadata, and save/open behavior.
- Encoding and line-ending round trips through the shared document core.
- Edit commands, find/replace/go-to, word wrap, font metadata, status bar, and status metadata.
- `File > Page Setup...` and `File > Print...` with native GTK print/page setup APIs plus deterministic automation print sink coverage.
- `Help > About Classic Notepad`.
- Automation capability reporting for platform differences.

Accepted Linux v1 capability differences:

- Spell checking is optional on Linux. With `libspelling-1-dev` and `hunspell-en-gb`, `getCapabilities` reports `spellCheck: true`; without them, spelling commands return graceful unavailable results.
- Dark mode is implemented through the shared appearance state. Set `CLASSIC_NOTEPAD_THEME=system`, `CLASSIC_NOTEPAD_THEME=light`, or `CLASSIC_NOTEPAD_THEME=dark`; `getCapabilities` reports `appearanceTheme`, `effectiveAppearance`, `darkMode`, and `highContrast`.

## macOS AppKit Target

The macOS target builds as `ClassicNotepadMac.app`. It opens one plain-text AppKit window, uses the shared core document loader/saver, supports New, Open, Save, Save As, command-line file open, missing command-line file creation prompts, unsaved-change prompts, editor commands, Find/Find Next/Replace, Go To, word wrap, font metadata, a status bar, AppKit appearance overrides, and closes the app when the document window closes.

The macOS File menu intentionally contains New, Open, Save, Save As, Page Setup, and Print. Close and Exit are not duplicated there; Quit lives in the standard application menu as `Quit Classic Notepad`.

Required macOS tools:

- Xcode Command Line Tools. Install with `xcode-select --install`.
- CMake on `PATH`.
- Ninja is optional. If it is installed, `scripts/build-macos.sh` uses it; otherwise it uses Unix Makefiles.

Build and test on Apple Silicon:

```bash
scripts/build-macos.sh
scripts/test-macos.sh
file build-macos/TextConversionTests
```

Run the app:

```bash
open build-macos/ClassicNotepadMac.app
```

Run the shared macOS automation suite:

```bash
python3 tests/automation/run_automation_tests.py --binary build-macos/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac --platform macos
```

The visible app uses the AppKit spelling helper on the real `NSTextView`. Headless automation reports spelling as unavailable rather than initializing AppKit spelling services outside the GUI app lifecycle.

The default macOS script build sets `CMAKE_OSX_ARCHITECTURES=arm64`. Universal builds can be requested explicitly:

```bash
scripts/build-macos.sh --build-dir build-macos-universal --universal
```

Build the current Release app in the default `build-macos/` directory:

```bash
scripts/build-macos.sh --configuration Release
```

Build the current universal Release app:

```bash
scripts/build-macos.sh --configuration Release --universal
lipo -archs build-macos/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac
```

Create a local signed universal zip package:

```bash
scripts/package-macos.sh --fresh
```

The package script builds `arm64;x86_64`, runs CTest, verifies the app binary with `lipo`, applies ad-hoc signing, runs the shared macOS automation suite, and writes a zip under `artifacts/macos/`.

The app icon is generated at build time from `assets/icons/classic_notepad_large_transparent.png` into `classic_notepad.icns` and copied into the app bundle resources.

Verify the universal binary manually:

```bash
lipo -archs build-macos-universal/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac
```

## Text File Behavior

- New files save as UTF-8 without BOM and CRLF line endings.
- UTF-8 with BOM is detected and saved with BOM.
- UTF-8 without BOM is detected and saved without BOM.
- UTF-16 LE with BOM is detected and saved with BOM.
- Files that are not valid UTF-8 fall back to the system ANSI code page.
- Normal Save preserves the detected encoding.
- Save As preserves the current document encoding; there is no encoding picker.
- CRLF, LF, and CR line endings are detected.
- The Win32 edit control normalizes line endings while editing.
- Normal Save writes the line-ending style detected when the file was opened.
- Mixed line endings are tracked as mixed and saved using the dominant detected style.
- Large files are supported up to available memory and platform limits (the fixed 256 MB guard was removed).
- Save now uses an atomic temp-file replace flow to reduce data-loss risk on interruptions.

## Spell Checking

Classic Notepad uses platform spell-checking services, not bundled dictionaries. Windows uses the Windows Spell Checking API and tries to create an `en-GB` spell checker on startup. Linux/GTK uses `libspelling` with the system Hunspell/Enchant British English dictionary when `libspelling-1-dev` and `hunspell-en-gb` are installed.

If British English spell checking is installed, misspelled words in the visible editor area are underlined and right-clicking a marked word opens spelling suggestions.

If British English spell checking is not installed, the app keeps the editor fully usable without spell checking.

To install the language support on Windows 10 or Windows 11:

1. Open **Settings**.
2. Go to **Time & language > Language & region**.
3. Add or select **English (United Kingdom)**.
4. Install the language features that include basic typing or proofing support.
5. Restart Classic Notepad.

## Required Free Software On Windows

Install these before building:

1. [Visual Studio Code](https://code.visualstudio.com/).
2. VS Code extension: `ms-vscode.cpptools` / **C/C++**.
3. [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) or Visual Studio Community. Visual Studio 2019 Build Tools also works.

Recommended VS Code extension:

- `ms-vscode.cmake-tools` / **CMake Tools**.

When installing Visual Studio Build Tools or Visual Studio Community, select:

- Workload: **Desktop development with C++**.
- Component: **MSVC C++ x64/x86 build tools**.
- Component: **Windows SDK**. The Windows 11 SDK is recommended; the build also works with a current Windows 10 SDK.
- Component: **C++ CMake tools for Windows**.

You do not need paid Visual Studio. Visual Studio Build Tools plus VS Code is enough.

The build script first looks for `cmake` on `PATH`, then looks for the CMake copy installed with Visual Studio's C++ CMake tools, then checks common standalone CMake install locations. It supports the Visual Studio 2022 and 2019 CMake generators, with 2022 recommended for a fresh machine.

## VS Code Setup

1. Install the required software above.
2. Open this repository folder in VS Code.
3. Accept the recommended extension prompt, or install the two recommended extensions manually from `.vscode/extensions.json`.
4. No CMake preset or Kit selection is required for F5 debugging; the repository tasks call the platform build scripts directly.
5. Press `Ctrl+Shift+B` to run the default **build debug** task.
6. Press `F5` and choose **Debug Classic Notepad (macOS)** or **Debug Classic Notepad (Windows)** if VS Code asks for a debug configuration.

The Windows F5 launch configuration runs the **build debug** task first:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Debug
```

The Windows debugger then launches:

```text
build\Debug\ClassicNotepad.exe
```

The macOS F5 launch configuration runs the **build macos debug** task first:

```bash
scripts/build-macos.sh --configuration Debug
```

The macOS debugger then launches:

```text
build-macos/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac
```

Useful VS Code tasks are already defined in `.vscode/tasks.json`:

- **build debug**: builds `ClassicNotepad.exe` and `TextConversionTests.exe` in Debug.
- **build release**: builds the Release configuration.
- **build macos debug**: builds `ClassicNotepadMac.app`, `TextConversionTests`, and `MacSpellingCompileTests` in Debug.
- **build macos release**: builds the macOS target in Release under `build-macos-release`.
- **package macos universal**: builds, signs, verifies, tests, and zips the universal macOS app.
- **test debug**: builds Debug and then runs CTest.
- **test macos debug**: builds macOS Debug and then runs CTest.
- **clean**: removes generated build output.

## Manual Build

From PowerShell in the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Debug
```

Run the app:

```powershell
.\build\Debug\ClassicNotepad.exe
```

Run tests after building:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test.ps1 -Configuration Debug
```

Build and test the native GTK target under Ubuntu/WSL:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-ubuntu.ps1 -Distro Ubuntu-24.04
```

The Ubuntu build verifies the cross-platform shared core, console tests, and native GTK app target. Linux setup instructions live in [docs/LINUX_BUILD_ENVIRONMENT.md](docs/LINUX_BUILD_ENVIRONMENT.md).

When `libspelling-1-dev` and `hunspell-en-gb` are installed, the GTK target enables British English spelling through `libspelling`. If those packages are missing, the app still builds and runs with spelling reported as unavailable.

Run the Ubuntu GTK app from an Ubuntu shell:

```bash
cd /mnt/c/vibe/classicNotepad
./build-ubuntu/ClassicNotepadGtk
```

Force a deterministic Linux appearance without changing desktop or WSL settings:

```bash
CLASSIC_NOTEPAD_THEME=dark ./build-ubuntu/ClassicNotepadGtk
CLASSIC_NOTEPAD_THEME=light ./build-ubuntu/ClassicNotepadGtk
```

You can pass a first file path argument to open it through the shared document loader:

```bash
./build-ubuntu/ClassicNotepadGtk README.md
```

Run the Linux automation suite after building:

```powershell
wsl.exe -d Ubuntu-24.04 -- bash -lc "cd /mnt/c/vibe/classicNotepad && python3 tests/automation/run_automation_tests.py --binary build-ubuntu/ClassicNotepadGtk --platform linux"
```

Build a Release-style Linux target in a separate directory:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-ubuntu.ps1 -Distro Ubuntu-24.04 -BuildDir build-ubuntu-release -Configuration Release
```

Build release:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Release
```

Windows Release builds through `scripts\build.ps1` increment the patch number in `VERSION` and embed it in the app. Debug builds, including F5 from VS Code, leave `VERSION` unchanged. To rebuild Windows Release without consuming a version number, add `-SkipVersionIncrement`. macOS and Linux builds embed the current `VERSION` value; bump `VERSION` before those Release builds when cutting a new release.

Clean generated files:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\clean.ps1
```

## Manual CMake Commands

The scripts are the recommended path because they locate CMake and choose a supported Visual Studio generator. On a current Visual Studio 2022 setup, the underlying commands are:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

If only Visual Studio 2019 Build Tools are installed, the script automatically uses:

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The Visual Studio generator is used intentionally because it works with free Visual Studio Build Tools and does not require installing Ninja separately.

## Troubleshooting

### `CMake was not found`

Install **C++ CMake tools for Windows** from the Visual Studio Installer, or install CMake separately and add it to `PATH`.

### `No supported Visual Studio CMake generator was found`

Install Visual Studio Build Tools 2022 or 2019 with the **Desktop development with C++** workload.

### `Visual Studio 17 2022 could not find any instance of Visual Studio`

Open the Visual Studio Installer and install **Visual Studio Build Tools 2022** with the **Desktop development with C++** workload. You can also install Visual Studio Build Tools 2019; the script will use the 2019 generator if that is the supported generator on the machine.

### F5 says `cppvsdbg` is unknown

Install the VS Code **C/C++** extension by Microsoft. Its extension ID is `ms-vscode.cpptools`.

### The executable is missing

Run the default build task with `Ctrl+Shift+B` and read the terminal output. The expected debug executable is:

```text
build\Debug\ClassicNotepad.exe
```

### Spell checking is unavailable

On Windows, install **English (United Kingdom)** language support in Windows Settings, including typing or proofing features, then restart Classic Notepad.

On Ubuntu/WSL, install the documented spelling packages:

```bash
sudo apt-get update
sudo apt-get install -y libspelling-1-dev hunspell-en-gb
```

## Project Layout

```text
src/
  document.cpp, document.h       Document path, modified state, load/save behavior
  encoding.cpp, encoding.h       UTF-8, UTF-16 LE, and ANSI conversion
  line_endings.cpp, .h           Line-ending detection and conversion
  appearance.cpp, .h             Shared System/Light/Dark appearance state
  text_metadata.cpp, .h          Shared status metadata labels and character counts
  spell_text_utils.cpp, .h       Word range and spelling range helpers
  file_io.h, ansi_encoding.h     Platform seams used by the shared core
  platform/
    portable/                    Portable fallback file/ANSI helpers
    windows/
      app.cpp, app.h             Win32 application, menus, dialogs, editor, printing, theme, spell UI
      main.cpp                   Win32 entry point
      spell_check.cpp, .h        Windows Spell Checking API wrapper
      resources.rc, resource.h   Menus, accelerators, dialogs, and version/icon resources
      win32_platform.h           Common Win32 compile definitions
    linux/
      gtk_app.cpp, .h            GTK application, menus, dialogs, editor, printing, status, appearance, and About UI
      gtk_actions.cpp, .h        GTK action and menu wiring
      gtk_automation.cpp, .h     Linux JSON-lines automation controller
      gtk_dialogs.cpp, .h        GTK file, find, replace, go-to, font, error, and About dialogs
      gtk_main.cpp               GTK entry point
      gtk_spelling.cpp, .h       Optional GTK/libspelling British English spell service
    macos/
      Info.plist.in             macOS app bundle metadata template
      mac_app.mm, .h            AppKit application, menus, dialogs, editor, printing, status, appearance, and automation host adapter
      mac_automation.mm, .h     macOS JSON-lines automation controller
      mac_main.mm               macOS entry point
      mac_appearance.mm, .h      AppKit appearance override and semantic text-view colors
      mac_spelling.mm, .h        AppKit spelling configuration helpers

assets/
  icons/                     Application icon resources

docs/
  README.md                  Index for planning, research, and review notes
  LINUX_BUILD_ENVIRONMENT.md Ubuntu/WSL setup and Linux verification commands

tests/
  text_conversion_tests.cpp   Console tests for text/document/spell utility behavior
  linux_spelling_probe.cpp    Ubuntu-only libspelling/dictionary probe when available
  automation/                 Shared Windows/Linux/macOS semantic automation suite

scripts/
  build.ps1                  Configure and build with CMake
  build-ubuntu.ps1           Configure, build, and test under Ubuntu/WSL
  build-macos.sh             Configure and build the macOS AppKit target
  package-macos.sh           Build, ad-hoc sign, verify, test, and zip the universal macOS app
  test-macos.sh              Run macOS CTest targets
  test.ps1                   Run CTest
  clean.ps1                  Remove generated build output
```

## Verification

The expected Windows verification pass is:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Debug
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test.ps1 -Configuration Debug
python .\tests\automation\run_automation_tests.py --binary .\build\Debug\ClassicNotepad.exe --platform windows
```

CTest should report these passing test targets on Windows:

```text
TextConversionTests
TextConversionPortableSmokeTests
```

The expected Linux parity verification pass is:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-ubuntu.ps1 -Distro Ubuntu-24.04
wsl.exe -d Ubuntu-24.04 -- bash -lc "cd /mnt/c/vibe/classicNotepad && python3 tests/automation/run_automation_tests.py --binary build-ubuntu/ClassicNotepadGtk --platform linux"
```

The expected macOS verification pass is:

```bash
scripts/build-macos.sh
scripts/test-macos.sh
python3 tests/automation/run_automation_tests.py --binary build-macos/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac --platform macos
```

Release sanity checks:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Release -SkipVersionIncrement
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test.ps1 -Configuration Release
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-ubuntu.ps1 -Distro Ubuntu-24.04 -BuildDir build-ubuntu-release -Configuration Release
```

macOS Release sanity checks:

```bash
scripts/build-macos.sh --configuration Release --universal
scripts/test-macos.sh --configuration Release
lipo -archs build-macos/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac
```
