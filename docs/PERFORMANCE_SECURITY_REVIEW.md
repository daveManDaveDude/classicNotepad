# Performance & Security Code Review (Classic Notepad)

Date: 2026-04-19 (updated after implementation)
Scope: `src/` and `tests/` (manual static review; no dynamic profiling in this environment).

## Executive summary

The codebase is generally careful with Win32 API usage and bounds checks, and it avoids many common unsafe C/C++ patterns (fixed-size dialog buffers, conversion error checks, and now atomic save replacement). The previously reported high-priority issues have been addressed in code:

1. ✅ **Overflow-hardening** for range arithmetic (`RangesOverlap`) now avoids unchecked `size_t` addition.
2. ✅ **Data-integrity hardening** for save now uses temp-file write + flush + atomic replace.
3. ✅ **Performance improvements** landed for find/replace snapshot reuse (replace path) and full-line width measurement allocation reduction.
4. ℹ️ **Large-file policy change**: the fixed 256 MB load cap was removed; file open is now bounded by practical process memory / `size_t` limits.

## What looks good

- File loads are no longer pinned to 256 MB; open now accepts large files until memory/`size_t` constraints.
- UTF-8 decode uses strict invalid-char flags; ANSI encode rejects lossy fallback behavior.
- Open/Save dialogs use large fixed buffers and path validation flags.
- Visible-region spell checking limits scanning to a bounded character budget (`96 KiB`) and uses a debounce timer.

## Findings

### 1) Potential integer overflow in overlap arithmetic — **RESOLVED**

**Location:** `src/spell_text_utils.cpp`

`RangesOverlap` computes `end = start + length` on unsigned `size_t` values with no overflow guard. In pathological cases this wraps and can produce false positives/negatives in overlap checks.

Why this matters:
- Today, many callers pass bounded editor/spell offsets, so practical exploitability is low.
- However, this function is generic utility code and should be robust against malformed or future-unbounded inputs.

**Implemented:** switched to subtraction-based overlap logic with explicit zero-length handling and added near-`size_t`-max tests.

---

### 2) Save path is non-atomic (integrity / crash-safety) — **RESOLVED**

**Location:** `src/document.cpp`

`WriteAllBytes` opens the destination with `CREATE_ALWAYS` and writes directly to the final path. If the process/system fails mid-write, the original file can be truncated or replaced with partial content.

Why this matters:
- This is a reliability and data-integrity issue more than a direct security bug.
- It can still be security-relevant where file integrity is critical (logs, scripts, config files).

**Implemented:** save now writes to a unique temp sibling file, flushes the handle, then replaces with `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`. Temp files are cleaned up on failure.

---

### 3) Find/replace performs repeated full-document copies and O(n*m) scanning — **PARTIALLY RESOLVED**

**Location:** `src/app.cpp`

`GetEditorText()` copies entire editor content into a new `std::wstring`; this is used in `FindNextWithFlags`, `ReplaceAllMatches`, and status/selection-dependent paths. Find logic scans position-by-position and invokes `CompareStringOrdinal` each attempt.

Impact:
- On larger documents, interactive latency increases notably.
- Multiple command paths may re-copy the same full text in one user flow.

**Implemented:** Replace-command flow now captures one snapshot and reuses it across selection-match and subsequent find when no replacement occurs, eliminating an extra full-document copy in that path.

**Remaining recommendation:** algorithmic scan is still linear with per-position compare; consider Boyer-Moore/Horspool as a future optimization.

---

### 4) Full-line width measurement allocates per-line substrings — **RESOLVED**

**Location:** `src/app.cpp`

`MeasureFullEditorLineWidth` performs `text.substr(...)` and tab-expansion per line. This is allocation-heavy and can amplify UI cost when recomputed.

Impact:
- More pronounced with long files and frequent layout recalculation.

**Implemented:** measurement now iterates by index range and reuses a scratch buffer for tab expansion, removing per-line `substr` allocations.

## Suggested priority order (remaining)

1. Optional: further optimize search algorithm for very large documents.
2. Add stress/perf benchmarks for 10MB–1GB+ files on target Windows builds.

## Validation approach for follow-up patch

- ✅ Added edge-case tests for near-`size_t`-max overlap math.
- ⏳ Save-failure simulation test remains recommended if a deterministic fault-injection hook is introduced.
- ⏳ Add microbenchmarks (or timed debug traces) for find/replace on 10MB+ documents.
