import re
import time
import unittest

from automation_context import automation_binary, automation_platform
from classic_notepad_driver import ClassicNotepadDriver


SB_THUMBPOSITION = 4


def wait_for_width_index(app, timeout=20.0):
    deadline = time.monotonic() + timeout
    state = app.command("getEditorWidthState")["editorWidth"]
    while state["measurementPending"] and time.monotonic() < deadline:
        time.sleep(0.02)
        state = app.command("getEditorWidthState")["editorWidth"]
    if state["measurementPending"]:
        raise AssertionError(f"Width index did not finish its bounded rebuild: {state}")
    return state


def normalized_edit_text(text):
    return re.sub(r"\r\n|\r|\n", "\r\n", text)


@unittest.skipUnless(automation_platform() == "windows", "Windows EDIT viewport anchor regression")
class WindowsEditorViewportAnchorTests(unittest.TestCase):
    def _scroll_both_axes(self, app, theme):
        rows = [f"row {index:04d} " + "i" * 60 for index in range(1400)]
        rows[120] = "W" * 6000
        text = "\r\n".join(rows)
        app.command("setAppearanceTheme", theme=theme)
        app.command("setStatusBarVisible", visible=True)
        app.command("resizeEditorViewport", width=980, height=720)
        app.command("setText", text=text)
        ready = wait_for_width_index(app)
        self.assertTrue(ready["horizontalScrollBarVisible"], ready)
        self.assertTrue(ready["verticalScrollBarVisible"], ready)

        app.command("setSelection", start=20, end=30)
        horizontal_track_position = 250 if theme == "dark" else 2000
        horizontal = app.command(
            "sendEditorHorizontalScroll", scrollCommand=SB_THUMBPOSITION, trackPosition=horizontal_track_position
        )["editorWidth"]
        self.assertGreater(horizontal["horizontalOffset"], 0, horizontal)
        self.assertGreater(horizontal["horizontalAnchorPixels"], 0, horizontal)
        app.command("scrollEditorByUnits", horizontalUnits=1, verticalLines=1)
        vertical = app.command("sendEditorVerticalScroll", scrollCommand=SB_THUMBPOSITION, trackPosition=600)["editorWidth"]
        self.assertGreater(vertical["verticalFirstVisibleLine"], 0, vertical)
        self.assertGreater(vertical["verticalTopContentPosition"], 0, vertical)
        return text, vertical, app.command("getSelection")["selection"], app.command("isModified")["modified"]

    def _assert_same_reachable_anchor(self, expected, actual):
        self.assertEqual(actual["horizontalAnchorPixels"], expected["horizontalAnchorPixels"], actual)
        self.assertEqual(actual["verticalTopContentPosition"], expected["verticalTopContentPosition"], actual)
        if actual["nativeHorizontalScrollInfoAvailable"]:
            self.assertLessEqual(
                abs(actual["customHorizontalOffsetPixels"] - actual["nativeHorizontalOffset"]),
                actual["horizontalScrollUnit"], actual
            )

    def test_horizontal_and_vertical_anchors_survive_layout_and_theme_cycles(self):
        for theme in ("light", "dark"):
            with self.subTest(theme=theme), ClassicNotepadDriver(automation_binary(), "windows") as app:
                _, anchor, selection, modified = self._scroll_both_axes(app, theme)
                for width, height, visible, next_theme in (
                    (1050, 760, False, "light"),
                    (720, 650, True, "dark"),
                    (980, 720, False, "light"),
                    (980, 720, False, "light"),
                ):
                    app.command("resizeEditorViewport", width=width, height=height)
                    app.command("setStatusBarVisible", visible=visible)
                    app.command("setAppearanceTheme", theme=next_theme)
                    state = app.command("getEditorWidthState")["editorWidth"]
                    self.assertTrue(state["horizontalScrollBarVisible"], state)
                    self.assertTrue(state["verticalScrollBarVisible"], state)
                    self._assert_same_reachable_anchor(anchor, state)
                    self.assertEqual(app.command("getSelection")["selection"], selection)
                    self.assertEqual(app.command("isModified")["modified"], modified)
                    self.assertEqual(state["generation"], anchor["generation"], state)
                    self.assertEqual(state["measurementBatches"], anchor["measurementBatches"], state)

    def test_scrollbar_visibility_combinations_and_legitimate_clamp(self):
        cases = (
            ("short", False, False),
            ("W" * 4000, True, False),
            ("\r\n".join(["short row"] * 1800), False, True),
            ("\r\n".join(["W" * 1200] * 1000), True, True),
        )
        for theme in ("light", "dark"):
            with self.subTest(theme=theme), ClassicNotepadDriver(automation_binary(), "windows") as app:
                app.command("setAppearanceTheme", theme=theme)
                app.command("resizeEditorViewport", width=900, height=720)
                for text, horizontal_visible, vertical_visible in cases:
                    app.command("setText", text=text)
                    state = wait_for_width_index(app)
                    self.assertEqual(state["horizontalScrollBarVisible"], horizontal_visible, state)
                    self.assertEqual(state["verticalScrollBarVisible"], vertical_visible, state)

                app.command("setText", text="W" * 180)
                wait_for_width_index(app)
                app.command("sendEditorHorizontalScroll", scrollCommand=1)  # SB_LINERIGHT
                before = app.command("getEditorWidthState")["editorWidth"]
                self.assertGreater(before["horizontalOffset"], 0, before)
                app.command("resizeEditorViewport", width=1500, height=1000)
                clamped = app.command("getEditorWidthState")["editorWidth"]
                self.assertFalse(clamped["horizontalScrollBarVisible"], clamped)
                self.assertEqual(clamped["horizontalMaximumOffset"], 0, clamped)
                self.assertEqual(clamped["horizontalOffset"], 0, clamped)

    def test_font_changes_preserve_anchors_with_wrap_off_and_on(self):
        rows = [f"line {index:04d} " + "i" * 80 for index in range(1200)]
        rows[40] += "W" * 5000
        text = "\r\n".join(rows)
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setAppearanceTheme", theme="light")
            app.command("resizeEditorViewport", width=980, height=720)
            app.command("setFont", font="Arial 11")
            app.command("setText", text=text)
            wait_for_width_index(app)
            app.command("setSelection", start=100, end=120)
            selection = app.command("getSelection")["selection"]
            modified = app.command("isModified")["modified"]
            app.command("sendEditorHorizontalScroll", scrollCommand=SB_THUMBPOSITION, trackPosition=2000)
            app.command("sendEditorVerticalScroll", scrollCommand=SB_THUMBPOSITION, trackPosition=400)
            before = app.command("getEditorWidthState")["editorWidth"]
            for font in ("Consolas 14", "Arial 11", "Consolas 14", "Consolas 14"):
                app.command("setFont", font=font)
                state = wait_for_width_index(app)
                self.assertLessEqual(
                    abs(state["horizontalAnchorPixels"] - before["horizontalAnchorPixels"]),
                    state["horizontalScrollUnit"], state
                )
                self.assertEqual(state["verticalTopContentPosition"], before["verticalTopContentPosition"], state)
                self.assertEqual(app.command("getSelection")["selection"], selection)
                self.assertEqual(app.command("isModified")["modified"], modified)
                before = state

            app.command("setWordWrap", enabled=True)
            wrapped_rows = ["wrapped content " * 20 for _ in range(1000)]
            wrapped_text = "\r\n".join(wrapped_rows)
            app.command("setText", text=wrapped_text)
            app.command("setSelection", start=100, end=120)
            wrap_selection = app.command("getSelection")["selection"]
            wrap_modified = app.command("isModified")["modified"]
            app.command("sendEditorVerticalScroll", scrollCommand=SB_THUMBPOSITION, trackPosition=800)
            before_wrap_font = app.command("getEditorWidthState")["editorWidth"]
            self.assertFalse(before_wrap_font["horizontalScrollBarVisible"], before_wrap_font)
            for font in ("Arial 11", "Consolas 14", "Consolas 14"):
                app.command("setFont", font=font)
                state = app.command("getEditorWidthState")["editorWidth"]
                self.assertEqual(
                    state["verticalTopContentPosition"],
                    before_wrap_font["verticalTopContentPosition"],
                    state,
                )
                self.assertEqual(app.command("getSelection")["selection"], wrap_selection)
                self.assertEqual(app.command("isModified")["modified"], wrap_modified)

    def test_font_change_during_pending_width_rebuild_keeps_reachable_anchor(self):
        long_row = "W" * 300000
        text = "\r\n".join([long_row] + [f"row {index:04d} " + "i" * 64 for index in range(1200)])
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setAppearanceTheme", theme="light")
            app.command("setFont", font="Arial 11")
            app.command("setText", text=text)
            pending = app.command("getEditorWidthState")["editorWidth"]
            self.assertTrue(pending["measurementPending"], pending)
            self.assertGreater(pending["editDisplayLineCount"], 96, pending)

            app.command("setSelection", start=0, end=0)
            app.command("sendEditorHorizontalScroll", scrollCommand=SB_THUMBPOSITION, trackPosition=2000)
            app.command("sendEditorVerticalScroll", scrollCommand=SB_THUMBPOSITION, trackPosition=400)
            before_font_change = app.command("getEditorWidthState")["editorWidth"]
            self.assertGreater(before_font_change["horizontalAnchorPixels"], 0, before_font_change)
            self.assertGreater(before_font_change["verticalTopContentPosition"], 0, before_font_change)

            app.command("setFont", font="Consolas 14")
            after_font_change = app.command("getEditorWidthState")["editorWidth"]
            self.assertGreater(after_font_change["generation"], pending["generation"], after_font_change)
            self.assertTrue(after_font_change["measurementPending"], after_font_change)
            self.assertLessEqual(
                abs(after_font_change["horizontalAnchorPixels"] - before_font_change["horizontalAnchorPixels"]),
                after_font_change["horizontalScrollUnit"], after_font_change
            )
            self.assertEqual(
                after_font_change["verticalTopContentPosition"],
                before_font_change["verticalTopContentPosition"],
                after_font_change,
            )

            settled = wait_for_width_index(app)
            self.assertEqual(settled["generation"], after_font_change["generation"], settled)
            self.assertLessEqual(
                abs(settled["horizontalAnchorPixels"] - before_font_change["horizontalAnchorPixels"]),
                settled["horizontalScrollUnit"], settled
            )
            self.assertEqual(
                settled["verticalTopContentPosition"],
                before_font_change["verticalTopContentPosition"],
                settled,
            )
            settled_width = settled["documentWidth"]
            settled_batches = settled["measurementBatches"]
            time.sleep(0.15)
            final = app.command("getEditorWidthState")["editorWidth"]
            self.assertEqual(final["generation"], settled["generation"], final)
            self.assertEqual(final["measurementBatches"], settled_batches, final)
            self.assertEqual(final["documentWidth"], settled_width, final)

    def test_production_wm_paste_preserves_post_scroll_caret_and_normalized_text(self):
        payloads = (
            "tiny paste",
            "first\nsecond\r\nthird\rfourth",
            "W" * 6000,
            "W" * (270 * 1024),
        )
        for payload in payloads:
            with self.subTest(length=len(payload)), ClassicNotepadDriver(automation_binary(), "windows") as app:
                initial = "head\r\nREPLACE\r\ntail"
                app.command("setText", text=initial)
                app.command("setSelection", start=6, end=13)
                result = app.command("pasteThroughProductionHandler", text=payload)
                expected = "head\r\n" + normalized_edit_text(payload) + "\r\ntail"
                actual = app.command("getText")["text"]
                self.assertEqual(actual, expected)
                expected_caret = 6 + len(normalized_edit_text(payload))
                self.assertEqual(result["selection"], {"start": expected_caret, "end": expected_caret})
                state = result["editorWidth"]
                if len(payload) > 6000:
                    state = wait_for_width_index(app)
                self.assertEqual(
                    state["pastePostCaretTopContentPosition"],
                    state["verticalTopContentPosition"],
                    state,
                )
                self.assertEqual(
                    state["pastePostCaretHorizontalOffset"],
                    state["nativeHorizontalOffset"],
                    state,
                )
                self.assertTrue(app.command("isModified")["modified"])


if __name__ == "__main__":
    unittest.main()
