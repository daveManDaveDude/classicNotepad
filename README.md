# Classic Notepad

Classic Notepad is a finished single-document Win32/C++ recreation of classic Windows Notepad. It keeps the plain local editor feel: one window, one text document, native Windows dialogs, no tabs, no cloud features, no telemetry, and no editor extras that would pull it away from the classic experience.

The project is built with C++17, Win32, CMake, and the free Visual Studio C++ Build Tools. It is intended to be opened, built, tested, and debugged from Visual Studio Code.

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
- Windows spell checking for British English (`en-GB`) when that language is installed.
- Spell checking runs over the visible editor region, draws red wavy underlines, and offers right-click suggestions.
- Spell check context actions include suggestions, Ignore Once, and Add to Dictionary.
- Keyboard accelerators: `Ctrl+N`, `Ctrl+O`, `Ctrl+S`, `Ctrl+Shift+S`, `Ctrl+F`, `F3`, `Ctrl+H`, `Ctrl+G`, `Ctrl+A`, `F5`, `Ctrl+Z`, `Ctrl+X`, `Ctrl+C`, `Ctrl+V`, and `Del`.
- Focused console tests for encoding, line-ending, document round-trip, metadata, new-file, and spelling text utility behavior.

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

Classic Notepad uses the Windows Spell Checking API, not a bundled dictionary. On startup it tries to create an `en-GB` spell checker.

If British English spell checking is installed, misspelled words in the visible editor area are underlined and right-clicking a marked word opens spelling suggestions.

If British English spell checking is not installed, the app shows one informational message and keeps the editor fully usable without spell checking.

To install the language support on Windows 10 or Windows 11:

1. Open **Settings**.
2. Go to **Time & language > Language & region**.
3. Add or select **English (United Kingdom)**.
4. Install the language features that include basic typing or proofing support.
5. Restart Classic Notepad.

## Required Free Software

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
4. No CMake preset or Kit selection is required for F5 debugging; the repository tasks call the PowerShell build script directly.
5. Press `Ctrl+Shift+B` to run the default **build debug** task.
6. Press `F5` and choose **Debug Classic Notepad** if VS Code asks for a debug configuration.

The F5 launch configuration runs the **build debug** task first:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Debug
```

The debugger then launches:

```text
build\Debug\ClassicNotepad.exe
```

Useful VS Code tasks are already defined in `.vscode/tasks.json`:

- **build debug**: builds `ClassicNotepad.exe` and `TextConversionTests.exe` in Debug.
- **build release**: builds the Release configuration.
- **test debug**: builds Debug and then runs CTest.
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

Build release:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Release
```

Each build through `scripts\build.ps1` increments the patch number in `VERSION` and embeds it in the app. To rebuild without consuming a version number, add `-SkipVersionIncrement`.

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

Install **English (United Kingdom)** language support in Windows Settings, including typing or proofing features, then restart Classic Notepad.

## Project Layout

```text
src/
  app.cpp, app.h              Win32 application, menus, dialogs, editor, printing, theme, spell UI
  document.cpp, document.h    Document path, modified state, load/save behavior
  encoding.cpp, encoding.h    UTF-8, UTF-16 LE, and ANSI conversion
  line_endings.cpp, .h        Line-ending detection and conversion
  spell_check.cpp, .h         Windows Spell Checking API wrapper
  spell_text_utils.cpp, .h    Word range and spelling range helpers
  resources.rc               Menus, accelerators, and Go To dialog resource

tests/
  text_conversion_tests.cpp   Console tests for text/document/spell utility behavior

scripts/
  build.ps1                  Configure and build with CMake
  test.ps1                   Run CTest
  clean.ps1                  Remove generated build output
```

## Verification

The expected verification pass is:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Debug
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test.ps1 -Configuration Debug
```

CTest should report one passing test target:

```text
TextConversionTests
```
