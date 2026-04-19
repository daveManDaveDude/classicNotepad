# Spell Check Research And Implementation Plan

Research date: 2026-04-18

## Current App Context

Classic Notepad is a native Win32/C++17 app. The main editor is currently a multiline `EDIT` control created in `ClassicNotepadApp::CreateEditor` in `src/app.cpp`, and the app already subclasses the editor for status-bar updates.

That matters because the classic `EDIT` control does not expose per-word formatting. It is excellent for plain text, but misspelling underlines are not something it can display through a simple range-formatting API. We either need to draw the underlines ourselves, or replace the control with Rich Edit and use its built-in spell-checking support.

## Options Considered

| Option | Dependency burden | British English control | Underlines | Context menu suggestions | Fit |
| --- | --- | --- | --- | --- | --- |
| Windows Spell Checking API plus current `EDIT` control | No third-party runtime dependencies; uses Windows COM API | Strong. Create an `en-GB` checker explicitly and fail clearly if unavailable | Custom drawing required | Fully controlled by us through `ISpellChecker::Suggest` | Best production fit |
| Rich Edit built-in spell checking | No third-party dependencies; uses `Msftedit.dll` from Windows | Weaker. Rich Edit spell language is tied to Rich Edit language/input handling unless we manage language formatting carefully | Built in with `IMF_SPELLCHECKING` | Likely available through Rich Edit's default spell UI, but less controllable | Fastest prototype, higher behaviour risk |
| Hunspell | Adds library plus `.aff`/`.dic` dictionary files | Strong, if we ship `en_GB` | Custom drawing required | Custom menu required | Good fallback if Windows spell checker is unsuitable |
| Nuspell | Adds Nuspell plus ICU | Strong, with Hunspell dictionaries | Custom drawing required | Custom menu required | Too much dependency weight for this app |
| WPF/WinUI/WebView/Scintilla editor replacement | Large framework/editor dependency | Varies | Usually built in | Usually built in | Too heavy for a classic Notepad clone |

## Recommendation

Use the Windows Spell Checking API with the existing `EDIT` control and implement the underline/context-menu layer ourselves.

Reasons:

- It keeps the current editor behaviour, file handling, find/replace, word wrap, printing, and status bar mostly intact.
- It has no third-party library or dictionary files to ship.
- It lets us explicitly request British English with the BCP-47 language tag `en-GB`.
- It gives us spell errors, suggestions, add-to-dictionary, and ignore-for-session through the OS spell checker.
- It avoids converting the app into a Rich Edit based editor, which could introduce subtle plain-text behaviour differences.

The main cost is custom underline drawing. That is real work, but it is scoped and testable. If we want a quick spike before committing, prototype the Rich Edit route for one session and verify these exact behaviours: en-GB spelling, default context-menu suggestions, word wrap parity, line/column status, loading large files, undo, and printing.

## Relevant API Findings

- Windows provides a Spell Checking API from Windows 8 / Windows Server 2012 onward. It is a COM API intended for desktop C/C++ apps.
- `ISpellCheckerFactory` is the entry point. It can list supported languages, check whether a language is supported, and create an `ISpellChecker`.
- `ISpellChecker::Check` returns `IEnumSpellingError`; each `ISpellingError` has a UTF-16 start index, length, replacement text, and corrective action.
- `ISpellChecker::Suggest` returns spelling suggestions for a word.
- `ISpellChecker::Add` updates the user's dictionary; `ISpellChecker::Ignore` ignores a word for the current checker session.
- Microsoft documents user dictionaries under `%AppData%\Microsoft\Spelling\<language tag>`.
- Supported spell-checking languages are specific tags like `en-GB`, not neutral tags like `en`.
- Rich Edit can enable spell checking with `IMF_SPELLCHECKING` through `EM_GETLANGOPTIONS` / `EM_SETLANGOPTIONS`.
- Edit and Rich Edit controls support `EM_CHARFROMPOS` and `EM_POSFROMCHAR`, which we can use to map mouse points and character offsets to editor positions.

## Implementation Steps

### 1. Add A Spell-Checking Service

Add `src/spell_check.h` and `src/spell_check.cpp`, then add them to the `ClassicNotepad` target in `CMakeLists.txt`.

The service should own the Windows spell checker and hide COM details from `ClassicNotepadApp`.

Suggested shape:

```cpp
struct SpellingErrorRange {
    DWORD start = 0;
    DWORD length = 0;
    std::wstring replacement;
    CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
};

class SpellCheckService {
public:
    bool Initialize(const wchar_t* languageTag = L"en-GB");
    bool IsAvailable() const;
    std::vector<SpellingErrorRange> Check(const std::wstring& text);
    std::vector<std::wstring> Suggest(const std::wstring& word, std::size_t limit);
    void Ignore(const std::wstring& word);
    void Add(const std::wstring& word);
};
```

Implementation notes:

- Include `<spellcheck.h>`, `<objbase.h>`, and optionally `<wrl/client.h>` for `Microsoft::WRL::ComPtr`.
- Link `Ole32.lib` by adding `ole32` to `target_link_libraries(ClassicNotepad ...)`.
- Initialize COM on the UI thread with `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)` and call `CoUninitialize` only if we initialized it successfully.
- Treat an empty input string as "no spelling errors" before calling `ISpellChecker::Check`.
- Create the factory with:

```cpp
CoCreateInstance(
    __uuidof(SpellCheckerFactory),
    nullptr,
    CLSCTX_INPROC_SERVER,
    IID_PPV_ARGS(&factory));
```

- Call `factory->IsSupported(L"en-GB", &supported)` before `CreateSpellChecker`.
- Do not silently fall back to `en-US`; British English is a feature requirement. If `en-GB` is unavailable, disable spell checking and show a clear disabled context-menu item or one-time status message.
- Convert returned COM strings with `CoTaskMemFree`.

### 2. Cache Error Ranges In The App

Add state to `ClassicNotepadApp`:

- `SpellCheckService spellChecker_;`
- `std::vector<SpellingErrorRange> spellingErrors_;`
- `bool spellCheckAvailable_ = false;`
- `UINT_PTR spellCheckTimer_ = ...;`
- A generation counter if we later move checks off the UI thread.

Use UTF-16 offsets directly. The edit control APIs and Windows spell checker both operate naturally in UTF-16 code units, which avoids extra offset conversion.

### 3. Schedule Checks Without Blocking Typing

Do not spell-check on every `EN_CHANGE` synchronously.

Initial approach:

- On `EN_CHANGE`, restart a short timer, for example 300 ms.
- On timer, call `GetEditorText()`, run `spellChecker_.Check(text)`, update `spellingErrors_`, and invalidate the editor.
- Recheck after `SetEditorText`, file open, New, Replace All, Time/Date, word-wrap recreation, and language availability changes.

Performance guardrails:

- For the first version, full-document checks are acceptable for normal Notepad-sized files.
- Add a size threshold before release, for example defer or visible-window-only checking above 1 MB.
- If UI pauses are visible, move `Check` to a worker thread and post a `WM_APP_SPELLCHECK_COMPLETE` message with a generation id so stale results are ignored.

### 4. Draw Red Wavy Underlines

Handle `WM_PAINT` in `EditorWindowProc` after the original edit procedure has painted.

Drawing plan:

- Use `GetDC` or a paint-compatible DC after default painting.
- Select the current editor font.
- Calculate visible lines with `EM_GETFIRSTVISIBLELINE`, `EM_LINEFROMCHAR`, `EM_LINEINDEX`, and the editor client rect.
- For each cached spelling range that intersects visible text, map the range start/end with `EM_POSFROMCHAR`.
- If a misspelling wraps across lines, split the drawing by line.
- Draw a small red zigzag near the text baseline. Use line height from `GetTextMetrics`.
- Invalidate on scroll, resize, font change, word-wrap toggle, and spell-check result update.

This is the fiddliest part. Start with single-line misspellings and no horizontal scroll edge cases, then harden:

- Word wrap on and off.
- Horizontal scrolling.
- Misspellings at line ends.
- Very long words.
- Selection highlight over an underlined word.
- High DPI.

### 5. Add A Spell-Aware Context Menu

Intercept `WM_CONTEXTMENU` in `EditorWindowProc` before the default edit control handles it.

Behaviour:

- Convert the click point to editor client coordinates.
- Use `EM_CHARFROMPOS` to get the character index under the mouse. For keyboard-invoked context menus, use the caret location.
- Expand that index to a word range. Use a better spelling word rule than the existing find helper: letters, digits, apostrophes inside words, and optional internal hyphens.
- Check whether the word range overlaps a cached misspelling.
- If it is misspelled, call `spellChecker_.Suggest(word, 5)` and build a popup menu:
  - Suggestions as the first items.
  - Disabled `No spelling suggestions` if none are returned.
  - Separator.
  - `Ignore Once`.
  - `Add to Dictionary`.
  - Separator.
  - Standard edit commands: Undo, Cut, Copy, Paste, Delete, Select All.
- If it is not misspelled, show the standard edit commands only.

When a suggestion is chosen:

- `EM_SETSEL` to the misspelled range.
- `EM_REPLACESEL` with undo enabled.
- Let `EN_CHANGE` mark the document dirty and schedule a fresh check.

When `Ignore Once` is chosen:

- Call `ISpellChecker::Ignore`.
- Re-run or locally remove matching ranges for that word.

When `Add to Dictionary` is chosen:

- Call `ISpellChecker::Add`.
- Re-run spell checking so all matching instances clear.

### 6. British English UX

Default language tag: `en-GB`.

If `en-GB` is not supported on the machine:

- Leave editing fully functional.
- Disable spelling underlines and spelling menu entries.
- Provide a short message such as `British English spell checking is not installed for Windows.`
- Document that users can install English (United Kingdom) under Windows Settings > Time & language > Language & region. If Windows offers optional language features, include Basic typing.

Acceptance examples for en-GB:

- `colour` should be accepted.
- `color` should be flagged.
- `centre` should be accepted.
- `center` should be flagged.
- `recieve` should be flagged and suggest `receive`.

### 7. Optional Menu Toggle

Add a simple toggle only if we want user control:

- `Format > Spell Check` or `Edit > Spelling`.
- Persisting the setting is optional; this app currently keeps settings session-local.
- The default should be on when `en-GB` is available.

### 8. Tests And Verification

Automated:

- Unit-test word-range expansion, especially apostrophes, hyphens, underscores, and punctuation.
- Unit-test overlap between cached error ranges and context-menu clicked words.
- Keep COM-dependent spell checking tests optional because language availability depends on the Windows installation.

Manual:

- Build Debug and Release.
- Open a new document and type: `teh colour color centre center recieve`.
- Confirm underlines appear after a short delay.
- Right-click `teh` and apply a suggestion.
- Right-click `recieve`, choose `receive`, and verify undo works.
- Use `Ignore Once` and `Add to Dictionary`.
- Toggle word wrap and verify underline positions stay aligned.
- Scroll vertically and horizontally.
- Open a larger `.txt` file and verify typing remains responsive.
- Verify Save/Open output remains plain text with the existing encoding and line-ending behaviour.

## Rich Edit Prototype Steps

If we want to test the built-in Rich Edit path before custom drawing:

1. Load `Msftedit.dll` before creating the editor.
2. Replace the editor class name `L"EDIT"` with `MSFTEDIT_CLASS` from `<richedit.h>`.
3. Keep the existing multiline styles where compatible.
4. After creating the control, enable spell checking:

```cpp
LRESULT options = SendMessageW(editor, EM_GETLANGOPTIONS, 0, 0);
options |= IMF_SPELLCHECKING;
SendMessageW(editor, EM_SETLANGOPTIONS, 0, options);
```

5. Test every current editor feature, especially text retrieval, line/column calculation, word wrap recreation, undo, find/replace, printing, font selection, and default context menu behaviour.
6. Only choose this route if it gives reliable en-GB behaviour and does not disturb the plain Notepad feel.

## Sources

- Microsoft Learn: About the Spell Checking API: https://learn.microsoft.com/en-us/windows/win32/intl/about-the-spell-checker-api
- Microsoft Learn: `ISpellCheckerFactory`: https://learn.microsoft.com/en-us/windows/win32/api/spellcheck/nn-spellcheck-ispellcheckerfactory
- Microsoft Learn: `ISpellCheckerFactory::CreateSpellChecker`: https://learn.microsoft.com/en-us/windows/win32/api/spellcheck/nf-spellcheck-ispellcheckerfactory-createspellchecker
- Microsoft Learn: `ISpellChecker`: https://learn.microsoft.com/en-us/windows/win32/api/spellcheck/nn-spellcheck-ispellchecker
- Microsoft Learn: `ISpellChecker::Check`: https://learn.microsoft.com/en-us/windows/win32/api/spellcheck/nf-spellcheck-ispellchecker-check
- Microsoft Learn: `ISpellChecker::Suggest`: https://learn.microsoft.com/en-us/windows/win32/api/spellcheck/nf-spellcheck-ispellchecker-suggest
- Microsoft Learn: `ISpellChecker::Add`: https://learn.microsoft.com/en-us/windows/win32/api/spellcheck/nf-spellcheck-ispellchecker-add
- Microsoft Learn: `ISpellChecker::Ignore`: https://learn.microsoft.com/en-us/windows/win32/api/spellcheck/nf-spellcheck-ispellchecker-ignore
- Microsoft Learn: `ISpellingError`: https://learn.microsoft.com/en-us/windows/win32/api/spellcheck/nn-spellcheck-ispellingerror
- Microsoft Learn: `CORRECTIVE_ACTION`: https://learn.microsoft.com/en-us/windows/win32/api/spellcheck/ne-spellcheck-corrective_action
- Microsoft Learn: `CoCreateInstance`: https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cocreateinstance
- Microsoft Learn: Rich Edit controls: https://learn.microsoft.com/en-us/windows/win32/controls/about-rich-edit-controls
- Microsoft Learn: `EM_GETLANGOPTIONS`: https://learn.microsoft.com/en-us/windows/win32/controls/em-getlangoptions
- Microsoft Learn: `EM_SETLANGOPTIONS`: https://learn.microsoft.com/en-us/windows/win32/controls/em-setlangoptions
- Microsoft Learn: `EM_CHARFROMPOS`: https://learn.microsoft.com/en-us/windows/win32/controls/em-charfrompos
- Microsoft Learn: `EM_POSFROMCHAR`: https://learn.microsoft.com/en-us/windows/win32/controls/em-posfromchar
- Microsoft Support: Windows language settings: https://support.microsoft.com/en-us/windows/manage-the-language-and-keyboard-input-layout-settings-in-windows-12a10cb4-8626-9b77-0ccb-5013e0c7c7a2
- Hunspell project: https://github.com/hunspell/hunspell
- Hunspell English dictionaries from SCOWL: https://wordlist.aspell.net/hunspell-readme/
- `dictionary-en-gb` packaging and license reference: https://www.npmjs.com/package/dictionary-en-gb
- Nuspell project: https://nuspell.github.io/
