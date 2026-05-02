# Cross-Platform Spelling And Dark Mode Plan

Date: 2026-04-26  
Status: active implementation plan; Phases 4, 5, and 6 completed for the current source tree on 2026-04-26.

## Purpose

Add spelling and dark-mode support to the cross-platform Classic Notepad work while keeping the native-per-platform architecture:

- macOS: AppKit and `NSTextView`.
- Linux: GTK 4, validated on Ubuntu 24.04 in WSL and on a normal desktop session.
- Windows: keep parity with the current Win32 implementation, which already has Windows spell checking and custom dark-mode chrome.

This document intentionally extends the earlier v1 plan, which excluded dark mode and deferred Linux spelling. From this point forward, spelling and dark mode should be planned as first-class cross-platform parity items.

## Decision Summary

### Spelling

Use platform spell services where the platform has a strong native answer:

- Windows: Windows Spell Checking API, already implemented in the Win32 app.
- macOS: AppKit spelling through `NSTextView` and `NSSpellChecker`.
- Linux/GTK 4: use `libspelling` first, backed by Enchant and Hunspell dictionaries.

The Linux choice is `libspelling` rather than raw Hunspell because it is built for GTK 4, is packaged for Ubuntu 24.04, exposes a higher-level checker API, and has a GTK text-buffer integration path. The required dictionary should be installed as an OS package, not bundled in the repository.

Primary Ubuntu package set:

```bash
sudo apt-get update
sudo apt-get install -y libspelling-1-dev hunspell-en-gb
```

Expected transitive packages include `libspelling-1-1`, `libenchant-2-2`, `libgtk-4-dev`, and `libgtksourceview-5-dev`.

Local Ubuntu-24.04 WSL status on 2026-04-26:

- `libspelling-1-dev` installed successfully.
- `pkg-config --modversion libspelling-1` reports `0.2.0`.
- `pkg-config --modversion gtksourceview-5` reports `5.12.0`.
- `enchant-2 -d en_GB -l` flags `teh`, `recieve`, `color`, and `center` while accepting `colour` and `centre`.

Fallback if `libspelling-1-dev` is unavailable on a supported Linux target:

- Keep the same app-level `SpellService` interface.
- Implement a lower-level Enchant 2 backend.
- Use Hunspell dictionaries through Enchant.
- Accept custom underline/context-menu work only for that fallback.

Do not choose raw Hunspell as the first Linux implementation unless `libspelling` fails the spike. Raw Hunspell would make us own dictionary discovery, personal dictionary behavior, GTK underline painting, and suggestion-menu wiring.

### Dark Mode

Use native appearance behavior where it is reliable, with an app-level override for deterministic tests:

- Windows: preserve the current Win32 dark-mode implementation.
- macOS: rely on AppKit dark appearances and semantic colors; add an app/test override for Light, Dark, and System.
- Linux/GTK 4: apply an app CSS provider and theme state (`System`, `Light`, `Dark`) instead of relying on hidden WSL or desktop theme configuration.

Ubuntu 24.04 ships an older GTK 4 than the latest GTK docs, so the Linux implementation should not depend solely on GTK 4.20 `prefers-color-scheme` media queries. Use explicit CSS classes or provider swapping for Ubuntu 24.04, and optionally read newer GTK color-scheme settings when available.

## Non-Goals

- Do not bundle third-party dictionaries in the repo unless packaging later requires it.
- Do not require users or test machines to edit WSL config files.
- Do not require global `GTK_THEME` hacks for automated tests.
- Do not add grammar checking in this pass.
- Do not make Linux spelling a hard crash/fail path when dictionaries are missing.

## Architecture

Add a shared spelling capability boundary that each platform can satisfy differently:

```text
core/
  spelling/
    spell_result.h
    word_ranges.h
    word_ranges_tests.cpp

platform/windows/
  WindowsSpellService

platform/macos/
  MacSpellService

platform/linux/
  GtkSpellingService
```

Suggested service shape:

```cpp
enum class SpellCapability {
    Available,
    MissingBackend,
    MissingDictionary,
    DisabledByBuild
};

struct SpellIssue {
    std::size_t startUtf16 = 0;
    std::size_t lengthUtf16 = 0;
};

class ISpellService {
public:
    virtual ~ISpellService() = default;
    virtual SpellCapability Capability() const = 0;
    virtual std::vector<SpellIssue> CheckVisibleText(std::u16string_view text) = 0;
    virtual std::vector<std::u16string> Suggest(std::u16string_view word, std::size_t limit) = 0;
    virtual void IgnoreOnce(std::u16string_view word) = 0;
    virtual void AddToDictionary(std::u16string_view word) = 0;
};
```

Keep word-boundary logic in shared core so context-menu behavior is consistent across platforms.

## Phase 1: Linux Spelling Spike

Goal: prove `libspelling` is the right GTK 4 backend before wiring the whole feature.

Deliverables:

- Install Ubuntu/WSL packages:

```bash
sudo apt-get update
sudo apt-get install -y libspelling-1-dev hunspell-en-gb
```

- Add CMake detection:

```cmake
pkg_check_modules(SPELLING QUIET libspelling-1)
```

- Add a tiny Linux-only probe executable or test target that:
  - initializes GTK/libspelling,
  - lists or selects British English when available,
  - checks `teh colour centre recieve`,
  - confirms `teh` and `recieve` are misspellings,
  - confirms `colour` and `centre` are accepted for the GB dictionary,
  - records whether the installed dictionary flags or accepts US variants such as `color` and `center`.

Acceptance checks:

- `pkg-config --exists libspelling-1` succeeds.
- The probe compiles under Ubuntu 24.04.
- Missing dictionary packages produce `MissingDictionary`, not a crash.
- The team decides whether strict GB-vs-US variant enforcement is required; if the OS dictionary accepts US variants, that policy needs a supplemental project word list instead of relying only on the dictionary.
- The implementation path is documented before full UI work begins.

## Phase 2: macOS Spelling

Use `NSTextView` continuous spelling where it preserves normal plain-text editing behavior.

Deliverables:

- Enable continuous spell checking on the editor.
- Select a GB English language by enumerating `NSSpellChecker.availableLanguages`; do not assume the exact identifier until verified on the target machine.
- Keep autocorrect off by default so Classic Notepad remains a plain editor.
- Expose spelling suggestions through the normal AppKit context menu where practical.
- Add a status/capability path when GB English is not installed.

Acceptance checks:

- `colour` and `centre` are accepted.
- `teh` and `recieve` are flagged.
- `color` and `center` behavior is recorded; strict GB enforcement needs an explicit policy if the platform dictionary accepts them.
- Suggestions can replace a word and remain undoable.
- Save/open remains plain text with no rich-text artifacts.

## Phase 3: Linux GTK UI Spelling

Use the result of Phase 1:

- Preferred: `libspelling` plus a GTK text-buffer integration path.
- If needed, move the Linux editor from plain `GtkTextView` to `GtkSourceView` configured as a plain text editor, because the `libspelling` adapter is designed for `GtkSourceBuffer`.
- Keep syntax highlighting, line numbers, minimap, and editor extras disabled.

Deliverables:

- Runtime capability detection.
- GB English dictionary selection.
- Red misspelling underlines.
- Context-menu suggestions, Ignore Once, and Add to Dictionary.
- Feature remains disabled but harmless when packages or dictionaries are unavailable.

Acceptance checks:

- Same GB English examples as macOS.
- Same GB-vs-US variant policy decision as macOS.
- Context-menu replacement participates in undo.
- Word wrap, selection, scrolling, font changes, and save/open are unaffected.
- App starts and edits text when spelling dependencies are missing.

## Phase 4: Dark Mode State Model

Add one shared user-facing appearance state:

```text
System
Light
Dark
```

Platform adapters map that state to native behavior:

- Windows: existing app dark-mode detection and custom drawing.
- macOS: AppKit appearance, semantic colors, and `effectiveAppearance`.
- Linux: GTK CSS provider/classes and optional system color-scheme observation.

Add a deterministic test override:

```text
CLASSIC_NOTEPAD_THEME=system
CLASSIC_NOTEPAD_THEME=light
CLASSIC_NOTEPAD_THEME=dark
```

This keeps WSL validation independent of hidden desktop or WSL configuration.

Implementation completed 2026-04-26:

- Added `src/appearance.*` with shared parsing, labels, and dark-mode resolution for `System`, `Light`, and `Dark`.
- Invalid `CLASSIC_NOTEPAD_THEME` values fall back to `System`.
- Windows, Linux, and macOS helper code use the shared model.
- Automation reports `appearanceTheme`, `effectiveAppearance`, `darkMode`, and `highContrast`.
- Automation supports `getAppearance` and `setAppearanceTheme`.
- Core tests cover parsing, invalid values, labels, and high-contrast resolution.

## Phase 5: macOS Dark Mode

Deliverables:

- Replace fixed colors with AppKit semantic colors.
- Ensure editor, status bar, menus, dialogs, and scrollbars remain readable in light and dark appearances.
- Keep printed output light/readable regardless of app chrome.
- Add a manual or automated launch path for forced dark/light testing.

Acceptance checks:

- System dark mode is followed.
- Forced `CLASSIC_NOTEPAD_THEME=dark` works without changing global macOS settings.
- Selection, caret, misspelling underlines, status text, and disabled menu items are readable.

Implementation completed 2026-04-26:

- Added `src/platform/macos/mac_appearance.*`.
- The helper applies `System`, `Light`, and `Dark` through AppKit `NSAppearance` at application or window scope.
- Plain `NSTextView` appearance uses semantic AppKit colors: `textBackgroundColor`, `textColor`, and text-color insertion point.
- `tests/macos_spelling_compile.mm` now compiles both spelling and appearance helper paths.
- The repository now contains the full `ClassicNotepadMac.app` editor target. Editor-wide macOS visual verification can be performed against the AppKit app with `CLASSIC_NOTEPAD_THEME=system|light|dark`.

## Phase 6: Linux Dark Mode

Deliverables:

- Add a GTK CSS provider for app-specific editor/status styling.
- Apply `classic-light` or `classic-dark` style classes at the app root.
- Use semantic GTK colors where possible and explicit colors only where needed for Notepad parity.
- For GTK 4.20+, optionally read `GtkSettings:gtk-interface-color-scheme`.
- For Ubuntu 24.04, keep the explicit app override path because GTK 4.20 color-scheme media queries are not guaranteed.

Acceptance checks:

- `CLASSIC_NOTEPAD_THEME=dark` renders a dark editor/status/menu surface in WSL without editing WSL config.
- `CLASSIC_NOTEPAD_THEME=light` renders a light surface.
- `CLASSIC_NOTEPAD_THEME=system` follows the desktop when available.
- High-contrast themes are not overridden with unreadable custom colors.
- GTK file/font/print dialogs remain usable.

Implementation completed 2026-04-26:

- `ClassicNotepadGtk` reads `CLASSIC_NOTEPAD_THEME` and applies `classic-light` or `classic-dark` CSS classes at the app root.
- The GTK CSS provider styles the editor, status bar, and menu bar with explicit Notepad-compatible light/dark colors.
- `System` reads `GtkSettings:gtk-interface-color-scheme` when available, falls back to `gtk-application-prefer-dark-theme`, then dark GTK theme names.
- High-contrast GTK theme names suppress forced custom light/dark colors and are reported through automation.
- The View menu now includes an Appearance submenu with System, Light, and Dark commands.
- The shared automation suite covers dark, light, system, invalid environment values, and runtime `setAppearanceTheme`.

## Phase 7: Build And Test Without Hidden WSL Config

WSL should be a reproducible validation environment, not a secret dependency.

Required WSL/Ubuntu setup command:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config libgtk-4-dev libspelling-1-dev hunspell-en-gb
```

Recommended repository script:

```text
scripts/setup-ubuntu-spelling.ps1
```

The script should:

- target the configured Ubuntu distro, defaulting to `Ubuntu-24.04`;
- install only documented apt packages;
- verify `pkg-config --exists gtk4`;
- verify `pkg-config --exists libspelling-1`;
- verify the GB dictionary package is installed;
- print the exact next build/test commands.

No WSL config file edits should be required.

## Test Matrix

Automated:

- Shared core unit tests on Windows, macOS, and Ubuntu.
- Word-boundary tests for apostrophes, hyphens, punctuation, CRLF/LF text, and UTF-16 offsets.
- Linux compile test with spelling enabled.
- Linux compile test with spelling disabled or dependency missing.
- Theme-state unit tests for `System`, `Light`, `Dark`, and invalid env values.
- Windows, Linux, and macOS automation tests for `CLASSIC_NOTEPAD_THEME=system|light|dark`, invalid fallback, and runtime theme switching.

Manual:

- macOS light and dark launch.
- Ubuntu desktop light and dark launch.
- WSL launch with `CLASSIC_NOTEPAD_THEME=dark`.
- Spelling sample: `teh colour centre recieve`, plus `color center` for GB-vs-US policy recording.
- Context-menu suggestions, Ignore Once, Add to Dictionary.
- Word wrap, font change, find/replace, status bar, open/save, print/page setup.

## Suggested Work Order

1. Add this scope to the cross-platform README and update older docs that say dark mode is out of scope.
2. Install/verify Linux spelling packages in WSL.
3. Add the shared spelling service interface and word-range tests.
4. Spike `libspelling` in a small Linux test target.
5. Add macOS spelling through `NSTextView`/`NSSpellChecker`.
6. Add Linux spelling UI integration.
7. Add the shared appearance state and test override. Completed 2026-04-26.
8. Add macOS dark-mode polish. Completed first as an AppKit helper, then verified through the `ClassicNotepadMac.app` target.
9. Add Linux CSS-based dark mode. Completed 2026-04-26.
10. Run the full test matrix on Windows, macOS, Ubuntu desktop, and WSL.

## Open Questions

Resolved 2026-04-26:

- Linux should stay on the current text-view path unless the `libspelling` adapter requires `GtkSourceView`. The current implementation already supports optional spelling and dark-mode CSS without turning on editor extras.
- Spelling should be enabled by default when the requested backend and dictionary exist. Missing backend/dictionary remains a capability state, not an error.
- The app should prefer GB English for parity tests and current product behavior. User-language selection is a future preference feature, not part of this pass.
- The first automated dark-mode check is semantic automation: appearance state, effective appearance, invalid fallback, and runtime switching. Screenshot and accessibility color checks should be added later as a visual smoke layer, with accessibility contrast checks preferred before brittle pixel assertions.

## References

- Apple `NSSpellChecker`: https://developer.apple.com/documentation/appkit/nsspellchecker
- Apple `NSTextView.isContinuousSpellCheckingEnabled`: https://developer.apple.com/documentation/appkit/nstextview/iscontinuousspellcheckingenabled
- Apple macOS appearance guidance: https://developer.apple.com/documentation/appkit/choosing-a-specific-appearance-for-your-macos-app
- GNOME libspelling API: https://gnome.pages.gitlab.gnome.org/libspelling/libspelling-1/
- GNOME GTK 4 API: https://docs.gtk.org/gtk4/
- GTK `GtkCssProvider:prefers-color-scheme`: https://docs.gtk.org/gtk4/property.CssProvider.prefers-color-scheme.html
- GTK `GtkSettings:gtk-interface-color-scheme`: https://docs.gtk.org/gtk4/property.Settings.gtk-interface-color-scheme.html
- GTK `gtk-application-prefer-dark-theme` deprecation note: https://docs.gtk.org/gtk4/property.Settings.gtk-application-prefer-dark-theme.html
- Ubuntu `libspelling-1-dev` package: https://launchpad.net/ubuntu/noble/arm64/libspelling-1-dev/0.2.0-2build3
- Ubuntu `hunspell-en-gb` package: https://code.launchpad.net/ubuntu/noble/+package/hunspell-en-gb
