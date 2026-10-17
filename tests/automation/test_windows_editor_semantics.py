import ctypes
from ctypes import wintypes
import os
import tempfile
import time
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver
from editor_semantics_fixtures import (
    MIXED_SOURCE_TEXT,
    MAPPED_PREFIX_BYTES,
    utf16_units,
    write_mapped_comparison_fixture,
)


def displayed_count(text: str) -> int:
    count = 0
    index = 0
    while index < len(text):
        count += 1
        if text[index] == "\r" and index + 1 < len(text) and text[index + 1] == "\n":
            index += 1
        index += 1
    return count


def status_count_text(count: int) -> str:
    return f"{count:,} {'character' if count == 1 else 'characters'}"


@unittest.skipUnless(automation_platform() == "windows", "Windows native/mapped editor parity")
class WindowsEditorSemanticsTests(unittest.TestCase):
    def test_native_and_mapped_comparison_fixture_has_pinned_source_coordinates(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "native-mapped-semantics.txt"
            suffix_start = write_mapped_comparison_fixture(path)
            suffix_length = utf16_units(MIXED_SOURCE_TEXT)

            with ClassicNotepadDriver(automation_binary(), "windows", timeout=180.0) as app:
                app.command("setWordWrap", enabled=False)
                app.command("setText", text=MIXED_SOURCE_TEXT)
                self.assertFalse(app.command("getLargeFileState")["largeFile"]["active"])
                self.assertEqual(app.command("getText")["text"], MIXED_SOURCE_TEXT)

                app.command("openFile", path=str(path))
                deadline = time.monotonic() + 120.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.05)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["active"], state)
                self.assertTrue(state["indexingComplete"], state)
                self.assertEqual(state["fileSize"], MAPPED_PREFIX_BYTES +
                    len(MIXED_SOURCE_TEXT.encode("utf-8")))
                self.assertEqual(
                    app.command("getLargeFileRange", start=suffix_start, length=suffix_length)["text"],
                    MIXED_SOURCE_TEXT,
                )

    @unittest.skipUnless(automation_platform() == "windows", "Windows active selection endpoint parity")
    def test_reverse_shift_selection_reports_active_caret_in_native_and_mapped_status(self):
        suffix = "a\r\nb\r\nc"
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            mapped_path = Path(temp_root) / "mapped-reverse-selection.txt"
            suffix_start = write_mapped_comparison_fixture(mapped_path, suffix)

            with ClassicNotepadDriver(automation_binary(), "windows", timeout=180.0) as app:
                for engine in ("native", "mapped"):
                    with self.subTest(engine=engine):
                        if engine == "native":
                            app.command("setText", text=suffix)
                            base = 0
                        else:
                            app.command("openFile", path=str(mapped_path))
                            deadline = time.monotonic() + 120.0
                            state = app.command("getLargeFileState")["largeFile"]
                            while not state["indexingComplete"] and time.monotonic() < deadline:
                                time.sleep(0.05)
                                state = app.command("getLargeFileState")["largeFile"]
                            self.assertTrue(state["indexingComplete"], state)
                            base = suffix_start

                        app.command("setSelection", start=base + 6, end=base + 6)
                        command = "sendEditorKey" if engine == "native" else "sendLargeFileKey"
                        app.command(command, key=0x25, shift=True)
                        self.assertEqual(app.command("getSelection")["selection"],
                            {"start": base + 4, "end": base + 6})
                        status = app.command("getStatusText")["statusText"]
                        self.assertIn("Ln 2, Col 2", status)

                        if engine == "mapped":
                            state = app.command("getLargeFileState")["largeFile"]
                            self.assertEqual(state["caret"], base + 4)
                        else:
                            dialog = app.command("probeGoToDialogDefault")
                            self.assertEqual(dialog["initialLine"], "2")

    def test_native_mouse_selection_preserves_shift_anchor_and_modal_caret(self):
        user32 = ctypes.windll.user32
        enum_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user32.EnumWindows.argtypes = (enum_type, wintypes.LPARAM)
        user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
        user32.GetDlgItem.argtypes = (wintypes.HWND, ctypes.c_int)
        user32.GetDlgItem.restype = wintypes.HWND
        user32.SendMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
        user32.SendMessageW.restype = ctypes.c_ssize_t
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="a\r\nb\r\nc")
            editors = []
            @enum_type
            def find_editor(window, _):
                process = wintypes.DWORD()
                user32.GetWindowThreadProcessId(window, ctypes.byref(process))
                if process.value == app._process.pid:
                    editor = user32.GetDlgItem(window, 1001)
                    if editor:
                        editors.append(editor)
                return True
            user32.EnumWindows(find_editor, 0)
            self.assertEqual(len(editors), 1)
            editor = editors[0]
            # Initial anchor/active, down/up offsets, Shift, expected bounds and active line.
            cases = (
                (0, 3, 6, 6, True, 0, 6, 3),
                (6, 3, 0, 0, True, 0, 6, 1),
                (0, 3, 3, 6, True, 0, 6, 3),
                (6, 3, 3, 0, True, 0, 6, 1),
                (3, 6, 0, 0, True, 0, 3, 1),
                (3, 6, 0, 6, True, 3, 6, 3),
                (0, 3, 6, 6, False, 6, 6, 3),
                (0, 3, 6, 0, False, 0, 6, 1),
                (6, 3, 0, 6, False, 0, 6, 3),
            )
            for anchor, active, down, up, shift, start, end, line in cases:
                with self.subTest(anchor=anchor, active=active, down=down, up=up, shift=shift):
                    app.command("setSelection", start=anchor, end=active)
                    points = [app.command("getVisibleTextPositionPoint", position=position)
                        for position in (down, up)]
                    packed = [(point["x"] & 0xffff) | ((point["y"] & 0xffff) << 16)
                        for point in points]
                    modifiers = 4 if shift else 0  # MK_SHIFT
                    user32.SendMessageW(editor, 0x201, 1 | modifiers, packed[0])
                    if down != up:
                        user32.SendMessageW(editor, 0x200, 1 | modifiers, packed[1])
                    user32.SendMessageW(editor, 0x202, modifiers, packed[1])
                    self.assertEqual(app.command("getSelection")["selection"], {"start": start, "end": end})
                    self.assertIn(f"Ln {line}, Col 1", app.command("getStatusText")["statusText"])
                    def snapshot():
                        return (
                            app.command("getText")["text"],
                            app.command("getSelection")["selection"],
                            app.command("getStatusText")["statusText"],
                            app.command("getDocumentMetadata")["metadata"],
                            app.command("isModified")["modified"],
                        )
                    before = snapshot()
                    self.assertEqual(app.command("probeGoToDialogDefault")["initialLine"], str(line))
                    self.assertEqual(snapshot(), before)

    def test_native_replacement_and_undo_reconcile_active_selection_endpoint(self):
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            for command in ("insertText", "typeText"):
                with self.subTest(command=command):
                    app.command("setText", text="abcdef")
                    app.command("setSelection", start=2, end=5)
                    app.command(command, text="X")
                    self.assertEqual(app.command("getText")["text"], "abXf")
                    self.assertEqual(app.command("getSelection")["selection"], {"start": 3, "end": 3})
                    self.assertIn("Ln 1, Col 4", app.command("getStatusText")["statusText"])
                    self.assertEqual(app.command("probeGoToDialogDefault")["initialLine"], "1")
                    app.command("undo")
                    self.assertEqual(app.command("getText")["text"], "abcdef")
                    self.assertEqual(app.command("getSelection")["selection"], {"start": 2, "end": 5})
                    self.assertIn("Ln 1, Col 6", app.command("getStatusText")["statusText"])
                    self.assertEqual(app.command("probeGoToDialogDefault")["initialLine"], "1")
                    app.command("sendEditorKey", key=0x27)
                    self.assertEqual(app.command("getSelection")["selection"], {"start": 6, "end": 6})

            # A no-op Undo preserves a reverse selection after WM_SETTEXT clears Undo.
            app.command("setText", text="a\r\nb\r\nc")
            app.command("setSelection", start=6, end=6)
            app.command("sendEditorKey", key=0x25, shift=True)
            app.command("undo")
            self.assertIn("Ln 2, Col 2", app.command("getStatusText")["statusText"])
            self.assertEqual(app.command("probeGoToDialogDefault")["initialLine"], "2")

            # A restored selection spanning lines also pins the dialog's active line.
            app.command("setText", text="a\r\nb\r\nc")
            app.command("setSelection", start=6, end=6)
            app.command("sendEditorKey", key=0x25, shift=True)
            app.command("insertText", text="X")
            self.assertIn("Ln 2, Col 3", app.command("getStatusText")["statusText"])
            self.assertEqual(app.command("probeGoToDialogDefault")["initialLine"], "2")
            app.command("undo")
            self.assertEqual(app.command("getText")["text"], "a\r\nb\r\nc")
            self.assertEqual(app.command("getSelection")["selection"], {"start": 4, "end": 6})
            self.assertIn("Ln 3, Col 1", app.command("getStatusText")["statusText"])
            self.assertEqual(app.command("probeGoToDialogDefault")["initialLine"], "3")

    def test_failed_candidate_adoption_preserves_reverse_orientation_and_undo(self):
        previous = os.environ.get("CLASSIC_NOTEPAD_TEST_EDITOR_ADOPTION_FAILURE")
        os.environ["CLASSIC_NOTEPAD_TEST_EDITOR_ADOPTION_FAILURE"] = "partial"
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                path = Path(temp_root) / "replacement.txt"
                path.write_text("replacement text", encoding="utf-8")
                with ClassicNotepadDriver(automation_binary(), "windows") as app:
                    app.command("setWordWrap", enabled=False)
                    app.command("setText", text="a\r\nb\r\nc")
                    app.command("setSelection", start=7, end=7)
                    app.command("typeText", text="!")
                    app.command("setSelection", start=6, end=6)
                    app.command("sendEditorKey", key=0x25, shift=True)
                    def snapshot():
                        return {name: app.command(name) for name in (
                            "getText", "getSelection", "getDocumentMetadata", "isModified",
                            "getWordWrap", "getStatusText")}
                    before = snapshot()
                    self.assertIn("Ln 2, Col 2", before["getStatusText"]["statusText"])
                    for operation in ("wrap", "open"):
                        with self.subTest(operation=operation):
                            if operation == "wrap":
                                app.command("setWordWrap", enabled=True)
                            else:
                                self.assertFalse(app.try_command("openFile", path=str(path)).get("ok", False))
                            # Responses carry request IDs, so compare their payloads.
                            after = snapshot()
                            for name in before:
                                self.assertEqual({k: v for k, v in after[name].items() if k != "id"},
                                    {k: v for k, v in before[name].items() if k != "id"})
                            self.assertEqual(app.command("probeGoToDialogDefault")["initialLine"], "2")
                            self.assertEqual(app.command("getSelection")["selection"], {"start": 4, "end": 6})
                    app.command("undo")
                    self.assertEqual(app.command("getText")["text"], "a\r\nb\r\nc")
                    self.assertIn("Ln 3, Col 2", app.command("getStatusText")["statusText"])
                    self.assertEqual(app.command("probeGoToDialogDefault")["initialLine"], "3")
        finally:
            if previous is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_EDITOR_ADOPTION_FAILURE", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_EDITOR_ADOPTION_FAILURE"] = previous

    @unittest.skipUnless(automation_platform() == "windows", "Windows mapped reverse drag caret orientation")
    def test_mapped_reverse_drag_keeps_active_caret_across_unicode_lines(self):
        suffix = "A😀B\r\nC漢D\r\nE"
        suffix_units = utf16_units(suffix)
        prefix = b"x" * (1024 * 1024)
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            mapped_path = Path(temp_root) / "mapped-reverse-drag.txt"
            with mapped_path.open("wb") as output:
                for _ in range(MAPPED_PREFIX_BYTES // len(prefix)):
                    output.write(prefix)
                output.write(b"\r\n")
                output.write(suffix.encode("utf-8"))
            suffix_start = MAPPED_PREFIX_BYTES + 2

            with ClassicNotepadDriver(automation_binary(), "windows", timeout=180.0) as app:
                for engine in ("native", "mapped"):
                    with self.subTest(engine=engine):
                        if engine == "native":
                            app.command("setText", text=suffix)
                            base = 0
                        else:
                            app.command("openFile", path=str(mapped_path))
                            deadline = time.monotonic() + 120.0
                            state = app.command("getLargeFileState")["largeFile"]
                            while not state["indexingComplete"] and time.monotonic() < deadline:
                                time.sleep(0.05)
                                state = app.command("getLargeFileState")["largeFile"]
                            self.assertTrue(state["indexingComplete"], state)
                            base = suffix_start

                        earlier = base + utf16_units("A😀")
                        later = base + suffix_units - 1
                        app.command("setSelection", start=later, end=later)
                        start_point = app.command("getVisibleTextPositionPoint", position=later)
                        end_point = app.command("getVisibleTextPositionPoint", position=earlier)
                        if engine == "native":
                            dragged = app.command("dragEditor",
                                startX=start_point["x"], startY=start_point["y"],
                                endX=end_point["x"], endY=end_point["y"])["selection"]
                        else:
                            large = app.command("sendLargeFileDrag",
                                startX=start_point["x"], startY=start_point["y"],
                                endX=end_point["x"], endY=end_point["y"])["largeFile"]
                            dragged = {"start": large["selectionStart"], "end": large["selectionEnd"]}
                            self.assertEqual(large["caret"], earlier)
                        self.assertEqual(dragged, {"start": earlier, "end": later})
                        status = app.command("getStatusText")["statusText"]
                        self.assertIn("Ln 2, Col 4" if engine == "mapped" else "Ln 1, Col 4", status)
                        actual = app.command("getLargeFileRange", start=base,
                            length=suffix_units)["text"] if engine == "mapped" else app.command("getText")["text"]
                        self.assertEqual(actual, suffix)

    @unittest.skipUnless(automation_platform() == "windows", "Windows native/mapped CJK word-navigation parity")
    def test_cjk_word_navigation_matches_native_with_scalar_safe_shift_extension(self):
        samples = (
            ("Café café 漢字 abc", {9: 11, 10: 11, 11: 13}, {11: 10, 12: 11, 13: 11}),
            ("漢字漢", {0: 1, 1: 2, 2: 3}, {1: 0, 2: 1, 3: 2}),
            ("A漢B", {0: 1, 1: 2, 2: 3}, {1: 0, 2: 1, 3: 2}),
            ("😀漢?漢😀", {0: 2, 2: 4, 3: 4, 4: 5, 5: 7}, {2: 0, 3: 2, 4: 2, 5: 4, 7: 5}),
        )
        mapped_offsets = []
        mapped_text = ""
        for text, _, _ in samples:
            mapped_offsets.append(utf16_units(mapped_text))
            mapped_text += text + "\r\n"
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            mapped_path = Path(temp_root) / "mapped-cjk-navigation.txt"
            mapped_base = write_mapped_comparison_fixture(mapped_path, mapped_text)
            with ClassicNotepadDriver(automation_binary(), "windows", timeout=180.0) as app:
                for engine in ("native", "mapped"):
                    if engine == "mapped":
                        app.command("openFile", path=str(mapped_path))
                        deadline = time.monotonic() + 120.0
                        state = app.command("getLargeFileState")["largeFile"]
                        while not state["indexingComplete"] and time.monotonic() < deadline:
                            time.sleep(0.05)
                            state = app.command("getLargeFileState")["largeFile"]
                        self.assertTrue(state["indexingComplete"], state)
                    for sample_index, (text, right_expectations, left_expectations) in enumerate(samples):
                        with self.subTest(sample=text, engine=engine):
                            if engine == "native":
                                app.command("setText", text=text)
                                offset = 0
                            else:
                                offset = mapped_base + mapped_offsets[sample_index]

                            key_command = "sendEditorKey" if engine == "native" else "sendLargeFileKey"
                            for direction, expectations in ((0x27, right_expectations), (0x25, left_expectations)):
                                for position, expected in expectations.items():
                                    with self.subTest(direction=direction, position=position):
                                        absolute = offset + position
                                        app.command("setSelection", start=absolute, end=absolute)
                                        moved = app.command(key_command, key=direction, control=True)
                                        selection = moved["selection"] if engine == "native" else {
                                            "start": moved["largeFile"]["selectionStart"],
                                            "end": moved["largeFile"]["selectionEnd"],
                                        }
                                        self.assertEqual(selection,
                                            {"start": offset + expected, "end": offset + expected})
                                        app.command("setSelection", start=absolute, end=absolute)
                                        extended = app.command(key_command, key=direction,
                                            control=True, shift=True)
                                        selection = extended["selection"] if engine == "native" else {
                                            "start": extended["largeFile"]["selectionStart"],
                                            "end": extended["largeFile"]["selectionEnd"],
                                        }
                                        self.assertEqual(selection, {
                                            "start": min(absolute, offset + expected),
                                            "end": max(absolute, offset + expected),
                                        })

    def test_scalar_navigation_crlf_deletion_and_overtype_match_in_both_engines(self):
        suffix = "A😀B\r\nC"
        suffix_units = utf16_units(suffix)
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            mapped_path = Path(temp_root) / "mapped-scalar-edit.txt"
            suffix_start = write_mapped_comparison_fixture(mapped_path, suffix)

            with ClassicNotepadDriver(automation_binary(), "windows", timeout=180.0) as app:
                engines = ("native", "mapped")
                for engine in engines:
                    with self.subTest(engine=engine):
                        if engine == "native":
                            app.command("setText", text=suffix)
                            self.assertFalse(app.command("getLargeFileState")["largeFile"]["active"])
                            base = 0
                        else:
                            app.command("openFile", path=str(mapped_path))
                            deadline = time.monotonic() + 120.0
                            state = app.command("getLargeFileState")["largeFile"]
                            while not state["indexingComplete"] and time.monotonic() < deadline:
                                time.sleep(0.05)
                                state = app.command("getLargeFileState")["largeFile"]
                            self.assertTrue(state["indexingComplete"], state)
                            base = suffix_start

                        def selection():
                            return app.command("getSelection")["selection"]

                        def range_text(start, length):
                            if engine == "native":
                                return app.command("getText")["text"][start:start + length]
                            return app.command(
                                "getLargeFileRange", start=base + start, length=length
                            )["text"]

                        def set_position(position):
                            app.command("setSelection", start=base + position, end=base + position)

                        def send_key(key, shift=False, control=False):
                            command = "sendEditorKey" if engine == "native" else "sendLargeFileKey"
                            app.command(command, key=key, shift=shift, control=control)

                        def undo():
                            app.command("undo")

                        self.assertEqual(range_text(0, suffix_units), suffix)

                        # The valid surrogate pair is one horizontal movement target.
                        set_position(1)
                        send_key(0x27)  # Right
                        self.assertEqual(selection(), {"start": base + 3, "end": base + 3})
                        send_key(0x25)  # Left
                        self.assertEqual(selection(), {"start": base + 1, "end": base + 1})
                        send_key(0x27, shift=True)
                        self.assertEqual(selection(), {"start": base + 1, "end": base + 3})

                        # Collapsed Delete and Backspace remove CRLF together; one Undo restores it.
                        for caret, key in ((4, 0x2E), (6, 0x08)):
                            set_position(caret)
                            self.assertEqual(selection(), {"start": base + caret, "end": base + caret})
                            if engine == "native" and key == 0x2E:
                                app.command("delete")
                            elif engine == "native":
                                app.command("typeText", text="\b")
                            else:
                                send_key(key)
                            expected = "A😀BC"
                            actual = range_text(0, suffix_units)
                            self.assertEqual(actual, expected, f"selection={selection()}")
                            undo()
                            self.assertEqual(range_text(0, suffix_units), suffix)

                        # Backspace and Delete remove the complete supplementary scalar.
                        for caret, key in ((3, 0x08), (1, 0x2E)):
                            set_position(caret)
                            if engine == "native" and key == 0x2E:
                                app.command("delete")
                            elif engine == "native":
                                app.command("typeText", text="\b")
                            else:
                                send_key(key)
                            expected = "AB\r\nC"
                            self.assertEqual(range_text(0, utf16_units(expected)), expected)
                            undo()
                            self.assertEqual(range_text(0, suffix_units), suffix)

                        # Overtype replaces the next scalar, but inserts before line breaks.
                        set_position(1)
                        app.command("pressInsert") if engine == "native" else send_key(0x2D)

                        if engine == "native":
                            app.command("typeText", text="Z")
                        else:
                            app.command("sendLargeFileText", text="Z")
                        expected = "AZB\r\nC"
                        self.assertEqual(range_text(0, utf16_units(expected)), expected)
                        undo()
                        self.assertEqual(range_text(0, suffix_units), suffix)

                        set_position(1)
                        if engine == "native":
                            app.command("typeText", text="😀")
                        else:
                            app.command("sendLargeFileText", text="😀")
                        expected = "A😀B\r\nC"
                        self.assertEqual(range_text(0, suffix_units), expected)
                        undo()
                        self.assertEqual(range_text(0, suffix_units), suffix)

                        set_position(4)
                        if engine == "native":
                            app.command("typeText", text="Z")
                        else:
                            app.command("sendLargeFileText", text="Z")
                        expected = "A😀BZ\r\nC"
                        self.assertEqual(range_text(0, utf16_units(expected)), expected)
                        undo()
                        self.assertEqual(range_text(0, suffix_units), suffix)
                        app.command("pressInsert") if engine == "native" else send_key(0x2D)

    def test_overtype_enter_inserts_crlf_without_consuming_following_text(self):
        suffix = "AB"
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            mapped_path = Path(temp_root) / "mapped-overtype-enter.txt"
            suffix_start = write_mapped_comparison_fixture(mapped_path, suffix)

            with ClassicNotepadDriver(automation_binary(), "windows", timeout=180.0) as app:
                for engine in ("native", "mapped"):
                    with self.subTest(engine=engine):
                        if engine == "native":
                            app.command("setText", text=suffix)
                            base = 0
                        else:
                            app.command("openFile", path=str(mapped_path))
                            deadline = time.monotonic() + 120.0
                            state = app.command("getLargeFileState")["largeFile"]
                            while not state["indexingComplete"] and time.monotonic() < deadline:
                                time.sleep(0.05)
                                state = app.command("getLargeFileState")["largeFile"]
                            self.assertTrue(state["indexingComplete"], state)
                            base = suffix_start

                        before_metadata = app.command("getDocumentMetadata")["metadata"]
                        before_modified = app.command("isModified")["modified"]
                        app.command("setSelection", start=base + 1, end=base + 1)
                        if engine == "native":
                            app.command("pressInsert")
                            app.command("typeText", text="\r")
                        else:
                            app.command("sendLargeFileKey", key=0x2D)
                            app.command("sendLargeFileText", text="\r")

                        actual = app.command("getText")["text"] if engine == "native" else app.command(
                            "getLargeFileRange", start=base, length=4)["text"]
                        self.assertEqual(actual, "A\r\nB")
                        self.assertEqual(app.command("getSelection")["selection"], {
                            "start": base + 3, "end": base + 3,
                        })
                        self.assertTrue(app.command("isModified")["modified"])
                        self.assertEqual(app.command("getDocumentMetadata")["metadata"], before_metadata)

                        app.command("undo")
                        restored = app.command("getText")["text"] if engine == "native" else app.command(
                            "getLargeFileRange", start=base, length=2)["text"]
                        self.assertEqual(restored, suffix)
                        self.assertEqual(app.command("getSelection")["selection"], {
                            "start": base + 1, "end": base + 1,
                        })
                        self.assertEqual(app.command("isModified")["modified"], before_modified)
                        self.assertEqual(app.command("getDocumentMetadata")["metadata"], before_metadata)

    def test_mapped_word_and_selection_navigation_matches_native(self):
        suffix = "one two three\r\nA😀 B\r\nabcd"
        suffix_units = utf16_units(suffix)
        line_two_start = utf16_units("one two three\r\n")
        line_three_start = utf16_units("one two three\r\nA😀 B\r\n")
        native_unicode_word_targets = None
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            mapped_path = Path(temp_root) / "mapped-word-navigation.txt"
            # Put the comparison text on its own logical row so mapped word
            # navigation never scans the giant activation row.
            suffix_start = write_mapped_comparison_fixture(mapped_path, "\r\n" + suffix) + 2

            with ClassicNotepadDriver(automation_binary(), "windows", timeout=180.0) as app:
                for engine in ("native", "mapped"):
                    with self.subTest(engine=engine):
                        if engine == "native":
                            app.command("setText", text=suffix)
                            base = 0
                        else:
                            app.command("openFile", path=str(mapped_path))
                            deadline = time.monotonic() + 120.0
                            state = app.command("getLargeFileState")["largeFile"]
                            while not state["indexingComplete"] and time.monotonic() < deadline:
                                time.sleep(0.05)
                                state = app.command("getLargeFileState")["largeFile"]
                            self.assertTrue(state["indexingComplete"], state)
                            base = suffix_start

                        def selection():
                            return app.command("getSelection")["selection"]

                        def set_position(position):
                            app.command("setSelection", start=base + position, end=base + position)

                        def key(virtual_key, *, shift=False, control=False):
                            command = "sendEditorKey" if engine == "native" else "sendLargeFileKey"
                            app.command(command, key=virtual_key, shift=shift, control=control)

                        def assert_position(position):
                            self.assertEqual(selection(), {"start": base + position, "end": base + position})

                        self.assertEqual(utf16_units(suffix), suffix_units)
                        if engine == "mapped":
                            self.assertEqual(app.command("getLargeFileRange", start=base,
                                length=suffix_units)["text"], suffix)

                        # Native EDIT's word starts are the compatibility reference.
                        set_position(0)
                        key(0x27, control=True)
                        assert_position(4)
                        key(0x27, control=True)
                        assert_position(8)
                        key(0x25, control=True)
                        assert_position(4)
                        key(0x25, control=True)
                        assert_position(0)

                        # Ordinary arrows move from the active end of a selection.
                        app.command("setSelection", start=base + line_three_start + 1,
                            end=base + line_three_start + 3)
                        key(0x25)
                        assert_position(line_three_start + 2)
                        app.command("setSelection", start=base + line_three_start + 1,
                            end=base + line_three_start + 3)
                        key(0x27)
                        assert_position(line_three_start + 4)

                        # Shift retains the original anchor and advances the active end.
                        app.command("setSelection", start=base + line_three_start + 1,
                            end=base + line_three_start + 3)
                        key(0x25, shift=True)
                        self.assertEqual(selection(), {
                            "start": base + line_three_start + 1,
                            "end": base + line_three_start + 2,
                        })
                        app.command("setSelection", start=base + line_three_start + 1,
                            end=base + line_three_start + 3)
                        key(0x27, shift=True)
                        self.assertEqual(selection(), {
                            "start": base + line_three_start + 1,
                            "end": base + line_three_start + 4,
                        })

                        # CRLF navigation advances as a pair, and movement across a
                        # valid surrogate pair never leaves a half-pair selection.
                        set_position(line_two_start - 2)
                        key(0x27)
                        assert_position(line_two_start)
                        key(0x25)
                        assert_position(line_two_start - 2)
                        set_position(line_two_start + 1)  # after A, before emoji
                        key(0x27)
                        assert_position(line_two_start + 3)
                        key(0x25)
                        assert_position(line_two_start + 1)

                        # Word movement around a supplementary punctuation scalar
                        # and across the following CRLF uses the same native
                        # boundary rule in both engines.
                        set_position(line_two_start)
                        key(0x27, control=True)
                        unicode_word_right = selection()["start"] - base
                        self.assertNotEqual(unicode_word_right, line_two_start + 3)
                        key(0x25, control=True)
                        unicode_word_left = selection()["start"] - base
                        self.assertNotEqual(unicode_word_left, line_two_start + 2)
                        if engine == "native":
                            native_unicode_word_targets = (unicode_word_right, unicode_word_left)
                        else:
                            self.assertEqual((unicode_word_right, unicode_word_left),
                                native_unicode_word_targets)
                        set_position(suffix_units)
                        key(0x27)
                        assert_position(suffix_units)
                        key(0x25)
                        assert_position(suffix_units - 1)

    def test_displayed_character_count_is_exact_above_native_limit_and_mapped_threshold(self):
        native_count_boundary = 256 * 1024
        native_text = "n" * (256 * 1024 + 17) + "\r\n" + "😀" * 3 + "\r" + "z\n"
        mapped_suffix = "mapped\r\n😀\r\nx\n"
        native_expected = displayed_count(native_text)
        mapped_expected = MAPPED_PREFIX_BYTES + displayed_count(mapped_suffix)

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            native_path = Path(temp_root) / "native-count.txt"
            mapped_path = Path(temp_root) / "mapped-count.txt"
            mapped_suffix_start = write_mapped_comparison_fixture(mapped_path, mapped_suffix)

            with ClassicNotepadDriver(automation_binary(), "windows", timeout=180.0) as app:
                def wait_for_status(expected, timeout=20.0):
                    wanted = status_count_text(expected)
                    deadline = time.monotonic() + timeout
                    status = app.command("getStatusText")["statusText"]
                    while wanted not in status and time.monotonic() < deadline:
                        time.sleep(0.05)
                        status = app.command("getStatusText")["statusText"]
                    self.assertIn(wanted, status)

                for boundary_units in (native_count_boundary - 1, native_count_boundary + 1):
                    boundary_text = "b" * boundary_units
                    app.command("setText", text=boundary_text)
                    wait_for_status(boundary_units)

                app.command("setText", text=native_text)
                wait_for_status(native_expected)
                native_end = utf16_units(native_text)
                app.command("setSelection", start=native_end, end=native_end)
                paste = app.command("pasteThroughProductionHandler", text="😀\r\n")
                self.assertEqual(paste["outcome"], "success")
                pasted_text = app.command("getText")["text"]
                self.assertEqual(displayed_count(pasted_text), native_expected + 2,
                    f"UTF-16 units={utf16_units(pasted_text)}")
                wait_for_status(native_expected + 2)
                app.command("undo")
                after_paste_undo = app.command("getText")["text"]
                self.assertTrue(
                    after_paste_undo == native_text,
                    f"native paste Undo mismatch: expected units={utf16_units(native_text)}, "
                    f"actual units={utf16_units(after_paste_undo)}, "
                    f"expected tail={native_text[-16:]!r}, actual tail={after_paste_undo[-16:]!r}",
                )
                wait_for_status(native_expected)
                app.command("setSelection", start=native_end, end=native_end)
                app.command("insertText", text="!")
                wait_for_status(native_expected + 1)
                app.command("undo")
                after_character_undo = app.command("getText")["text"]
                self.assertTrue(
                    after_character_undo == native_text,
                    f"native character Undo mismatch: expected units={utf16_units(native_text)}, "
                    f"actual units={utf16_units(after_character_undo)}, "
                    f"expected tail={native_text[-16:]!r}, actual tail={after_character_undo[-16:]!r}",
                )
                wait_for_status(native_expected)
                app.command("saveAs", path=str(native_path))
                app.command("openFile", path=str(native_path))
                wait_for_status(native_expected)

                app.command("openFile", path=str(mapped_path))
                deadline = time.monotonic() + 120.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.05)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["active"], state)
                self.assertTrue(state["indexingComplete"], state)
                self.assertEqual(
                    app.command("getLargeFileRange", start=mapped_suffix_start,
                        length=utf16_units(mapped_suffix))["text"], mapped_suffix)
                wait_for_status(mapped_expected, timeout=10.0)
                mapped_end = state["characterCount"]
                app.command("setSelection", start=mapped_end, end=mapped_end)
                app.command("sendLargeFileText", text="!")
                wait_for_status(mapped_expected + 1, timeout=10.0)
                app.command("undo")
                wait_for_status(mapped_expected, timeout=10.0)
                app.command("save")
                app.command("openFile", path=str(mapped_path))
                deadline = time.monotonic() + 120.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.05)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"], state)
                wait_for_status(mapped_expected, timeout=10.0)


if __name__ == "__main__":
    unittest.main()
