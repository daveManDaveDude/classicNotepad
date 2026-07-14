# macOS Large-Document Testing And Parity Handoff

Copy this document into a Codex session running on the target Mac, or tell Codex to read this file and carry out the task autonomously.

## Objective

Build and test Classic Notepad on macOS until large files use a genuinely responsive, editable AppKit surface with the same shared large-document core used by the Windows and Linux implementations.

Do not stop after a successful build, shared-core tests, or JSON automation. Verify the visible Release application using real mouse and keyboard input, including Save As into a deliberately selected directory.

The current repository snapshot has mapped large-document implementations for Windows and GTK Linux, but `src/platform/macos/mac_app.mm` still loads ordinary documents through `Document::Load` and displays them in an `NSTextView`. Assume that macOS large-document UI integration may still need to be implemented. Do not claim macOS large-file support merely because the portable piece-tree tests pass.

## Repository Safety

Before editing:

1. Read every `AGENTS.md` in the repository.
2. Read:
   - `README.md`
   - `docs/README.md`
   - `docs/MACOS_BUILD_PLAN.md`
   - `docs/PERFORMANCE_SECURITY_REVIEW.md`
   - `CMakeLists.txt`
   - `scripts/build-macos.sh`
   - `scripts/test-macos.sh`
   - `src/large_document.*`
   - `src/large_file_index.*`
   - `src/platform/macos/mac_app.*`
   - `src/platform/macos/mac_automation.*`
   - `src/platform/macos/mac_spelling.*`
   - `src/platform/windows/large_file_view.*`
   - `src/platform/linux/gtk_large_file_view.*`
   - `tests/text_conversion_tests.cpp`
   - `tests/automation/test_large_file_mode.py`
   - the complete shared automation suite
3. Inspect `git status`, `git diff`, and all untracked files.
4. Preserve existing Windows, Linux, and shared-core work. Do not use destructive Git commands.
5. Do not edit generated files under any build directory.
6. Keep large fixtures, saved copies, hashes, screenshots, logs, and performance output outside source control and remove test-generated files in a cleanup/finally path.

## macOS Architecture Target

Ordinary files should continue using the existing `NSTextView`. Files at or above the existing 48 MiB large-mode threshold should use an AppKit large-document view backed by the shared `LargeDocument` and `LargeFileLineScanner` facilities.

Retain:

- Immutable 64-bit memory-mapped source storage.
- The shared aggregate piece-tree document model.
- Sparse source decoding/checkpoints.
- Visible-region decoding and rendering.
- Streaming encoding and sibling-temporary-file atomic save.
- Bounded clipboard and viewport operations.

Do not materialize a large file as one `NSString`, `std::wstring`, `NSData`, `NSMutableString`, `NSTextStorage`, or undo snapshot.

The large AppKit view must provide:

- First responder and focus behavior.
- Visible caret and insertion point.
- Mouse hit testing, click placement, drag selection, and autoscroll.
- Arrow, Home/End-equivalent, Command+Up/Down, Page Up/Down, Option word movement, and Shift selection.
- Typing, Enter, Tab, multiline paste, Delete, Forward Delete, Select All, Cut, Copy, Paste, and multiple Undo.
- Context-menu edit commands and macOS spelling suggestions.
- Visible spelling underlines using bounded visible-row checking.
- Stable vertical and longest-line-based horizontal scrolling.
- Font changes without document-sized relayout.
- Dirty title, line/column/count/encoding status, Find, Replace, Replace All, and Go To.
- Save and Save As to the exact `NSSavePanel` URL.
- A wait/progress cursor while a foreground save is completing.
- Safe handling when Save As is requested before background indexing finishes.

Use AppKit APIs and repository code; do not introduce a web view, Rich Text dependency, or large third-party editor library.

## Phase Gates

Complete these in order. Do not proceed past a failing gate by weakening a test.

### 1. Baseline

Record:

```bash
uname -a
sw_vers
uname -m
xcodebuild -version
cmake --version
python3 --version
git status --short
```

Build and test the unchanged baseline:

```bash
scripts/build-macos.sh --build-dir build-macos --configuration Debug
scripts/test-macos.sh --build-dir build-macos --configuration Debug
python3 tests/automation/run_automation_tests.py \
  --binary build-macos/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac \
  --platform macos
```

Open an ordinary small file visibly and confirm that typing, undo, Save As, context menus, spelling, and closing still work.

### 2. Large-Mode Activation And First Usable Display

Add or verify macOS automation diagnostics equivalent to the bounded Linux/Windows state:

- active/editable/indexing state
- file size and indexed bytes
- UTF-16 character and logical line counts
- caret and selection positions
- top visible line
- piece count and inserted-buffer size
- first usable and full-index timings
- bounded rendered/text-range retrieval only

Opening a large file must return a caret-ready, typing-ready visible viewport within two seconds on local storage. First paint alone is not sufficient.

### 3. Visible Editing

Using actual visible input rather than direct model mutation, prove:

- A click moves the caret to the clicked character.
- Typing occurs at that position and is immediately painted.
- Shift+Arrow and mouse drag select the expected text.
- Page Down moves the caret/viewport without blank or fragmented frames.
- Delete, Forward Delete, multiline paste, Select All, bounded Copy, Cut, Paste, and Undo operate on the edited tree.
- The context menu invokes the same edit implementation as the menu bar and shortcuts.
- Misspellings in visible rows are underlined and a spelling suggestion replaces text in the large document.

### 4. Search And Navigation

Verify forward and backward Find, match case, whole word, Replace, scalable Replace All, and 64-bit Go To Line. Operations must not materialize the document. Reject stale results if the document changes during an operation.

### 5. Save, Save As, And Reopen

Test both immediate Save As while indexing is still active and Save As after indexing completes.

For each test:

1. Select a destination directory in `NSSavePanel` that is different from the source directory.
2. Use a distinctive filename.
3. Observe the wait/progress cursor while saving.
4. Verify that the window title changes to the selected filename only after success.
5. Verify the output exists inside the selected directory and does not exist beside the source.
6. Verify the source hash is unchanged.
7. Compare the complete saved-file hash with an independently constructed expected hash.
8. Reopen the saved file and verify every edit.
9. Make another edit, use normal Save, and verify the same destination changes atomically.
10. Confirm no sibling temporary save file remains.

Exercise paths containing spaces and non-ASCII characters as well as a normal ASCII path.

### 6. Real 700 MiB Acceptance

Create the fixture outside the repository and always remove the entire temporary directory on exit. The exact required source size is 734,003,200 bytes.

One suitable deterministic generator is:

```bash
fixture_root="$(mktemp -d -t classic-notepad-macos-large.XXXXXX)"
export FIXTURE_ROOT="$fixture_root"
trap 'rm -rf "$fixture_root"' EXIT

python3 - <<'PY'
import os
from pathlib import Path

root = Path(os.environ["FIXTURE_ROOT"])
path = root / "macos-large-734003200.txt"
size = 734_003_200
line = ("macOS large document acceptance 🚀 " + "0123456789abcdef" * 5 + "\r\n").encode("utf-8")

with path.open("wb") as output:
    remaining = size
    block = line * 8192
    while remaining >= len(block):
        output.write(block)
        remaining -= len(block)
    while remaining >= len(line):
        output.write(line)
        remaining -= len(line)
    output.write((b"Z" * remaining))

assert path.stat().st_size == size
print(path)
PY
```

For this fixture, visibly verify:

- beginning, indexed middle, and end content
- click-to-caret and typing at all three locations
- insertion, deletion, multiline paste, Time/Date, Find, Replace, Replace All, Undo, Select All, and bounded Copy
- scrollbar stability and longest-line horizontal range
- dirty title and clean-state restoration through Undo
- immediate Save As, source-hash preservation, full output hash, reopen, another edit, and normal Save

Record:

- open-command time
- first viewport paint
- first caret-ready/typing-ready time
- full-index time
- click/caret latency
- local edit and Undo latency
- Page Down and Go To latency
- Find, Replace, and Replace All latency
- Save As and normal Save time
- peak resident memory and physical footprint where available
- piece count and inserted-buffer size
- source/output sizes and SHA-256 hashes

Performance gates:

- First genuinely interactive display: at most 2 seconds.
- Local edit and Undo after the location is available: normally below 100 ms.
- Go To after indexing: normally below 500 ms.
- No visible flashing, partial frame, or document-sized allocation.

## Visible UI Requirement

Use the Codex environment's available macOS computer-control or screen-interaction capability. Launch the final Release `.app`, interact with the actual AppKit window, and retain screenshots or concise visual observations for the report.

Do not accept semantic automation commands such as `setSelection` or `insertText` as proof that click hit testing, first-responder behavior, caret painting, context menus, spelling, or Save As panels work.

If Accessibility permission is required, report the exact permission needed and pause for the user rather than bypassing macOS security controls.

## Final Matrix

After the final source change, run:

```bash
scripts/build-macos.sh --build-dir build-macos --configuration Debug
scripts/test-macos.sh --build-dir build-macos --configuration Debug
python3 tests/automation/run_automation_tests.py \
  --binary build-macos/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac \
  --platform macos

scripts/build-macos.sh --build-dir build-macos-release --configuration Release
scripts/test-macos.sh --build-dir build-macos-release --configuration Release
python3 tests/automation/run_automation_tests.py \
  --binary build-macos-release/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac \
  --platform macos

file build-macos-release/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac
codesign --verify --deep --strict build-macos-release/ClassicNotepadMac.app
git diff --check
git status --short
```

If a universal release is in scope, also run:

```bash
scripts/build-macos.sh --build-dir build-macos-universal --configuration Release --universal
lipo -archs build-macos-universal/ClassicNotepadMac.app/Contents/MacOS/ClassicNotepadMac
```

## Completion Report

Do not report success until the visible acceptance test passes. Include:

- Mac model, macOS version, CPU architecture, RAM, and storage type.
- Architecture implemented and exact files changed.
- What shared Windows/Linux/core code was reused.
- Caret, selection, viewport, spelling, search, and save design.
- Debug build, CTest, and automation results.
- Release build, CTest, and automation results.
- Visible small-file regression results.
- Visible large-file results and screenshots/observations.
- Exact fixture sizes, timings, memory measurements, and hashes.
- Confirmation that Save As used the selected directory.
- Confirmation that source files were unchanged and temporary files were deleted.
- Known limitations and remaining risks.

Continue autonomously through implementation, tests, visible verification, failure diagnosis, and cleanup. Do not substitute a plan or model-only test result for a working macOS large-document editor.
