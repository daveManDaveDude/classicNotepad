# Linux Spell Checking Fix Plan

Date: 2026-05-02

## Scope

Fix Linux spell-checking behavior without changing the working Windows spell-checking path.

The target symptom is: misspellings are underlined in the GTK editor, but the right-click menu does not offer useful spelling suggestions; the language submenu appears but does not seem to apply a working spelling action.

## Current Project State

- Windows uses `src/platform/windows/spell_check.cpp` plus custom Win32 context-menu handling. It is reported working and should stay out of scope.
- Linux uses GTK4, GtkSourceView, and optional `libspelling-1` in `src/platform/linux/gtk_spelling.cpp`.
- Linux chooses British English (`en_GB` / `en-GB` / `en_GB.UTF-8`) and has direct automation commands for `checkSpelling`, `suggestSpelling`, `ignoreSpelling`, and `addSpelling`.
- Linux underlines depend on `SpellingTextBufferAdapter`, so the visible underline behavior strongly suggests the backend and dictionary are present.
- The likely broken area is UI menu/action integration, not dictionary lookup.

## Library Decision

Keep `libspelling` as the Linux spell-checking integration library.

Reasons:

- It is the GNOME/GTK4-native spelling integration layer and explicitly supports `GtkSourceBuffer`, which this app already uses.
- It provides the exact surface the app needs: underlines through a text-buffer adapter, correction menu model, checker API, language selection, suggestions, ignore, and add-to-dictionary.
- It sits above Enchant and system dictionaries, which is the right Linux model for a native GTK app.
- Direct Hunspell would give dictionary checking and suggestions, but we would have to maintain GTK text tags, context-menu actions, language menu behavior, and dictionary discovery ourselves.
- Direct Enchant would reduce direct Hunspell coupling, but still leaves the GTK editor integration and context menu for us to build.
- Nuspell is a modern C++ spell checker, but adopting it would add a heavier new dependency and still require custom GTK UI integration.

Recommended stack:

```text
GtkSourceView editor
  -> libspelling SpellingTextBufferAdapter
  -> Enchant/provider layer
  -> system Hunspell British English dictionary, e.g. hunspell-en-gb
```

Fallback position: only reconsider direct Enchant/Hunspell if `libspelling` proves unavailable or unstable on target Linux distributions. For Ubuntu 24.04, `libspelling-1-dev` plus `hunspell-en-gb` remains the best fit.

## Probable Root Cause

The current app creates a `SpellingTextBufferAdapter` and reads its menu model:

- `spelling_text_buffer_adapter_new(...)`
- `spelling_text_buffer_adapter_get_menu_model(...)`

But libspelling's documented GTK integration also inserts the adapter as a widget action group:

```c
gtk_widget_insert_action_group(GTK_WIDGET(source_view), "spelling", G_ACTION_GROUP(adapter));
```

The current Linux app appears to append the libspelling menu model into a custom `GMenu`, then opens its own `GtkPopoverMenu`. If the `"spelling"` action group is not inserted into the text view/action hierarchy, visible menu items can exist but activating them has nowhere useful to dispatch.

There is a second likely issue: libspelling updates corrections based on the current cursor position. The current right-click handler opens the custom popover at the mouse point, but it does not visibly move or prepare the spelling adapter for the clicked word before showing the menu. That explains why underlines can work but suggestions are missing.

## Fix Strategy

### 1. Preserve The Windows Path

Do not touch:

- `src/platform/windows/spell_check.cpp`
- `src/platform/windows/spell_check.h`
- Windows-specific context-menu code in `src/platform/windows/app.cpp`

If shared tests are expanded, keep them platform-neutral or add Linux-only coverage guarded by the existing GTK/libspelling build flags.

### 2. Correct libspelling Attachment On Linux

Update the Linux spelling attachment so the adapter is connected to both the buffer and the view.

Recommended shape:

- Change `GtkSpellingService::Attach(GtkTextBuffer* buffer)` to also accept `GtkWidget* textView` or `GtkSourceView* view`.
- After creating the adapter, insert the action group:

```c
gtk_widget_insert_action_group(GTK_WIDGET(view), "spelling", G_ACTION_GROUP(adapter_));
```

- On detach/destruction, remove the action group:

```c
gtk_widget_insert_action_group(GTK_WIDGET(view), "spelling", nullptr);
```

- Keep the current capability checks so builds without `libspelling-1` remain graceful.

### 3. Rework The Linux Context Menu Path

Prefer the most GTK-native path first:

- Use `gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(textView_), spellingMenuModel)` for libspelling's menu model.
- Let GTK construct the text view's context menu so built-in text actions and libspelling actions resolve through the widget action hierarchy.

If Classic Notepad's custom context menu must be kept for parity:

- Keep the app's custom edit menu model.
- Still insert the `"spelling"` action group on `textView_`.
- Before opening the popover, call a new `GtkSpellingService::PrepareContextMenuAt(...)` helper that:
  - converts widget click coordinates to buffer coordinates,
  - gets the `GtkTextIter` at the clicked position,
  - places the cursor on or near the clicked misspelled word,
  - calls `spelling_text_buffer_adapter_update_corrections(adapter_)`,
  - then opens the popover.

This should make the correction section populate for the clicked misspelling rather than only showing a generic language submenu.

### 4. Keyboard Context Menu Support

GTK text views support keyboard context-menu activation. The final Linux behavior should also work for keyboard invocation, not just right-click.

Acceptance target:

- Right-click a misspelled word: suggestions appear.
- Press the context-menu key or `Shift+F10` while the caret is inside a misspelled word: suggestions appear.

This is another reason to prefer `gtk_text_view_set_extra_menu(...)` over a mouse-only custom popover, unless we add explicit keyboard handling.

### 5. Language Behavior

Keep British English as the default and do not silently fall back to US English.

Expected examples when the GB dictionary is active:

- `colour` accepted.
- `centre` accepted.
- `teh` flagged.
- `recieve` flagged and offers `receive` or another sensible correction.
- `color` and `center` may be flagged depending on the installed `en_GB` dictionary; the existing tests already avoid overcommitting here.

The language submenu should either:

- work through libspelling's own action group, or
- be removed/hidden if we choose not to support runtime language switching.

A submenu that opens but does nothing is worse than no submenu.

### 6. Diagnostics Before Coding

On Ubuntu/WSL, verify the environment before changing code:

```bash
pkg-config --modversion gtk4
pkg-config --modversion gtksourceview-5
pkg-config --modversion libspelling-1
enchant-2 -d en_GB -l
./build-ubuntu/LinuxSpellingProbe
```

Then run the semantic automation:

```powershell
wsl.exe -d Ubuntu-24.04 -- bash -lc "cd /mnt/c/vibe/classicNotepad && python3 tests/automation/run_automation_tests.py --binary build-ubuntu/ClassicNotepadGtk --platform linux"
```

If the probe and semantic automation pass but the UI menu fails, the fix should stay in GTK menu/action wiring.

## Test Plan

Automated:

- Keep `LinuxSpellingProbe` passing.
- Keep `tests/automation/test_spell_capability.py` passing on Linux.
- Add Linux-only coverage if practical for:
  - spelling action group is inserted on the editor widget when spell checking is available,
  - suggestions are non-empty for `teh`,
  - `ignoreSpelling` clears `teh` for the current session,
  - dry-run add-to-dictionary does not persist.

Manual UI:

1. Build Linux Debug with spelling packages installed.
2. Run `./build-ubuntu/ClassicNotepadGtk`.
3. Type `teh colour centre recieve`.
4. Confirm `teh` and `recieve` are underlined.
5. Right-click `teh`; confirm suggestions appear above the normal edit commands.
6. Pick a suggestion; confirm the word is replaced and Undo works.
7. Right-click `recieve`; confirm `Ignore Once` clears the current underline.
8. Use `Add to Dictionary` on a made-up word only in a disposable test environment.
9. Confirm `Shift+F10` / context-menu key works when the caret is inside a misspelled word.
10. Confirm Windows spelling behavior is unchanged by running the existing Windows automation suite.

## Implementation Order

1. Verify current Ubuntu spelling capability with the probe and automation.
2. Patch Linux-only attachment to insert/remove the `"spelling"` action group.
3. Prefer `gtk_text_view_set_extra_menu(...)` for the spelling menu; otherwise prepare the custom context menu at the clicked word and force `update_corrections`.
4. Rebuild Linux and rerun CTest plus automation.
5. Manually verify right-click and keyboard context-menu behavior.
6. Run a quick Windows build/test sanity pass only to prove the untouched Windows path still compiles.

## Sources

- libspelling namespace docs: https://gnome.pages.gitlab.gnome.org/libspelling/libspelling-1/
- libspelling README integration example: https://sources.debian.org/src/libspelling/0.4.8-1/README.md
- libspelling `SpellingTextBufferAdapter` docs: https://world.pages.gitlab.gnome.org/Rust/libspelling-rs/stable/latest/docs/libspelling/struct.TextBufferAdapter.html
- GTK `gtk_text_view_set_extra_menu`: https://docs.gtk.org/gtk4/method.TextView.set_extra_menu.html
- GTK `gtk_widget_insert_action_group`: https://docs.gtk.org/gtk4/method.Widget.insert_action_group.html
- GTK `gtk_text_view_get_iter_at_position`: https://docs.gtk.org/gtk4/method.TextView.get_iter_at_position.html
- GTK `gtk_text_view_window_to_buffer_coords`: https://docs.gtk.org/gtk4/method.TextView.window_to_buffer_coords.html
- GtkSourceView overview: https://gnome.pages.gitlab.gnome.org/gtksourceview/gtksourceview5/overview.html
- Hunspell overview: https://hunspell.github.io/
- Nuspell documentation: https://nuspell.github.io/documentation.html
