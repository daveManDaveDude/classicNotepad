import unittest

from automation_context import automation_binary, automation_platform
from classic_notepad_driver import ClassicNotepadDriver


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


if __name__ == "__main__":
    unittest.main()
