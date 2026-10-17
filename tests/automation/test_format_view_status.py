import ctypes
from ctypes import wintypes
import hashlib
import tempfile
import time
import unittest
import os
from contextlib import contextmanager
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


@contextmanager
def visible_automation_window():
    previous_visible = os.environ.get("CLASSIC_NOTEPAD_AUTOMATION_VISIBLE")
    previous_delay = os.environ.get("CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY")
    os.environ["CLASSIC_NOTEPAD_AUTOMATION_VISIBLE"] = "1"
    os.environ["CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY"] = "0.05"
    try:
        yield
    finally:
        if previous_visible is None:
            os.environ.pop("CLASSIC_NOTEPAD_AUTOMATION_VISIBLE", None)
        else:
            os.environ["CLASSIC_NOTEPAD_AUTOMATION_VISIBLE"] = previous_visible

        if previous_delay is None:
            os.environ.pop("CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY", None)
        else:
            os.environ["CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY"] = previous_delay


def send_visible_app_keys(app, *virtual_keys):
    if automation_platform() != "windows":
        raise AssertionError("Win32 menu key input is Windows-only")

    user32 = ctypes.windll.user32
    user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    user32.IsWindowVisible.argtypes = (wintypes.HWND,)
    user32.IsWindowVisible.restype = wintypes.BOOL
    user32.ShowWindow.argtypes = (wintypes.HWND, ctypes.c_int)
    user32.ShowWindow.restype = wintypes.BOOL
    user32.AllowSetForegroundWindow.argtypes = (wintypes.DWORD,)
    user32.AllowSetForegroundWindow.restype = wintypes.BOOL
    user32.SetForegroundWindow.argtypes = (wintypes.HWND,)
    user32.SetForegroundWindow.restype = wintypes.BOOL
    user32.SetActiveWindow.argtypes = (wintypes.HWND,)
    user32.SetActiveWindow.restype = wintypes.HWND
    user32.SetFocus.argtypes = (wintypes.HWND,)
    user32.SetFocus.restype = wintypes.HWND
    user32.BringWindowToTop.argtypes = (wintypes.HWND,)
    user32.BringWindowToTop.restype = wintypes.BOOL
    user32.AttachThreadInput.argtypes = (wintypes.DWORD, wintypes.DWORD, wintypes.BOOL)
    user32.AttachThreadInput.restype = wintypes.BOOL
    user32.GetForegroundWindow.restype = wintypes.HWND
    user32.keybd_event.argtypes = (wintypes.BYTE, wintypes.BYTE, wintypes.DWORD, ctypes.c_size_t)
    kernel32 = ctypes.windll.kernel32
    kernel32.GetCurrentThreadId.restype = wintypes.DWORD
    target_pid = app._process.pid
    candidates = []
    enum_proc_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = (enum_proc_type, wintypes.LPARAM)
    user32.EnumWindows.restype = wintypes.BOOL

    @enum_proc_type
    def collect_window(hwnd, _):
        process_id = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(process_id))
        if process_id.value == target_pid and user32.IsWindowVisible(hwnd):
            candidates.append(hwnd)
        return True

    user32.EnumWindows(collect_window, 0)
    if not candidates:
        raise AssertionError(f"No visible window found for automation process {target_pid}")
    hwnd = candidates[0]
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.AllowSetForegroundWindow(target_pid)
    target_thread = user32.GetWindowThreadProcessId(hwnd, None)
    foreground = user32.GetForegroundWindow()
    foreground_thread = user32.GetWindowThreadProcessId(foreground, None) if foreground else 0
    current_thread = kernel32.GetCurrentThreadId()
    attached = []
    for thread_id in (foreground_thread, target_thread):
        if thread_id and thread_id != current_thread and user32.AttachThreadInput(current_thread, thread_id, True):
            attached.append(thread_id)
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    user32.SetActiveWindow(hwnd)
    user32.SetFocus(hwnd)
    for thread_id in reversed(attached):
        user32.AttachThreadInput(current_thread, thread_id, False)
    time.sleep(0.1)
    if user32.GetForegroundWindow() != hwnd:
        raise AssertionError("The visible automation window could not be brought to the foreground")

    def key(vk, down):
        user32.keybd_event(vk, 0, 0 if down else 2, 0)

    for combination in virtual_keys:
        keys = combination if isinstance(combination, tuple) else (combination,)
        for vk in keys:
            key(vk, True)
        for vk in reversed(keys):
            key(vk, False)
        time.sleep(0.08)


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


class FormatViewStatusTests(unittest.TestCase):
    @unittest.skipUnless(automation_platform() == "windows", "Win32 native wrap scroll behavior")
    def test_native_word_wrap_recreation_resets_scroll_position(self):
        rows = [f"{row:03d} " + "x" * 200 for row in range(100)]
        source = "\r\n".join(rows)
        row_50_start = sum(len(row) + 2 for row in rows[:50])
        self.assertEqual(row_50_start, 10300)

        with visible_automation_window():
            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=30.0) as app:
                app.command("setWordWrap", enabled=False)
                app.command("setText", text=source)
                app.command("resizeEditorViewport", width=400, height=160)
                app.command("setSelection", start=row_50_start + 8, end=row_50_start + 8)
                app.command("sendEditorVerticalScroll", scrollCommand=4, trackPosition=50)

                def snapshot():
                    text = app.command("getText")["text"]
                    return {
                        "textHash": hashlib.sha256(text.encode("utf-16-le")).hexdigest(),
                        "selection": app.command("getSelection")["selection"],
                        "modified": app.command("isModified")["modified"],
                        "metadata": app.command("getDocumentMetadata")["metadata"],
                    }

                before = snapshot()
                before_windows = app.command("getEditorVisibleSpellWindows")["windows"]
                self.assertTrue(before_windows)
                self.assertEqual(before_windows[0]["start"], row_50_start, before_windows[:2])
                self.assertTrue(before_windows[0]["text"].startswith("050 "), before_windows[:2])

                # Native EDIT recreation retains document and selection state,
                # but Windows resets its vertical scroll position during wrap.
                app.command("invokeWordWrapMenuCommand")
                after = snapshot()
                after_windows = app.command("getEditorVisibleSpellWindows")["windows"]
                menu = app.command("getWordWrapMenuState")
                self.assertTrue(menu["checked"], menu)
                self.assertEqual(after, before)
                self.assertTrue(after_windows)
                self.assertLess(after_windows[0]["start"], row_50_start, after_windows[:2])
                self.assertNotIn("050 ", after_windows[0]["text"], after_windows[:2])

    @unittest.skipUnless(automation_platform() == "windows", "Win32 Format menu interaction")
    def test_word_wrap_real_menu_keyboard_state_and_native_undo_contract(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "menu-wrap-source.txt"
            path.write_bytes(b"alpha\r\n\xf0\x9f\x98\x80 beta\r\n")

            with visible_automation_window():
                with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=20.0) as app:
                    app.command("openFile", path=str(path))
                    app.command("setSelection", start=5, end=5)
                    app.command("insertText", text="!")
                    app.command("setSelection", start=2, end=7)

                    def snapshot():
                        text = app.command("getText")["text"]
                        return {
                            "text": text,
                            "sha256": hashlib.sha256(text.encode("utf-16-le")).hexdigest(),
                            "metadata": app.command("getDocumentMetadata")["metadata"],
                            "modified": app.command("isModified")["modified"],
                            "selection": app.command("getSelection")["selection"],
                        }

                    original = snapshot()
                    self.assertTrue(original["modified"])
                    self.assertEqual(original["metadata"]["lineEnding"], "Windows (CRLF)")
                    menu = app.command("getWordWrapMenuState")
                    self.assertTrue(menu["enabled"], menu)
                    self.assertFalse(menu["checked"], menu)
                    native_windows_before = app.command("getEditorVisibleSpellWindows")["windows"]
                    self.assertTrue(native_windows_before)

                    # Alt+O opens Format and W activates the existing menu item's
                    # mnemonic through TrackPopupMenuEx and WM_COMMAND.
                    for theme in ("light", "dark"):
                        app.command("setAppearanceTheme", theme=theme)
                        native_windows_before = app.command("getEditorVisibleSpellWindows")["windows"]
                        send_visible_app_keys(app, (0x12, 0x4F), 0x57)  # Alt+O, W
                        state = app.command("getWordWrapMenuState")
                        self.assertTrue(state["enabled"], state)
                        self.assertTrue(state["checked"], state)
                        self.assertTrue(state["activeEditorFocused"], state)
                        self.assertTrue(state["nativeEditorFocused"], state)
                        self.assertEqual(snapshot(), original)
                        native_windows_after = app.command("getEditorVisibleSpellWindows")["windows"]
                        self.assertTrue(native_windows_after)
                        self.assertNotEqual(
                            native_windows_after[0]["sessionId"], native_windows_before[0]["sessionId"])
                        current_tags = (
                            native_windows_after[0]["sessionId"],
                            native_windows_after[0]["contentRevision"],
                            native_windows_after[0]["layoutRevision"],
                        )
                        self.assertTrue(all((
                            window["sessionId"], window["contentRevision"], window["layoutRevision"]
                        ) == current_tags for window in native_windows_after))

                        # A second menu invocation through arrows and Enter
                        # selects the same command exactly once.
                        send_visible_app_keys(app, (0x12, 0x4F), 0x28, 0x0D)  # Alt+O, Down, Enter
                        state = app.command("getWordWrapMenuState")
                        self.assertTrue(state["enabled"], state)
                        self.assertFalse(state["checked"], state)
                        self.assertTrue(state["activeEditorFocused"], state)
                        self.assertEqual(snapshot(), original)

                        # Escape closes Format without dispatching its item.
                        send_visible_app_keys(app, (0x12, 0x4F), 0x1B)
                        state = app.command("getWordWrapMenuState")
                        self.assertFalse(state["checked"], state)
                        self.assertTrue(state["activeEditorFocused"], state)
                        self.assertEqual(snapshot(), original)

                    # Native wrap recreates EDIT and clears its native Undo
                    # buffer. Undo after the menu action must leave the edit
                    # intact instead of claiming it can be restored.
                    app.command("invokeWordWrapMenuCommand")
                    after_wrap = snapshot()
                    app.command("undo")
                    self.assertEqual(snapshot(), after_wrap)

    @unittest.skipUnless(automation_platform() == "windows", "Win32 mapped Format menu interaction")
    def test_word_wrap_real_menu_is_enabled_and_focuses_mapped_view(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            source_path = Path(temp_root) / "mapped-wrap-source.txt"
            source_units = 48 * 1024 * 1024
            block = b"x" * (1024 * 1024)
            with source_path.open("wb") as output:
                remaining = source_units
                while remaining:
                    chunk = min(remaining, len(block))
                    output.write(block[:chunk])
                    remaining -= chunk
                output.write(b"TAIL")
            source_digest = file_sha256(source_path)

            with visible_automation_window():
                with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=180.0) as app:
                    app.command("openFile", path=str(source_path))
                    deadline = time.monotonic() + 120.0
                    large = app.command("getLargeFileState")["largeFile"]
                    while not large["indexingComplete"] and time.monotonic() < deadline:
                        time.sleep(0.05)
                        large = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(large["active"], large)
                    self.assertTrue(large["indexingComplete"], large)
                    self.assertFalse(large["wordWrap"], large)
                    original_metadata = app.command("getDocumentMetadata")["metadata"]

                    menu = app.command("getWordWrapMenuState")
                    self.assertTrue(menu["enabled"], menu)
                    self.assertFalse(menu["checked"], menu)
                    self.assertTrue(menu["activeEditorFocused"], menu)
                    self.assertFalse(menu["nativeEditorFocused"], menu)

                    tail_start = large["characterCount"] - 4
                    self.assertEqual(
                        app.command("getLargeFileRange", start=tail_start, length=4)["text"], "TAIL")
                    app.command("setSelection", start=tail_start, end=tail_start)
                    app.command("insertText", text="!")
                    after_edit = app.command("getLargeFileState")["largeFile"]
                    self.assertEqual(after_edit["characterCount"], large["characterCount"] + 1)
                    selection_after_edit = app.command("getSelection")["selection"]
                    self.assertTrue(app.command("isModified")["modified"])
                    edited_metadata = app.command("getDocumentMetadata")["metadata"]
                    self.assertEqual(edited_metadata["path"], original_metadata["path"])
                    windows_before_wrap = app.command("getLargeFileVisibleWindows")["windows"]

                    app.command("setAppearanceTheme", theme="dark")
                    send_visible_app_keys(app, (0x12, 0x4F), 0x57)  # Alt+O, W
                    state = app.command("getLargeFileState")["largeFile"]
                    menu = app.command("getWordWrapMenuState")
                    self.assertTrue(state["wordWrap"], state)
                    self.assertEqual(state["caret"], after_edit["caret"])
                    self.assertTrue(menu["enabled"] and menu["checked"], menu)
                    self.assertTrue(menu["activeEditorFocused"], menu)
                    self.assertFalse(menu["nativeEditorFocused"], menu)
                    wrapped_windows = app.command("getLargeFileVisibleWindows")["windows"]
                    self.assertTrue(wrapped_windows)
                    self.assertEqual(wrapped_windows[0]["sessionId"], windows_before_wrap[0]["sessionId"])
                    self.assertEqual(wrapped_windows[0]["contentRevision"], windows_before_wrap[0]["contentRevision"])
                    self.assertGreater(wrapped_windows[0]["layoutRevision"], windows_before_wrap[0]["layoutRevision"])
                    revision = (
                        wrapped_windows[0]["sessionId"],
                        wrapped_windows[0]["contentRevision"],
                        wrapped_windows[0]["layoutRevision"],
                    )
                    self.assertTrue(all(
                        (window["sessionId"], window["contentRevision"], window["layoutRevision"])
                        == revision for window in wrapped_windows
                    ))

                    send_visible_app_keys(app, (0x12, 0x4F), 0x28, 0x0D)  # Alt+O, Down, Enter
                    state = app.command("getLargeFileState")["largeFile"]
                    menu = app.command("getWordWrapMenuState")
                    self.assertFalse(state["wordWrap"], state)
                    self.assertEqual(state["caret"], after_edit["caret"])
                    self.assertEqual(app.command("getSelection")["selection"], selection_after_edit)
                    self.assertTrue(menu["enabled"], menu)
                    self.assertFalse(menu["checked"], menu)
                    self.assertTrue(menu["activeEditorFocused"], menu)
                    self.assertFalse(menu["nativeEditorFocused"], menu)
                    self.assertTrue(app.command("isModified")["modified"])
                    self.assertEqual(app.command("getDocumentMetadata")["metadata"], edited_metadata)
                    self.assertEqual(
                        app.command("getLargeFileRange", start=tail_start, length=5)["text"], "!TAIL")

                    # The mapped document's Undo record survives wrap changes.
                    app.command("undo")
                    undone = app.command("getLargeFileState")["largeFile"]
                    self.assertEqual(undone["characterCount"], large["characterCount"])
                    self.assertEqual(
                        app.command("getLargeFileRange", start=tail_start, length=4)["text"], "TAIL")
                    self.assertFalse(app.command("isModified")["modified"])
                    self.assertEqual(app.command("getDocumentMetadata")["metadata"], original_metadata)
                    self.assertEqual(file_sha256(source_path), source_digest)

                    # New exits mapped mode and a later Open re-enters it;
                    # the same HMENU item stays available and tracks its check.
                    app.command("newDocument")
                    menu = app.command("getWordWrapMenuState")
                    self.assertFalse(menu["largeFileActive"], menu)
                    self.assertTrue(menu["enabled"], menu)
                    self.assertFalse(menu["checked"], menu)
                    app.command("openFile", path=str(source_path))
                    reopened = app.command("getLargeFileState")["largeFile"]
                    deadline = time.monotonic() + 120.0
                    while not reopened["indexingComplete"] and time.monotonic() < deadline:
                        time.sleep(0.05)
                        reopened = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(reopened["active"], reopened)
                    menu = app.command("getWordWrapMenuState")
                    self.assertTrue(menu["largeFileActive"], menu)
                    self.assertTrue(menu["enabled"], menu)
                    self.assertFalse(menu["checked"], menu)

    def test_word_wrap_and_status_bar_toggles_preserve_text(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("setText", text="a long line that should remain unchanged")

            app.command("setWordWrap", enabled=True)
            self.assertTrue(app.command("getWordWrap")["enabled"])
            app.command("setWordWrap", enabled=False)
            self.assertFalse(app.command("getWordWrap")["enabled"])

            app.command("setStatusBarVisible", visible=False)
            self.assertFalse(app.command("getStatusBarVisible")["visible"])
            app.command("setStatusBarVisible", visible=True)
            self.assertTrue(app.command("getStatusBarVisible")["visible"])

            self.assertEqual(app.command("getText")["text"], "a long line that should remain unchanged")

    def test_font_metadata_does_not_affect_saved_file_bytes(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "font.txt"

            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                app.command("setText", text="font test")
                app.command("setFont", font="Consolas 14")
                self.assertEqual(app.command("getFont")["font"], "Consolas 14")

                app.command("saveAs", path=str(path))
                self.assertEqual(path.read_bytes(), b"font test")

    def test_status_line_column_updates_after_caret_moves(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("setText", text="abc\r\ndef")
            app.command("setSelection", start=5, end=5)

            status = app.command("getStatusText")["statusText"]
            self.assertIn("Ln 2, Col 1", status)
            self.assertIn("7 characters", status)
            self.assertIn("Windows (CRLF)", status)
            self.assertIn("UTF-8", status)

    def test_linux_view_status_bar_menu_item_dismisses_after_toggle(self):
        if automation_platform() != "linux":
            self.skipTest("GTK menu popover dismissal is Linux-specific")

        with visible_automation_window():
            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                view = app.command("activateMenuLabel", label="View")
                self.assertTrue(view["activated"])
                self.assertGreater(view["openPopovers"], 0)

                status = app.command("activateMenuLabel", label="Status Bar")
                self.assertTrue(status["activated"])
                self.assertFalse(status["statusBarVisible"])
                self.assertEqual(status["openPopovers"], 0)

                self.assertEqual(app.command("getOpenMenuPopoverCount")["openPopovers"], 0)


if __name__ == "__main__":
    unittest.main()
