# Windows Printing Qualification

## Scope and test environment

This records Phase D6 qualification for the existing native EDIT path: File → Page Setup, File → Print, `PrintDlgW`, the production GDI print routine, and the installed Microsoft Print to PDF queue. The machine has one printer queue: **Microsoft Print to PDF**, port `PORTPROMPT:`. The evidence qualifies only that driver/configuration. **Physical-printer qualification is OPEN; qualification of other printer drivers and ports is OPEN.** Large mapped documents continue to report printing unavailable; their production command path is checked below.

Test host: Windows 11 Pro, build 10.0.26200, 64-bit. Output fixtures and raw evidence are locally retained generated artifacts under `build/d6-print-evidence/` (ignored by Git, not checked-in evidence). The PDF matrix used the pre-seam Release binary SHA-256 `20b7440bff9a0b7c35fd13dc1921efcf9b9349e1945ea6d4258a73502cf4fa74`. The automation fault-seam and recovery runs used `build-d5-review/Release/ClassicNotepad.exe`, SHA-256 `68fa5122a3402e1fb6553e4a3abd645e81e38e1a9155c73fca4b6149c9a5b024`.

The source tree is dirty from the preceding D1–D5 work and these D6 changes; base revision is `d506e83f58d050da75d97cd5c2c81b0453b74ca2`. The generated `build/d6-print-evidence/controlled-gdi-failures.json` records the binary hash and reproducible hashes for the relevant current sources and automation helpers. Current relevant source hashes are:

- `src/platform/windows/app.cpp`: `85bbd31654061dd91746b52f0ea2cf292ab343d136335d47f5c55876f2695cf1`
- `src/platform/windows/app.h`: `7fcfbb6811b263d6e3bc1fd492f97e17816d160f36b087100ad7ccbd637b5566`
- `src/platform/windows/automation.cpp`: `fef9fcefde363f23af3143184486ae6d65f881b892af95972bac055c24664105`

All sample manifests record the production route as `WM_COMMAND ID_FILE_PRINT (40006) → PrintDlgW → production GDI → Microsoft Print to PDF`. The test helper selected/verified the printer in the visible Print dialog and entered the exact output path in the actual Save Print Output As prompt. It did not call the test sink or `PrintEditorText` directly.

## Regenerate local artifacts

The original PDFs, manifests, screenshots, and raw probes remain under the ignored `build/d6-print-evidence/` directory on the evidence machine. The helper scripts are also generated local evidence, not checked-in test code. The commands below copy the helpers to a fresh ignored directory so an exploratory rerun does not overwrite the retained originals. They rebuild the current checkout; regenerated files receive new source/binary identities and are not expected to match the historical PDF hashes byte-for-byte.

From the repository root in PowerShell:

```powershell
$Evidence = 'build\d6-print-evidence-rerun'
New-Item -ItemType Directory -Force "$Evidence\pdfs" | Out-Null
Copy-Item -Path 'build\d6-print-evidence\*.py', 'build\d6-print-evidence\*.ps1' -Destination $Evidence -Force
cmake --build build-d5-review --config Release
py -3 "$Evidence\make_fixtures.py"
New-Item -ItemType File -Force "$Evidence\fixtures\empty.txt" | Out-Null
```

Start each sample in one PowerShell window; for example:

```powershell
py -3 "$Evidence\run_print_sample.py" --fixture "$Evidence\fixtures\numbered-lines.txt" --output "$Evidence\pdfs\numbered-lines.pdf" --font "Consolas 11" --orientation portrait --margins "0.75,1.00,0.50,0.75"
```

While that helper has the actual production Print dialog open, use a second PowerShell window to select Microsoft Print to PDF and save through the actual driver prompt. Use the PID printed by the sample helper and the exact same output path:

```powershell
$Evidence = 'build\d6-print-evidence-rerun'
powershell -NoProfile -ExecutionPolicy Bypass -File "$Evidence\click_print_button.ps1"
$SamplePid = 12345 # replace with the PID printed by the sample helper
py -3 "$Evidence\drive_output_prompt.py" --pid $SamplePid --action save --output "$Evidence\pdfs\numbered-lines.pdf"
```

The fixture preparation above generates the source fixtures; the separate `New-Item` command creates the empty fixture. Run `run_print_sample.py` once per fixture/output pair to generate the six named PDF samples:

```powershell
py -3 "$Evidence\run_print_sample.py" --fixture "$Evidence\fixtures\empty.txt" --output "$Evidence\pdfs\empty.pdf"
py -3 "$Evidence\run_print_sample.py" --fixture "$Evidence\fixtures\numbered-lines.txt" --output "$Evidence\pdfs\numbered-lines.pdf" --font "Consolas 11" --orientation portrait --margins "0.75,1.00,0.50,0.75"
py -3 "$Evidence\run_print_sample.py" --fixture "$Evidence\fixtures\tabs-blank-long-rows.txt" --output "$Evidence\pdfs\tabs-blank-long-rows.pdf"
py -3 "$Evidence\run_print_sample.py" --fixture "$Evidence\fixtures\unicode.txt" --output "$Evidence\pdfs\unicode.pdf"
py -3 "$Evidence\run_print_sample.py" --fixture "$Evidence\fixtures\geometry-large-font.txt" --output "$Evidence\pdfs\geometry-large-font.pdf" --font "Consolas 18"
py -3 "$Evidence\run_print_sample.py" --fixture "$Evidence\fixtures\geometry-landscape.txt" --output "$Evidence\pdfs\geometry-landscape.pdf" --orientation landscape --margins "0.50,0.50,0.75,0.75"
```

For every invocation, complete the real Print dialog and output-path prompt using the second-window steps above, substituting that sample's exact PDF path. The helper accepts `--fixture`, `--output`, `--font`, `--orientation`, and `--margins`. `inspect_pdfs.py` can regenerate the page-count, dimensions, text-marker, and PDF-hash summary after all six outputs exist:

```powershell
py -3 "$Evidence\inspect_pdfs.py" > "$Evidence\pdf-inspection.json"
```

To repeat the controlled production-handler failure stages through the actual print UI path, run:

```powershell
py -3 "$Evidence\run_controlled_gdi_faults.py" --stages startDoc startPage textOut endPage endDoc
```

The controlled stage failures are automation-seam evidence, not real-driver failures. The commands do not change the default printer/queue or system printer configuration; they select the qualified queue in the app's Print dialog.

## Real driver matrix

The six production PDFs below were parsed for page count, dimensions, byte hash, and fixture markers. All 39 pages were rendered at 96 DPI and visually reviewed; no clipping, missing rows, or unexpected page transitions were observed. The long row continues across pages as intended. Greek/CJK and Latin text rendered legibly; emoji used the driver's monochrome fallback glyphs.

| Fixture and settings | PDF result | Integrity / pagination evidence |
|---|---|---|
| Empty document, Letter portrait | `empty.pdf`: 1 page, 612×792 pt, 819 bytes; SHA-256 `3f914cc9ba8a663aaacb75112b8974faca81124f533f04b6b293caa5307dc5ea` | Valid blank one-page PDF. |
| 300 numbered rows plus trailing CRLF, Letter portrait, Consolas 11; margins L/R/T/B = 0.75/1.00/0.50/0.75 in | `numbered-lines.pdf`: 6 pages, 52,750 bytes; SHA-256 `00fe8ffbdb669504adb7fd1aa0b12196d952094a3ffaf3a25b7a26375dc99c3d` | `ROW-0001` through `ROW-0300` each once and in order. Device metrics independently predict 53 rows/page: 600 DPI, printable device rectangle `(450,300)-(4500,6150)`, 5,850 px height, 110 px font line height; `ceil(300/53)=6`. The trailing terminator does not add a printed row. |
| Tabs, blank lines, long wrapped row and continuation/end markers, Letter portrait | `tabs-blank-long-rows.pdf`: 2 pages, 51,149 bytes; SHA-256 `2d5151498b250cfae69da374c3fc655bd4fa6b3410194b8134143fc073fb1bc3` | TAB-BEGIN, COLUMN-2/3, BLANK-AFTER-HEADER, LONG-BEGIN, CONTINUATION-MARKER, LONG-END, TAB-END, and SHORT-END all present once. |
| Latin, Greek, CJK and supplementary emoji, Letter portrait | `unicode.pdf`: 3 pages, 4,100,870 bytes; SHA-256 `25d5306c0236e430cb46e055d50ca811cbd8af7ec879deb555d68dee9713cbee` | Repeated Unicode markers and text extracted from all pages; no missing rows in visual inspection. Emoji display uses fallback glyphs. |
| 300 rows, larger Consolas 18 font, Letter portrait | `geometry-large-font.pdf`: 19 pages, 62,521 bytes; SHA-256 `9f5eaef264fc4d39fef2f2fbd53b29fda918660f5c55dc31ba60f533f6563c13` | First/last row markers once; larger font changed pagination from 6 to 19 pages. |
| 300 rows, Letter landscape, margins L/R/T/B = 0.50/0.50/0.75/0.75 in | `geometry-landscape.pdf`: 8 pages, 792×612 pt, 54,351 bytes; SHA-256 `f2a1970aa6df1c78259065afa0f8221e535aea1cef3f6eb2484c42704a0f350b` | First/last markers once. Device metrics predict 38 rows/page from 4,200 px printable height / 110 px line height; `ceil(300/38)=8`. |

Letter was explicitly selected in the real Page Setup dialog; an exploratory default DEVMODE query reported A4, so default settings were not used to predict these outputs. Portrait and landscape Page Setup values were accepted, reopened and read back; changing one field and cancelling retained the previously accepted values. Print dialog cancel, output filename cancel, Page Setup cancel, and a deliberately nonexistent output parent were exercised against the actual dialogs. Those paths left selection, content/hash, dirty state, title, metadata, status and font unchanged; the invalid path was rejected by the driver/OS and produced no file. Evidence is in `cancel-probes.json`, per-sample `.run.json` manifests, `invalid-destination.json`, `printer_metrics_actual.json`, `pdf-inspection.json`, and `rendered/`.

## Controlled production-handler failure checks

The fault hook is automation-only. It can inject at StartDoc, StartPage, TextOut, EndPage or EndDoc; normal non-automation runs cannot select or trigger these faults. Each case used a fresh visible automation process and posted `WM_COMMAND(ID_FILE_PRINT)` to the app. The production Print dialog was confirmed, and for stages after StartDoc the actual output dialog was given a unique temporary path before collecting diagnostics. Every failure left document content/hash, selection, modified state, title, metadata, status and font unchanged. Each same-process retry with the fault cleared produced a parseable one-page recovery PDF.

| Injected stage | Production result and cleanup | Failed-job artifact | Same-process recovery |
|---|---|---|---|
| StartDoc | Reports “The print job could not be started.” No job began, so AbortDoc is not called. | No output prompt or file. | Valid 1-page PDF, 39,103 bytes. |
| StartPage | Reports “The first page could not be started.” AbortDoc called and succeeded. | Save path was accepted, then the aborted output remained zero bytes. | Valid 1-page PDF, 39,357 bytes. |
| TextOut | Reports “Text could not be sent to the printer.” AbortDoc called and succeeded. | Zero-byte aborted output. | Valid 1-page PDF, 39,652 bytes. |
| EndPage | Reports “The last page could not be completed.” AbortDoc called and succeeded. | Zero-byte aborted output. | Valid 1-page PDF, 39,363 bytes. |
| EndDoc | Reports “The print job could not be completed.” AbortDoc called and succeeded. | Zero-byte aborted output. | Valid 1-page PDF, 39,107 bytes. |

These are controlled injected failures, not failures reproduced from the real printer driver. Diagnostics and paths are retained in `controlled-gdi-failures.json` and `fault-recovery/{startDoc,startPage,textOut,endPage,endDoc}.json`; the zero-byte files are retained as evidence. The recovery PDFs were independently parsed as one-page PDFs.

## Mapped printing availability

A 50,331,660-byte mapped fixture was opened in the normal app. Posting the real `ID_FILE_PRINT` command displayed the app's mapped-print unavailable notice. No Print dialog, output filename prompt, or PDF appeared; the fixture hash/size, title and document state were unchanged. This confirms the current unavailable response only, not printing support for mapped documents. The evidence is `mapped-print-unavailable.json`.

## Checks and limits

- Release x64 build: `cmake --build build-d5-review --config Release` — passed.
- Printing automation: `py -3 tests/automation/run_automation_tests.py --binary build-d5-review/Release/ClassicNotepad.exe --platform windows --pattern test_printing.py` — 2 passed.
- Core CTest: `ctest --test-dir build-d5-review -C Release --output-on-failure` — 3/3 passed (`TextConversionTests`, `WindowsAnsiEncodingTests`, `TextConversionPortableSmokeTests`).
- `py_compile` passed for the controlled GDI runner and the D6 Python evidence helpers.
- Real driver qualification is limited to Microsoft Print to PDF on this Windows build, with the recorded fonts, Letter paper and margin/orientation settings. Physical-printer qualification and qualification of other printer drivers/ports are OPEN; other OS builds remain untested. Page Setup and print cancellation were tested; no real driver GDI stage failure was available, so stage-failure outcomes above are controlled-seam evidence.
