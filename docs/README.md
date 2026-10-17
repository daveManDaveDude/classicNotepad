# Project Notes

Long-form planning, research, and review notes live here so the repository root stays focused on the build entry points and top-level project metadata.

- `WINDOWS_RELIABILITY_STATUS.md`: current reviewed Windows Release evidence, links to C4/D6 measurements, implementation-item disposition, and open qualification gates.
- `WINDOWS_QUALIFICATION_BASELINE.md`: E13 current-host configure/build/CTest record, historical E10 Full identity mismatch, and open qualification gates.
- `WINDOWS_ACCEPTANCE_COVERAGE.md`: evidence and gaps for all 17 rows of the historical Section 6 Windows acceptance matrix; it does not close qualification gates.
- `WINDOWS_NATIVE_EDITOR_LIMITS.md`: controlled native EDIT long-row and production `WM_PASTE` measurements; real OS clipboard qualification remains open.
- `WINDOWS_PRINTING_QUALIFICATION.md`: production Page Setup/Print and Microsoft Print to PDF evidence for the recorded Windows 11 configuration; no physical printer is qualified.
- `WINDOWS_RELIABILITY_REVIEW_AND_PLAN.md`: historical 2 October 2026 Windows baseline, reproduced save/long-line/scrollbar findings, and ordered plan. Its historical findings are preserved; current dispositions are in `WINDOWS_RELIABILITY_STATUS.md`.
- `CLASSIC_NOTEPAD_BUILD_PLAN.md`: historical original Windows implementation plan and completion notes.
- `WIN11_NOTEPAD_DARK_PARITY_PLAN.md`: historical Windows appearance design plan; later state and evidence are linked from `WINDOWS_RELIABILITY_STATUS.md`.
- `../clasicNotepadCrossPlatform/README.md` and `../clasicNotepadCrossPlatform/docs/02_CURRENT_CLASSIC_NOTEPAD_BEHAVIOR_SPEC.md`: historical cross-platform research/status pack and its behavior baseline.
- `LINUX_BUILD_ENVIRONMENT.md`: WSL/Ubuntu packages, build commands, automation commands, and current Linux capability notes.
- `LINUX_SPELLING_FIX_PLAN.md`: Linux spell-checking diagnosis, library decision, and context-menu fix plan.
- `MACOS_BUILD_PLAN.md`: macOS AppKit implementation notes, build/package commands, and original Apple Silicon-first plan; it is separate from Windows qualification evidence.
- `MACOS_LARGE_FILE_TEST_HANDOFF.md`: Codex handoff for implementing and visibly accepting mapped large-document editing on macOS.
- `PERFORMANCE_SECURITY_REVIEW.md`: dated static performance/security review; its resolved labels are historical and are not current failure-mode qualification.
- Windows large-document architecture is summarized in the root `README.md`, including the balanced piece tree, bounded caches, undo policy, and Windows save/rebase flow; portable fallback save qualification is separate.
- `SPELL_CHECK_RESEARCH.md`: Windows spell checking API research.
- `WIN11_NOTEPAD_DARK_PARITY_PLAN.md`: Windows 11 Notepad dark-mode parity plan.
