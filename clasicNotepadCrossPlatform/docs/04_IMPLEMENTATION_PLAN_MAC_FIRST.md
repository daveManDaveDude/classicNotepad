# Implementation Plan: Mac-First Cross-Platform Classic Notepad

Date: 2026-04-20  
Status: historical proposal. The repository now contains native Win32, GTK4 Linux, and AppKit macOS targets; current build and verification commands live in the root `README.md`.

## 0) Principles

- Native first.
- Small executable and fast startup.
- Dark mode was excluded from this original v1 plan, then added by the 2026-04-26 spelling/dark-mode follow-up.
- Use free toolchains only.
- Keep core logic shared and heavily unit-tested.

## 1) Repo Bootstrap (new `clasicNotepadCrossPlatform` repository)

Deliverables:
- `README.md` with goals, constraints, supported platforms.
- `docs/` with this spec + research + mapping docs.
- `CMakeLists.txt` root + per-target subdirectories.
- CI skeleton for format/build/test matrix (macOS + Linux; optional Windows build-only).

Acceptance checks:
- `cmake -S . -B build` works on macOS and Linux.
- Empty app target compiles on both.

## 2) Step 1: Hello World Native Window (macOS first)

Deliverables:
- Minimal AppKit app launched from Objective-C++ entrypoint.
- Single titled window appears.
- Build/run command documented.

Acceptance checks:
- App launches from terminal and from IDE.
- Window closes cleanly; exit code 0.

## 3) Step 2: Shared Core Skeleton

Deliverables:
- `core/document` class.
- `core/encoding` module with interfaces and tests.
- `core/line_endings` module with interfaces and tests.
- Unit-test executable in C++.

Acceptance checks:
- Tests pass on macOS.
- No platform headers in `core/`.

## 4) Step 3: macOS Editor Surface

Deliverables:
- `NSTextView` main editor in scroll view.
- Basic menu bar with File/Edit/Format/View/Help shells.
- Title updates for dirty state.

Acceptance checks:
- Typing works.
- Dirty marker appears and clears on save.

## 5) Step 4: File Operations (New/Open/Save/Save As)

Deliverables:
- Native open/save panels.
- Unsaved-changes prompt for destructive operations.
- Command-line file open.
- Missing-file create confirmation flow.

Acceptance checks:
- Round-trip open/edit/save passes for UTF-8 and UTF-16 LE BOM test files.
- Save As path updates title.

## 6) Step 5: Encoding + Line Ending Parity

Deliverables:
- Detect UTF-8 BOM / UTF-8 / UTF-16 LE BOM / ANSI fallback behavior.
- Detect CRLF/LF/CR/Mixed and save using source or dominant style.
- Status metadata fields wired.

Acceptance checks:
- Regression corpus tests pass for each encoding/line-ending mode.

## 7) Step 6: Edit Command Parity

Deliverables:
- Undo/Cut/Copy/Paste/Delete/Select All.
- Find, Find Next, Replace, Go To line, Time/Date.
- Keyboard accelerators equivalent to baseline where OS conventions allow.

Acceptance checks:
- Manual parity checklist passes on macOS.

## 8) Step 7: Word Wrap + Font + Status Bar

Deliverables:
- Word-wrap toggle.
- Font chooser integration.
- Status bar toggle with line/column/char count and metadata.

Acceptance checks:
- Wrap toggle preserves caret/selection where practical.
- Font applies to full editor display.

## 9) Step 8: Printing + Page Setup (macOS)

Deliverables:
- Native page setup and print flow.
- Plain text print layout.
- Session-level page settings persistence.

Acceptance checks:
- Print preview and print dialog complete successfully.

## 10) Step 9: Linux Bring-up (Ubuntu default desktop)

Deliverables:
- GTK app/window/text view equivalent.
- Same command surface wired to shared core.
- Build doc for Ubuntu dependencies and commands.

Acceptance checks:
- Open/edit/save workflow functions on Ubuntu.
- Shared-core tests pass unchanged.

## 11) Step 10: Optional Linux Spell Strategy (post-v1 gate)

Deliverables:
- Decision doc: defer vs implement optional Enchant-backed feature.
- If implemented, runtime capability detection + graceful disable path.

Acceptance checks:
- No impact to core editor usability when unavailable.

## 12) Step 11: Windows Buildability Target

Deliverables:
- CMake targets compile on Windows toolchain.
- Platform adapter stubs or minimal implementation enough for build verification.

Acceptance checks:
- CI or documented local build command succeeds on Windows.

## 13) QA and Release Checklist (v1)

- Shared appearance state and Linux/Windows dark-mode automation coverage are now part of the follow-up scope.
- Startup time and memory spot-checks.
- Open/save reliability checks (large files, interruption-safe strategy).
- Documentation complete:
  - build environment setup (macOS/Linux/Windows)
  - developer quickstart
  - architecture notes
  - known gaps (Linux spell check status)

## 14) Suggested Initial Milestones

- Milestone A: Steps 1–3
- Milestone B: Steps 4–6
- Milestone C: Steps 7–8
- Milestone D: Step 9
- Milestone E: Steps 10–12 + hardening
