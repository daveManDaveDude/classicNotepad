import unittest

from automation_context import automation_binary, automation_platform
from classic_notepad_driver import ClassicNotepadDriver


class EditCommandTests(unittest.TestCase):
    def test_undo_reverses_inserted_text(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("insertText", text="typed")
            self.assertEqual(app.command("getText")["text"], "typed")

            app.command("undo")
            self.assertEqual(app.command("getText")["text"], "")

    def test_clipboard_delete_and_select_all_commands(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("setText", text="abcdef")
            app.command("setSelection", start=1, end=4)
            app.command("copy")
            app.command("setSelection", start=6, end=6)
            app.command("paste")
            self.assertEqual(app.command("getText")["text"], "abcdefbcd")

            app.command("setSelection", start=1, end=4)
            app.command("cut")
            self.assertEqual(app.command("getText")["text"], "aefbcd")

            app.command("setSelection", start=1, end=3)
            app.command("deleteSelection")
            self.assertEqual(app.command("getText")["text"], "abcd")

            app.command("selectAll")
            self.assertEqual(app.command("getSelection")["selection"], {"start": 0, "end": 4})

    def test_delete_removes_character_after_caret(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("setText", text="abcdef")
            app.command("setSelection", start=2, end=2)

            app.command("delete")

            self.assertEqual(app.command("getText")["text"], "abdef")
            self.assertEqual(app.command("getSelection")["selection"], {"start": 2, "end": 2})

    def test_delete_removes_last_character_on_line(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("setText", text="abc\r\ndef")
            app.command("setSelection", start=2, end=2)

            app.command("delete")

            self.assertEqual(app.command("getText")["text"], "ab\r\ndef")
            self.assertEqual(app.command("getSelection")["selection"], {"start": 2, "end": 2})

    def test_delete_removes_final_character_in_document(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("setText", text="abc")
            app.command("setSelection", start=2, end=2)

            app.command("delete")

            self.assertEqual(app.command("getText")["text"], "ab")
            self.assertEqual(app.command("getSelection")["selection"], {"start": 2, "end": 2})

    def test_delete_at_end_of_document_does_nothing(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("setText", text="abcdef")
            app.command("setSelection", start=6, end=6)

            app.command("delete")

            self.assertEqual(app.command("getText")["text"], "abcdef")
            self.assertEqual(app.command("getSelection")["selection"], {"start": 6, "end": 6})

    def test_time_date_inserts_non_empty_text(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            app.command("insertTimeDate")
            self.assertNotEqual(app.command("getText")["text"], "")
            self.assertTrue(app.command("isModified")["modified"])


if __name__ == "__main__":
    unittest.main()
