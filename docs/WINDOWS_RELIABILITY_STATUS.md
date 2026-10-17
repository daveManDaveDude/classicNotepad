# Windows reliability status

- **Status updated:** 5 October 2026
- **Current project version:** [`VERSION`](../VERSION) (`1.3.1`)

This page separates implementation and item-review results from platform qualification. Passing an automation suite does not, by itself, qualify every supported Windows release or the visible application in every configuration.

## Latest review-reconciled Release evidence

The review-reconciled Release set in this record ran on Windows 11 `10.0.26200.0`, x64, using Visual Studio 2026 and a dirty working tree based on revision `d506e83f58d050da75d97cd5c2c81b0453b74ca2`. It passed the Release build, all registered CTest targets, and the Ordinary suite. The same Release binary was used for the other three manifest suites. A later Ordinary-only gate record, [20261004T235223Z-25504](../build/windows-release-verification-evidence/windows-release-ordinary-20261004T235223Z-25504.json), also passed; the earlier linked set remains the one with the reviewed four-suite reconciliation summary.

| Evidence | Result | Record |
| --- | --- | --- |
| Release build and Ordinary suite | Passed; selected/executed 39, passed 37, skipped 2, failed/errors 0 | [Gate record](../build/windows-release-verification-evidence/windows-release-ordinary-20261004T233639Z-9236.json), [Ordinary JSON](../build/windows-release-verification-evidence/20261004T233639Z-9236-automation-ordinary.json), [CTest output](../build/windows-release-verification-evidence/20261004T233639Z-9236-ctest-release.stdout.log) |
| Stress suite | Selected/executed 45, passed 43, skipped 2, failed/errors 0 | [JSON](../build/windows-release-verification-evidence/20261004T233852Z-round2-stress.json) |
| Failure suite | Selected/executed 36, passed 36, skipped 0, failed/errors 0 | [JSON](../build/windows-release-verification-evidence/20261004T233852Z-round2-failure.json) |
| Full suite | Selected/executed 120, passed 116, skipped 4, failed/errors 0 | [JSON](../build/windows-release-verification-evidence/20261004T233852Z-round2-full.json) |
| Four-suite reconciliation | Passed; suite IDs/counts and identities reconcile | [Linked summary](../build/windows-release-verification-evidence/20261004T233852Z-round2-summary.json) |

The gate's CTest log reports **11/11 passed**: `TextConversionTests`, `DocumentTransitionCoordinatorTests`, `LargeTextWidthSummaryTests`, `LargeTextSummaryTests`, `InsertedTextValidationTests`, `WindowsAnsiEncodingTests`, `WindowsMappedSourceTests`, `WindowsTextBoundaryTests`, `WindowsSpellingOccurrenceTests`, `WindowsMenuTextTests`, and `TextConversionPortableSmokeTests`.

The reviewed Release executable SHA-256 is `A4DBF6C4435C6AEFD9A569196D0F0D207AEF3E4BC39061B59AFFA63CD44A8D78`. The automation runner SHA-256 is `DF322910A6645530343AEBECD33FCEDD5210C3F4AB63AFC7D7D2B4CD16BF1F10`; the suite manifest SHA-256 is `7B5B01579EC5420F4A60A23D5F14538CB11FA4D78B39D13B5F438B65A209EB4F`. `VERSION` remained unchanged with SHA-256 `3C85C66D06C31DF0C6511AD18B7E8576FCF8E76A011C5C28BBD66C57F3ED851E`. The [gate record](../build/windows-release-verification-evidence/windows-release-ordinary-20261004T233639Z-9236.json) retains tool versions, environment, hashes, command results, and logs.

## Current-host qualification baseline (E13)

The [E13 baseline report](WINDOWS_QUALIFICATION_BASELINE.md) links the current [run record](../build/windows-qualification/20261005T025405Z-e13-r2-final/baseline.json): a fresh Visual Studio 2026 Release configure/build and CTest run on Windows 11 build 26200. Configure and build succeeded, and CTest passed 11/11. That original helper record remains unchanged with pending exit code 2 because the current executable SHA-256 (`B408D6F0…BF4962A0`) differs from retained E10 Full (`A4DBF6C4…A8D78`). E13 now has independently reviewed supplemental Full evidence for the exact executable: [active-host run record](../build/windows-qualification/20261005T081102Z-e13-active-host-full-once/execution/full-run.command.json) and [results](../build/windows-qualification/20261005T081102Z-e13-active-host-full-once/execution/full-results.json), with 120 selected/executed, 116 passed, 4 platform-specific skips, zero failures/errors. The user authorized this active-host run despite clipboard effects; its record says `hostClipboardAccessed=true`, `clipboardIsolation=false`, and no clipboard contents were captured. This scoped E13 PASS does not close the broader supported-Windows, visible-app, Phase C clipboard-exit, printer, or other platform gates. Missing contemporaneous edition/SID/window-station/desktop fields remain explicitly unverified; see the [E13 report](WINDOWS_QUALIFICATION_BASELINE.md#supplemental-exact-binary-full-evidence).

## Evidence for current Windows-specific claims

| Claim or feature area | Evidence and limit |
| --- | --- |
| Section 6 acceptance coverage | The [acceptance coverage ledger](WINDOWS_ACCEPTANCE_COVERAGE.md) maps each of the 17 historical matrix rows to retained cases, exact run identities, assertions, and remaining gaps. It is an evidence inventory, not a matrix pass. |
| Automation suite sizes/defaults and safe Release workflow | The checked-in [suite manifest](../tests/automation/test_suite_manifest.json), [runner](../tests/automation/run_automation_tests.py), [test script](../scripts/test.ps1), and [Release gate](../scripts/verify-windows-release.ps1) define the current selection and defaults. The records above show the actual run. |
| Native EDIT paste and long-row measurements | [Windows native EDIT results](WINDOWS_NATIVE_EDITOR_LIMITS.md) records 108 controlled samples on its separately identified build. The 39 file-loaded long-row records support exact stored text length/hash through 1,048,576 UTF-16 units; long-row Undo was not measured. Separately, the 69 paste records support exact post-paste text/selection and one-Undo baseline restoration through the tested 16 Mi UTF-16 payload envelope. See the locally retained [C4 summary](../build/native-editor-limit-measurements-corrected/summary.json) and [raw JSONL records](../build/native-editor-limit-measurements-corrected/measurements.jsonl); [reproduction instructions](WINDOWS_NATIVE_EDITOR_LIMITS.md#reproduce) use a fresh output directory. This is not a general capacity threshold; the real OS clipboard smoke remains open. |
| Native/mapped editor and command behavior | The same-binary automation records include the focused semantic, viewport, spelling-window/action, save, metadata, wrap, and mapped-file tests. Those are automated evidence, not a complete visible-app or supported-Windows qualification. |
| Printing and Page Setup | [Windows printing qualification](WINDOWS_PRINTING_QUALIFICATION.md) records real production-dialog/GDI PDF output and controlled failure-seam evidence for Microsoft Print to PDF on Windows 11 build 26200. Locally retained generated artifacts include the [PDF inspection summary](../build/d6-print-evidence/pdf-inspection.json) and [controlled failure record](../build/d6-print-evidence/controlled-gdi-failures.json); [regeneration commands](WINDOWS_PRINTING_QUALIFICATION.md#regenerate-local-artifacts) are documented. Physical printers and other drivers remain OPEN. Mapped-file printing remains unavailable. |
| Version-preserving Release checks | The gate record and [gate script](../scripts/verify-windows-release.ps1) show the dedicated Release build, CTest, suite, cache identity checks, and unchanged `VERSION` hash. The retained evidence used Visual Studio 2026; this run does not demonstrate the gate on VS2022/2019. |

The root [README](../README.md) lists the implemented feature surface and the reproducible commands. The dated [Windows reliability review and plan](WINDOWS_RELIABILITY_REVIEW_AND_PLAN.md) remains the historical baseline and is not a current unresolved-issues list.

The linked `build/` reports, PDFs, logs, and JSONL samples are locally retained generated evidence and are ignored by Git; they are not source-controlled release assets. Use the [C4 reproduction commands](WINDOWS_NATIVE_EDITOR_LIMITS.md#reproduce) and [D6 regeneration commands](WINDOWS_PRINTING_QUALIFICATION.md#regenerate-local-artifacts) to generate new local evidence in fresh output directories. C4 and D6 record separate binaries/configurations and should be read with their own identities.

## Open qualification gates

- **Phase C clipboard exit gate: OPEN.** The E13 Full suite exercised the application on the active host under explicit user authorization; the evidence records `hostClipboardAccessed=true` and no clipboard contents were captured. This is not isolated clipboard evidence and does not complete the broader Phase C size/shape/failure exit gate. The earlier private `CreateWindowStationW` isolation attempt failed closed; see the [E13 report](WINDOWS_QUALIFICATION_BASELINE.md#supplemental-exact-binary-full-evidence).
- **Supported-Windows matrix: OPEN.** The current Release evidence is Windows 11 build 26200 x64 / VS2026 only. Other Windows versions/builds and the VS2022/2019 Release configurations are not qualified by this run.
- **Visible-app qualification: OPEN.** The automation evidence does not cover the complete visible UI, DPI scaling, mixed-DPI monitor transitions, assistive technology, broad input/focus behavior, or resource-lifetime matrix. D6 exercised its production dialogs but qualifies only the recorded print configuration.
- **Printer qualification beyond PDF: OPEN.** D6 covers Microsoft Print to PDF on the recorded Windows 11 configuration. Physical printers and other printer drivers/configurations have not been qualified.
- **Other platform runtime qualification: OPEN in this record.** Linux and macOS runtime results are outside the Windows Release run; use their separate platform build notes and evidence.

The implementation item reviews recorded for Phases A–D and E1–E13 are PASS. That item status does not close the open exit gates above. In particular, C5 retained current routing because the measured C4 envelope did not establish a general native hard limit or a concrete failure requiring promotion.
