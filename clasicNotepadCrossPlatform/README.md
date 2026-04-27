# clasicNotepadCrossPlatform

This directory contains the cross-platform research and parity planning pack for Classic Notepad.

Current implementation status:

- Windows remains the full native Win32 app.
- Linux builds as the native GTK4 `ClassicNotepadGtk` target on Ubuntu/WSL.
- The shared JSON-lines automation suite now passes against both Windows Debug and Linux Debug binaries.
- Linux now uses optional GTK/libspelling British English spell checking when `libspelling-1-dev` and `hunspell-en-gb` are installed; missing packages or dictionaries remain graceful unavailable states.
- Shared appearance state is implemented for `System`, `Light`, and `Dark`, with the deterministic `CLASSIC_NOTEPAD_THEME` override.
- Linux GTK applies app-specific light/dark CSS classes and reports `appearanceTheme`, `effectiveAppearance`, `darkMode`, and `highContrast` through automation.
- macOS has an AppKit appearance helper for application/window overrides and semantic `NSTextView` colors; the full macOS editor target is still future adapter work.

Current contents:

- `docs/01_TECHNOLOGY_RESEARCH_AND_DECISION.md`
- `docs/02_CURRENT_CLASSIC_NOTEPAD_BEHAVIOR_SPEC.md`
- `docs/03_PLATFORM_API_MAPPING.md`
- `docs/04_IMPLEMENTATION_PLAN_MAC_FIRST.md`
- `docs/05_SPELLING_AND_DARK_MODE_PLAN.md`
- `docs/05_WINDOWS_FIRST_CROSS_PLATFORM_MIGRATION_PLAN.md`
- `docs/05_SPELLING_AND_DARK_MODE_PLAN.md`
- `docs/06_LINUX_FEATURE_PARITY_AND_AUTOMATION_PLAN.md`

Linux build environment setup is documented in `../docs/LINUX_BUILD_ENVIRONMENT.md`.
