# Win11 Notepad Dark Parity Plan

## Purpose

Bring Classic Notepad closer to the Windows 11 Notepad dark-mode appearance shown in the comparison screenshot.

The requested differences to address are:

1. Add character count, line-ending, and encoding information to the status bar.
2. Make the bottom-right resize grip dark in dark mode.
3. Make scrollbars dark and show them only when the document does not fit in the current editor viewport.
4. Make the menu bar dark.

This plan assumes the existing dark-mode work remains in place, including:

- Detecting Windows app dark mode through `AppsUseLightTheme`.
- Avoiding direct `SetWindowTheme(..., "DarkMode_Explorer", ...)` calls because that path previously broke under the debugger in `uxtheme.dll`.
- Using DWM only for the title bar and explicit Win32 painting for app-owned areas.

## Screenshot Observations

Classic Notepad currently has:

- Dark title bar and editor.
- Light menu bar.
- Light standard scrollbars.
- Light status-bar resize grip.
- Status bar text with only `Ln`, `Col`.

Windows 11 Notepad shows:

- Dark app chrome and command/menu region.
- Status details such as `Ln 4, Col 1`, character count, line ending, and `UTF-8`.
- Dark low-contrast scrollbar treatment.
- Dark bottom status area with a matching resize corner.

## Implementation Strategy

Use app-owned drawing for the pieces that Windows classic controls do not darken reliably.

Do not reintroduce `SetWindowTheme` or `uxtheme` dark-mode theme names. Prefer predictable custom drawing and child controls.

## 1. Status Bar Details

### Desired Text

Update the status bar to display:

```text
Ln 4, Col 1 | 35 characters | Windows (CRLF) | UTF-8
```

Use this as the target order:

1. Caret position: `Ln N, Col N`
2. Character count: `N character` or `N characters`
3. Line endings:
   - `Windows (CRLF)`
   - `Unix (LF)`
   - `Macintosh (CR)`
   - `Mixed`
4. Encoding:
   - `UTF-8`
   - `UTF-8 with BOM`
   - `UTF-16 LE`
   - `ANSI`

Do not add `Plain text` or zoom unless requested later.

### Code Changes

Files:

- `src/document.h`
- `src/document.cpp`
- `src/app.cpp`
- `src/app.h`
- `tests/text_conversion_tests.cpp`

Steps:

1. Add document metadata getters:
   - `Document::Encoding() const`
   - `Document::LineEnding() const`
   - `Document::SaveLineEnding() const` if needed for mixed files.
2. Add small formatting helpers in `app.cpp`:
   - `FormatEncoding(Document::TextEncoding encoding)`
   - `FormatLineEnding(Document::LineEndingStyle lineEnding)`
   - `FormatCharacterCount(std::size_t count)`
3. In `UpdateStatusBar()`, build the full status text from:
   - current line/column
   - current editor text character count
   - current document line ending
   - current document encoding
4. Decide character-count semantics and keep them consistent:
   - Recommended: count user-visible text characters from `GetEditorText()`, treating each CRLF editor newline as one character.
   - Implementation: normalize `"\r\n"` to one logical newline for counting.
   - Include newline characters in the count because Windows Notepad appears to include them.
5. Preserve current dark-mode owner-draw status text path:
   - Keep storing the full status text in `statusBarText_`.
   - In dark mode, continue using `SBT_OWNERDRAW`.
   - In light mode, use standard `SB_SETTEXTW`.

### Tests

Add focused tests for:

- Default new document reports CRLF and UTF-8.
- Loaded LF file reports Unix (LF).
- Loaded UTF-8 BOM file reports UTF-8 with BOM.
- Loaded UTF-16 LE file reports UTF-16 LE.

The exact status-bar string is UI code, so core tests can cover document metadata and helper formatting if helpers are made testable.

## 2. Dark Resize Grip

### Problem

The status bar currently uses `SBARS_SIZEGRIP`, which lets the common control draw a light grip. In dark mode that clashes with the dark status bar.

### Desired Behavior

In dark mode:

- Draw a dark status bar background.
- Draw a subtle light-gray diagonal resize grip at the bottom-right corner.
- Preserve resize behavior when the user drags that bottom-right grip area.

In light mode:

- Keep the existing standard Windows grip if possible.

### Code Changes

Files:

- `src/app.cpp`
- `src/app.h`

Steps:

1. Stop using `SBARS_SIZEGRIP` when creating the status bar, or gate it so dark mode disables the standard grip.
2. Add a helper:
   - `RECT GetResizeGripRect() const`
3. Draw the dark-mode grip in the same owner-draw/status painting path used for dark status text:
   - Fill grip background with `kDarkStatusBackground`.
   - Draw three small diagonal lines using a muted gray, for example `RGB(140, 140, 140)`.
4. Add parent-window `WM_NCHITTEST` handling:
   - Convert the cursor point to client coordinates.
   - If it falls inside `GetResizeGripRect()` and the window is resizable, return `HTBOTTOMRIGHT`.
5. Recompute the grip rect after `WM_SIZE` and status bar visibility changes.

### Verification

- In dark mode, the bottom-right grip is not white.
- Dragging the custom grip resizes the window.
- Window edge resizing still works.
- Light mode remains visually normal.

## 3. Dark Scrollbars and Auto Visibility

### Interpretation

The request says “only show them if the current doc can be fully displayed in the current viewable window.” This plan interprets that as:

- Hide scrollbars when the document fully fits in the editor viewport.
- Show a vertical scrollbar only when not all lines fit.
- Show a horizontal scrollbar only when word wrap is off and at least one rendered line is wider than the viewport.

### Important Constraint

The current editor is a standard Win32 `EDIT` control with `WS_VSCROLL` and `WS_HSCROLL`. Standard non-client scrollbars do not reliably follow app dark mode without the theme APIs that already caused debugger crashes.

To make the scrollbars genuinely dark, do not rely on standard `EDIT` scrollbars.

### Recommended Approach

Replace the editor's built-in scrollbars with app-owned dark scrollbar controls.

Files:

- `src/app.cpp`
- `src/app.h`

Steps:

1. Create the `EDIT` control without `WS_VSCROLL` and without `WS_HSCROLL`.
2. Add two child windows owned by the app:
   - `verticalScrollBar_`
   - `horizontalScrollBar_`
3. Implement them as custom-drawn lightweight scrollbar windows, not standard scrollbar controls.
   - Register a small `ClassicNotepadScrollBar` window class.
   - Draw dark track/thumb in dark mode and system-like colors in light mode.
   - Support mouse drag, click track, mouse wheel routing, and keyboard scroll synchronization.
4. Add layout helpers:
   - `UpdateScrollBars()`
   - `UpdateScrollBarVisibility()`
   - `ResizeEditorAndScrollBars()`
5. Update visibility rules:
   - Vertical visible when total visible document height exceeds editor viewport height.
   - Horizontal visible when word wrap is off and widest visible/logical line exceeds viewport width.
   - Account for one scrollbar reducing the other axis's available size.
6. Sync scrolling:
   - On vertical scrollbar input, send `EM_LINESCROLL` to the editor.
   - On horizontal scrollbar input, send `WM_HSCROLL`/`EM_LINESCROLL` equivalent behavior as needed.
   - On editor `WM_VSCROLL`, `WM_HSCROLL`, mouse wheel, text change, font change, resize, word-wrap toggle, and file load, recompute thumb positions.
7. Preserve caret scrolling:
   - After text input, search, Go To, paste, and selection movement, let `EM_SCROLLCARET` run first, then refresh scrollbar state.
8. Keep spell-check underline coordinates correct:
   - Any custom scrollbar layout must leave the edit control client area accurately sized.
   - Existing underline drawing should continue to use editor client coordinates.

### Lower-Risk First Step

If the full custom scrollbar implementation is too large for one pass, split it:

1. First pass: dynamically show/hide the existing standard scrollbars based on content fit.
2. Second pass: replace those standard scrollbars with app-owned dark scrollbars.

This gives functional auto-visibility quickly while isolating the harder dark-scrollbar work.

### Verification

- Short document: no scrollbars visible.
- Many lines: vertical scrollbar visible.
- Long line with word wrap off: horizontal scrollbar visible.
- Long line with word wrap on: no horizontal scrollbar.
- Dark mode: track and thumb are dark, not white.
- Light mode: scrollbars remain readable.
- Mouse wheel, Page Up/Down, Home/End, typing, paste, Find, Replace, Go To, font change, and word-wrap toggle all keep scroll state correct.

## 4. Dark Menu Bar

### Problem

The current menu is a native Win32 menu loaded from `src/resources.rc`. Native menu bars are system-drawn and stay light in this app.

### Desired Behavior

In dark mode:

- The menu bar background is dark.
- Top-level menu labels are light.
- Hover/focus states are dark but visible.
- Opened popup menus are not glaring white if practical.

In light mode:

- Keep the classic native look.

### Recommended Approach

Replace the visible native menu bar with an app-owned menu strip in dark mode.

Files:

- `src/app.cpp`
- `src/app.h`
- `src/resources.rc`
- `src/resource.h`

Steps:

1. Keep the existing `HMENU` resource as the command source.
2. In dark mode, hide/remove the native menu bar from the window:
   - Store the loaded menu in a member such as `mainMenu_`.
   - Use a custom child window for the visual menu strip.
3. Add a `ClassicNotepadMenuBar` child window:
   - Height should match classic menu-bar height.
   - Paint dark background and top-level labels: `File`, `Edit`, `Format`, `View`, `Help`.
   - Track hover and keyboard focus.
4. On click or keyboard activation:
   - Use `TrackPopupMenuEx` with the corresponding submenu from `mainMenu_`.
   - Keep command IDs exactly the same so existing `WM_COMMAND` handling continues to work.
5. For popup menus:
   - First pass can allow native popup menus if speed matters.
   - Full parity pass should make popup menus owner-drawn or use a custom popup window to avoid light popups.
6. Update layout:
   - `ResizeEditor()` must reserve vertical space for the custom menu strip when it is visible.
   - Theme changes must swap between native menu bar and custom menu strip cleanly.
7. Keyboard behavior:
   - `Alt` focuses the menu strip.
   - Arrow keys move between top-level menus.
   - Escape closes menu mode.
   - Existing accelerators continue to work.

### Verification

- Dark mode: visible menu bar is dark.
- Light mode: native menu behavior remains unchanged.
- Menu commands still work.
- Alt-key navigation works.
- Accelerators such as Ctrl+O, Ctrl+S, Ctrl+F still work.
- Theme changes at runtime do not duplicate menu bars or leave blank layout space.

## Suggested Work Order

1. Status bar details.
2. Dark resize grip.
3. Scrollbar auto-visibility using existing scrollbars.
4. App-owned dark scrollbars.
5. Dark menu bar top-level strip.
6. Dark popup menus, if desired after the top-level strip is stable.

This order gives visible progress while keeping risky custom chrome work isolated.

## Verification Checklist

Run after each implementation phase:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\test.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Release
powershell -ExecutionPolicy Bypass -File .\scripts\test.ps1 -Configuration Release
```

Manual checks:

- Start in Windows dark app mode.
- Start in Windows light app mode.
- Toggle Windows app theme while Classic Notepad is running.
- Test high-contrast mode and confirm app does not override high-contrast colors.
- Open new, existing, missing, UTF-8 BOM, UTF-16 LE, LF, CRLF, and mixed-line-ending files.
- Resize the window from edges and from the bottom-right grip.
- Confirm no debugger break in `uxtheme.dll`.

## Risks

- Native Win32 menu bars and scrollbars are not easily darkened safely. Owner-drawn/custom replacements are the reliable path.
- Custom scrollbars are the largest change because they must stay synchronized with the edit control.
- Custom menu bars must preserve keyboard accessibility and accelerator behavior.
- Status-bar character count should be validated against Windows Notepad behavior, especially around CRLF and non-BMP Unicode characters.

## Non-Goals For This Pass

- Tabs.
- Windows 11 command bar buttons.
- Zoom UI.
- `Plain text` status item unless explicitly requested.
- Rounded Win11 app frame imitation.
- Reintroducing undocumented or crash-prone theme APIs.
