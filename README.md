# Classic Notepad

A deliberately small Win32/C++ recreation of classic single-document Notepad.

This repository is currently at Phase 6: a native Windows window, classic menu bar, multiline edit control, New/Open/Save/Save As, dirty-state prompts, command-line file open, resize behavior, encoding and line-ending preservation, focused text conversion tests, classic edit commands, Find/Replace, Go To, Time/Date, keyboard accelerators, Word Wrap, Font selection, an optional status bar with line/column display, Page Setup, Print, and an About dialog. Spell checking remains a future phase.

## What Is Built Through Phase 6

- Native Win32 desktop app.
- C++17 codebase.
- CMake project.
- Main window titled `Untitled - Classic Notepad`.
- Classic menu bar.
- Multiline `EDIT` control that fills the window.
- Resize handling.
- `File > New`.
- `File > Open...`.
- `File > Save`.
- `File > Save As...`.
- `File > Page Setup...`.
- `File > Print...`.
- `File > Exit`.
- `Help > About Classic Notepad`.
- Dirty-state tracking with a leading `*` in the window title.
- Confirmation before New/Open/Exit discards unsaved edits.
- Opening a file passed as the first command-line argument.
- UTF-8 with BOM, UTF-8 without BOM, UTF-16 LE with BOM, and ANSI fallback loading.
- Normal Save preserves the detected encoding.
- CRLF, LF, and CR line-ending detection.
- CRLF and LF files preserve their line endings after open/edit/save.
- Mixed line endings are tracked and saved using the detected dominant style.
- Focused console tests for encoding, line-ending, and document round-trip behavior.
- `Edit > Undo`.
- `Edit > Cut`, `Copy`, `Paste`, and `Delete`.
- `Edit > Find...`, `Find Next`, and `Replace...`.
- `Edit > Go To...`.
- `Edit > Select All`.
- `Edit > Time/Date`.
- `Format > Word Wrap`.
- `Format > Font...`.
- `View > Status Bar`.
- Status bar line/column updates after editing, selection/caret movement, scrolling, and Go To.
- Word Wrap recreates the edit control while preserving text, selection, and modified state where practical.
- Page Setup uses the standard Windows dialog and stores margins/printer settings for the current app session.
- Print uses the standard Windows print dialog and sends plain Unicode text through GDI using the selected editor font.
- Classic keyboard shortcuts: `Ctrl+N`, `Ctrl+O`, `Ctrl+S`, `Ctrl+Shift+S`, `Ctrl+F`, `F3`, `Ctrl+H`, `Ctrl+G`, `Ctrl+A`, `Ctrl+Z`, `Ctrl+X`, `Ctrl+C`, and `Ctrl+V`.
- VS Code F5 debugging through `.vscode/launch.json`.

The first milestone deliberately remains a single-document, local Win32 editor. It does not include tabs, cloud features, syntax highlighting, spell checking, autosave, telemetry, or modern editor extras.

## Current Text File Behavior

The current build includes explicit text loading and saving behavior:

- UTF-8 with BOM is detected and saved with BOM.
- UTF-8 without BOM is detected and saved without BOM.
- UTF-16 LE with BOM is detected and saved with BOM.
- Files that are not valid UTF-8 fall back to the system ANSI code page.
- New files save as UTF-8 without BOM.
- CRLF, LF, and CR line endings are normalized for the Win32 edit control while editing.
- Normal Save uses the line-ending style detected when the file was opened.
- Mixed line endings are recorded as mixed; because the Win32 edit control does not retain per-line origin information, saves use the dominant detected line-ending style.

## Required Free Software

Install these before building:

1. [Visual Studio Code](https://code.visualstudio.com/).
2. VS Code extension: `ms-vscode.cpptools` / **C/C++**.
3. Optional but useful VS Code extension: `ms-vscode.cmake-tools` / **CMake Tools**.
4. [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) or Visual Studio Community.

When installing Visual Studio Build Tools or Visual Studio Community, select:

- Workload: **Desktop development with C++**.
- Component: **MSVC C++ x64/x86 build tools**.
- Component: **Windows 11 SDK**. The latest available SDK is fine.
- Component: **C++ CMake tools for Windows**.

You do not need paid Visual Studio. Visual Studio Build Tools plus VS Code is enough.

The build script first looks for `cmake` on `PATH`, then looks for the CMake copy installed with Visual Studio's C++ CMake tools. It supports the Visual Studio 2022 and 2019 CMake generators, with 2022 recommended for a fresh machine.

## Build And Debug From VS Code

1. Open this folder in VS Code.
2. Install the recommended extensions when VS Code prompts you.
3. Press `F5`.
4. Choose **Debug Classic Notepad** if VS Code asks for a debug configuration.

The F5 launch configuration runs the `build debug` task first. That task calls:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Debug
```

The debugger then launches:

```text
build\Debug\ClassicNotepad.exe
```

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

Clean generated files:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\clean.ps1
```

## Manual CMake Commands

On a current Visual Studio 2022 setup, the build script uses these commands after locating `cmake`:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

The Visual Studio generator is used intentionally because it works well with free Visual Studio Build Tools and does not require installing Ninja separately.

If only Visual Studio 2019 Build Tools are installed, the script automatically uses:

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Debug
```

## Troubleshooting

### `CMake was not found`

Install **C++ CMake tools for Windows** from the Visual Studio Installer, or install CMake separately and add it to `PATH`.

### `Visual Studio 17 2022 could not find any instance of Visual Studio`

Open the Visual Studio Installer and install **Visual Studio Build Tools 2022** with the **Desktop development with C++** workload.

### F5 says `cppvsdbg` is unknown

Install the VS Code **C/C++** extension by Microsoft. Its extension ID is `ms-vscode.cpptools`.

### The executable is missing

Run the default build task with `Ctrl+Shift+B` and read the terminal output. The expected debug executable is:

```text
build\Debug\ClassicNotepad.exe
```

## Next Phase

The classic milestone is now functionally complete. Future work should be planned separately, starting with optional lightweight spell checking only if the classic single-document feel is preserved.
