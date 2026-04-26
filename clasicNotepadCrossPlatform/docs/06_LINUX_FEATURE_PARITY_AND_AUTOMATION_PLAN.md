# Linux Feature Parity And Cross-Binary Automation Plan

Date: 2026-04-26

Status: Phase 10 parity gate completed for Debug builds; Release sanity builds pass on Windows and Ubuntu.

## Goal

Make the native Linux GTK binary feature-compatible with the current Win32 Classic Notepad binary.

The project is done when the same automated feature suite runs successfully against both binaries:

- Windows: `ClassicNotepad.exe`
- Linux: `ClassicNotepadGtk`

The Linux implementation should preserve the product shape of the Windows version: single-document plain text editor, native desktop UI, local files, no tabs, no cloud features, no telemetry.

## Current Branch State

Completed so far:

- Shared core was separated from Win32 file I/O and encoding APIs.
- Core builds and tests on Windows and Ubuntu/WSL.
- Existing Win32 app was moved to `src/platform/windows/`.
- Portable file I/O and ANSI fallback sources exist under `src/platform/portable/`.
- Shared status metadata helpers exist in `text_metadata.*`.
- Early GTK app target exists at `src/platform/linux/gtk_main.cpp`.
- `ClassicNotepadGtk` currently provides:
  - native GTK window
  - `GtkTextView` editor surface
  - status label
  - first command-line file load
  - shared encoding/line-ending/document metadata display
  - native File/Edit/Format/View menu actions through GTK actions
  - New/Open/Save/Save As file workflow with dirty-state prompts
  - undo/cut/copy/paste/delete/select-all/time-date editing commands
  - editor context menu entries for common editing commands
  - Find/Find Next/Replace/Replace All/Go To support
  - word-wrap, font metadata, and status-bar visibility controls
  - Page Setup and Print menu actions backed by GTK print/page setup APIs
  - Help > About Classic Notepad
  - automation `pageSetup` and `printToTestSink` commands for deterministic print validation
  - spell-check automation capability reporting, with Linux v1 gracefully unavailable
  - shared JSON-lines automation coverage through phase 10

Accepted v1 platform differences in `ClassicNotepadGtk`:

- No native Linux spell-check provider is enabled in v1. The app reports `spellCheck == false` and spelling commands return graceful unavailable results.
- Linux dark mode is out of cross-platform v1 scope. The app reports `darkMode == false`.

## Definition Of Done

The Linux binary is feature-compatible when:

1. `ClassicNotepadGtk` builds on Ubuntu through:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-ubuntu.ps1 -Distro Ubuntu-24.04
   ```

2. Existing core tests pass on Windows and Ubuntu.

3. A shared automated feature suite can run against both binaries.

4. The automated suite covers the feature inventory below.

5. Windows and Linux both pass the suite in Debug builds.

6. Release builds still pass core tests.

7. Any deliberate platform differences are documented and either excluded from v1 parity or represented as capability flags in the test harness.

## Feature Inventory To Match

### File Workflow

- New document.
- Open existing text file.
- Save existing document.
- Save As.
- Command-line first-argument file open.
- Missing command-line file create flow or documented Linux equivalent.
- Dirty-state title marker.
- Dirty-state confirmation before New/Open/Exit.
- Error reporting when open/save fails.

### Text Encoding And Line Endings

- New files save UTF-8 without BOM.
- UTF-8 with BOM is detected and preserved.
- UTF-8 without BOM is detected and preserved.
- UTF-16 LE with BOM is detected and preserved.
- Non-UTF-8 bytes fall back to ANSI behavior.
- CRLF/LF/CR/Mixed detection.
- Save preserves source line-ending style where practical.
- Mixed files save using dominant style.
- Status metadata displays encoding and line-ending labels.

### Editing Commands

- Undo.
- Cut.
- Copy.
- Paste.
- Delete.
- Select All.
- Time/Date.
- Keyboard shortcuts where OS conventions allow.
- Context menu with common edit commands.

### Find, Replace, And Navigation

- Find.
- Find Next.
- Replace.
- Replace All.
- Match case.
- Whole word.
- Direction behavior where applicable.
- Go To line with validation.
- Selection/caret behavior after find/replace/go-to.

### Format And View

- Word Wrap toggle.
- Font chooser.
- Status Bar toggle.
- Status bar line and column.
- Status bar character count.
- Status bar encoding and line-ending metadata.

### Help

- Help menu.
- About Classic Notepad dialog.

### Print

- Page Setup.
- Print.
- Plain text print output using selected editor font.
- Session-level page setup persistence where the platform supports it.

### Spell Checking

- Windows currently supports `en-GB` through the Windows Spell Checking API.
- Linux v1 may expose spell checking as unavailable unless a native-friendly strategy is chosen.
- Tests should treat spell checking as a capability:
  - `spellCheck == true` on configured Windows machines with `en-GB`.
  - `spellCheck == false` is acceptable on Linux v1 if documented and graceful.

## Automation Strategy

Do not rely primarily on fragile screen-coordinate automation.

Build a shared semantic automation harness that drives both desktop binaries through the same app-level contract.

Recommended approach:

- Add an optional `--automation` launch mode to both binaries.
- In automation mode, the app opens normally enough to exercise real application code, but also exposes a JSON-lines command channel over stdin/stdout or a local named endpoint.
- Tests send semantic commands and assert semantic state and file bytes.
- Windows automation calls the existing Win32 command handlers.
- Linux automation calls the GTK adapter command handlers.
- The same Python test suite runs against both binaries by changing only the binary path and platform capability config.

Why this approach:

- Same tests can run against both binaries.
- Avoids brittle UI coordinates.
- Avoids needing separate Windows UI Automation and Linux AT-SPI implementations before parity work can proceed.
- Still exercises real app command flow, document state, encoding, line endings, status metadata, and native adapter behavior.
- Leaves room for a smaller visual smoke layer later.

## Automation Protocol Shape

Use JSON Lines. One request per line, one response per line.

Example launch:

```powershell
.\build\Debug\ClassicNotepad.exe --automation
```

```bash
./build-ubuntu/ClassicNotepadGtk --automation
```

Example request:

```json
{"id":1,"command":"setText","text":"hello\r\nworld"}
```

Example response:

```json
{"id":1,"ok":true}
```

All responses should include:

- `id`
- `ok`
- optional `error`
- optional command-specific result

Initial required commands:

- `getCapabilities`
- `newDocument`
- `openFile`
- `save`
- `saveAs`
- `setText`
- `getText`
- `insertText`
- `getTitle`
- `isModified`
- `getDocumentMetadata`
- `getStatusText`
- `getSelection`
- `setSelection`
- `selectAll`
- `undo`
- `cut`
- `copy`
- `paste`
- `deleteSelection`
- `find`
- `findNext`
- `replace`
- `replaceAll`
- `goToLine`
- `insertTimeDate`
- `setWordWrap`
- `getWordWrap`
- `setStatusBarVisible`
- `getStatusBarVisible`
- `setFont`
- `getFont`
- `pageSetup`
- `printToTestSink`
- `close`

Capability response example:

```json
{
  "id": 1,
  "ok": true,
  "capabilities": {
    "platform": "windows",
    "nativeUi": "win32",
    "printing": true,
    "pageSetup": true,
    "fontChooser": true,
    "spellCheck": true,
    "darkMode": true
  }
}
```

Linux v1 may report:

```json
{
  "platform": "linux",
  "nativeUi": "gtk4",
  "printing": true,
  "pageSetup": true,
  "fontChooser": true,
  "spellCheck": false,
  "darkMode": false
}
```

Tests should skip only explicitly accepted capability gaps. Feature parity work should reduce skips over time.

## Test Harness Layout

Suggested files:

```text
tests/automation/
  run_automation_tests.py
  classic_notepad_driver.py
  test_file_workflow.py
  test_encoding_line_endings.py
  test_edit_commands.py
  test_find_replace_go_to.py
  test_format_view_status.py
  test_printing.py
  test_spell_capability.py
  fixtures/
```

The runner should accept:

```powershell
python tests\automation\run_automation_tests.py --binary build\Debug\ClassicNotepad.exe --platform windows
```

```bash
python3 tests/automation/run_automation_tests.py --binary build-ubuntu/ClassicNotepadGtk --platform linux
```

The runner should:

- Create an isolated temp directory per test.
- Launch the binary in automation mode.
- Send JSON commands.
- Capture stdout/stderr logs.
- Kill the app on timeout.
- Compare saved file bytes, not only editor text.
- Produce a clear summary of pass/fail/skip.

## Automated Test Matrix

### File Workflow Tests

- New document starts as `Untitled - Classic Notepad`, unmodified.
- Typing marks document modified and title gains `*`.
- Save As writes file, clears dirty marker, updates title.
- Save writes existing path and clears dirty marker.
- Open loads file content and updates title.
- New with dirty document triggers a save/discard/cancel decision path.
- Open with dirty document triggers a save/discard/cancel decision path.
- Exit with dirty document triggers a save/discard/cancel decision path.
- Command-line open loads first argument.
- Missing command-line file behavior matches documented policy.

### Encoding And Line-Ending Tests

- Save new text produces UTF-8 without BOM and CRLF.
- Open/save UTF-8 BOM preserves BOM.
- Open/save UTF-8 no BOM preserves no BOM.
- Open/save UTF-16 LE BOM preserves UTF-16 LE BOM.
- Open/save LF file preserves LF.
- Open/save CRLF file preserves CRLF.
- Open/save CR file preserves CR.
- Mixed file reports Mixed and saves dominant style.
- Invalid UTF-8 fixture falls back to ANSI.
- ANSI save rejects characters that cannot be represented.
- Status metadata reports expected encoding and line-ending labels after each fixture load.

### Edit Command Tests

- Undo reverses typing.
- Cut removes selection and updates clipboard/test clipboard.
- Copy writes selection to clipboard/test clipboard.
- Paste inserts clipboard/test clipboard content.
- Delete removes selection.
- Select All selects full document.
- Time/Date inserts a non-empty platform-formatted value.
- Context menu commands map to the same command handlers, if automation can trigger them semantically.

### Find/Replace/Go-To Tests

- Find selects first match.
- Find Next advances through matches.
- Find reports not found.
- Match-case option distinguishes text.
- Whole-word option distinguishes word boundaries.
- Replace replaces current match.
- Replace All replaces every match and returns count.
- Go To valid line moves caret.
- Go To invalid line reports validation error and preserves caret.

### Format/View/Status Tests

- Word Wrap toggles on/off and preserves text.
- Font setting changes display font metadata and does not affect file bytes.
- Status Bar toggle hides/shows status.
- Line/column updates after caret movement.
- Character count treats CRLF and surrogate pairs consistently.
- Encoding and line-ending status parts match shared `text_metadata` helpers.

### Printing Tests

Printing is hard to validate through real native dialogs in CI. Use an automation-only print sink:

- `printToTestSink` renders the same text that would be printed into a deterministic text or PDF-like artifact.
- Windows implementation should share as much layout/input code as possible with real print path.
- Linux implementation can start with text sink before real `GtkPrintOperation`.

Tests:

- Page setup stores margins/session settings.
- Print sink includes full document text.
- Print sink respects selected font metadata where practical.
- Empty document print is handled gracefully.

### Spell Capability Tests

- `getCapabilities` reports spell-check availability.
- If available:
  - known misspelling returns suggestions.
  - ignore once clears one word occurrence.
  - add to dictionary path reports success or platform-supported result.
- If unavailable:
  - app remains usable.
  - spelling commands return a graceful unavailable result.

## Implementation Plan

### Phase 1: Windows Automation Harness First

Purpose: freeze current Win32 behavior in executable tests before changing Linux heavily.

Tasks:

1. Add `--automation` argument parsing to Win32 `main.cpp`.
2. Add an automation controller for Win32:
   - owns JSON line read/write loop
   - runs on UI thread or marshals commands to UI thread
   - calls existing `ClassicNotepadApp` command handlers or new app-level public automation methods
3. Expose safe automation methods from `ClassicNotepadApp`.
4. Add initial Python runner.
5. Add tests for:
   - new/type/saveAs/save/open
   - encoding/line-ending round trips
   - title/dirty/status metadata
6. Run suite against Windows Debug binary until green.

Acceptance:

- Windows build/tests pass.
- Automation suite passes against `ClassicNotepad.exe`.
- No automation mode behavior leaks into normal app mode.

### Phase 2: Linux Automation Contract

Purpose: make the same tests able to launch and talk to `ClassicNotepadGtk`.

Tasks:

1. Add `--automation` parsing to GTK app.
2. Add Linux automation controller using the same JSON protocol.
3. Implement enough commands to pass the Phase 1 test set.
4. Keep command behavior routed through Linux app command functions, not direct test-only document mutation where avoidable.

Acceptance:

- Same Python tests pass against both binaries for Phase 1 coverage.

### Phase 3: Linux Command Architecture

Purpose: stop growing all Linux behavior inside `gtk_main.cpp`.

Tasks:

1. Split Linux adapter into:
   - `gtk_main.cpp`
   - `gtk_app.h/.cpp`
   - `gtk_actions.h/.cpp`
   - `gtk_dialogs.h/.cpp`
   - `gtk_automation.h/.cpp`
2. Define command methods matching the Windows automation surface:
   - `NewDocument`
   - `OpenFile`
   - `Save`
   - `SaveAs`
   - `ConfirmDiscard`
   - `UpdateTitle`
   - `UpdateStatus`
3. Keep shared document behavior in `Document`.

Acceptance:

- Existing Linux app behavior still works.
- Automation tests still pass.

### Phase 4: Linux File Workflow Parity

Tasks:

1. Add native GTK menu/actions:
   - File > New/Open/Save/Save As/Exit
2. Add native file dialogs.
3. Add dirty-state prompt dialogs.
4. Add command-line missing-file create flow or documented Linux equivalent.
5. Wire title updates and dirty marker.

Tests to enable:

- Full file workflow test group against Linux.

Acceptance:

- File workflow tests pass on Windows and Linux.

### Phase 5: Linux Editing Commands

Tasks:

1. Wire GTK actions for undo/cut/copy/paste/delete/select-all.
2. Add Time/Date insertion.
3. Add keyboard shortcuts.
4. Add editor context menu.
5. Ensure dirty state updates match Windows.

Tests to enable:

- Edit command test group against Linux.

Acceptance:

- Edit command tests pass on Windows and Linux.

### Phase 6: Linux Find/Replace/Go-To

Tasks:

1. Move reusable find/replace helpers into shared core if needed.
2. Add Linux find/replace UI.
3. Add Find Next action.
4. Add Go To dialog with validation.
5. Match Windows selection/caret behavior as closely as practical.

Tests to enable:

- Find/replace/go-to group against Linux.

Acceptance:

- Find/replace/go-to tests pass on Windows and Linux.

### Phase 7: Linux Format/View/Status

Tasks:

1. Add word-wrap toggle.
2. Add font chooser.
3. Add status-bar toggle.
4. Ensure status text uses shared metadata helpers.
5. Preserve selection/caret/text through toggles.

Tests to enable:

- Format/view/status group against Linux.

Acceptance:

- Format/view/status tests pass on Windows and Linux.

### Phase 8: Linux Printing

Tasks:

1. Add automation print sink first.
2. Add native `GtkPrintOperation`.
3. Add page setup/session settings if GTK support is suitable.
4. Document platform differences.

Tests to enable:

- Printing group against Linux, except any explicitly accepted native-dialog-only behavior.

Acceptance:

- Print automation tests pass on Windows and Linux.

### Phase 9: Spell Strategy

Tasks:

1. Add `ISpellService`-style capability surface if not already present.
2. Keep Windows implementation backed by current Windows Spell Checking API.
3. For Linux v1, choose:
   - unavailable capability, or
   - optional Enchant-backed feature behind build/runtime detection.
4. Ensure unavailable state is graceful.

Acceptance:

- Spell capability tests pass on both platforms.
- Linux unavailable state is accepted only if documented.

### Phase 10: Final Parity Gate

Tasks:

1. Run Windows Debug build. Completed 2026-04-26.
2. Run Windows CTest. Completed 2026-04-26.
3. Run Windows automation suite. Completed 2026-04-26.
4. Run Ubuntu build. Completed 2026-04-26.
5. Run Ubuntu CTest. Completed 2026-04-26.
6. Run Ubuntu automation suite. Completed 2026-04-26.
7. Run Windows Release build/test. Completed 2026-04-26 with `-SkipVersionIncrement`.
8. Run Ubuntu Release-style build. Completed 2026-04-26 with `-Configuration Release` and `build-ubuntu-release/`.
9. Update README, Linux setup, and known-gaps docs. Completed 2026-04-26.

Acceptance:

- Same automation suite passes against both binaries.
- No unapproved skips remain.
- Known accepted gaps are documented as platform capabilities.

Verification results:

- Windows Debug build: passed.
- Windows Debug CTest: `TextConversionTests` and `TextConversionPortableSmokeTests` passed.
- Windows Debug automation: 20 tests passed.
- Ubuntu Debug build: passed.
- Ubuntu Debug CTest: `TextConversionTests` passed.
- Ubuntu Debug automation: 20 tests passed.
- Windows Release build with `-SkipVersionIncrement`: passed.
- Windows Release CTest: `TextConversionTests` and `TextConversionPortableSmokeTests` passed.
- Ubuntu Release-style build and CTest: passed.

Accepted capability differences:

- Linux spell checking remains unavailable in v1 (`spellCheck: false`).
- Linux dark mode remains unavailable in v1 (`darkMode: false`).

## Suggested Commands For Future Context

Windows:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Debug
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test.ps1 -Configuration Debug
```

Ubuntu:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-ubuntu.ps1 -Distro Ubuntu-24.04
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-ubuntu.ps1 -Distro Ubuntu-24.04 -BuildDir build-ubuntu-release -Configuration Release
```

Run current GTK binary manually:

```bash
cd /mnt/c/vibe/classicNotepad
./build-ubuntu/ClassicNotepadGtk
./build-ubuntu/ClassicNotepadGtk README.md
```

Future automation examples:

```powershell
python tests\automation\run_automation_tests.py --binary build\Debug\ClassicNotepad.exe --platform windows
```

```bash
python3 tests/automation/run_automation_tests.py --binary build-ubuntu/ClassicNotepadGtk --platform linux
```

Linux build environment setup is documented in `../../docs/LINUX_BUILD_ENVIRONMENT.md`.

## Risks And Notes

- GTK native dialogs and Win32 native dialogs will differ visually; tests should assert behavior, not pixels.
- Clipboard tests need isolation. Prefer a test clipboard abstraction in automation mode, with one smaller native clipboard smoke test per platform.
- Printing tests use a deterministic print sink before native print-dialog behavior is relied on.
- WSLg may emit Mesa/EGL warnings. Treat them as non-fatal if the app stays running and tests can connect.
- Building under `/mnt/c` may produce occasional Make clock-skew warnings. A future improvement is building inside the Ubuntu filesystem and copying artifacts only when needed.
- The automation protocol must not become a second implementation path. It should route through the same commands the UI uses.
