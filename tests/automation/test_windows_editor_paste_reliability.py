import time
import unittest

from automation_context import automation_binary, automation_platform
from classic_notepad_driver import ClassicNotepadDriver


EN_CHANGE_MASK = 1
EN_ERRSPACE_MASK = 2
EN_ERRSPACE_CODE = 0x0500
EN_MAXTEXT_MASK = 4
SB_THUMBPOSITION = 4
SB_BOTTOM = 7


def normalized_edit_text(text):
    return text.replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\r\n")


def utf16_units(text):
    return len(text.encode("utf-16-le")) // 2


def wait_for_width_index(app, timeout=20.0):
    deadline = time.monotonic() + timeout
    state = app.command("getEditorWidthState")["editorWidth"]
    while state["measurementPending"] and time.monotonic() < deadline:
        time.sleep(0.02)
        state = app.command("getEditorWidthState")["editorWidth"]
    if state["measurementPending"]:
        raise AssertionError(f"Width index did not finish its bounded rebuild: {state}")
    return state


@unittest.skipUnless(automation_platform() == "windows", "Windows EDIT paste reliability")
class WindowsEditorPasteReliabilityTests(unittest.TestCase):
    def test_line_endings_unicode_and_positions_past_65535(self):
        payloads = (
            "a\r\nb\n",
            "first\nsecond\rthird\r\nfourth\r",
            "BMP: café 漢字; supplementary: 😀",
        )
        for payload in payloads:
            with self.subTest(payload=payload), ClassicNotepadDriver(automation_binary(), "windows") as app:
                initial = "A" * 140002
                insertion = 70001
                app.command("setText", text=initial)
                app.command("setSelection", start=insertion, end=insertion + 3)
                result = app.command("pasteThroughProductionHandler", text=payload)
                expected_payload = normalized_edit_text(payload)
                expected = initial[:insertion] + expected_payload + initial[insertion + 3 :]
                self.assertEqual(result["outcome"], "success")
                self.assertEqual(app.command("getText")["text"], expected)
                expected_caret = insertion + utf16_units(expected_payload)
                self.assertEqual(result["selection"], {"start": expected_caret, "end": expected_caret})

        for selection_start in (0, 70000, 140002):
            with self.subTest(position=selection_start), ClassicNotepadDriver(automation_binary(), "windows") as app:
                initial = "x" * 140002
                app.command("setText", text=initial)
                app.command("setSelection", start=selection_start, end=selection_start)
                result = app.command("pasteThroughProductionHandler", text="🙂")
                expected = initial[:selection_start] + "🙂" + initial[selection_start:]
                self.assertEqual(result["outcome"], "success")
                self.assertEqual(app.command("getText")["text"], expected)
                caret = selection_start + 2
                self.assertEqual(result["selection"], {"start": caret, "end": caret})

    def test_empty_clipboard_is_a_true_no_op_and_keeps_undo(self):
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="abcdef")
            app.command("setSelection", start=6, end=6)
            app.command("typeText", text="X")
            app.command("setSelection", start=1, end=4)
            before_selection = app.command("getSelection")["selection"]
            before_text = app.command("getText")["text"]
            before_modified = app.command("isModified")["modified"]

            result = app.command("pasteThroughProductionHandler", text="")

            self.assertEqual(result["outcome"], "empty")
            self.assertEqual(app.command("getText")["text"], before_text)
            self.assertEqual(app.command("getSelection")["selection"], before_selection)
            self.assertEqual(app.command("isModified")["modified"], before_modified)
            app.command("undo")
            self.assertEqual(app.command("getText")["text"], "abcdef")

    def test_preparation_and_acquisition_faults_preserve_state_and_prior_undo(self):
        stages = (
            ("no-text", "no-text"),
            ("open", "acquisition-failure"),
            ("get", "acquisition-failure"),
            ("size", "acquisition-failure"),
            ("lock", "acquisition-failure"),
            ("conversion", "invalid-data"),
            ("invalid", "invalid-data"),
            ("unicode-odd-size", "invalid-data"),
            ("unicode-unterminated", "invalid-data"),
            ("clipboard-allocation", "allocation-failure"),
            ("copy", "allocation-failure"),
            ("normalization", "allocation-failure"),
            ("capacity", "capacity-rejected"),
            ("native-replacement", "allocation-failure"),
            ("native-errspace", "native-failure"),
        )
        for theme in ("light", "dark"):
            for stage, expected_outcome in stages:
                with self.subTest(theme=theme, stage=stage), ClassicNotepadDriver(
                    automation_binary(), "windows"
                ) as app:
                    app.command("setAppearanceTheme", theme=theme)
                    rows = ["W" * 1200] + [f"line {index:04d}" for index in range(1200)]
                    original_text = "\r\n".join(rows)
                    app.command("setText", text=original_text)
                    wait_for_width_index(app)
                    app.command("setSelection", start=12, end=17)
                    app.command("typeText", text="!")
                    app.command("setSelection", start=12, end=17)
                    app.command("sendEditorHorizontalScroll", scrollCommand=SB_THUMBPOSITION, trackPosition=1000)
                    app.command("sendEditorVerticalScroll", scrollCommand=SB_BOTTOM)
                    before_state = app.command("getEditorWidthState")["editorWidth"]
                    before_text = app.command("getText")["text"]
                    before_selection = app.command("getSelection")["selection"]
                    before_modified = app.command("isModified")["modified"]
                    before_metadata = app.command("getDocumentMetadata")["metadata"]

                    app.command("setPasteFault", stage=stage)
                    result = app.command("pasteThroughProductionHandler", text="replacement\ntext")

                    after_state = result["editorWidth"]
                    clipboard = result["clipboardDiagnostics"]
                    self.assertEqual(result["outcome"], expected_outcome)
                    self.assertEqual(app.command("getText")["text"], before_text)
                    self.assertEqual(result["selection"], before_selection)
                    self.assertEqual(result["modified"], before_modified)
                    self.assertEqual(app.command("getDocumentMetadata")["metadata"], before_metadata)
                    for key in (
                        "generation",
                        "horizontalAnchorPixels",
                        "verticalTopContentPosition",
                        "horizontalOffset",
                        "verticalFirstVisibleLine",
                    ):
                        self.assertEqual(after_state[key], before_state[key], (stage, key, after_state, before_state))

                    self.assertEqual(clipboard["openCalls"], 1, stage)
                    self.assertEqual(clipboard["closeCalls"], 0 if stage == "open" else 1, stage)
                    if stage in ("no-text", "open", "get", "size", "unicode-odd-size"):
                        self.assertEqual(clipboard["unlockCalls"], 0, (stage, clipboard))
                    elif stage == "lock":
                        self.assertEqual(clipboard["lockCalls"], 1)
                        self.assertEqual(clipboard["unlockCalls"], 0)
                    else:
                        self.assertEqual(clipboard["unlockCalls"], 1, (stage, clipboard))
                    if stage in ("get", "invalid", "unicode-unterminated", "clipboard-allocation"):
                        self.assertEqual(clipboard["ansiGetDataCalls"], 0, (stage, clipboard))
                    if stage == "native-errspace":
                        self.assertEqual(result["nativeNotification"], EN_ERRSPACE_CODE)

                    app.command("setPasteFault", stage="")
                    app.command("undo")
                    self.assertEqual(app.command("getText")["text"], original_text)

    def test_production_clipboard_parser_bounds_unicode_and_uses_explicit_ansi_length(self):
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="")
            app.command("setSelection", start=0, end=0)
            app.command("setPasteFault", stage="unicode-padding")
            result = app.command("pasteThroughProductionHandler", text="bounded payload")
            self.assertEqual(result["outcome"], "success")
            self.assertEqual(app.command("getText")["text"], "bounded payload")
            diag = result["clipboardDiagnostics"]
            self.assertEqual(diag["unicodeGetDataCalls"], 1)
            self.assertEqual(diag["ansiGetDataCalls"], 0)
            self.assertEqual(diag["unlockCalls"], 1)
            self.assertEqual(diag["closeCalls"], 1)

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="")
            app.command("setSelection", start=0, end=0)
            app.command("setPasteFault", stage="ansi")
            result = app.command("pasteThroughProductionHandler", text="ansi payload")
            self.assertEqual(result["outcome"], "success")
            self.assertEqual(app.command("getText")["text"], "ansi payload")
            diag = result["clipboardDiagnostics"]
            self.assertEqual(diag["unicodeGetDataCalls"], 0)
            self.assertEqual(diag["ansiGetDataCalls"], 1)
            self.assertEqual(diag["conversionCalls"], 2)
            self.assertEqual(diag["lastAnsiSourceLength"], len(b"ansi payload"))
            self.assertEqual(diag["lockCalls"], 1)
            self.assertEqual(diag["unlockCalls"], 1)
            self.assertEqual(diag["closeCalls"], 1)

    def test_unicode_present_but_invalid_does_not_fall_back_to_ansi(self):
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="preserve")
            app.command("setSelection", start=2, end=5)
            app.command("setPasteFault", stage="invalid")
            result = app.command("pasteThroughProductionHandler", text="valid ANSI fallback")
            self.assertEqual(result["outcome"], "invalid-data")
            self.assertEqual(app.command("getText")["text"], "preserve")
            diag = result["clipboardDiagnostics"]
            self.assertEqual(diag["unicodeGetDataCalls"], 1)
            self.assertEqual(diag["ansiGetDataCalls"], 0)
            self.assertEqual(diag["lockCalls"], 1)
            self.assertEqual(diag["unlockCalls"], 1)
            self.assertEqual(diag["closeCalls"], 1)

    def test_native_errspace_dispatch_reports_unchanged_and_partial_outcomes(self):
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="ABCDEF")
            app.command("setSelection", start=6, end=6)
            app.command("typeText", text="!")
            app.command("setSelection", start=1, end=4)
            before_text = app.command("getText")["text"]
            before_selection = app.command("getSelection")["selection"]
            before_width = wait_for_width_index(app)
            app.command("setPasteFault", stage="native-errspace")

            result = app.command("pasteThroughProductionHandler", text="WXYZ")

            self.assertEqual(result["outcome"], "native-failure")
            self.assertEqual(result["nativeNotification"], EN_ERRSPACE_CODE)
            self.assertEqual(app.command("getText")["text"], before_text)
            self.assertEqual(result["selection"], before_selection)
            after_width = result["editorWidth"]
            for key in ("generation", "horizontalAnchorPixels", "verticalTopContentPosition", "horizontalOffset", "verticalFirstVisibleLine"):
                self.assertEqual(after_width[key], before_width[key], key)
            app.command("setPasteFault", stage="")
            app.command("undo")
            self.assertEqual(app.command("getText")["text"], "ABCDEF")

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="iiabcdef")
            app.command("setSelection", start=0, end=2)
            before_width = wait_for_width_index(app)
            app.command("setPasteFault", stage="native-errspace-changed")

            result = app.command("pasteThroughProductionHandler", text="WXYZ")

            self.assertEqual(result["outcome"], "native-partial")
            self.assertEqual(result["nativeNotification"], EN_ERRSPACE_CODE)
            self.assertEqual(app.command("getText")["text"], "WXYabcdef")
            self.assertTrue(result["modified"])
            self.assertEqual(result["selection"], {"start": 3, "end": 3})
            after_width = wait_for_width_index(app)
            self.assertEqual(after_width["localEditMeasuredTextUnits"], len("WXYabcdef"))
            self.assertGreater(after_width["documentWidth"], before_width["documentWidth"])
            for key in ("horizontalAnchorPixels", "verticalTopContentPosition", "horizontalOffset", "verticalFirstVisibleLine"):
                self.assertEqual(after_width[key], before_width[key], key)

    def test_control_limit_preflight_counts_normalization_and_replacement_space(self):
        for limit, expected_outcome in ((6, "capacity-rejected"), (7, "success"), (8, "success")):
            with self.subTest(limit=limit), ClassicNotepadDriver(automation_binary(), "windows") as app:
                app.command("setText", text="ABCDEF")
                app.command("setSelection", start=1, end=4)
                self.assertEqual(app.command("setEditorTextLimit", limit=limit)["limit"], limit)
                result = app.command("pasteThroughProductionHandler", text="WXYZ")
                self.assertEqual(result["outcome"], expected_outcome)
                expected_text = "ABCDEF" if limit == 6 else "AWXYZEF"
                self.assertEqual(app.command("getText")["text"], expected_text)
                if limit == 6:
                    self.assertEqual(result["selection"], {"start": 1, "end": 4})

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="")
            app.command("setSelection", start=0, end=0)
            app.command("setEditorTextLimit", limit=3)
            rejected = app.command("pasteThroughProductionHandler", text="a\nb")
            self.assertEqual(rejected["outcome"], "capacity-rejected")
            self.assertEqual(app.command("getText")["text"], "")

            app.command("setEditorTextLimit", limit=4)
            accepted = app.command("pasteThroughProductionHandler", text="a\nb")
            self.assertEqual(accepted["outcome"], "success")
            self.assertEqual(app.command("getText")["text"], "a\r\nb")

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="abcdefgh")
            app.command("setSelection", start=0, end=6)
            app.command("setEditorTextLimit", limit=6)
            result = app.command("pasteThroughProductionHandler", text="WXYZ")
            self.assertEqual(result["outcome"], "success")
            self.assertEqual(app.command("getText")["text"], "WXYZgh")

    def test_native_limit_probe_records_partial_en_maxtext_without_undo_rollback(self):
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            result = app.command(
                "probeNativeReplace",
                text="ABCDEF",
                start=1,
                end=4,
                replacement="WXYZ",
                limit=6,
            )
            self.assertEqual(result["textLimit"], 6)
            self.assertEqual(result["textBefore"], "ABCDEF")
            self.assertEqual(result["textAfter"], "AWXYEF")
            self.assertNotEqual(result["notificationMask"] & EN_CHANGE_MASK, 0)
            self.assertNotEqual(result["notificationMask"] & EN_MAXTEXT_MASK, 0)
            self.assertEqual(result["notificationMask"] & EN_ERRSPACE_MASK, 0)

    def test_unexpected_native_maxtext_is_reported_as_partial(self):
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="ABCDEF")
            app.command("setSelection", start=1, end=4)
            app.command("setPasteFault", stage="native-maxtext")

            result = app.command("pasteThroughProductionHandler", text="WXYZ")

            self.assertEqual(result["outcome"], "native-partial")
            self.assertEqual(app.command("getText")["text"], "AWXYEF")
            self.assertTrue(app.command("isModified")["modified"])
            self.assertNotEqual(result["selection"], {"start": 1, "end": 4})

    def test_large_paste_has_one_undo_and_neighboring_edits_remain_separate(self):
        payload = "p" * (270 * 1024) + "\n😀"
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="before")
            app.command("setSelection", start=6, end=6)
            app.command("typeText", text="X")
            before_paste = app.command("getText")["text"]
            result = app.command("pasteThroughProductionHandler", text=payload)
            self.assertEqual(result["outcome"], "success")
            expected = before_paste + normalized_edit_text(payload)
            self.assertEqual(app.command("getText")["text"], expected)
            app.command("undo")
            self.assertEqual(app.command("getText")["text"], before_paste)

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="a")
            app.command("setSelection", start=1, end=1)
            app.command("typeText", text="x")
            app.command("pasteThroughProductionHandler", text="one")
            app.command("pasteThroughProductionHandler", text="two")
            app.command("typeText", text="y")
            self.assertEqual(app.command("getText")["text"], "axonetwoy")
            app.command("undo")
            self.assertEqual(app.command("getText")["text"], "axone")

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text="a")
            app.command("setSelection", start=1, end=1)
            app.command("pasteThroughProductionHandler", text="one")
            app.command("pasteThroughProductionHandler", text="two")
            app.command("undo")
            self.assertEqual(app.command("getText")["text"], "aone")


if __name__ == "__main__":
    unittest.main()
