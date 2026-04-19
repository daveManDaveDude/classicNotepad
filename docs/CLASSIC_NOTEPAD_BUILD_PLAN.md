# Classic Notepad Clone Build Plan

## Goal

Build a Windows-native, fast, efficient clone of classic pre-Windows-11 Notepad.

This first milestone is intentionally narrow: one document per process, one plain text editor window, no tabs, no cloud features, no autosave, no formatting beyond font selection, and no modern Windows 11 extras. Spell checking and cross-platform ports are future phases and must not be implemented in this milestone.

The application should feel like classic Notepad: small, quick to launch, dependable, and boring in the best possible way.

## Implementation Prompt For Codex

You are building a Windows-native classic Notepad clone. Use free tools only. Prefer C++ with the Win32 API and a native multiline edit control. Do not use Electron, WebView, Qt commercial tooling, .NET-only UI frameworks, or heavyweight app frameworks. The result should build from VS Code using a free compiler toolchain.

Build only the classic single-file Notepad clone described in this document. Do not add tabs, spell checking, cross-platform abstractions, recent files, markdown preview, syntax highlighting, AI features, cloud sync, autosave, settings accounts, ribbons, command palettes, or any other modern editor features.

## Recommended Technology

Use this stack for the first implementation:

- Language: C++17 or newer, kept close to plain Win32 style.
- UI: Win32 API.
- Main text area: native multiline edit control, preferably `EDIT` first for classic behavior.
- Build system: CMake.
- Compiler options:
  - Primary: Microsoft Visual Studio Build Tools / MSVC Community tools.
  - Alternative: LLVM/Clang for Windows or MinGW-w64.
- Editor: VS Code.
- Dependencies: none for milestone 1 unless a tiny, clearly justified helper library is needed.

Keep the executable small and the architecture simple. A good target is a single native `.exe` with no runtime dependency beyond the normal Windows system libraries and compiler runtime requirements.

## Non-Goals For Milestone 1

Do not implement:

- Tabs.
- Spell checking.
- Multiple document interface.
- Recent files list.
- Autosave.
- Session restore.
- Rich text.
- Formatting inside the document.
- Markdown, syntax highlighting, code editor behavior, minimap, folding, or line numbers.
- Cloud, telemetry, update checks, sign-in, or network access.
- Plugin architecture.
- Cross-platform UI layer.
- Store packaging.

## User-Facing Feature Scope

### Core File Behavior

Implement:

- New file.
- Open existing text file.
- Save.
- Save As.
- Exit.
- Dirty-state tracking with confirmation before losing unsaved changes.
- Window title that reflects current file name and modified state.
- Opening a file by command-line argument.
- Basic drag-and-drop file open, if it can be done cleanly without adding complexity.

### Text Editing

Implement classic plain text editing:

- Type, delete, select, undo, redo if provided by the control, cut, copy, paste.
- Select All.
- Find.
- Find Next.
- Replace.
- Go To line.
- Insert Time/Date.
- Optional Delete command if naturally supported by the menu and accelerator handling.

Use standard Windows dialogs and controls where possible.

### Menus

Use a classic menu bar, not a ribbon or toolbar.

Target menu shape:

- File
  - New
  - Open...
  - Save
  - Save As...
  - Page Setup...
  - Print...
  - Exit
- Edit
  - Undo
  - Cut
  - Copy
  - Paste
  - Delete
  - Find...
  - Find Next
  - Replace...
  - Go To...
  - Select All
  - Time/Date
- Format
  - Word Wrap
  - Font...
- View
  - Status Bar
- Help
  - About

If Page Setup and Print become too large for the first pass, stub them only if the UI clearly communicates they are not implemented yet. Prefer implementing the core file and editing behavior first.

### Status Bar

Implement a simple optional status bar:

- Current line and column.
- Visible only when enabled.
- If matching older Notepad behavior, disable or hide status bar when Word Wrap is enabled. If that adds unnecessary complexity, keep it simple and document the chosen behavior.

### Word Wrap

Implement Word Wrap as a toggle.

Important: classic Win32 edit controls often require recreating the edit control to change horizontal scrolling and wrapping behavior. Handle this carefully:

- Preserve text.
- Preserve selection/caret where practical.
- Preserve modified state.
- Preserve scroll position if practical.

### Font Selection

Implement a global editor font using the standard Windows Choose Font dialog.

This changes only the editor display font. It must not create rich text or save font metadata into files.

### Dialogs

Use native Windows common dialogs:

- Open file dialog.
- Save file dialog.
- Choose font dialog.
- Message boxes for confirmations and errors.
- Find/Replace dialog may use either standard common dialog behavior or a small custom dialog if the common dialog path becomes awkward.

## Text Encoding Requirements

Milestone 1 must support common plain text files without surprising data loss.

Implement at least:

- UTF-8 with BOM.
- UTF-8 without BOM.
- UTF-16 LE with BOM.
- ANSI/system code page fallback for older Windows text files.

Recommended internal representation:

- Keep editor text as UTF-16 because Win32 wide APIs and Unicode edit controls use UTF-16.
- Decode file bytes into UTF-16 on open.
- Encode UTF-16 back to the selected or detected file encoding on save.

Encoding behavior:

- Detect BOM when present.
- For files without BOM, attempt UTF-8 validation first.
- If valid UTF-8, treat as UTF-8 without BOM.
- Otherwise fall back to the system ANSI code page.
- Preserve the detected encoding on normal Save.
- Save As may default to the current encoding. A later milestone may add an encoding picker, but do not add that unless requested.

Warn before saving if text cannot be represented in the target legacy ANSI encoding. Prefer UTF-8 rather than silent character replacement.

## Line Ending Requirements

Support both Windows and Unix line endings.

Opening:

- Accept CRLF (`\r\n`).
- Accept LF (`\n`).
- Accept old Mac CR (`\r`) if easy.
- Normalize to the edit control's expected internal line breaks if needed.

Saving:

- Detect the dominant line ending style on open.
- Preserve that style on Save.
- For new files, default to Windows CRLF.
- For Save As, default to the current document's detected or chosen line ending style.
- Do not silently convert Unix LF files to CRLF unless the user explicitly chooses to in a future feature.

Track line ending mode in the document model:

- `CRLF`
- `LF`
- `CR`
- `Mixed`

For `Mixed`, preserve line endings if the implementation has a robust line-ending map. If that is too much for milestone 1, save using the dominant detected line ending and document the behavior in code comments and tests.

## Suggested Project Layout

Use a small, readable structure:

```text
classic-notepad/
  CMakeLists.txt
  README.md
  src/
    main.cpp
    app.h
    app.cpp
    document.h
    document.cpp
    encoding.h
    encoding.cpp
    line_endings.h
    line_endings.cpp
    dialogs.h
    dialogs.cpp
    resource.h
    resources.rc
  tests/
    encoding_tests.cpp
    line_ending_tests.cpp
  .vscode/
    tasks.json
    launch.json
```

Keep UI code and document/file transformation code separate. The encoding and line-ending logic should be testable without launching the GUI.

## Architecture

### App Layer

Responsibilities:

- Register window class.
- Create main window.
- Create child edit control.
- Create menus and accelerators.
- Handle resize.
- Dispatch menu commands.
- Manage message loop.
- Coordinate document loading and saving.
- Update title, menu enabled states, status bar, and modified marker.

### Document Layer

Responsibilities:

- Current file path.
- Current display text.
- Modified state.
- Detected encoding.
- Detected line ending mode.
- New/open/save/save-as transitions.
- Dirty-state confirmation logic.

### Encoding Layer

Responsibilities:

- Read bytes.
- Detect encoding.
- Decode to UTF-16.
- Encode from UTF-16.
- Report conversion errors.

### Line Ending Layer

Responsibilities:

- Analyze original file line endings.
- Normalize text for the editor.
- Convert editor text back to target line endings for save.
- Track CRLF/LF/CR/Mixed behavior.

### Dialog Layer

Responsibilities:

- Open/Save dialogs.
- Font dialog.
- Find/Replace dialogs if custom.
- About box.
- Error messages.

## Build Requirements

The project must be buildable with free tools.

### Preferred Build Path

Install:

- VS Code.
- CMake.
- Visual Studio Build Tools or Visual Studio Community with C++ desktop workload.

Expected commands:

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build --config Release
```

If Ninja is not installed, use the default Visual Studio generator or document the exact alternative command.

### VS Code Integration

Provide:

- `.vscode/tasks.json` with build and clean tasks.
- `.vscode/launch.json` for debugging the native executable.
- A short `README.md` explaining how to build and run with free tools.

## Implementation Phases

### Phase 1: Skeleton Native Window

Deliver:

- CMake project.
- Win32 application entry point.
- Main window.
- Menu bar.
- Multiline edit control filling the client area.
- Correct resize behavior.
- Basic About dialog.

Acceptance:

- App launches quickly.
- Text can be typed into the editor.
- Window closes cleanly.

### Phase 2: File Open, Save, And Dirty State

Deliver:

- New/Open/Save/Save As.
- Command-line file open.
- Dirty-state detection.
- Confirm before New/Open/Exit when unsaved changes exist.
- Window title updates.

Acceptance:

- Opening a text file displays its contents.
- Saving writes the expected bytes.
- Unsaved changes cannot be accidentally discarded without confirmation.

### Phase 3: Encoding And Line Endings

Deliver:

- BOM detection.
- UTF-8 validation.
- UTF-16 LE support.
- ANSI fallback.
- CRLF and LF detection.
- Preserve line endings on Save.
- Tests for encoding and line-ending behavior.

Acceptance:

- A Unix LF file remains LF after open/edit/save.
- A Windows CRLF file remains CRLF after open/edit/save.
- UTF-8 files remain UTF-8.
- UTF-16 LE files remain UTF-16 LE unless Save As behavior explicitly says otherwise.

### Phase 4: Classic Edit Commands

Deliver:

- Undo.
- Cut/copy/paste/delete.
- Select All.
- Find.
- Find Next.
- Replace.
- Go To.
- Time/Date.
- Keyboard accelerators matching classic Windows conventions.

Acceptance:

- Common shortcuts work: Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S if chosen, Ctrl+F, F3, Ctrl+H, Ctrl+G, Ctrl+A, Ctrl+Z, Ctrl+X, Ctrl+C, Ctrl+V.
- Menu state roughly matches selection/edit state where practical.

### Phase 5: Format And View

Deliver:

- Word Wrap toggle.
- Font dialog.
- Optional status bar with line/column.

Acceptance:

- Word Wrap works without losing text or modified state.
- Font changes display only.
- Status bar line/column updates after typing, clicking, scrolling, and using Go To.

### Phase 6: Print Basics

Deliver if still in scope:

- Page Setup.
- Print.

Acceptance:

- Plain text can be printed using standard Windows printing APIs.
- If printing is deferred, remove the menu items or clearly mark them as not implemented in the README.

## Testing Plan

Prioritize tests around behavior that can cause data loss.

### Unit Tests

Add tests for:

- UTF-8 BOM detection.
- UTF-8 without BOM detection.
- Invalid UTF-8 fallback.
- UTF-16 LE BOM detection.
- CRLF preservation.
- LF preservation.
- CR preservation if implemented.
- Mixed line ending handling.
- Round-trip saving for representative files.

### Manual GUI Tests

Run these before considering milestone 1 complete:

1. Launch app and type text.
2. Save a new file.
3. Reopen the saved file.
4. Open a file from the command line.
5. Open a CRLF file, edit it, save it, and verify CRLF remains.
6. Open an LF file, edit it, save it, and verify LF remains.
7. Open UTF-8 with non-ASCII text and verify round trip.
8. Open UTF-16 LE with non-ASCII text and verify round trip.
9. Try closing with unsaved changes and verify confirmation.
10. Use Find, Replace, Go To, and Time/Date.
11. Toggle Word Wrap on and off.
12. Change font and verify the file contents are not affected.

## Performance Targets

Classic Notepad should feel instant for ordinary files.

Targets:

- Cold launch should be fast on a normal Windows machine.
- Typing should not lag for normal text files.
- Opening small files should feel immediate.
- Avoid background services, indexing, watchers, telemetry, and unnecessary worker threads.
- Avoid loading large frameworks.

For very large files, classic Notepad behavior can be modest. The app should not promise to be a large-file editor unless the implementation later switches to a custom virtualized text control.

## Data Safety Rules

These rules are more important than matching every classic Notepad edge case:

- Never silently discard unsaved edits.
- Never silently replace characters with question marks during save.
- Never convert line endings unexpectedly.
- Never change file encoding unexpectedly on normal Save.
- Show a clear error if a file cannot be opened or saved.
- Write saves carefully enough to avoid leaving a corrupt file if possible.

For safer saving, consider writing to a temporary file in the same directory and then replacing the target file. Preserve this only if it does not create surprising permission or metadata behavior.

## Definition Of Done

Milestone 1 is complete when:

- The app is a native Windows desktop application.
- It builds using only free tools.
- It opens as a single classic editor window.
- It supports one file at a time.
- It implements New, Open, Save, Save As, and Exit.
- It supports basic classic edit operations.
- It supports Find, Replace, Go To, Word Wrap, Font, and optional Status Bar.
- It preserves UTF-8, UTF-16 LE, ANSI fallback behavior, and common line endings as specified.
- It has focused tests for encoding and line endings.
- It has clear README build instructions for VS Code and free compilers.
- It does not include tabs or modern Notepad features.

## Future Milestones, Not For This Build

After the classic clone is complete, future work can be planned separately:

- Add lightweight spell checking while preserving the classic single-document interface.
- Add a settings file only if needed for font, wrap, and spell-check preferences.
- Investigate a fast cross-platform version using a lightweight native toolkit.
- Keep the cross-platform port separate enough that the Windows-native classic version remains small and clean.

