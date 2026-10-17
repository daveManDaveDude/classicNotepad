import tempfile
import time
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver
from editor_semantics_fixtures import MAPPED_PREFIX_BYTES, utf16_units


class FindReplaceGoToTests(unittest.TestCase):
    def test_find_and_find_next_select_matches(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("setText", text="alpha beta alpha")

            response = app.command("find", text="alpha")
            self.assertTrue(response["found"])
            self.assertEqual(response["selection"], {"start": 0, "end": 5})

            response = app.command("findNext")
            self.assertTrue(response["found"])
            self.assertEqual(response["selection"], {"start": 11, "end": 16})

            response = app.command("find", text="missing")
            self.assertFalse(response["found"])

    def test_match_case_and_whole_word_options(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("setText", text="Alpha alpha alphabet alpha")

            response = app.command("find", text="alpha", matchCase=True)
            self.assertTrue(response["found"])
            self.assertEqual(response["selection"], {"start": 6, "end": 11})

            response = app.command("find", text="alpha", matchCase=False)
            self.assertTrue(response["found"])
            self.assertEqual(response["selection"], {"start": 0, "end": 5})

            response = app.command("find", text="alpha", wholeWord=True)
            self.assertTrue(response["found"])
            self.assertEqual(response["selection"], {"start": 0, "end": 5})

            response = app.command("findNext", wholeWord=True)
            self.assertTrue(response["found"])
            self.assertEqual(response["selection"], {"start": 6, "end": 11})

    def test_replace_and_replace_all(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("setText", text="one two one")
            app.command("find", text="two")

            response = app.command("replace", text="two", replacement="2")
            self.assertTrue(response["replaced"])
            self.assertEqual(app.command("getText")["text"], "one 2 one")
            self.assertEqual(response["selection"], {"start": 5, "end": 5})

            response = app.command("replaceAll", text="one", replacement="1")
            self.assertEqual(response["count"], 2)
            self.assertEqual(app.command("getText")["text"], "1 2 1")

    def test_go_to_line_validation_and_selection(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("setText", text="one\r\ntwo\r\nthree")

            response = app.command("goToLine", line=2)
            self.assertEqual(response["selection"], {"start": 5, "end": 5})

            response = app.try_command("goToLine", line=99)
            self.assertFalse(response["ok"])
            self.assertIn("Line number must be between 1 and 3.", response["error"])
            self.assertEqual(app.command("getSelection")["selection"], {"start": 5, "end": 5})

    @unittest.skipUnless(automation_platform() == "windows", "Windows native logical line coordinates")
    def test_wrapped_native_status_and_go_to_use_logical_lines(self):
        first_line = "abcdefghij" * 600
        text = first_line + "\r\nsecond\r\nlast"
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text=text)
            app.command("setWordWrap", enabled=True)
            app.command("setSelection", start=5000, end=5000)
            status = app.command("getStatusText")["statusText"]
            self.assertIn("Ln 1, Col 5,001", status)

            line_two_start = len(first_line) + 2
            response = app.command("goToLine", line=2)
            self.assertEqual(response["selection"], {"start": line_two_start, "end": line_two_start})
            self.assertIn("Ln 2, Col 1", app.command("getStatusText")["statusText"])

            line_three_start = line_two_start + len("second") + 2
            response = app.command("goToLine", line=3)
            self.assertEqual(response["selection"], {"start": line_three_start, "end": line_three_start})

    @unittest.skipUnless(automation_platform() == "windows", "Windows native Go To dialog state")
    def test_go_to_dialog_defaults_to_active_caret_line_and_cancel_preserves_selection(self):
        text = "a\r\nb\r\nc"
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text=text)
            app.command("setSelection", start=0, end=6)
            before_text = app.command("getText")["text"]
            before_selection = app.command("getSelection")["selection"]
            before_modified = app.command("isModified")["modified"]
            self.assertEqual(before_text, text)
            self.assertEqual(before_selection, {"start": 0, "end": 6})
            self.assertIn("Ln 3, Col 1", app.command("getStatusText")["statusText"])

            dialog = app.command("probeGoToDialogDefault")
            self.assertEqual(dialog["initialLine"], "3")

            self.assertEqual(app.command("getText")["text"], before_text)
            self.assertEqual(app.command("getSelection")["selection"], before_selection)
            self.assertEqual(app.command("isModified")["modified"], before_modified)

    @unittest.skipUnless(automation_platform() == "windows", "Windows reverse-selection caret status and Go To")
    def test_go_to_dialog_and_status_use_active_endpoint_of_reverse_selection(self):
        text = "a\r\nb\r\nc"
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text=text)
            app.command("setSelection", start=len(text), end=len(text))
            app.command("typeText", text="!")
            app.command("setSelection", start=6, end=6)
            app.command("sendEditorKey", key=0x25, shift=True)

            self.assertEqual(app.command("getSelection")["selection"], {"start": 4, "end": 6})
            before = {
                "text": app.command("getText")["text"],
                "selection": app.command("getSelection")["selection"],
                "modified": app.command("isModified")["modified"],
                "status": app.command("getStatusText")["statusText"],
            }
            self.assertIn("Ln 2, Col 2", before["status"])
            self.assertTrue(before["modified"])

            dialog = app.command("probeGoToDialogDefault")
            self.assertEqual(dialog["initialLine"], "2")
            self.assertEqual(app.command("getText")["text"], before["text"])
            self.assertEqual(app.command("getSelection")["selection"], before["selection"])
            self.assertEqual(app.command("isModified")["modified"], before["modified"])
            self.assertEqual(app.command("getStatusText")["statusText"], before["status"])

    @unittest.skipUnless(automation_platform() == "windows", "Windows production Find/Replace parity")
    def test_production_find_replace_matches_native_and_mapped_logical_contract(self):
        suffix = "Café café caféine _café\r\n漢字😀漢字?\r\nababa\r\n"
        suffix_units = utf16_units(suffix)
        first_lower = utf16_units(suffix[:suffix.index("café")])
        cjk_start = utf16_units(suffix[:suffix.index("漢字")])
        emoji_start = utf16_units(suffix[:suffix.index("😀")])
        overlap_start = utf16_units(suffix[:suffix.index("ababa")])

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            mapped_path = Path(temp_root) / "mapped-find-replace-parity.txt"
            with mapped_path.open("wb") as mapped_source:
                block = b" " * (1024 * 1024)
                remaining = MAPPED_PREFIX_BYTES
                while remaining:
                    amount = min(remaining, len(block))
                    mapped_source.write(block[:amount])
                    remaining -= amount
                mapped_source.write(suffix.encode("utf-8"))
            suffix_start = MAPPED_PREFIX_BYTES

            with ClassicNotepadDriver(automation_binary(), "windows", timeout=180.0) as app:
                for engine in ("native", "mapped"):
                    with self.subTest(engine=engine):
                        native_prefix = ""
                        if engine == "native":
                            native_prefix = "x" * 65540 + "\r\n"
                            app.command("setText", text=native_prefix + suffix)
                            base = utf16_units(native_prefix)
                            self.assertGreater(base, 65535)
                        else:
                            app.command("openFile", path=str(mapped_path))
                            deadline = time.monotonic() + 120.0
                            state = app.command("getLargeFileState")["largeFile"]
                            while not state["indexingComplete"] and time.monotonic() < deadline:
                                time.sleep(0.05)
                                state = app.command("getLargeFileState")["largeFile"]
                            self.assertTrue(state["active"], state)
                            self.assertTrue(state["indexingComplete"], state)
                            base = suffix_start

                        def text_range():
                            if engine == "native":
                                return app.command("getText")["text"][len(native_prefix):]
                            current_units = app.command("getLargeFileState")["largeFile"][
                                "characterCount"] - base
                            return app.command("getLargeFileRange", start=base,
                                length=current_units)["text"]

                        def dispatch(action, text, replacement="", **options):
                            response = app.command("productionFindReplace", action=action,
                                text=text, replacement=replacement, **options)
                            state = response["largeFile"]
                            if engine == "mapped":
                                operation_name = "Replace All" if action == "replaceAll" else "Find"
                                deadline = time.monotonic() + 120.0
                                while (state["operationName"] != operation_name or
                                        state["operationInProgress"]) and time.monotonic() < deadline:
                                    time.sleep(0.02)
                                    state = app.command("getLargeFileState")["largeFile"]
                                self.assertEqual(state["operationName"], operation_name, state)
                                self.assertTrue(state["operationCompleted"], state)
                                self.assertTrue(state["operationSucceeded"], state["operationError"])
                            return response

                        self.assertEqual(text_range(), suffix)
                        if engine == "native":
                            app.command("setSelection", start=base, end=base)
                            self.assertIn("Ln 2, Col 1", app.command("getStatusText")["statusText"])
                        if engine == "mapped":
                            second_line_start = suffix.index("\r\n") + 2
                            second_line_units = utf16_units(suffix[:second_line_start])
                            goto = app.command("goToLine", line=2)
                            self.assertEqual(goto["selection"], {
                                "start": base + second_line_units,
                                "end": base + second_line_units,
                            })
                            self.assertIn("Ln 2, Col 1", app.command("getStatusText")["statusText"])
                            goto = app.command("goToLine", line=4)
                            self.assertEqual(goto["selection"], {
                                "start": base + suffix_units,
                                "end": base + suffix_units,
                            })
                            self.assertIn("Ln 4, Col 1", app.command("getStatusText")["statusText"])

                        # The actual common-dialog callback path shares case
                        # and whole-word behavior, including accented letters
                        # and underscore boundaries.
                        app.command("setSelection", start=base, end=base)
                        dispatch("findNext", "café", matchCase=False, wholeWord=True, searchDown=True)
                        self.assertEqual(app.command("getSelection")["selection"],
                            {"start": base, "end": base + 4})
                        dispatch("findNext", "café", matchCase=False, wholeWord=True, searchDown=True)
                        second_lower = first_lower
                        self.assertEqual(app.command("getSelection")["selection"],
                            {"start": base + second_lower, "end": base + second_lower + 4})
                        selection_before_no_match = app.command("getSelection")["selection"]
                        no_whole_word_match = app.command("findNext", matchCase=False,
                            wholeWord=True, searchDown=True)
                        self.assertFalse(no_whole_word_match["found"])
                        self.assertEqual(app.command("getSelection")["selection"], selection_before_no_match)

                        app.command("setSelection", start=base, end=base)
                        dispatch("findNext", "café", matchCase=True, wholeWord=False, searchDown=True)
                        self.assertEqual(app.command("getSelection")["selection"],
                            {"start": base + first_lower, "end": base + first_lower + 4})

                        app.command("setSelection", start=base, end=base)
                        dispatch("findNext", "漢字", matchCase=True, wholeWord=True, searchDown=True)
                        self.assertEqual(app.command("getSelection")["selection"],
                            {"start": base + cjk_start, "end": base + cjk_start + 2})
                        dispatch("findNext", "漢字", matchCase=True, wholeWord=True, searchDown=True)
                        self.assertEqual(app.command("getSelection")["selection"],
                            {"start": base + cjk_start + 4, "end": base + cjk_start + 6})

                        app.command("setSelection", start=base, end=base)
                        dispatch("findNext", "😀", matchCase=True, wholeWord=False, searchDown=True)
                        self.assertEqual(app.command("getSelection")["selection"],
                            {"start": base + emoji_start, "end": base + emoji_start + 2})

                        # Backward search keeps the shared overlapping-match
                        # boundary: in "ababa", the last occurrence starts at 2.
                        app.command("setSelection", start=base + suffix_units, end=base + suffix_units)
                        dispatch("findNext", "aba", searchDown=False)
                        self.assertEqual(app.command("getSelection")["selection"],
                            {"start": base + overlap_start + 2, "end": base + overlap_start + 5})

                        # A selected Replace is one edit and continues to the
                        # next valid whole-word match in the production path.
                        app.command("setSelection", start=base, end=base + 4)
                        dispatch("replace", "café", "tea cake", matchCase=False,
                            wholeWord=True, searchDown=True)
                        self.assertEqual(text_range(),
                            "tea cake café caféine _café\r\n漢字😀漢字?\r\nababa\r\n")
                        self.assertEqual(app.command("getSelection")["selection"],
                            {"start": base + 9, "end": base + 13})
                        app.command("undo")
                        self.assertEqual(text_range(), suffix)

                        # Replace All observes the same whole-word boundaries
                        # and remains a single Undo operation in both engines.
                        dispatch("replaceAll", "café", "tea cake", matchCase=False,
                            wholeWord=True, searchDown=True)
                        if engine == "mapped":
                            self.assertEqual(app.command("getLargeFileState")["largeFile"][
                                "operationResultCount"], 2)
                        replaced_all = "tea cake tea cake caféine _café\r\n漢字😀漢字?\r\nababa\r\n"
                        self.assertEqual(text_range(), replaced_all)
                        app.command("undo")
                        self.assertEqual(text_range(), suffix)

                        # The callback accepts exact Unicode and CRLF text in
                        # a replacement; Undo restores the original bytes' text.
                        app.command("setSelection", start=base + cjk_start,
                            end=base + cjk_start + 2)
                        dispatch("replace", "漢字", "茶😀\r\n", matchCase=True,
                            wholeWord=True, searchDown=True)
                        expected_newline_replace = suffix.replace("漢字", "茶😀\r\n", 1)
                        self.assertEqual(text_range(), expected_newline_replace)
                        app.command("undo")
                        self.assertEqual(text_range(), suffix)


if __name__ == "__main__":
    unittest.main()
