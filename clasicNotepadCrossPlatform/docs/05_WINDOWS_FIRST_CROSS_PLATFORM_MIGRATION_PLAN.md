# Windows-First Cross-Platform Migration Plan

Date: 2026-04-26

Status: active plan for the `feature/cross-platform` branch.

## Goal

Convert the existing working Win32 Classic Notepad source into a cross-platform source tree while keeping Windows as the only required build/run target for the current phase.

The near-term success condition is not a macOS or Linux GUI. It is a Windows build that still behaves like the finished Classic Notepad, with shared core code that is cleanly buildable by non-Windows toolchains when native macOS/Linux adapters are added.

## Context From Existing Docs

- Keep native UI per platform: Win32 on Windows, AppKit on macOS, GTK on Linux.
- Keep document, encoding, line-ending, and text helper behavior in shared C++.
- Avoid Electron/WebView/heavy cross-platform UI frameworks.
- Preserve the current single-document plain-text workflow.
- Dark mode is out of scope for cross-platform v1, even though the current Windows app already has it.
- Spell checking remains platform-specific and optional outside Windows/macOS.

## Migration Principles

- Keep the Windows app building at every step.
- Prefer small seams over a broad rewrite.
- Preserve existing tests and add cross-platform smoke coverage as soon as a portable path exists.
- Move platform-specific implementation behind narrow helpers before moving large UI files.
- Do not remove current Windows features just because they are not in cross-platform v1 scope.

## Staged Plan

1. Establish baseline.
   - Read root docs and cross-platform planning docs.
   - Build Debug on Windows.
   - Run CTest.

2. Separate shared core from Win32 APIs.
   - Move byte-level file I/O behind `file_io.h`.
   - Move system ANSI code-page conversion behind `ansi_encoding.h`.
   - Keep Windows implementations backed by Win32 APIs.
   - Add portable fallback implementations for non-Windows builds.
   - Keep `Document`, `encoding`, `line_endings`, and spelling text utilities free of Windows headers.

3. Split CMake by platform.
   - Build `ClassicNotepadCore` and tests everywhere.
   - Build the existing `ClassicNotepad` Win32 GUI only on Windows.
   - Enable RC language only on Windows.
   - On Windows, add a portable-core smoke test target so portable fallbacks compile during Windows development.

4. Formalize source layout.
   - Keep current files stable while tests guard behavior.
   - Gradually move Windows-only GUI files under a platform-specific area when the build split is stable.
   - Keep shared core headers in a location that macOS/Linux adapters can include without Win32 leakage.

5. Extract app-level contracts.
   - Identify UI-to-core operations: New, Open, Save, Save As, dirty-state metadata, encoding/line-ending status, find/replace helpers.
   - Avoid forcing macOS/Linux adapters through Win32 concepts like `HWND`, resource IDs, or Windows message loops.

6. Add native adapter skeletons later.
   - macOS: AppKit `.mm` target guarded by `APPLE`.
   - Linux: GTK target guarded by UNIX/non-APPLE checks and `pkg-config`.
   - These targets should start as native windows wired to shared core tests, not full parity implementations.

7. Bring feature parity forward in slices.
   - File workflows and dirty prompts.
   - Encoding and line-ending round trips.
   - Find/replace/go-to/time-date helpers.
   - Word wrap, font, and status metadata.
   - Printing and page setup.
   - Optional spell-service capability layer.

8. Verify continuously.
   - Windows Debug build after every slice.
   - Windows CTest after every slice.
   - Release build before larger checkpoints.
   - Manual GUI smoke checks when desktop automation is practical: launch, type, save, reopen, verify bytes.

## Current Checkpoint

Completed in the first implementation slice:

- `ClassicNotepadCore` no longer requires Win32 APIs for document file I/O or text encoding.
- Windows file I/O still uses `CreateFileW`, `ReadFile`, `WriteFile`, `FlushFileBuffers`, and `MoveFileExW`.
- Windows ANSI behavior still uses `CP_ACP`.
- Portable fallback sources exist for future macOS/Linux builds.
- CMake builds the Win32 GUI only on Windows.
- CMake includes a Windows-only portable-core smoke test target.

Completed in the second implementation slice:

- Existing Win32 app files moved under `src/platform/windows/`.
- Shared core files remain under `src/`.
- `ClassicNotepad` still builds only on Windows, but it is now physically separated from the shared core and portable fallback sources.
- Windows resource paths were adjusted for the new source layout.

Completed in the third implementation slice:

- Status metadata formatting moved from the Win32 app into shared core:
  - encoding labels
  - line-ending labels
  - thousands-separated numbers
  - character-count text
  - status character counting where CRLF and UTF-16 surrogate pairs count as one character
- Existing Windows status bar behavior now uses the shared helpers.
- Core tests cover the shared metadata behavior.

Verification at this checkpoint:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Debug
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test.ps1 -Configuration Debug
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-ubuntu.ps1 -Distro Ubuntu-24.04
```

Expected CTest targets:

- `TextConversionTests`
- `TextConversionPortableSmokeTests` on Windows

Ubuntu/WSL checkpoint:

- Ubuntu distro used: `Ubuntu-24.04`.
- CMake, Ninja, `pkg-config`, Python 3, and GTK4 development packages installed through Ubuntu package manager.
- Build directory: `build-ubuntu/`.
- Release-style build directory: `build-ubuntu-release/`.
- Current Ubuntu target coverage: `ClassicNotepadCore`, `TextConversionTests`, and feature-parity `ClassicNotepadGtk`.
- Expected Ubuntu CTest target: `TextConversionTests`.
- The Windows GUI target is intentionally not built on Ubuntu.
- The GTK target currently provides native File/Edit/Format/View/Help menus, file workflow, edit commands, find/replace/go-to, word wrap, font metadata, status bar, About dialog, page setup, print, optional GTK/libspelling spell checking, and automation coverage.
- Linux spell checking is enabled when `libspelling-1` and the GB Hunspell dictionary are present. Missing backend or dictionary states report graceful unavailable responses.

Detailed Linux setup instructions are maintained in `../../docs/LINUX_BUILD_ENVIRONMENT.md`.

Run the current Ubuntu GTK binary from an Ubuntu shell:

```bash
cd /mnt/c/vibe/classicNotepad
./build-ubuntu/ClassicNotepadGtk
./build-ubuntu/ClassicNotepadGtk README.md
```

Current Windows adapter location:

```text
src/platform/windows/
  app.cpp, app.h
  main.cpp
  spell_check.cpp, spell_check.h
  resource.h, resources.rc
  win32_platform.h
  ansi_encoding_win32.cpp
  file_io_win32.cpp
```
