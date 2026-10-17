# Classic Notepad: Windows reliability review and improvement plan

> **Historical baseline:** this review records findings reproduced on 2 October 2026 at source revision `d506e83f58d050da75d97cd5c2c81b0453b74ca2`. Its dates, test counts, and defect descriptions are retained as written to preserve the audit record; they are not a current implementation-status ledger. For later item-review dispositions, current Release evidence, and qualification gaps, see [Windows reliability status](WINDOWS_RELIABILITY_STATUS.md). In particular, passing implementation items does not close the real clipboard, supported-Windows matrix, or visible-app qualification gates.

**Review date:** 2 October 2026  
**Reviewed source:** `d506e83f58d050da75d97cd5c2c81b0453b74ca2` — version `1.3.0`  
**Scope:** repository survey, deep Windows and shared-core review, fresh Windows Release build, existing tests, and targeted failure probes.  
**Product goal:** a small, dependable classic Notepad with spell checking. Improve existing behaviour, correctness, responsiveness, and maintainability. Freeze feature expansion and defer further macOS/Linux development until Windows meets the acceptance gates below.

## 1. Assessment

The native C++ direction fits this product. The shared encoding and line-ending helpers, platform spelling services, test driver, and mapped-file editor provide a useful foundation. There is no reason to replace the application with a heavy framework or expand its feature set.

The Windows version is **not yet ready to be treated as fully reliable**, particularly around large-document saving and very long lines. The most urgent problems involve document integrity, followed by scrolling and bounded work. Passing the current suite does not establish that these paths are safe.

The review reproduced all of the following:

- Save As during background indexing reported success after writing only a **16 MiB prefix of a roughly 192 MiB file**.
- A mapped-source read failure during an edit returned failure but left a previously nonempty document with **zero characters**.
- A four-row, approximately 48 MiB CSV exposed a horizontal range for its full lines, but display/hit testing stopped at **65,536 UTF-16 units** per line.
- Adding 1,000 rows to a large document left the vertical scrollbar using the original line count.
- In the ordinary editor, moving the widest row offscreen and refreshing layout made the horizontal scrollbar disappear in both light and dark modes.
- Saving an edited large LF-only file converted its line endings to CRLF.
- A valid emoji crossing a streaming chunk boundary could not be saved as UTF-8.
- Opening and saving the ordinary file bytes `a\0b` silently produced only `a`.
- Replace All could leave a partially undoable operation when the undo budget trimmed records; deleted insertion buffers remained allocated after undo eviction.

These findings explain why an app that works well with many short lines can fail badly on a CSV with only four enormous rows. File size, line length, line count, Unicode boundaries, and edit history need independent qualification.

**Recommended order:** secure Save and document transitions; repair the shared model; make long-line access bounded; correct both scrollbar implementations; then finish spelling, editing consistency, visual qualification, and code cleanup.

## 2. What was reviewed and tested

### Repository coverage

The baseline contains **115 tracked files**, approximately **1.40 MB of source/assets/documentation** and **36,816 text lines**. Every tracked file was included in the inventory and repository survey. The appendix identifies each file and its review coverage.

Windows and shared-core implementation paths received the deepest inspection, particularly opening, indexing, editing, Unicode conversion, source reads, snapshots, saving, scrolling, viewport layout, spelling, search, undo, and automation. Linux/macOS code was surveyed for architecture and shared-core dependencies; their runtime behaviour was not qualified. Documentation was reviewed as historical context and checked against current code rather than accepted as proof of completion. Binary icons were inventoried; this was not an artwork review.

Generated builds and synthetic fixtures stayed outside the repository. The pre-existing untracked `test33.txt`, `test33.txt.classic-notepad-save-KU28MP`, and `test333333.txt` were left untouched. No application implementation was changed in this review.

### Fresh build and baseline tests

| Check | Result | Meaning |
| --- | --- | --- |
| Isolated Windows x64 Release build | Passed | Current source builds using MSVC 19.29 / Visual Studio 2019 Build Tools and Windows SDK 10.0.26100.0. |
| CTest | 2/2 passed | `TextConversionTests` and `TextConversionPortableSmokeTests`. |
| Shared Windows automation suite | 30 passed, 4 skipped; 34 discovered | Existing semantic coverage passed. Skips were platform-specific Linux/macOS tests. |
| Additional core failure probes | Multiple failures reproduced | Tests exercised the compiled production core. |
| Additional Windows probes | Multiple failures reproduced | Tests used the fresh binary and a controlled paste harness as described below. |

Release was built directly through CMake to avoid the build script's automatic `VERSION` increment. The existing 700 MiB performance acceptance script was inspected but was **not run in this review**. Its 128-byte-row fixture is useful for many-line performance, but cannot qualify enormous logical lines.

### Paste checks and their limits

The paste harness included the production `app.cpp` and delivered `WM_PASTE` to the real native edit control. It replaced clipboard acquisition with a controlled UTF-16 provider, so the production paste handler, normalization, `EM_REPLACESEL`, and native control were exercised without changing the user's clipboard.

| Payload | Light mode | Dark mode | Content result |
| --- | --- | --- | --- |
| 7,000,000 UTF-16 units across 250,000 rows | Approximately 833 ms | Approximately 828 ms | Exact expected content; Undo restored the original text. |
| 1,048,576 UTF-16 units in one row | Approximately 93 ms | Approximately 92 ms | Exact expected content. |

These checks did **not** reproduce content loss on those paste sizes. They also do not establish that every clipboard producer, lock contention case, or larger payload works. Windows clipboard acquisition itself was substituted. The normal-editor scrollbar failure was reproduced separately on a 360,002-unit document.

Tests used hidden application windows, automation commands, and targeted Win32 messages. This review did not complete visible mouse/keyboard acceptance, screenshot comparison, real printer testing, screen-reader testing, or a DPI/multiple-monitor matrix. Timings are observations on this machine, not portable performance guarantees.

### Evidence terminology

- **Reproduced:** observed in an additional runtime probe.
- **Fault-injected:** observed with a deliberately failing source reader or constrained resource budget.
- **Code-path finding:** the implementation shows the defect or risk, but the full user interaction was not reproduced here.
- **Qualification gap:** behaviour still needs direct acceptance testing; this is not a claim that it fails.

## 3. Architecture and why the trouble is concentrated here

Windows has two editing engines:

1. Ordinary files use a subclassed multiline Win32 `EDIT` control. The application adds spelling overlays, custom dark menus/scrollbars, metadata, and file handling.
2. Files at or above **48 MiB** use `LargeFileView`: a custom Win32 drawing/input surface, a mapped source, a worker indexer, and the shared `LargeDocument` piece tree.

The threshold is decided when opening a file. Large pasted text in an ordinary document continues through the native control; it does not automatically become the mapped editor. A source-size threshold therefore does not solve large paste workloads.

`LargeDocument` uses a randomized balanced tree with aggregate character and line-break summaries. Original source pieces refer to mapped bytes; inserted text uses append-only buffers. This is a sound starting point, but several implementation assumptions undermine it:

- Source pieces are targeted at 64 KiB but cut only at newline boundaries. An enormous line becomes an enormous piece.
- Partial source reads, piece splitting, and prefix summaries can repeatedly decode or scan those enormous pieces.
- ASCII byte/character translation does not populate the sparse checkpoints used on multibyte paths.
- The viewport reads a fixed prefix of a line instead of the horizontal segment that is actually visible.
- Some scrollbar calculations still use the original scanner index, while status telemetry uses the edited tree.
- A save snapshot can contain only the indexed prefix; the save API does not require full indexing.
- Background operation code also changes mapping and viewport state owned by the UI.

The clean direction is to make the current model authoritative and its work genuinely bounded. Preserve the native ordinary editor for now. Avoid a wholesale replacement until measured limits justify it.

## 4. Prioritized findings

P0 means document integrity or unsaved-work loss needs immediate attention. P1 means an existing behaviour is broken or a substantial reliability risk exists. P2 means consistency, efficiency, or qualification work follows the integrity fixes.

| ID | Priority | Finding | Evidence |
| --- | --- | --- | --- |
| W01 | P0 | Save can commit an incompletely indexed document | Reproduced |
| W02 | P0 | Save-before-close/New/Open treats asynchronous start as completion | Code-path finding |
| W03 | P0 | Failed tree splitting can destroy document content | Fault-injected |
| W04 | P1 | Valid Unicode fails at read and streaming boundaries | Reproduced |
| W05 | P1 | Enormous lines are neither fully accessible nor processed with bounded work | Reproduced and code-path findings |
| W06 | P1 | Large-document vertical scrolling uses the original line count after edits | Reproduced |
| W07 | P1 | Ordinary-editor horizontal scrollbar depends on currently visible rows | Reproduced |
| W08 | P1 | Large-file metadata forces LF/CR saves toward CRLF; CR-only character offsets are wrong | Reproduced |
| W09 | P1 | Worker save/rebase touches UI state and can race subsequent edits | Code-path finding |
| W10 | P1 | Load can silently truncate at embedded NUL; load commit is not transactional | Reproduced and code-path finding |
| W11 | P1 | Replace All undo grouping and memory accounting are unreliable under pressure | Fault-injected and code-path findings |
| W12 | P1 | Spell-check coordinates and viewport extraction fail to cover long/scrolled text correctly | Code-path findings |
| W13 | P1 | Background errors and command availability do not reliably reflect operation state | Reproduced failure state and code-path findings |
| W14 | P2 | Existing wrap implementation is inaccessible from the large-file menu and has layout inconsistencies | Code-path findings |
| W15 | P2 | Search, snapshot cloning, and cancellation can do excessive work | Code-path findings |
| W16 | P2 | Status counts and editor transitions have inconsistent semantics | Code-path findings |
| W17 | P2 | Maintainability and release evidence lag the actual implementation | Repository survey |

### W01 — Save can commit only the indexed prefix

**Source:** `src/platform/windows/large_file_view.cpp:288` (`StartSaveAsync`), `:948` (`EnsureEditable`), and the incremental document builder at `:857`.

`EnsureEditable` requires valid encoding and a non-null model. It does not require complete indexing. `StartSaveAsync` clones that model and streams it as if it were the whole document. Background indexing subsequently changes the generation, but the stale-generation branch still replaces the destination with the completed snapshot output.

**Reproduction:** generated a 201,850,880-byte CRLF file; waited until the first indexed region was editable; inserted `X` at position zero; started asynchronous Save As before indexing completed. The operation reported `operationSucceeded: true`. The destination was **16,777,216 bytes**, whereas the full edited output should have been **201,850,881 bytes**. The source fixture remained separate from the destination.

This proves truncated Save As output. In-place Save uses the same snapshot entry point, so the issue is also a serious overwrite risk; this review did not deliberately overwrite the test source to demonstrate that variant.

**Repair:** initially gate Save and whole-document operations until complete, validated indexing. Preserve prefix editing only if it is safe and clearly accounted for. A later internal implementation may retain the entire unindexed source in the model, but that requires a formal completeness invariant; it must not be approximated by an editable prefix.

**Acceptance:** every successful save includes the full source plus the intended edit. Test Save and Save As immediately after the first usable viewport, during indexing, after indexing, and during concurrent edits. Compare complete byte hashes against independently generated expected output.

### W02 — Choosing Save does not guarantee saving before a document is discarded

**Source:** `src/platform/windows/app.cpp:4686` (`HandleSave`), `:4713` (`HandleSaveAs`), `:6054` (`ConfirmSaveChanges`), `:4661` (New), `:6977` (close); `large_file_view.cpp:146` and `:158` (shutdown/cancellation).

The ordinary save path returns after saving. The large path returns true after starting a worker. `ConfirmSaveChanges` returns that true value to New/Open/close, allowing the current view to be reset or destroyed while Save is still running. Destruction cancels and joins the operation. A user can choose Save and still lose unsaved work.

There is a related prompt risk: large-view dirty notifications are deferred, but confirmation consults the facade `Document` dirty flag. An immediate document transition can run before that facade receives the edit notification.

**Repair:** use an explicit document-transition protocol. Choosing Save queues the intended transition. Execute it only after a successful, complete save of the required edit revision; on failure or cancellation retain the document. Read authoritative dirty state at the time of confirmation. Keep the event loop responsive while waiting. A save that started successfully is not a completed save.

Also consolidate the disk replacement rules. The large-file helper passes `REPLACEFILE_WRITE_THROUGH`, which Microsoft documents as unsupported, and retries replacement failures before potentially deleting the complete temporary file. Some documented replacement errors can leave the original pathname absent or moved. Preserve the recoverable output when replacement has entered that state; do not assume every failure leaves the original intact. See [Microsoft's ReplaceFileW contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew).

**Acceptance:** edit a large file, request close/New/Open, choose Save, and verify the final file before the transition occurs. Inject write, flush, replace, and reopen failures. Failed Save retains the current document, dirty state, and a recoverable complete file where applicable. Repeat with close immediately after a keystroke.

### W03 — A failed edit can destroy the tree

**Source:** `src/large_document.cpp:625` (`SplitPiece`), the recursive `Split`, and `Replace` around `:853`.

`Replace` moves ownership of the live root into splitting operations. Those operations can fail while reading or summarizing an original piece. Their failure paths do not consistently return the moved tree, so reconstructing from partial outputs can lose the original document.

**Fault-injected reproduction:** initialized an original piece containing `abcdef`; made a source read fail during insertion at position three. `Replace` returned false with a mapped-source error, but `CharacterCount()` became zero.

**Repair:** provide strong transactional edit guarantees. Perform fallible boundary/summary work before changing live ownership, or use path copying/rollback that retains every original subtree until commit. Apply the same rule to Undo and compound replacements. Do not deep-copy the whole document for each keystroke.

**Acceptance:** fail every source-read stage of insertion, deletion, replacement, and undo. Content, selection, generation, dirty state, and undo history are unchanged whenever an operation reports failure. Add randomized edit sequences checked against a small independent text model.

### W04 — Unicode boundaries are unsafe

**Source:** `src/large_document.cpp:409` (`IsCharacterBoundary`), `:734` (original reads), `:803` (`ReadRange`), `:1141` (`StreamText`), and `src/encoding.cpp` chunk encoding.

Two separate failures were observed:

1. For UTF-8 source text `a😀b`, the valid boundary before the emoji was rejected. A one-unit read at the emoji reported success while returning zero units. Boundary validation asks for individual UTF-16 units, while the decoder cannot satisfy that request consistently for a supplementary scalar.
2. An original piece containing 65,535 ASCII units followed by an emoji was streamed in fixed 65,536-unit chunks. That split the surrogate pair, and per-chunk UTF-8 encoding rejected otherwise valid document text.

**Repair:** distinguish UTF-16 document offsets from scalar decode boundaries. Define a consistent contract for a code-unit read, a scalar-safe text window, and an edit boundary. Streaming chunks must keep surrogate pairs intact or use an encoder that carries pending Unicode state across chunks. Maintain CRLF state across chunks independently.

**Acceptance:** valid supplementary characters remain readable, editable, searchable, copyable, and savable at positions immediately before/after 64 Ki boundaries, piece boundaries, mapping boundaries, and viewport boundaries. Invalid sequences fail explicitly without changing the document or destination.

### W05 — Huge lines break access and bounded processing

**Source:** `large_file_view.cpp:857` (`AppendIndexedDocument`), `:1015` (`RebuildViewportCache`), `:1483` (`ReadEditedLine`), `:1644` (`HitTest`); `large_document.cpp:376`, `:529`, `:625`, and `:1141`.

**Reproduction:** a 50,331,698-byte four-row CSV had approximately 12.6 million UTF-16 units per row. Diagnostics reported four pieces. Reading the first row returned only 65,536 units and took about 2.1 seconds. After scrolling to the horizontal right edge, a click placed the caret at 65,536 rather than near the actual first-row end, 12,582,923. The UI therefore cannot access the tail through the intended scrolling/hit-test path.

The model compounds this:

- A 64 KiB target is not a bound when piece cuts require newlines. Splitting a very long original piece summarizes both resulting halves.
- `SummarizePrefix` can decode a large partial piece just to calculate a caret coordinate.
- ASCII handling in `OriginalCharacterToByte` bypasses checkpoint creation. Two successive one-unit tail reads on a 4 MiB ASCII piece each requested approximately 4 MiB again. Computing the tail caret coordinate requested approximately 12 MiB.
- `StreamText` decodes an entire original piece before handing off smaller chunks. Save/search memory and cancellation latency can therefore scale with the longest line.
- Horizontal extent uses average character width and is capped to an `int` pixel range. That is not sufficient for tabs, proportional fonts, or enormous logical offsets.

**Repair:** bound source pieces independently of line endings, cutting at valid scalar boundaries and preserving CR/LF summaries across pieces. Populate checkpoints on ASCII and multibyte paths, and use efficient checkpoint lookup. Decode the visible horizontal segment plus a small context margin. Cache its document range and measured geometry so paint, caret placement, selection, spelling, and hit testing use the same layout. Stream source decoding in bounded blocks with cancellation checks between blocks.

Keep 64-bit document coordinates and logical horizontal positions separate from bounded Win32 scrollbar/pixel coordinates. Avoid allocating a string proportional to an entire row for paint, caret movement, one-character edits, or streaming Save.

**Acceptance:** the start, middle, and last marker of a one-row file and a four-row CSV remain reachable by keyboard, scrollbar, mouse selection, Find, and Go To. Qualify 48 MiB, 256 MiB, and 700 MiB total sizes, including rows longer than a mapping window. Show memory and source-read work bounds as well as timings.

### W06 — Edited rows do not determine the vertical range

**Source:** `large_file_view.cpp:1161` (`IndexedLineCount`), `:1187` (`EstimatedTotalLines`), `:1207` / `:1220` (position conversion), and `:1231` (`RefreshScrollBars`).

`EstimatedTotalLines` uses original scanner offsets after indexing. `GetInfo` subsequently overrides its reported count from the edited tree, so diagnostics can look correct even when the actual scrollbar remains wrong.

**Reproduction:** an original 1,736,705-line document was extended to 1,737,705 lines. Scrollbar range/page data stayed unchanged. `SB_BOTTOM` selected original line 1,736,704 as the top line, rather than reaching the appended region.

There is also a scaling contract problem: conversion maps a line across the full integer range while `nPage` reduces the effective last thumb position. Windows clamps scroll positions according to the page size. Conversion and page handling must use the same reachable top-row domain. See [Microsoft's SetScrollInfo contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setscrollinfo).

**Repair:** after complete indexing, derive extents from the current tree and current layout. Compute the legal top row as a function of content and viewport height; map it consistently to the legal thumb range. Update after insertion, deletion, Undo, Replace All, font changes, and resize. Track a shrinking maximum line width as well as a growing one.

**Acceptance:** after adding/removing rows at the beginning, middle, and end, thumb drag, bottom command, wheel, Page Down, and caret navigation agree. Last content is reachable without excessive blank space. Undo restores both content and ranges.

### W07 — Ordinary horizontal scrolling depends on what is visible

**Source:** `app.cpp:3103` (`MeasureWidestEditorLine`), `:3183` (`MeasureVisibleEditorLineWidth`), `:3275` (`SetEditorScrollBarVisibility`), and custom scrollbar metrics.

Above 256 Ki UTF-16 units, the width calculation switches from the whole document to visible rows. That visible width drives whether a horizontal scrollbar is needed. Hiding it also resets horizontal position.

**Reproduction:** a 360,002-unit document had a 10,000-character first row followed by short rows. After `EM_LINESCROLL` moved to row 29,999 and the window layout refreshed, the light editor lost its horizontal-scroll style and the dark horizontal control lost its visible style. The long row remained in the document. This verifies a control-state fault; screenshots were not used.

This is directly relevant to the reported large-paste scrollbar trouble: pasting can cross the 256 Ki threshold without changing editor engines.

**Repair:** maintain a stable document-level width estimate/index invalidated by edits and font changes. Visible measurement may refine geometry, but cannot decide that offscreen wide text does not exist. Bound expensive measurement and preserve the horizontal anchor across layout changes. Check tabs and variable-width fonts rather than treating character count as exact width.

**Acceptance:** paste mixed-width rows, scroll the widest row offscreen and back, resize repeatedly, and switch light/dark appearance. The bar remains available whenever the document requires it; its position does not unexpectedly reset. Delete the wide row and verify that the range shrinks correctly.

### W08 — Large-file line-ending metadata does not describe the source

**Source:** `src/document.cpp:75` (`OpenReadOnlyLargeFile`), large-mode opening in `app.cpp:5260`, and streaming output in `large_file_view.cpp:2254`. CR-only offset accounting is in `src/large_file_index.cpp`.

`OpenReadOnlyLargeFile` initializes both line-ending fields to CRLF. The scanner has actual CRLF/LF/CR counters, but the save path receives the facade's CRLF setting.

**Reproduction:** a 50,331,648-byte LF-only source, with one inserted ASCII character, saved as 58,720,257 bytes. Its prefix changed from `abcde\n` to `Xabcde\r\n`.

The CR-only scanner probe also returned byte/character offsets `(2,3)` and `(4,5)` for `a\rb\rc`; expected offsets were `(2,2)` and `(4,4)`. Publishing a delayed CR boundary after counting the next character creates the discrepancy.

**Repair:** carry detected source encoding and line-ending analysis into the authoritative document metadata. Preserve the existing mixed-line-ending policy: dominant-style output is already the documented ordinary-editor behaviour. Correct pending-CR character accounting. Do not add an encoding or line-ending picker.

**Acceptance:** both engines preserve UTF-8/BOM/UTF-16 LE and the intended CRLF/LF/CR policy across Open, edit, Save, Save As, and reopen. Test CRLF split across every relevant chunk boundary and files ending with an isolated CR.

### W09 — Worker completion changes state that belongs to the UI

**Source:** `large_file_view.cpp:315` through the save/rebase worker, `CloseMapping`, `OpenMapping`, `InvalidateViewportCache`, selection fields, and document generation checks.

The save worker reads top-line/selection state, closes and reopens mappings, invalidates viewport caches, resets the live document, and starts indexing. Paint/hit-test/UI paths also access some of this state. A document mutex does not protect every viewport/cache access. The stale-generation check and later reset occur in separate critical sections, allowing a later edit to race the reset.

Snapshots also retain a reader tied to the mutable view mapping. Cloning tree nodes does not establish an immutable backing-source lifetime. These are code-path risks; a nondeterministic UI race was not reproduced in this review.

**Repair:** workers own immutable snapshots and backing-source handles, and return completion data to the UI thread. UI-owned selection, viewport caches, live editor attachment, path/title, and generation adoption change only on the UI thread. Recheck the exact revision at adoption. Distinguish the revision written to disk from subsequent edits still dirty in memory. Publish completion once and acknowledge it once.

**Acceptance:** keep typing, selecting, scrolling, resizing, and switching appearance during Save/Find/Replace All. A stale result cannot reset newer content, clear its dirty state, apply a stale selection, or free a source still used by a snapshot. Add deterministic barriers to tests around generation validation and adoption.

### W10 — Loading needs to be all-or-nothing

**Source:** `src/document.cpp:10` (`Load`), `app.cpp:5233` (editor adoption), `:6111` (`SetEditorText`), and editor recreation.

Decoded text can contain embedded NUL, while `SetWindowTextW` consumes a terminated string. Its result is not used to establish that the editor accepted the complete document. File metadata may already have changed by then.

**Reproduction:** ordinary source bytes `[97,0,98]` displayed `a` and saved bytes `[97]`. This is silent truncation, even though the decoder accepted the source.

**Repair:** validate control compatibility before adoption. Either safely represent a supported text character or reject unsupported NUL content with a clear error; never silently shorten it. Stage decoded content and metadata, confirm complete control adoption, then commit the new document. Retain the previous document on allocation/control failures. Handle edit-control capacity/allocation notifications explicitly.

**Acceptance:** embedded NUL, malformed Unicode, invalid ANSI conversions, inaccessible paths, and simulated allocation/control failures cannot silently replace a good open document with partial content. Failed Open leaves its previous path, text, dirty state, and selection intact.

### W11 — Undo budgeting does not bound retained memory or preserve Replace All semantics

**Source:** `large_document.cpp:834` / `:840` (budget/trim), `Replace`, `:1063` (`ReplaceAll`), and insertion-buffer ownership.

Replace All records the starting undo-vector index, then calls `Replace` repeatedly. Each replacement can trim earlier records, invalidating the grouping assumption.

**Fault-injected reproduction:** with an eight-byte undo budget, `aaaa` became `bbbb`; Undo remained available, but one Undo produced `abbb`. A logical Replace All became partially undoable without a clear transaction policy.

Another probe inserted and deleted 1,048,576 units with a tiny undo budget. The document had zero characters and zero charged undo bytes but retained **2,097,152 inserted-buffer bytes**. Append-only buffers are not reclaimed when history is evicted. Original-piece undo descriptors also need a real metadata charge, rather than treating retained source references as free.

**Repair:** prepare a compound undo record transactionally and make the budget decision for the entire operation. Preserve a complete Undo, or apply an explicit consistent over-budget policy; do not advertise a partial undo as the original action. Account for nodes, descriptors, buffer capacity, history, and snapshots. Reclaim unreachable inserted chunks safely, with shared ownership if snapshots/history still reference them.

**Acceptance:** undo-budget boundaries, dense Replace All, repeated large paste/delete cycles, and saved-state restoration behave predictably. Retained memory stabilizes once both live text and undo history stop retaining the inserted payload. No UI keystroke triggers an unbounded full-history copy.

### W12 — Spell checking needs the same coordinates as the visible editor

**Source:** `app.cpp:3006` (`GetVisibleEditorText`), `:4198` / `:4219` (checking/underlines), `:4448` (context hit testing); `large_file_view.cpp` viewport/spelling extraction; `spell_check.cpp` result handling.

The native context menu extracts the low word of `EM_CHARFROMPOS` as an absolute document position. The standard edit control returns only the low 16 bits of that index, so this is insufficient beyond 65,535 units. See [Microsoft's EM_CHARFROMPOS contract](https://learn.microsoft.com/en-us/windows/win32/controls/em-charfrompos).

Visible spelling text is gathered from line prefixes rather than the horizontally visible segment. Very long lines and far-right viewports therefore cannot be checked reliably. Some line-buffer/coordinate APIs also have 16-bit limits; underline placement must not assume they return unrestricted positions.

Other contract issues: the menu says Ignore Once but calls the checker word-ignore operation used for session suppression; Add/Ignore discard backend result codes; a backend check failure is indistinguishable from a clean text result. These were found in code, not visually reproduced.

**Repair:** map the visible scalar-safe text window to full document offsets. Associate annotations with document revision and layout revision, invalidate immediately after edits, and apply suggestions only after verifying the target text still matches. Derive native mouse positions using a full line start and a bounded measured segment instead of a truncated absolute index. Make the existing Ignore Once label and its actual behaviour agree. Preserve graceful operation when `en-GB` proofing is unavailable, and report actual action failures.

Retain the OS spelling backend and visible-region work strategy. A numeric CSV should not cause giant prefix extraction or a whole-file spelling pass.

**Acceptance:** `teh colour centre recieve` is checked in the intended British-English configuration. Place misspellings after position 65,535 and far along a long line; scroll, wrap, edit preceding text, right-click, and apply a suggestion. The correct occurrence changes and Undo restores it. Cover repeated Ignore Once occurrences, missing dictionary, backend failure, and both themes. Avoid permanently adding test words to the user's real dictionary.

### W13 — Background failures and menu state need to reflect the real operation

**Source:** `app.cpp:3718` (`UpdateMenuState`), `:7003` (`WM_APP + 101`), and `large_file_view.cpp` operation result fields.

A save to a nonexistent directory correctly produced a failed operation and the error “The temporary large-document save file could not be created.” However, the main completion handler only handles successful saves; it does not present the recorded failure. The worker knows something the visible application does not reliably communicate.

Large-mode menu enabling also uses broad `editable`/`modified` flags rather than actual selection, clipboard availability, undo availability, full-index readiness, and active-operation state. Some commands can be offered when their worker rejects them. Search completion similarly needs clear no-match/cancel/failure handling.

**Repair:** return a typed operation result to the UI and consume it exactly once. Show a specific failure with the actual destination and preserve the dirty state. Drive menus, shortcuts, and context commands from one capability/state calculation. Derive readiness from the operation's real prerequisites, including W01 completeness.

**Acceptance:** unavailable commands are consistently disabled or return a clear existing-style error. Failure does not look like completion. Save As updates title/path only at the appropriate success point. Repeated notifications do not repeat dialogs or reset state.

### W14 — Existing wrap support needs a consistent user contract

**Source:** `app.cpp:3718` (large-mode menu), normal editor recreation, and `large_file_view.cpp:1015` (wrapped viewport building/scrolling).

The large view implements a wrap state and automation tests can enable it, but the real large-file menu disables Word Wrap and always shows it unchecked. The README describes large-document wrap support. That is an implementation/menu/documentation mismatch.

The underlying layout also advances to the next logical line after processing a capped prefix. Vertical scrolling mixes logical-line and visual-row quantities. A single enormous wrapped line can require many screen rows while its logical line count remains one. Native wrap recreation preserves text/selection but creates a new control and loses its native undo buffer.

**Repair:** finish the already-implemented wrap path before claiming it works. Represent viewport position with a logical anchor plus continuation offset; calculate wrap segments lazily, with scalar-safe boundaries and bounded shaping. Use this layout for wheel/page/thumb navigation, caret movement, and spelling. Preserve document edits and state during normal editor recreation; make undo continuity a tested requirement or explicitly document any deliberate native limitation.

**Acceptance:** toggling wrap preserves content and dirty state; every continuation of a very long row is reachable. Last-row scroll, resize, font change, selection, and caret visibility agree. Test the real menu/shortcut state, not only `setWordWrap` automation.

### W15 — Search and cancellation still have unbounded costs

**Source:** `large_document.cpp` Find/Replace All/StreamText and `:240` (`Clone`); `large_file_view.cpp` operation start/stop; native Find/Replace code in `app.cpp`.

The shared search uses a linear pattern matcher, which is a good improvement over repeated full-string comparisons. However, source streaming starts from the beginning even for searches beginning far into the document, and backward work is not tightly bounded by the requested range. Original-piece decoding remains a longest-line allocation. Cloning copies inserted buffers/tree/history on the foreground path. Dense Replace All accumulates every match and checks cancellation during scanning but not through every replacement in the application loop. Joining such work can block a transition or close.

The native and large engines also use different case-folding mechanisms (`CompareStringOrdinal` versus `towlower`), which can produce size-dependent Unicode search results.

**Repair:** stream only the requested search interval with the required overlap and an explicit wrapping policy. Share immutable insertion/source buffers between snapshots. Bound match metadata and apply compound edits with frequent cancellation checks. Make cancellation transactional and promptly observable. Establish consistent case/whole-word behaviour from tests before optimizing implementation.

**Acceptance:** forward/backward Find, F3, whole word, case-insensitive Unicode, no match, and cancellation behave consistently across the threshold. Dense replacements do not require an unchecked per-match memory expansion or freeze close indefinitely.

### W16 — Status and editor transitions need one semantic definition

**Source:** `app.cpp:3915` / `:3940` (status counts), normal overtype/deletion, native editor recreation, and large-mode input handling.

Ordinary status counting uses the shared character-count helper for some paths but switches to UTF-16 control length for large edited content. CRLF and supplementary characters therefore risk making the count change meaning after an edit. Native and mapped input paths independently implement overtype, deletion, selection, and line-ending handling.

**Repair:** define document offsets as UTF-16 units and define displayed character counts separately, consistently with existing small-file behaviour. Maintain the displayed count incrementally if a whole-document recount is too expensive. Centralize small shared input rules for surrogate pairs, CRLF, and overtype without trying to abstract the entire native UI.

**Acceptance:** identical text produces identical metadata before/after paste, edit, Undo, Save, and reopen, including CRLF, emoji, tabs, blank trailing rows, and both sides of the large-mode threshold. Backspace/Delete/overtype cannot strand a surrogate half or accidentally change a line break.

### W17 — Code structure and release evidence need maintenance

`app.cpp` is approximately 7,600 lines and mixes document transitions, rendering, custom chrome, input, dialogs, printing, spelling, automation hooks, and scrolling. `large_file_view.cpp` mixes mapping/indexing, model operations, layout, input, painting, and worker lifecycles. This concentration makes the state errors above harder to isolate.

`ClassicNotepadCore` also includes the Windows view/control implementation. Separate shared text/model code from Windows UI targets so the dependency boundary is meaningful. The portable-core smoke target is useful, but a Windows compile does not qualify 32-bit `wchar_t` behaviour or a real macOS/Linux UI.

Documentation is inconsistent: README says version 1.2.2 while `VERSION` is 1.3.0; historical review notes mark earlier issues resolved and reference pre-migration paths; claims about bounded pieces, edited-tree authority, wrap access, and save guarantees are stronger than current behaviour. The build script increments the version on a Release build, which makes ordinary release-mode testing mutate source metadata. No tracked CI workflow was found.

**Repair:** extract coherent modules after behaviour is protected. Consolidate save coordination first, then model/source ownership, scroll/layout calculations, spelling annotation handling, and chrome. Prefer ordinary RAII wrappers for handles/mappings/DC resources to duplicated cleanup branches. Reduce magic-limit ambiguity: name UTF-16-unit, byte, and pixel limits accurately. Keep a single current Windows contract and release checklist, with historical plans clearly labelled.

## 5. Implementation plan: small, reviewable changes

This is an ordered backlog, not a feature roadmap. Each change should land with evidence for the behaviour it repairs. Avoid combining a broad UI refactor with an integrity fix.

### Phase A — Stop data loss

| Change | Work | Exit gate |
| --- | --- | --- |
| A1. Full-document save precondition | Introduce an explicit complete/validated source invariant; gate both save entry points and whole-document operations. | W01 fixture saves the entire expected file or is safely refused/queued until complete. |
| A2. Save/transition coordinator | Queue close/New/Open until save completion; read authoritative dirty state; handle busy/failure/cancellation. | Selecting Save cannot discard unsaved work. |
| A3. Transactional tree edits | Protect ownership on every split/read/summary failure; commit only a complete edit. | All fault-injected read failures leave the previous state intact. |
| A4. Safe completion ownership | UI thread adopts completion; snapshots own immutable source lifetime; generation check and adoption are coherent. | Edits during Save are preserved and remain dirty when appropriate. |
| A5. Disk commit failure contract | Unify ordinary/large save replacement and diagnostics; preserve recoverable output after destructive replacement states. | Write/flush/replace/reopen failures preserve a usable document and recoverable content. |
| A6. Unicode/load integrity | Fix scalar/chunk boundaries and reject silent NUL/control truncation; stage metadata until adoption succeeds. | Encoding and control failures cannot silently change text or overwrite expected output. |

Do not defer these changes behind visual polishing. They are the release-blocking group.

### Phase B — Make large documents work with enormous lines

1. Bound original pieces by bytes/UTF-16 units independently of newline placement, with valid scalar cuts and correct CRLF summaries.
2. Correct ASCII/multibyte checkpoints and CR-only scanner offsets. Make repeated nearby reads reuse the index.
3. Replace whole-piece decoding with bounded streaming and cancellable source access.
4. Make paint and hit testing decode the horizontally visible text range. Store a shared document-to-layout mapping for caret, selection, and spelling.
5. Repair edited-tree vertical extents, page-aware thumb scaling, and width shrink after deletion/Undo.
6. Fix compound Undo/budgeting and release unreachable insertion storage. Share immutable snapshot buffers.
7. Carry source encoding/line-ending metadata through Save and rebase.

**Exit gate:** one-row and four-row fixtures are fully accessible and editable; expected hashes match after Save; per-viewport/keystroke allocations and source-read work stay bounded as line length increases.

### Phase C — Make ordinary paste and scrolling dependable

1. Maintain stable whole-document horizontal extent without rescanning everything on every input event.
2. Preserve horizontal/vertical anchors through paste, bar visibility changes, resize, font changes, and appearance changes.
3. Check clipboard acquisition failures, payload limits, native control allocation limits, newline normalization, and Undo grouping.
4. Establish explicit measured limits for the native control with long rows and large Unicode pastes. Do not invent a size-only promotion mechanism that needs another whole-document copy.
5. If measurements show a native control hard limit, decide the smallest internal routing change that preserves content, selection, undo policy, metadata, and current commands. That decision follows evidence; it is not a prerequisite rewrite.

**Exit gate:** real clipboard smoke tests and deterministic production-handler tests both pass; no silent truncation; wide-row scrollbars remain stable in light/dark modes.

### Phase D — Finish the existing editing and spelling experience

1. Use revision-tagged visible text windows for spelling, with full document coordinates.
2. Correct right-click hit testing beyond 65,535 units and at horizontal offsets.
3. Make suggestion application, Ignore Once, and dictionary actions match their labels and handle backend failures.
4. Complete accessible menu state for existing wrap support after its layout passes the large-line gate.
5. Align Find/Replace/Go To, status metadata, overtype, CRLF deletion, and Unicode navigation across engines.
6. Qualify existing print/page-setup output through a real Windows printer path. The deterministic sink does not prove real pagination or printer integration. Keep mapped-file printing's documented current limitation explicit; adding a new large-document print engine is outside this review's implementation scope.

**Exit gate:** every shipped command either works correctly in its supported mode or has a truthful, consistent documented availability rule.

### Phase E — Clean the code and qualify Windows release behaviour

Extract modules around ownership and testable calculations, rather than creating a generic cross-platform UI framework:

| Boundary | Responsibility |
| --- | --- |
| Document session/coordinator | Path, encoding, line endings, dirty/saved revisions, transitions, operation results. |
| Shared text/model core | Transactional edit tree, summaries, bounded decode/streaming, search, history. |
| Windows source/save services | Handle ownership, mappings, temporary output, flush/replace, error recovery. |
| Windows viewport/layout | Visible ranges, measured segments, caret/hit testing, wrap continuations, scroll conversion. |
| Windows spelling annotations | Backend adapter, revision-tagged ranges, viewport mapping, context actions. |
| Existing window/chrome/dialog layer | Native menus/dialogs, appearance, focus, printing and command dispatch. |

Move these incrementally while regression tests remain green. Do not refactor the whole repository before fixing the failures.

Automate a reproducible Windows Release gate; register ordinary semantic tests in the normal verification workflow and schedule heavier stress/failure runs separately. Make Release build/testing reproducible without consuming a version number. Update the root README and historical status claims when the implementation is verified.

**Exit gate:** supported Windows versions pass the matrix below on a clean Release build, and the visible application has been checked directly. Resume macOS/Linux work only after this gate; carry the fixed shared-core contracts and fixtures into their later qualification.

## 6. Acceptance matrix

The important dimensions are orthogonal. A many-row 700 MiB test cannot stand in for a four-row 256 MiB test, and a semantic automation command cannot stand in for a native menu/clipboard interaction.

| Area | Required cases | Required evidence |
| --- | --- | --- |
| Size boundaries | Empty, small, near 256 Ki units, 47/48/49 MiB, 256 MiB, 700 MiB; source bytes and in-memory units distinguished. | Correct engine/readiness; no unexpected data loss or behaviour change. |
| Line shape | Many 128-byte rows; one huge row; four enormous CSV rows; mixed short/huge rows; no final newline. | Start/middle/end reachable through all existing navigation paths. |
| Paste | Many rows and one row; CRLF/LF/CR/mixed; ASCII/BMP/emoji; empty clipboard; lock failure; oversized selection. | Exact contents, explicit failures, one coherent undo action, stable scrollbars. |
| Edited extents | Add/delete rows and widest row at start/middle/end; Undo; Replace All. | Range/page/position agree with the edited document. |
| Unicode boundaries | Emoji before/on/after 64 Ki, piece, mapping, selection, and stream boundaries; malformed input. | Correct offsets and byte output; invalid input fails without mutation. |
| Save lifecycle | During indexing; during later edits; close/New/Open after Save choice; busy, cancelled, failed save. | Whole-file expected hash; correct path/title/dirty state; no discarded newer edits. |
| Save failures | Create/write/flush/replace/reopen failures; read-only destination; sharing violation; insufficient space; changed source. | Prior document retained; original or recoverable complete output survives; error shown once. |
| File metadata | UTF-8 BOM/no BOM, UTF-16 LE BOM, existing ordinary ANSI support; CRLF/LF/CR/mixed. | Encoding and documented line-ending policy preserved. |
| Memory/history | Repeated large paste/delete; dense replacements; undo budget boundaries; concurrent snapshots. | Stable retained memory; complete operation semantics; predictable cancellation. |
| Search | Forward/backward, wrap policy, whole word, Unicode case behaviour, no match, cancellation. | Same contract across engines; no stale result adoption. |
| Spelling | British-English examples; missing dictionary; >65 Ki offset; far-right viewport; edit before error; suggestions/Ignore/Add. | Correct visible word/range and safe Undo; no dictionary pollution by tests. |
| Wrap/font | Toggle wrap; huge-row continuations; tabs/proportional fonts; font size changes; resize. | Text/state preserved and every continuation reachable. |
| Input/focus | Accelerators, menu/context dispatch, Home/End/Page keys, Shift selection, drag/autoscroll, overtype, IME/Unicode input. | Existing editing actions work through the visible application. |
| Appearance | System/light/dark, high contrast, font settings, 100/150/200% scaling, mixed-DPI monitor move. | Readable text/caret/underlines; correct hit targets and no scroll/layout jumps. |
| Accessibility | Keyboard-only menus/context actions; assistive technology against the custom editor. | Existing text/input/selection state is usable; explicitly record unsupported behaviour. |
| Printing | Empty/multipage text, tabs/long rows, margins/font, printer cancellation/failure. | Real output matches supported print behaviour; sink alone is insufficient. |
| Resource lifetime | Repeated open/save/wrap/font/theme cycles, operation cancellation, close during work. | No accumulating mappings, threads, GDI handles, or stale callbacks. |

### Performance measurements

Record input size/shape, hardware, build, median and tail latency, peak working set/private bytes, source bytes requested, and relevant handle counts. Use repeated runs where timing matters. Separate:

- Time to actual visible paint from time to a non-null model or published first-index counter.
- Background complete-index time from UI stalls caused by viewport/status queries.
- Worker time from foreground snapshot allocation/clone time.
- Allocated/retained inserted storage from the charged undo-budget counter.

Initial targets should be set on a declared reference machine: ordinary typing/scrolling should not visibly pause; large-document viewport interaction should avoid repeated multi-second work; cancel/close should respond promptly. First prove algorithmic bounds and record a baseline, then establish realistic numerical release limits. A misleading “first usable” counter is not acceptance evidence.

## 7. Test and review workflow

For each repair:

1. Add the smallest meaningful regression that fails on the reviewed baseline.
2. Make the implementation change with an explicit ownership/state invariant.
3. Run focused core/Windows tests, then the existing suite once the affected behaviour passes.
4. For UI changes, deliver native messages or interact with the visible command path. Do not test only a parallel automation implementation.
5. For Save, generate expected bytes independently and verify the entire output, not just a beginning/end marker.
6. Run the relevant long-line/memory stress case and retain compact logs.
7. Update the current behaviour contract when a limitation is intentionally retained.

Keep generated fixtures out of source control. Introduce deterministic source/save failure injection at narrow internal seams. Keep user clipboard, dictionaries, printers, and files isolated or explicitly protected during tests.

### Useful existing assets to retain

- `tests/text_conversion_tests.cpp`: shared round-trip, metadata, model and text utility coverage.
- `tests/automation/classic_notepad_driver.py`: repeatable application interaction.
- `tests/automation/test_large_file_mode.py`: real large-mode message dispatch and edited-tree checks, extended with long-row and initial-index cases.
- `tests/automation/large_file_performance.py`: retain its many-short-line benchmark and add orthogonal shapes.
- Existing capability handling and appearance overrides: useful for deterministic tests, with visible validation added.

The current Windows `save`/`saveAs` automation helpers take a synchronous route, while menu Save uses `StartSaveAsync`. This is why ordinary save tests can pass while the actual user lifecycle is unsafe. Route new tests through the production asynchronous entry point and completion/transition protocol.

## 8. Windows completion gate and deferred platform work

Windows is ready for the next stage when:

- W01–W03 have deterministic regression coverage and cannot lose text on failure.
- Successful Save writes the entire intended revision with the correct encoding/line endings.
- Save-before-close/New/Open works through the visible prompt flow.
- Huge lines are fully navigable and editable with bounded decoding/layout work.
- Both ordinary and mapped scrollbars follow current content after paste, edit, Undo, resize, and appearance changes.
- Spell checking applies to the intended visible text and correct document offsets.
- Existing commands, supported limitations, menus, and documentation agree.
- Real Windows UI, clipboard, printing, DPI, input, and lifetime qualification has been recorded.
- Release build and verification can be repeated without accidental version changes.

Linux/macOS stay in maintenance scope until then. Shared fixes should continue to compile through the portable smoke target, but do not spend this phase expanding parity or duplicating a broken Windows contract.

One surveyed issue to retain for later platform qualification: `src/platform/portable/file_io_std.cpp` retries failed rename by removing the existing destination, then attempts rename again and may remove the temporary output on failure. That is a destructive fallback and needs repair before claiming portable atomic-save safety. It is not the shipping Windows ordinary-save implementation.

## 9. Suggested first implementation sequence

The first six reviewable changes should be:

1. **Block incomplete-index save and add the full-hash regression.** This is the smallest immediate protection against demonstrated truncated output.
2. **Make Save-before-transition wait for acknowledged success**, including authoritative dirty state and failed-save reporting.
3. **Make source-backed edits transactional**, with failures injected at every read/split boundary.
4. **Repair Unicode streaming/read boundaries and source metadata**, protecting valid text and LF/CR preservation.
5. **Bound source pieces/checkpoints/decoding independently of line length**, then make the viewport read the visible horizontal segment.
6. **Repair edited-document scrollbar extents and ordinary wide-row stability**, with light/dark message-level and visible regressions.

Follow with history/memory ownership, worker adoption, spelling coordinates, wrap consistency, and extraction of the now-tested responsibilities. The purpose of each change is a more dependable existing editor, with the same small product identity.

## Appendix A. Evidence bundle

The accompanying review evidence archive contains source probes, their compact results, the tracked-file inventory with hashes, and reproduction notes. It excludes generated binaries, large fixtures, clipboard payloads, and user files.

The probe sources are review tools, not production changes or a replacement for permanent regression tests. Their recorded outcomes belong to the reviewed commit and machine. The four-row fixture is approximately 48 MiB, not the user's original hundreds-of-megabytes geographic CSV. The initial-index save fixture is approximately 192 MiB. These sizes were sufficient to reproduce the underlying failures.

## Appendix B. File-by-file repository coverage

Coverage labels distinguish the deep Windows/shared review from structural surveys of deferred platforms. Listing a file does not certify every runtime branch. Text files were included in the full repository scan; the deepest behavioural inspection concentrated on the paths discussed above.

| Baseline file | Text lines | Coverage | Review focus |
| --- | ---: | --- | --- |
| `.gitignore` | 16 | Repository/config review | Generated build/fixture exclusions. |
| `.vscode/extensions.json` | 7 | Repository/config review | Recommended tooling. |
| `.vscode/launch.json` | 30 | Repository/config review | Debug entry points and task dependencies. |
| `.vscode/tasks.json` | 112 | Repository/config review | Build/test/package/clean task wiring. |
| `CMakeLists.txt` | 380 | Repository/config review | Target/source boundaries, dependencies, warning flags, generated version, and test registration. |
| `LICENSE` | 5 | Repository/config review | MIT licence; no application behaviour. |
| `README.md` | 480 | Documentation review | Product contract, current claims, large-mode limitations, build instructions, stale version. |
| `VERSION` | 1 | Repository/config review | Baseline version 1.3.0; compared with generated resources and documentation. |
| `assets/icons/classic_notepad_large_transparent.ico` | binary | Binary inventory | Asset path/size/hash and build references; appearance not certified. |
| `assets/icons/classic_notepad_large_transparent.png` | binary | Binary inventory | Asset path/size/hash and build references; appearance not certified. |
| `clasicNotepadCrossPlatform/README.md` | 28 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `clasicNotepadCrossPlatform/docs/01_TECHNOLOGY_RESEARCH_AND_DECISION.md` | 110 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `clasicNotepadCrossPlatform/docs/02_CURRENT_CLASSIC_NOTEPAD_BEHAVIOR_SPEC.md` | 100 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `clasicNotepadCrossPlatform/docs/03_PLATFORM_API_MAPPING.md` | 58 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `clasicNotepadCrossPlatform/docs/04_IMPLEMENTATION_PLAN_MAC_FIRST.md` | 159 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `clasicNotepadCrossPlatform/docs/05_SPELLING_AND_DARK_MODE_PLAN.md` | 365 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `clasicNotepadCrossPlatform/docs/05_WINDOWS_FIRST_CROSS_PLATFORM_MIGRATION_PLAN.md` | 156 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `clasicNotepadCrossPlatform/docs/06_LINUX_FEATURE_PARITY_AND_AUTOMATION_PLAN.md` | 682 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `docs/CLASSIC_NOTEPAD_BUILD_PLAN.md` | 513 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `docs/LINUX_BUILD_ENVIRONMENT.md` | 127 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `docs/LINUX_SPELLING_FIX_PLAN.md` | 208 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `docs/MACOS_BUILD_PLAN.md` | 294 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `docs/MACOS_LARGE_FILE_TEST_HANDOFF.md` | 276 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `docs/PERFORMANCE_SECURITY_REVIEW.md` | 88 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `docs/README.md` | 13 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `docs/SPELL_CHECK_RESEARCH.md` | 271 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `docs/WIN11_NOTEPAD_DARK_PARITY_PLAN.md` | 343 | Documentation review | Historical/current scope, behavioural claims, acceptance criteria and source consistency. |
| `scripts/build-macos.sh` | 153 | Build/tooling review | AppKit build/configuration/architecture routing; deferred runtime. |
| `scripts/build-ubuntu.ps1` | 96 | Build/tooling review | WSL/Linux dependency/build routing; deferred runtime. |
| `scripts/build.ps1` | 323 | Build/tooling review | Compiler/CMake discovery, build reset and Release version increment. |
| `scripts/clean.ps1` | 12 | Build/tooling review | Generated build cleanup target; not executed. |
| `scripts/generate-macos-icon.py` | 39 | Build/tooling review | PNG-to-bundle-icon tooling; deferred artwork/runtime. |
| `scripts/package-macos.sh` | 121 | Build/tooling review | Universal packaging/signing/test routing; deferred runtime. |
| `scripts/test-large-file.ps1` | 13 | Build/tooling review | Release performance-runner wiring; not executed. |
| `scripts/test-macos.sh` | 49 | Build/tooling review | macOS build/CTest wiring; deferred runtime. |
| `scripts/test.ps1` | 78 | Build/tooling review | Build/CTest wiring and configuration. |
| `src/ansi_encoding.h` | 13 | Interface review | Shared ANSI conversion contract. |
| `src/app_version.h.in` | 13 | Interface review | Generated version definitions and resource integration. |
| `src/appearance.cpp` | 103 | Deep shared-core review | Shared appearance parsing and capability state. |
| `src/appearance.h` | 22 | Interface review | Shared API/representation/ownership contract and implementation references. |
| `src/document.cpp` | 146 | Deep shared-core review | Load/save adoption, metadata, encoding/line endings, dirty state, large-mode facade. |
| `src/document.h` | 40 | Interface review | Shared API/representation/ownership contract and implementation references. |
| `src/encoding.cpp` | 344 | Deep shared-core review | Detection/conversion, UTF-16 representation, validation, chunk encoding. |
| `src/encoding.h` | 42 | Interface review | Shared API/representation/ownership contract and implementation references. |
| `src/file_io.h` | 15 | Interface review | Shared file read/atomic-write contract. |
| `src/large_document.cpp` | 1196 | Deep shared-core review | Tree ownership, summaries, checkpoints, edits, Unicode, undo, snapshots, search/streaming. |
| `src/large_document.h` | 211 | Interface review | Shared API/representation/ownership contract and implementation references. |
| `src/large_file_index.cpp` | 243 | Deep shared-core review | Incremental scan, UTF-8/UTF-16 validation, CR/LF carry state and offsets. |
| `src/large_file_index.h` | 85 | Interface review | Shared API/representation/ownership contract and implementation references. |
| `src/line_endings.cpp` | 142 | Deep shared-core review | Normalization, detection/dominant style, output conversion. |
| `src/line_endings.h` | 28 | Interface review | Shared API/representation/ownership contract and implementation references. |
| `src/platform/linux/gtk_actions.cpp` | 379 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_actions.h` | 12 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_app.cpp` | 3203 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_app.h` | 256 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_automation.cpp` | 1178 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_automation.h` | 20 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_dialogs.cpp` | 803 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_dialogs.h` | 60 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_large_file_view.cpp` | 1049 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_large_file_view.h` | 181 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_main.cpp` | 45 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_spelling.cpp` | 531 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/linux/gtk_spelling.h` | 51 | Deferred platform survey | GTK actions/lifecycle, interfaces, shared-core use and platform boundaries; no Linux runtime qualification. |
| `src/platform/macos/Info.plist.in` | 34 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/macos/mac_app.h` | 15 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/macos/mac_app.mm` | 2111 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/macos/mac_appearance.h` | 30 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/macos/mac_appearance.mm` | 94 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/macos/mac_automation.h` | 138 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/macos/mac_automation.mm` | 1024 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/macos/mac_large_file_view.h` | 60 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/macos/mac_large_file_view.mm` | 671 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/macos/mac_main.mm` | 7 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/macos/mac_spelling.h` | 23 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/macos/mac_spelling.mm` | 91 | Deferred platform survey | AppKit actions/lifecycle, interfaces, shared-core use and platform boundaries; no macOS runtime qualification. |
| `src/platform/portable/ansi_encoding_portable.cpp` | 68 | Shared boundary review | Portable fallback and conversion differences; smoke-build coverage only. |
| `src/platform/portable/file_io_std.cpp` | 132 | Shared boundary review | Portable read/write and destructive rename retry; deferred platform risk. |
| `src/platform/windows/ansi_encoding_win32.cpp` | 106 | Deep Windows review | Windows code-page conversion and lossy conversion rejection. |
| `src/platform/windows/app.cpp` | 7633 | Deep Windows review | Production file lifecycle, paste, scrollbars, menus, spelling, status, dialogs, print and input paths. |
| `src/platform/windows/app.h` | 437 | Deep Windows review | State ownership, UI/automation boundaries, operation interfaces. |
| `src/platform/windows/automation.cpp` | 1407 | Deep Windows review | JSON command dispatch, synchronous vs asynchronous production-path differences. |
| `src/platform/windows/automation.h` | 13 | Deep Windows review | Automation entry point and application contract. |
| `src/platform/windows/file_io_win32.cpp` | 206 | Deep Windows review | Ordinary source reads, temporary output, flush and MoveFileEx replacement. |
| `src/platform/windows/large_file_view.cpp` | 2784 | Deep Windows review | Mapping/index publication, viewport/input/scrolling, asynchronous operations and save/rebase. |
| `src/platform/windows/large_file_view.h` | 310 | Deep Windows review | View/model/worker state, limits, ownership and diagnostic interfaces. |
| `src/platform/windows/main.cpp` | 48 | Deep Windows review | Command-line and visible/hidden automation startup. |
| `src/platform/windows/resource.h` | 44 | Deep Windows review | Command/dialog identifiers. |
| `src/platform/windows/resources.rc` | 130 | Deep Windows review | Menus, accelerators, dialogs, icon and version resources. |
| `src/platform/windows/spell_check.cpp` | 157 | Deep Windows review | COM backend initialization, checks/suggestions, Ignore/Add error handling. |
| `src/platform/windows/spell_check.h` | 34 | Deep Windows review | Backend API and range representation. |
| `src/platform/windows/win32_platform.h` | 9 | Deep Windows review | Windows compatibility/includes boundary. |
| `src/spell_text_utils.cpp` | 100 | Deep shared-core review | Spelling ranges and text utilities. |
| `src/spell_text_utils.h` | 18 | Interface review | Shared API/representation/ownership contract and implementation references. |
| `src/spelling.cpp` | 21 | Deep shared-core review | Shared spelling capability states. |
| `src/spelling.h` | 21 | Interface review | Shared API/representation/ownership contract and implementation references. |
| `src/text_metadata.cpp` | 82 | Deep shared-core review | Displayed character/line/column definitions. |
| `src/text_metadata.h` | 17 | Interface review | Shared API/representation/ownership contract and implementation references. |
| `tests/automation/automation_context.py` | 24 | Test coverage review | Binary/platform and fixture-location configuration. |
| `tests/automation/classic_notepad_driver.py` | 165 | Test coverage review | Process lifecycle, JSON requests/timeouts, hidden/visible operation. |
| `tests/automation/large_file_performance.py` | 318 | Test coverage review | 700 MiB benchmark structure, short-row assumptions, hashes and telemetry; not run. |
| `tests/automation/run_automation_tests.py` | 55 | Test coverage review | Discovery/configuration and baseline suite execution. |
| `tests/automation/test_appearance_theme.py` | 113 | Test coverage review | Existing feature assertions and Windows suite coverage; platform-only branches not qualified here. |
| `tests/automation/test_edit_commands.py` | 102 | Test coverage review | Edit/clipboard/overtype coverage; internal clipboard vs native WM_PASTE distinction. |
| `tests/automation/test_encoding_line_endings.py` | 86 | Test coverage review | Existing feature assertions and Windows suite coverage; platform-only branches not qualified here. |
| `tests/automation/test_file_workflow.py` | 71 | Test coverage review | Existing feature assertions and Windows suite coverage; platform-only branches not qualified here. |
| `tests/automation/test_find_replace_go_to.py` | 71 | Test coverage review | Existing feature assertions and Windows suite coverage; platform-only branches not qualified here. |
| `tests/automation/test_format_view_status.py` | 90 | Test coverage review | Existing feature assertions and Windows suite coverage; platform-only branches not qualified here. |
| `tests/automation/test_large_file_mode.py` | 448 | Test coverage review | Windows large-file edit/search/wrap/message/save coverage; missing enormous rows/early async Save. |
| `tests/automation/test_printing.py` | 59 | Test coverage review | Deterministic print sink/margins/font coverage; real printer path unqualified. |
| `tests/automation/test_spell_capability.py` | 58 | Test coverage review | British English/capability/action tests; visible underline/context path unqualified. |
| `tests/automation/test_status_metadata.py` | 60 | Test coverage review | Existing feature assertions and Windows suite coverage; platform-only branches not qualified here. |
| `tests/linux_spelling_probe.cpp` | 142 | Deferred test survey | Optional backend/dictionary validation target; not run on Linux. |
| `tests/macos_spelling_compile.mm` | 23 | Deferred test survey | AppKit helper compile test; not run on macOS. |
| `tests/text_conversion_tests.cpp` | 739 | Deferred test survey | Core regression inventory; native and portable-smoke executable runs. |
