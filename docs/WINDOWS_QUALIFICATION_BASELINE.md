# Windows qualification baseline — E13

- **Started:** 5 October 2026, 02:54:25.267 UTC
- **Completed:** 5 October 2026, 02:54:46.966 UTC
- **Run ID timestamp:** `02:54:05 UTC` is encoded in the run ID `20261005T025405Z-e13-r2-final`; it is not the capture time.
- **Original helper disposition:** **Pending (exit code 2), preserved unchanged.** Configure, Release build, and CTest passed 11/11. The current executable did not match the binary used by retained E10 Full evidence, so that evidence was not reused. At the time of the original 02:54 helper capture, Full had not been rerun.
- **Current E13 disposition:** **PASS for the current-host baseline with independently reviewed supplemental Full evidence**, run once against the exact E13 executable under the user's explicit active-host clipboard override. See [supplemental Full evidence](#supplemental-exact-binary-full-evidence).
- **Current helper-bound run:** [`baseline.json`](../build/windows-qualification/20261005T025405Z-e13-r2-final/baseline.json), SHA-256 `C69366ED24B840410DD2CD8A1C90DCE16A7D76BBDE257F005700A024C75C2A1E`
- **Prior 02:42 capture:** [`baseline.json`](../build/windows-qualification/20261005T023517Z-e13-7c5a255b/baseline.json), SHA-256 `C340102B9DE5E424EDE40089361E49CBE0CE0A998A28664EC16DA88A698A936B`
- **Original 02:22 run, preserved unchanged:** [`baseline.json`](../build/windows-qualification/20261005T022115Z-e13-42c781bf/baseline.json), SHA-256 `21F54533370F6025F2362E369D1AF96FFCE2439726AD4B3ECA4E4F12698E773A`
- **Interpretation correction chain:** [original correction](../build/windows-qualification/20261005T022115Z-e13-42c781bf/baseline-interpretation-correction.json), [02:42 amendment](../build/windows-qualification/20261005T022115Z-e13-42c781bf/baseline-interpretation-correction-amendment.json), and [02:54 capture amendment](../build/windows-qualification/20261005T022115Z-e13-42c781bf/baseline-interpretation-correction-amendment-20261005T025405Z.json)

The `baseline.json` remains an immutable historical helper capture and still reports its original pending identity-mismatch exit code. The supplemental record below closes the exact-binary Full evidence gap for E13 on this host; it does not infer a supported-Windows set or close the wider clipboard, visible-application, or platform matrix gates.

## Supplemental exact-binary Full evidence

On 5 October 2026, the Full suite ran once against the exact E13 Release executable after the user explicitly authorized an active-host run despite clipboard effects. The run used the current copied automation sources from the evidence workspace and left the historical baseline JSON unchanged.

- Evidence directory: [`20261005T081102Z-e13-active-host-full-once`](../build/windows-qualification/20261005T081102Z-e13-active-host-full-once/)
- Full result: [`full-results.json`](../build/windows-qualification/20261005T081102Z-e13-active-host-full-once/execution/full-results.json), SHA-256 `6C0D0B7F89E417221DB7CFC408C81D9222482636DAB1F9AAD63F8F153A728F07`
- Outcome: **120 selected/executed, 116 passed, 4 skipped, 0 failed/errors**, native process exit 0; duration 249.788 seconds. The four source-justified skips are Linux appearance-menu dismissal, Linux status-bar-menu dismissal, Linux GTK large-file mode, and macOS AppKit large-file mode. The invocation omitted `--pattern`; the runner used its `test_*.py` default.
- Exact app binary SHA-256: `B408D6F029B272FAA1B3B7539EC870152B6D8A16305A6799AECA5EE6BF4962A0`. The runner, manifest, driver, `VERSION`, Python, all 135 source inputs, and all 23 copied automation files matched their verified identities before/after.
- Captured outputs: [stdout](../build/windows-qualification/20261005T081102Z-e13-active-host-full-once/execution/full.stdout.log), SHA-256 `34E3C43B4BE5CC39DA47DA74DCB148692A08D20C463E3ECA646B4BA050386755`; [stderr](../build/windows-qualification/20261005T081102Z-e13-active-host-full-once/execution/full.stderr.log), SHA-256 `DB9D04A473C45A231F9DA90EE3DFC2F6ECC3522449DD26A4A02DFEE0EAD62903`; [artifact checksum list](../build/windows-qualification/20261005T081102Z-e13-active-host-full-once/artifact-sha256sums.txt), SHA-256 `6FC1E83822543BED3AAA7FED14B16644E5F0C2833FF3C082858E50319ABF9BFE`.
- Preservation: the original automation scratch's 12 depth-0 entries match the baseline; `tmp8ah_ivmt` stayed opaque and was not traversed. The root user `automation-large-file-tests` folder and its accessible contents, plus the three named user files, matched before/after checks. Test-created scratch belongs to the copied evidence workspace.
- Clipboard record: `isolation=active_host_session`, `clipboardIsolation=false`, `hostClipboardAccessed=true`. Clipboard contents were not captured. Sandbox was not enabled or launched.
- Independent review: **PASS for this authorized direct-host execution**, with limits below.

The contemporaneous environment record includes host/user/domain, Windows build 26200, AMD64 process architecture, Python executable/path hash, and full process environment. It records `session=null` and does not include Windows edition, user SID, Python version text, or Unicode window-station/desktop names; a separate process observation records the application child in session 1. These missing fields remain unverified and are not reconstructed after the run. This evidence does not establish clipboard isolation, the supported-Windows set, complete visible-app coverage, or other platform/printer qualification.

## Original 02:54 helper capture — host and source identity

This section records the host and source identity captured by the original 02:54 helper run in `20261005T025405Z-e13-r2-final`. It does not describe the later supplemental direct-host Full run, whose contemporaneous record is above.

The original helper ran on machine `DAVE-SPEEDY`, Windows 11 Pro `10.0.26200`, x64, with Visual Studio Community 2026 `18.9.12120.119` (instance `5f70fc34`) and generator `Visual Studio 18 2026`. CMake and CTest were `4.3.1-msvc1`; Python was `3.13.7`. The observed processor was AMD Ryzen 7 8700F, with 16 logical processors and 32 GiB of physical memory.

The original helper recorded source revision `d506e83f58d050da75d97cd5c2c81b0453b74ca2` and a dirty working tree. Its build-input fingerprint was `A3B1D3DFF7CB5485D8A994355D4AD28A5A1EAC00309542D110C06841FF85144F` both before and after the helper capture. Documentation is excluded from that fingerprint. The helper script SHA-256 was `08E3095170724E2E202BCDE60811DAE0218756A37FFB7DAA7C01E5D8CFDC7AD8`. `VERSION` stayed at SHA-256 `3C85C66D06C31DF0C6511AD18B7E8576FCF8E76A011C5C28BBD66C57F3ED851E`.

The retained E10 Full record predates the original helper's source fingerprint and has no comparable full build-input hash. Its revision and dirty flag match that helper capture, but exact source-tree identity for the historical Full result remains unverified.

## Original 02:54 helper capture — commands and results

This section documents only the commands and results of the original 02:54 helper capture. The supplemental direct-host Full run above was a separate execution with separate command and result evidence.

In the original 02:54 helper capture, all four build/test commands returned exit code 0:

```text
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File scripts\capture-windows-qualification-baseline.ps1 -RunId 20261005T025405Z-e13-r2-final
cmake -S C:\vibe\classicNotepad -B C:\vibe\classicNotepad\build\windows-qualification\20261005T025405Z-e13-r2-final\cmake-build -G "Visual Studio 18 2026" -A x64 -DCMAKE_GENERATOR_INSTANCE="C:\Program Files\Microsoft Visual Studio\18\Community"
cmake --build C:\vibe\classicNotepad\build\windows-qualification\20261005T025405Z-e13-r2-final\cmake-build --config Release --parallel 4
ctest --test-dir C:\vibe\classicNotepad\build\windows-qualification\20261005T025405Z-e13-r2-final\cmake-build -C Release --show-only=json-v1
ctest --test-dir C:\vibe\classicNotepad\build\windows-qualification\20261005T025405Z-e13-r2-final\cmake-build -C Release --output-on-failure
```

The original helper launch used PowerShell 5.1 with process-scoped `ExecutionPolicy Bypass`; it made no persistent execution-policy change. All 12 child commands recorded by that helper returned exit code 0. Its pending baseline records helper `overallExitCode` 2.

CTest registered and passed all 11 tests: `TextConversionTests`, `DocumentTransitionCoordinatorTests`, `LargeTextWidthSummaryTests`, `LargeTextSummaryTests`, `InsertedTextValidationTests`, `WindowsAnsiEncodingTests`, `WindowsMappedSourceTests`, `WindowsTextBoundaryTests`, `WindowsSpellingOccurrenceTests`, `WindowsMenuTextTests`, and `TextConversionPortableSmokeTests`.

The executable built by the original helper is [`ClassicNotepad.exe`](../build/windows-qualification/20261005T025405Z-e13-r2-final/cmake-build/Release/ClassicNotepad.exe), SHA-256 `B408D6F029B272FAA1B3B7539EC870152B6D8A16305A6799AECA5EE6BF4962A0`. The E10 Full executable SHA-256 is `A4DBF6C4435C6AEFD9A569196D0F0D207AEF3E4BC39061B59AFFA63CD44A8D78`. The original helper record reports `pending_identity_mismatch`; its only failed reuse check is `binarySha256Matches`. That helper did not reuse historical E10 Full. Its `fullAutomationRerun` and `clipboardAccessed` fields are false.

The original helper capture retained configure, build, CTest discovery, and CTest output in `../build/windows-qualification/20261005T025405Z-e13-r2-final/logs/`. See the [configure log](../build/windows-qualification/20261005T025405Z-e13-r2-final/logs/09-cmake-configure-release-x64.stdout.log), [build log](../build/windows-qualification/20261005T025405Z-e13-r2-final/logs/10-cmake-build-release-x64.stdout.log), [CTest discovery log](../build/windows-qualification/20261005T025405Z-e13-r2-final/logs/11-ctest-list-release.stdout.log), and [CTest result log](../build/windows-qualification/20261005T025405Z-e13-r2-final/logs/12-ctest-release.stdout.log).

## Historical interpretation corrections for the original 02:22–02:54 captures

This section describes the interpretation amendments attached to the original 02:22, 02:42, and 02:54 helper captures and the historical E10 reuse decision. It is separate from the supplemental direct-host Full run above.

The 02:42 capture at [`baseline.json`](../build/windows-qualification/20261005T023517Z-e13-7c5a255b/baseline.json) remains prior history: it started at `02:41:46.788Z`, completed at `02:42:08.990Z`, and its `02:35:17Z` timestamp comes from the run ID, not capture time. It is preserved with SHA-256 `C340102B9DE5E424EDE40089361E49CBE0CE0A998A28664EC16DA88A698A936B`.

The original 02:22 [`baseline.json`](../build/windows-qualification/20261005T022115Z-e13-42c781bf/baseline.json) had a misleading `fullAutomationEvidenceType` value. Its adjacent [correction JSON](../build/windows-qualification/20261005T022115Z-e13-42c781bf/baseline-interpretation-correction.json) binds the interpretation to that file's SHA-256 and records that E10 Full was not reused. That correction's `planned_not_yet_run` field described its state when it was created. The [02:42 amendment](../build/windows-qualification/20261005T022115Z-e13-42c781bf/baseline-interpretation-correction-amendment.json) links the correction to the prior capture. The [02:54 capture amendment](../build/windows-qualification/20261005T022115Z-e13-42c781bf/baseline-interpretation-correction-amendment-20261005T025405Z.json) binds the unchanged original baseline, correction, and prior amendment hashes to the latest baseline SHA and pending/exit-2 result. These historical JSON records were not modified.

## Historical E10 Full reconciliation by the original 02:54 helper

This is the original 02:54 helper's reconciliation of retained E10 records. It does not describe or replace the supplemental direct-host Full execution above.

The retained [E10 Full record](../build/windows-release-verification-evidence/20261004T233852Z-round2-full.json) reports 120 selected and executed cases: 116 passed, four skipped, zero failed, and zero errors. The retained [four-suite summary](../build/windows-release-verification-evidence/20261004T233852Z-round2-summary.json), [Release gate](../build/windows-release-verification-evidence/windows-release-ordinary-20261004T233639Z-9236.json), and [Full stdout](../build/windows-release-verification-evidence/20261004T233852Z-round2-full.stdout.log) and [stderr](../build/windows-release-verification-evidence/20261004T233852Z-round2-full.stderr.log) were present and reconciled by the original 02:54 E13 helper.

The original helper found that runner and manifest hashes, operating system and architecture, Visual Studio instance and generator, CMake/CTest/Python versions, source revision and dirty flag, `VERSION` before/after hashes, Full summary counts, and linked Full logs matched their retained records. The executable SHA was that helper's only failed identity check. E10 remains historical evidence for its recorded executable; it does not qualify the fresh E13 binary.

## Original 02:54 helper capture — preservation and open gates

The original 02:54 E13 helper captured the source and `VERSION` before and after; both were unchanged. Its depth-zero inventory of the pre-existing `build/automation-tests` entries reported every existing entry preserved, with no removals, changes, or additions. That helper capture did not access the clipboard and did not invoke Full automation.

These environment details were not captured by the original 02:54 helper and remain unverified: Windows power plan, per-display DPI/scaling, font/application theme/high-contrast settings, spelling provider/dictionary availability or selection, WinSta0/window-station/desktop isolation capability, storage type, and ANSI code page. No WinSta0 or desktop-isolation capability probe was performed. The helper's machine-readable inventory lists these items explicitly. No Full suite or clipboard run occurred in that original helper capture; the separate supplemental direct-host Full run above records active-host clipboard access. The supported-Windows matrix, visible-application qualification, separate real OS clipboard smoke, reference-hardware performance/process-lifetime matrix, and physical or other printer-driver qualification remain open.
