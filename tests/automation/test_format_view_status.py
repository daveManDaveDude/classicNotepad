import tempfile
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


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


if __name__ == "__main__":
    unittest.main()
