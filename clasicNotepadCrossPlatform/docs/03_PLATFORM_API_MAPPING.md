# Cross-Platform API Mapping (No Dark Mode in v1)

Date: 2026-04-20

## Architecture Split

- `core/` (shared C++):
  - document model
  - encoding detection/conversion
  - line-ending detection/conversion
  - find/replace engine and text helpers
- `platform/macos/`:
  - AppKit windowing + menus + dialogs + printing
- `platform/linux/`:
  - GTK application/windowing + menus + dialogs + printing bridge
- `platform/windows/` (buildability target; optional at first):
  - minimal compile target/stub until full parity work starts

## Function Mapping Table

| Capability | Shared Core | macOS Native API | Linux Native API (Ubuntu desktop) |
|---|---|---|---|
| Main window + text area | no | `NSWindow` + `NSTextView` | `GtkApplicationWindow` + `GtkTextView` |
| Menu bar and actions | command IDs and dispatcher | `NSMenu`/`NSMenuItem` | `GMenuModel`/application actions (or classic GTK menu equivalents as needed) |
| Open/Save dialogs | path + io routines | `NSOpenPanel` / `NSSavePanel` | GTK file chooser/dialog APIs |
| Dirty prompt dialogs | message model | `NSAlert` | `GtkAlertDialog` or compatible dialog API |
| Page setup / print | print text layout helper | `NSPageLayout` / `NSPrintOperation` | `GtkPrintOperation` |
| Font chooser | font model | `NSFontPanel` | GTK font chooser dialog APIs |
| Clipboard | text abstraction | `NSPasteboard` (if needed directly) | `GdkClipboard` / GTK text buffer integration |
| Status bar | computed status text | `NSTextField` status area | `GtkLabel`/status widget |
| Spell check (optional v1 on Linux) | token ranges, UI hooks | `NSSpellChecker` / `NSTextView` integration | defer or optional Enchant-backed integration behind flag |

## Notes on Spell Checking

- macOS has first-party text/spell APIs in AppKit, so v1 macOS can include native spell services.
- Linux desktop environments typically rely on toolkit/library-based spell integrations; there is no single mandatory distro-wide spell service contract.
- Therefore:
  - v1 release criteria should not block on Linux spell support.
  - Add a capability layer (`ISpellService`) with `Available/Unavailable` runtime states.

## File/Encoding and Line Endings (Shared)

Keep all transformation logic platform-independent in shared C++ to guarantee parity:
- encoding sniff
- UTF conversions
- line-ending normalization + dominant-style save policy
- dirty-state transitions and document metadata

This minimizes platform divergence and reduces regression risk.

## Non-goals for v1

- Dark mode.
- Cross-platform rich text.
- Plugin system.
- Cloud/sync/telemetry.
