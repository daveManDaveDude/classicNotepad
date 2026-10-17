# Windows native EDIT measurement results

This is the measured envelope for the ordinary Windows native `EDIT` control in the configuration below. It is not a general capacity limit or a promise about other Windows builds, fonts, DPI settings, or viewport sizes. C4 adds an opt-in measurement harness and automation-only diagnostics; it does not change editing behavior.

The harness uses a controlled Unicode provider to send the test payload through the production `WM_PASTE` handler. The real system/user clipboard and `Winsta0` are not opened or changed. The separate real clipboard smoke test remains unavailable because `CreateWindowStationW` was denied, so these measurements do not complete the overall Phase C exit gate.

## Reproduce

From the repository root on Windows, build the Release x64 binary, syntax-check the runner, and execute the full matrix:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe' build\ClassicNotepad.vcxproj /m:4 /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
py -3 -m py_compile tests\windows\measure_native_editor_limits.py
py -3 -u tests\windows\measure_native_editor_limits.py --binary build\Release\ClassicNotepad.exe --output-dir build\native-editor-limit-measurements-rerun
```

The runner is [measure_native_editor_limits.py](../tests/windows/measure_native_editor_limits.py). It writes the JSONL measurements, summary, and generated fixtures under the selected `--output-dir`; a run reinitializes that output directory's measurement file. Use a new output directory, as in the command above, to preserve the locally retained corrected evidence at `build/native-editor-limit-measurements-corrected/`. Build output and measurement evidence are generated locally under ignored `build/` paths, not checked-in artifacts; each summary records the binary and source identity. The default matrix uses three fresh application processes per case and stops if an operation takes 90 seconds, private bytes reach 1 GiB, or the run exceeds 30 minutes. A resource stop is inconclusive and is not treated as a measured limit.

## Run configuration and method

The corrected run was captured 2026-10-04 on Windows 11 build 26200, AMD64, using `build/Release/ClassicNotepad.exe` (571,392 bytes; SHA-256 `e8229f9157774a974b0fb4055c040cd3d54b82f16dc08a152e4a6669ea7489c4`). It used Consolas 11, light theme, 96 DPI, a requested 1000 × 700 window, word wrap off, and the native default text limit. `EM_GETLIMITTEXT` returned 2,147,483,646 in the measured samples.

The actual editor viewport was 963 × 606 in 90 samples and 980 × 606 in 18. All 69 paste samples and 21 long-row samples used 963 × 606. The 18 samples at 980 × 606 were the 1,000–32,768-unit long rows. Each JSONL record and the generated summary include the actual viewport, so the scroll measurements can be compared with the viewport used for that sample.

Generated `summary.json` and every JSONL record carry the binary SHA-256 and source identity. The run used base revision `d506e83f58d050da75d97cd5c2c81b0453b74ca2` with a dirty working tree. SHA-256 values at run time:

| Source | SHA-256 |
| --- | --- |
| `src/platform/windows/app.cpp` | `5aa1881d5dc8a8be3d230935753b30a4c3aa7cd4c3b6f4fbe4680199a7588a2a` |
| `src/platform/windows/app.h` | `3dcb1dac49e22633d7e743b8987118ec1ee14112443aff5011aa84a83a3b47d2` |
| `src/platform/windows/automation.cpp` | `0ff5ea39ad685678c7441db9507089a1e0b777a667efb2d277a84bf4cf739a79` |
| `tests/windows/measure_native_editor_limits.py` | `0e8a9e982b8e018e77951953b639b240c8bfc955860081cde93ad103c21e3950` |

The 108 samples comprise 39 long-row measurements, 45 ASCII paste measurements, and 24 Unicode, line-ending, and insertion-position measurements. Each sample ran in a fresh application process. The long-row ladder loaded deterministic `W` text from UTF-16LE files. The paste ladder read payloads from UTF-16LE files into the controlled Unicode provider, then sent synchronous `WM_PASTE` through the production handler. The runner records fixture preparation, provider file read, provider setup, the direct synchronous `WM_PASTE` dispatch, provider cleanup, paste-command wall time, initial width-state read, width-settlement polling, text export, file hash, and Undo separately. No text was copied or hashed during the timed production dispatch.

After each paste, the harness recorded expected and actual stored UTF-16-unit lengths and SHA-256 hashes, and compared them. It also read the post-paste selection through `EM_GETSEL` and checked that it was collapsed at `insertion + normalized payload units`. Every paste case used one Undo and checked expected and actual baseline length and hash against the exact 140,002-unit baseline. A mismatch stops that case before it can be recorded as successful. The process's private bytes and working set were sampled every 10 ms. Paste records contain phase memory baselines and peaks for paste command, width settlement, export, hash, Undo, Undo export, and Undo hash; the whole-process peak is retained for the 1 GiB stop cap. Each phase record includes start/end snapshots, sampled peak, and sample count.

The runner additionally recorded `EM_GETLIMITTEXT`, native horizontal `SCROLLINFO`, display-row count, maximum native display-line length, and the C1 estimated width. [Microsoft's `EM_CHARFROMPOS` documentation](https://learn.microsoft.com/en-us/windows/win32/controls/em-charfrompos) specifies the edit-control character index in `LOWORD` and line index in `HIWORD`; the runner saves only those raw words and does not interpret the character word as a full-width index.

Long-row tail checks focused the native EDIT, selected the end position, tried `EM_LINESCROLL` and `SB_THUMBPOSITION` horizontally, then sent vertical `SB_BOTTOM`. When `EM_POSFROMCHAR` reported the last-character coordinate inside the client area, the harness delivered native `WM_LBUTTONDOWN` and `WM_LBUTTONUP` messages at that point (one pixel into the character), then read the resulting selection with pointer-based `EM_GETSEL`. The selection indices are full-width `DWORD` values. Only these click results are used as full-index tail evidence; the raw `EM_CHARFROMPOS` words are diagnostics.

## Long-row results

All 39 file-loaded long rows (13 lengths × three fresh runs) had exact stored UTF-16LE length and hash. Their native display rows and maximum line length follow; the “C1 width” is the recorded conservative estimate, not a measured scrollable width.

| Logical row units | Actual viewport | Native display rows | Maximum display-line length | C1 width estimate (px) | Native horizontal max (px) | Full-width click caret from `EM_GETSEL` |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1,000 | 980 × 606 | 1 | 1,000 | 64,000 | 7,028 | Inconclusive; no in-view coordinate |
| 4,000 | 980 × 606 | 4 | 1,024 | 256,000 | 7,220 | Inconclusive; no in-view coordinate |
| 8,000 | 980 × 606 | 8 | 1,024 | 512,000 | 7,220 | Inconclusive; no in-view coordinate |
| 16,000 | 980 × 606 | 16 | 1,024 | 1,024,000 | 7,220 | Inconclusive; no in-view coordinate |
| 32,767 | 980 × 606 | 32 | 1,024 | 2,097,088 | 7,220 | Verified; caret 32,766 |
| 32,768 | 980 × 606 | 32 | 1,024 | 2,097,152 | 7,220 | Verified; caret 32,767 |
| 65,535 | 963 × 606 | 64 | 1,024 | 4,194,240 | 7,237 | Verified; caret 65,534 |
| 65,536 | 963 × 606 | 64 | 1,024 | 4,194,304 | 7,237 | Verified; caret 65,535 |
| 65,537 | 963 × 606 | 65 | 1,024 | 4,194,368 | 7,237 | Inconclusive; no in-view coordinate |
| 131,072 | 963 × 606 | 128 | 1,024 | 8,388,608 | 7,237 | Verified; caret 131,071 |
| 262,144 | 963 × 606 | 256 | 1,024 | 16,777,216 | 7,237 | Verified; caret 262,143 |
| 300,000 | 963 × 606 | 293 | 1,024 | 19,200,000 | 7,237 | Verified; caret 299,999 |
| 1,048,576 | 963 × 606 | 1,024 | 1,024 | 67,108,864 | 7,237 | Verified; caret 1,048,575 |

The effective native horizontal maximum is `nMax - nMin - nPage + 1`. The 18 long-row samples with a 980 × 606 viewport ranged from 7,028 to 7,220 pixels; the 21 long-row samples with a 963 × 606 viewport reported 7,237 pixels. At 300,000 `W` units in the 963 × 606 viewport, the C1 estimate was 19.2 million pixels while the reported effective native horizontal range was 7,237 pixels. That is a navigation/display measurement; it does not indicate that the text was truncated. Every long-row text hash remained exact, including at 300,000 and 1,048,576 units. The native text limit reported by `EM_GETLIMITTEXT` is not a measured usable capacity.

Full-width tail click results were route- and length-dependent in this run:

| Tested row length | Result from the actual scroll sequence and full-width click read |
| ---: | --- |
| 1,000–16,000 | Inconclusive: `EM_POSFROMCHAR` did not provide an in-client last-character point for the click probe. |
| 32,767–32,768 | Native click messages placed the caret at the last character; full-width `EM_GETSEL` returned 32,766 and 32,767 respectively. |
| 65,535–65,536 | Native click messages placed the caret at the last character; full-width `EM_GETSEL` returned 65,534 and 65,535. |
| 65,537 | Inconclusive: the reported x-coordinate was −7,233 pixels after `SB_BOTTOM`, outside the viewport, so no click was sent. |
| 131,072–1,048,576 | Native click messages placed the caret at the last character; full-width `EM_GETSEL` returned the exact last-character index in every tested row length. |

`EM_CHARFROMPOS` reports a 16-bit character word in `LOWORD` and a display-line word in `HIWORD`; the harness retains those raw values and does not calculate a full-index delta from them. The verified clicks show full-width tail caret positions for the listed samples. The inconclusive coordinates at 1,000–16,000 and 65,537 do not establish that those rows are inaccessible by every possible route, and these data do not establish a monotonic accessibility threshold.

## Large paste results

For each size below, the one-row, four-row, and 1,024-row shapes contained the same normalized number of UTF-16 units and were repeated three times each. The timing cells show median direct `WM_PASTE` dispatch time / median width-settlement time in milliseconds; parentheses give native display rows after the paste. Width settlement is the time to observe the width measurement leave its pending state, polled every 20 ms. It is separate from dispatch time.

| Normalized payload units | One logical row: dispatch / settle (display rows) | Four logical rows: dispatch / settle (display rows) | 1,024 logical rows: dispatch / settle (display rows) |
| ---: | ---: | ---: | ---: |
| 276,480 (270 Ki) | 25.95 / 104.96 (407) | 25.85 / 78.38 (408) | 27.16 / 196.73 (1,160) |
| 1,048,576 (1 Mi) | 71.11 / 231.82 (1,161) | 70.92 / 201.76 (1,162) | 72.57 / 575.28 (1,162) |
| 4,194,304 (4 Mi) | 259.56 / 697.21 (4,233) | 264.99 / 706.28 (4,234) | 260.43 / 2,036.48 (4,234) |
| 7,340,032 (7 Mi) | 449.35 / 1,209.37 (7,305) | 446.30 / 1,202.35 (7,306) | 448.89 / 1,227.33 (7,306) |
| 16,777,216 (16 Mi) | 1,021.51 / 2,718.19 (16,521) | 1,019.50 / 2,711.48 (16,522) | 1,018.23 / 2,723.69 (16,522) |

All 45 ASCII ladder cases completed with exact output length/hash and one Undo restoring the exact baseline. The expected and actual collapsed post-paste selection indices also matched in all 69 paste cases. The largest tested payload was 16,777,216 UTF-16 units; it completed in each row shape. The maximum observed direct dispatch time in the table was 1,030.81 ms; the maximum width-settlement time was 2,821.59 ms. These are observed times for this build and configuration, not thresholds.

Each JSONL paste record keeps the timing stages separate: `fixturePreparationMs`, `providerFileReadMs`, `providerSetupMs`, `productionDispatchMs`, `providerCleanupMs`, `pasteHelperWallMs`, `pasteCommandWallMs`, `initialWidthStateReadMs`, `widthSettlementAfterInitialStateMs`, post-paste export and hash times, `undoMs`, and Undo export and hash times. At 16 Mi, median provider file read was 61.72–63.64 ms, provider setup 2.66–2.75 ms, provider cleanup 0.00 ms at the recorded microsecond resolution, and initial width-state read 0.14–0.19 ms across the three shapes. For example, the four-row 16 Mi case had median fixture preparation 19.48 ms, direct dispatch 1,019.50 ms, width settlement 2,711.48 ms, export operation 27.23 ms, hash 75.62 ms, and Undo 22.66 ms.

The additional 270 Ki source cases covered BMP text (`漢`), emoji (`😀`), CRLF, LF, CR, mixed line endings, and emoji insertion at the beginning and end. All 24 cases across those categories and three repeats passed expected/actual length, hash, collapsed selection, and Undo checks. The normalization target was checked after Windows line-ending normalization; line-ending variants can therefore have different stored UTF-16 lengths from their source fixture.

Phase memory peaks below are maxima across the 69 paste records. Every phase record also carries baseline/end snapshots, sampled peak, and number of samples observed during that phase.

| Paste phase | Maximum PrivateUsage peak (bytes) | Maximum working-set peak (bytes) |
| --- | ---: | ---: |
| Paste command | 206,036,992 | 216,850,432 |
| Width settlement | 72,761,344 | 92,045,312 |
| Post-paste export | 106,668,032 | 125,886,464 |
| Post-paste hash | 72,761,344 | 92,049,408 |
| Undo | 105,652,224 | 124,903,424 |
| Undo export | 105,889,792 | 124,903,424 |
| Undo hash | 105,652,224 | 124,903,424 |

Across all 108 samples, the largest whole-process sampled private-byte value was 206,036,992 bytes and the largest working set was 216,850,432 bytes. No case hit the 1 GiB whole-process private-byte cap, 90-second operation watchdog, or 30-minute total budget; no truncation, native-partial result, crash, selection/hash/Undo mismatch, or early stop was recorded. This confirms only the tested envelope above. It does not establish behavior beyond those payload sizes or close the separate real clipboard smoke-test gate.

## C5 routing decision

The corrected run used 108 samples: 39 long-row cases and 69 paste cases. **Long-row integrity:** all 39 file-loaded rows had exact stored UTF-16 lengths and hashes through 1,048,576 units in three repeats; these long-row samples did not measure Undo. **Paste integrity and Undo:** all 69 paste records had matching expected/actual post-paste length and hash, the expected collapsed selection, and one Undo that restored the expected 140,002-unit baseline with matching hashes. All nine 16 Mi paste cases (16,777,216 normalized UTF-16 units; one, four, and 1,024 logical rows, three fresh-process repeats each) produced exact post-paste text and passed the one-Undo check. These are separate measured dimensions on the recorded build and configuration; the long-row results do not imply an Undo result at that row length.

The tail click probe verified a full-width caret at the last character in 24 long-row samples. Fifteen probes were inconclusive: lengths 1,000–16,000 and 65,537. The C1 comparison between an estimated 19.2-million-pixel row width and a finite native horizontal scroll range is a navigation/display observation, not proof of text truncation or a general native hard limit. `EM_CHARFROMPOS` exposes only a 16-bit character word in `LOWORD`; its raw values cannot establish a full character index. The verified and inconclusive click results therefore do not establish a general accessibility limit. Separately, C3's deliberately reduced `EM_SETLIMITTEXT` case was rejected by the production preflight; it did not measure an actual capacity threshold.

Current evidence does not justify changing editor routing. Retain current routing and reconsider it only if a reproducible hard limit or a concrete integrity or accessibility failure establishes a need. This disposition is separate from the still-open Phase C gate: real OS clipboard smoke remains unavailable because `CreateWindowStationW` was denied. Visible navigation for the inconclusive cases, other fonts, DPI settings, themes, and inputs beyond the measured envelope remains unqualified.
