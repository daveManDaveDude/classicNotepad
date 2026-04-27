# Classic Notepad Current Behavior Spec (Baseline for Cross-Platform Port)

Date: 2026-04-20  
Baseline source: the finished `classicNotepad` repository behavior and docs.

## Product Identity

- Single-document plain text desktop editor.
- Native menus/dialogs and local-file workflow.
- No tabs, no cloud features, no telemetry.

## Window & Document Model

- One top-level editor window with title format:
  - `Untitled - Classic Notepad` for new files.
  - `*` prefix when modified.
- Dirty-state confirmation before destructive actions (New, Open, Exit).
- Command-line file open via first argument.
- Optional create flow when command-line file does not exist.

## Menus and Commands

## File
- New
- Open...
- Save
- Save As...
- Page Setup...
- Print...
- Exit

## Edit
- Undo
- Cut / Copy / Paste / Delete
- Find... / Find Next
- Replace...
- Go To...
- Select All
- Time/Date

## Format
- Word Wrap toggle
- Font...

## View
- Status Bar toggle

## Help
- About

## Core Editing Behavior

- Plain text editing only.
- Standard keyboard shortcuts for major commands.
- Modeless Find/Replace behavior consistent with native platform patterns.
- Go To line with validation.
- Context menu for common edit commands.

## Status Bar

- Shows line, column, character count.
- Also reports detected line-ending and encoding metadata.

## File Encoding Behavior

- New files save UTF-8 without BOM.
- Detect/open/save support for:
  - UTF-8 with BOM
  - UTF-8 without BOM
  - UTF-16 LE with BOM
  - ANSI/system-code-page fallback for non-UTF-8 bytes
- Normal Save preserves detected encoding.
- Save As preserves current document encoding (no encoding picker in current behavior).

## Line Ending Behavior

- Detect CRLF / LF / CR / Mixed.
- Editor-normalized line endings while editing.
- Save preserves source style where practical.
- Mixed files save using dominant style (documented compromise).

## Printing

- Native page setup and print dialogs.
- Plain text printing using selected editor font.
- Session-level page setup persistence.

## Spell Checking (Windows-specific current implementation)

- Uses Windows Spell Checking API for `en-GB` when installed.
- Draws red underlines for misspellings in visible region.
- Right-click suggestions + Ignore Once + Add to Dictionary.
- If unavailable, editor remains fully functional.

## Scope Adjustment for Cross-Platform v1

For the new cross-platform project, this baseline was initially carried forward with dark mode deferred and Linux spell-check treated as optional. The 2026-04-26 spelling/dark-mode follow-up supersedes that initial scope:

- Dark mode is a cross-platform parity item through a shared `System` / `Light` / `Dark` appearance state.
- Linux spell checking is optional at build/runtime through GTK/libspelling and remains graceful when unavailable.
