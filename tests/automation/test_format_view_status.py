import tempfile
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


class FormatViewStatusTests(unittest.TestCase):
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
