# clasicNotepadCrossPlatform

This directory contains the cross-platform research and parity planning pack for Classic Notepad.

Current implementation status:

- Windows remains the full native Win32 app.
- Linux builds as the native GTK4 `ClassicNotepadGtk` target on Ubuntu/WSL.
- The shared JSON-lines automation suite now passes against both Windows Debug and Linux Debug binaries.
- Linux v1 deliberately reports `spellCheck: false`; spelling commands are graceful no-ops until a native-friendly provider is chosen.
- Dark mode remains out of cross-platform v1 scope.

Current contents:

- `docs/01_TECHNOLOGY_RESEARCH_AND_DECISION.md`
- `docs/02_CURRENT_CLASSIC_NOTEPAD_BEHAVIOR_SPEC.md`
- `docs/03_PLATFORM_API_MAPPING.md`
- `docs/04_IMPLEMENTATION_PLAN_MAC_FIRST.md`
- `docs/05_WINDOWS_FIRST_CROSS_PLATFORM_MIGRATION_PLAN.md`
- `docs/06_LINUX_FEATURE_PARITY_AND_AUTOMATION_PLAN.md`

Linux build environment setup is documented in `../docs/LINUX_BUILD_ENVIRONMENT.md`.
