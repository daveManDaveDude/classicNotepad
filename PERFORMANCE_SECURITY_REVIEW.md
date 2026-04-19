# Performance & Security Code Review (Classic Notepad)

Date: 2026-04-19
Scope: `src/` and `tests/` (manual static review; no dynamic profiling in this environment).

## Executive summary

The codebase is generally careful with Win32 API usage and bounds checks, and it avoids many common unsafe C/C++ patterns (notably with fixed-size dialog buffers, explicit file-size caps, and conversion error checks). That said, there are a few notable risks:

1. **Correctness/security-hardening risk** in range arithmetic (`RangesOverlap`) due to unchecked `size_t` addition overflow.
2. **Data-integrity risk** from non-atomic save behavior (`CREATE_ALWAYS` directly on target path).
3. **Performance scaling limits** from repeated full-buffer extraction and linear scan matching in find/replace and measuring logic.

## What looks good

- File loads are bounded to 256 MB, reducing memory-exhaustion exposure for this build.
- UTF-8 decode uses strict invalid-char flags; ANSI encode rejects lossy fallback behavior.
- Open/Save dialogs use large fixed buffers and path validation flags.
- Visible-region spell checking limits scanning to a bounded character budget (`96 KiB`) and uses a debounce timer.

## Findings

### 1) Potential integer overflow in overlap arithmetic

**Location:** `src/spell_text_utils.cpp`

`RangesOverlap` computes `end = start + length` on unsigned `size_t` values with no overflow guard. In pathological cases this wraps and can produce false positives/negatives in overlap checks.

Why this matters:
- Today, many callers pass bounded editor/spell offsets, so practical exploitability is low.
- However, this function is generic utility code and should be robust against malformed or future-unbounded inputs.

**Recommendation:** switch to a subtraction-based overlap check (`a_start < b_end` rewritten to avoid unchecked addition), or saturating end computation.

---

### 2) Save path is non-atomic (integrity / crash-safety)

**Location:** `src/document.cpp`

`WriteAllBytes` opens the destination with `CREATE_ALWAYS` and writes directly to the final path. If the process/system fails mid-write, the original file can be truncated or replaced with partial content.

Why this matters:
- This is a reliability and data-integrity issue more than a direct security bug.
- It can still be security-relevant where file integrity is critical (logs, scripts, config files).

**Recommendation:** write to a temp file in the same directory, flush, then atomically replace with `MoveFileEx(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.

---

### 3) Find/replace performs repeated full-document copies and O(n*m) scanning

**Location:** `src/app.cpp`

`GetEditorText()` copies entire editor content into a new `std::wstring`; this is used in `FindNextWithFlags`, `ReplaceAllMatches`, and status/selection-dependent paths. Find logic scans position-by-position and invokes `CompareStringOrdinal` each attempt.

Impact:
- On larger documents, interactive latency increases notably.
- Multiple command paths may re-copy the same full text in one user flow.

**Recommendation:**
- Cache one immutable snapshot per command invocation and reuse it.
- Consider Boyer-Moore/Horspool for non-whole-word cases (or an API-assisted search strategy).
- Keep current word-boundary logic as post-filtering.

---

### 4) Full-line width measurement allocates per-line substrings

**Location:** `src/app.cpp`

`MeasureFullEditorLineWidth` performs `text.substr(...)` and tab-expansion per line. This is allocation-heavy and can amplify UI cost when recomputed.

Impact:
- More pronounced with long files and frequent layout recalculation.

**Recommendation:**
- Iterate with index ranges and avoid temporary substring allocations.
- Reuse a scratch buffer for tab expansion.

## Suggested priority order

1. Fix overlap arithmetic hardening (`RangesOverlap`) — low effort, high confidence.
2. Implement atomic save/replace flow.
3. Optimize find/replace snapshot usage and search loop.
4. Optimize full-line measuring allocations.

## Validation approach for follow-up patch

- Add edge-case tests for near-`size_t`-max overlap math.
- Add a save-failure simulation test (where possible) verifying original-file preservation on write failure.
- Add microbenchmarks (or timed debug traces) for find/replace on 10MB+ documents.

