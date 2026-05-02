# macOS Build And Port Plan

Date: 2026-05-02  
Status: implemented. The original plan is retained below as historical context and acceptance coverage.

## Goal

Maintain the native macOS version of Classic Notepad. The app now runs as an AppKit bundle on Apple Silicon and can be built as a universal `arm64;x86_64` Release app.

The macOS app should keep the same project direction as the existing Windows and Linux versions:

- Native desktop UI, not Electron or web UI.
- Shared C++17 document, encoding, line-ending, metadata, spelling utility, and appearance logic.
- AppKit-specific UI under `src/platform/macos/`.
- Shared JSON-lines automation coverage for parity with Windows and Linux.

## Current State

The repository now includes the native macOS app target:

- `ClassicNotepadCore` builds on macOS using the portable platform sources.
- `ClassicNotepadMac.app` builds from:
  - `src/platform/macos/mac_main.mm`
  - `src/platform/macos/mac_app.h`
  - `src/platform/macos/mac_app.mm`
  - `src/platform/macos/mac_automation.h`
  - `src/platform/macos/mac_automation.mm`
  - `src/platform/macos/Info.plist.in`
- The macOS code includes AppKit helpers for spelling and appearance:
  - `src/platform/macos/mac_spelling.mm`
  - `src/platform/macos/mac_appearance.mm`
- `MacSpellingCompileTests` verifies that the current AppKit helper code compiles and runs.
- `TextConversionTests` verifies the shared document/encoding/line-ending behavior on macOS.
- The shared JSON-lines automation runner accepts `--platform macos`.
- The visible macOS app supports file workflow, edit commands, Find/Find Next/Replace, Go To, word wrap, font metadata, status bar, page setup/print sink coverage, appearance override, and AppKit spelling configuration.
- The macOS File menu contains New, Open, Save, Save As, Page Setup, and Print. Close and Exit are intentionally not duplicated in that menu; Quit remains in the standard application menu.

Current Debug build and CTest path:

```bash
scripts/build-macos.sh
scripts/test-macos.sh
```

Current automation path:

```bash
python3 tests/automation/run_automation_tests.py --binary build-macos/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac --platform macos
```

Current universal Release build path:

```bash
scripts/build-macos.sh --configuration Release --universal
lipo -archs build-macos/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac
```

Current package path:

```bash
scripts/package-macos.sh --fresh
```

## Resolved Review Findings

### P1: No Runnable macOS App Target

Resolved. `CMakeLists.txt` now defines `ClassicNotepadMac` as a `MACOSX_BUNDLE` executable with `Info.plist`, icon resources, menu/window entrypoint, and automation host sources.

### P1: Automation Cannot Target macOS

Resolved. `tests/automation/run_automation_tests.py` accepts `macos`, and the macOS automation host implements the shared JSON-lines command surface.

### P2: macOS Build Path Is Undocumented

Resolved. The root README documents macOS build, test, automation, universal Release build, and package commands.

## Original Implementation Plan

### 1. Establish The Apple Silicon Baseline

Add a dedicated macOS build path before adding the full app:

- Add `scripts/build-macos.sh`.
- Add `scripts/test-macos.sh`.
- Document required tools:
  - Xcode Command Line Tools.
  - CMake.
  - Optional Ninja, while still supporting Unix Makefiles.
- Default the first macOS build to `arm64`.
- Keep this phase limited to current core/helper tests.

Acceptance checks:

```bash
scripts/build-macos.sh
scripts/test-macos.sh
file build-macos/TextConversionTests
```

Expected result: tests pass and binaries are `arm64` on Apple Silicon.

### 2. Add The First Runnable AppKit Target

Create a native macOS executable target, likely named `ClassicNotepadMac`.

Suggested source files:

- `src/platform/macos/mac_main.mm`
- `src/platform/macos/mac_app.h`
- `src/platform/macos/mac_app.mm`
- `src/platform/macos/Info.plist.in`

CMake work:

- Add `add_executable(ClassicNotepadMac MACOSX_BUNDLE ...)`.
- Link `ClassicNotepadCore` and `ClassicNotepadMacSpelling`.
- Link AppKit.
- Set bundle identifier, bundle name, executable name, display version, and build version.
- Add the app icon once an `.icns` is generated.

Acceptance checks:

- The app bundle builds.
- `open build-macos/ClassicNotepadMac.app` launches one window.
- Closing the window exits cleanly.

### 3. Port The Core App Shell

Implement the minimum useful Notepad shell:

- `NSApplication` lifecycle.
- Single `NSWindow`.
- `NSTextView` inside `NSScrollView`.
- App menu and File/Edit/Format/View/Help menus.
- Dirty title state using the existing title convention.
- Command-line file open.
- Missing command-line file creation prompt.
- Unsaved-changes prompt before New, Open, or Exit.
- New, Open, Save, Save As, and Exit.

Use shared `Document` behavior for open/save, encoding, line-ending, and metadata.

Acceptance checks:

- Create, edit, save, reopen, and save-as work.
- UTF-8, UTF-8 BOM, UTF-16 LE BOM, and ANSI fallback files round trip through the shared core.
- CRLF, LF, CR, and mixed line-ending metadata reports match the shared tests.

### 4. Bring Over Editor Feature Parity

Implement the app commands already present on Windows and Linux:

- Undo, Cut, Copy, Paste, Delete, Select All.
- Find, Find Next, Replace, Replace All.
- Go To line.
- Time/Date.
- Word Wrap.
- Font chooser.
- Status Bar.
- About.

Use AppKit-native controls where practical:

- `NSTextView` for text editing.
- `NSFindPanel` only if it can match the project behavior; otherwise use small native custom dialogs.
- `NSFontPanel` for font selection.
- `NSTextField` or a simple bottom bar for status text.

Acceptance checks:

- Manual parity checklist passes on Apple Silicon.
- Shared automation tests pass once the macOS automation host exists.

### 5. Integrate macOS Spelling And Appearance

Use the existing helper code:

- `ConfigureTextViewSpelling` should configure the real editor `NSTextView`.
- `ApplyApplicationAppearance` and `ApplyWindowAppearance` should support `System`, `Light`, and `Dark`.
- `ConfigurePlainTextViewAppearance` should keep semantic AppKit colors for the editor.

Acceptance checks:

- `CLASSIC_NOTEPAD_THEME=dark` launches a dark app without changing global macOS settings.
- `CLASSIC_NOTEPAD_THEME=light` launches a light app.
- Spell checking reports available when a British English dictionary is available and graceful unavailable when missing.

### 6. Add macOS Automation Early

Extend the automation system before the port grows too large:

- Add `macos` to `tests/automation/run_automation_tests.py`.
- Add a macOS automation controller using the same JSON-lines protocol as Windows and Linux.
- Keep command names and response shapes identical wherever possible.
- Add platform-specific skips only for genuinely OS-specific behavior.

Suggested source files:

- `src/platform/macos/mac_automation.h`
- `src/platform/macos/mac_automation.mm`

Acceptance checks:

```bash
python3 tests/automation/run_automation_tests.py --binary build-macos/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac --platform macos
```

Expected result: core file, encoding, edit, find/replace, status, appearance, spell capability, and print sink tests pass or have documented platform-specific skips.

### 7. Add Universal Build Support

After Apple Silicon is stable, add a universal build mode:

```bash
cmake -S . -B build-macos-universal -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build-macos-universal --config Release
lipo -archs build-macos-universal/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac
```

Acceptance checks:

- `lipo` reports `x86_64 arm64`.
- The app runs natively on Apple Silicon.
- The Intel slice launches under Rosetta with `arch -x86_64` where possible.
- A real Intel Mac smoke test is completed before claiming Intel support.

### 8. Package For Local Use

Add basic packaging once the app is usable:

- Generate `.icns` from `assets/icons/classic_notepad_large_transparent.png`.
- Add bundle metadata through `Info.plist`.
- Add ad-hoc codesigning for local development builds.
- Optionally create a `.zip` or `.dmg` artifact.

Notarization is only required for broader distribution outside local developer machines.

Acceptance checks:

- The app launches from Finder.
- The app launches from Terminal.
- The app can be copied to `/Applications` and run locally.

## First Useful Milestone

The first high-value milestone is:

- `ClassicNotepadMac.app` builds on Apple Silicon.
- The app opens one native AppKit window with an editable `NSTextView`.
- New/Open/Save/Save As work through the shared document core.
- Dirty title state works.
- Core CTest passes.
- The file workflow automation group can run with `--platform macos`.

This milestone proves the app architecture, build target, bundle shape, and automation path before investing in the full command surface.

## Suggested Follow-Up Milestones

### Milestone A: Apple Silicon Shell

- Build script.
- Test script.
- Runnable AppKit bundle.
- Single editable window.
- New/Open/Save/Save As.
- Dirty-state title.

### Milestone B: Automation And Core Parity

- `macos` automation platform support.
- File workflow tests.
- Encoding and line-ending tests.
- Edit command tests.
- Status metadata tests.

### Milestone C: Full Feature Parity

- Find/Replace/Go To.
- Word Wrap.
- Font chooser.
- Status bar.
- Appearance override.
- Spell capability.
- Page setup and printing.

### Milestone D: Universal And Packaging

- Universal `arm64;x86_64` build.
- `.icns` app icon.
- `Info.plist` metadata.
- Ad-hoc signing.
- Local zip or DMG artifact.
- Real Intel Mac smoke test.
