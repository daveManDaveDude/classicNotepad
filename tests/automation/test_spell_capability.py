import unittest

from automation_context import automation_binary, automation_platform
from classic_notepad_driver import ClassicNotepadDriver


class SpellCapabilityTests(unittest.TestCase):
    def test_spell_commands_follow_platform_capability(self):
        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
            capabilities = app.command("getCapabilities")["capabilities"]
            available = capabilities["spellCheck"]

            response = app.command("checkSpelling", text="teh")
            self.assertEqual(response["available"], available)

            suggestions = app.command("suggestSpelling", word="teh", limit=5)
            self.assertEqual(suggestions["available"], available)
            self.assertIsInstance(suggestions["suggestions"], list)

            ignore = app.command("ignoreSpelling", word="teh")
            self.assertEqual(ignore["available"], available)

            add = app.command("addSpelling", word="teh", dryRun=True)
            self.assertEqual(add["available"], available)
            self.assertFalse(add["persisted"])

            if available:
                self.assertGreaterEqual(len(response["errors"]), 1)
                self.assertEqual(response["errors"][0]["text"].lower(), "teh")
                self.assertGreaterEqual(len(suggestions["suggestions"]), 1)
                self.assertEqual(app.command("checkSpelling", text="teh")["errors"], [])
            else:
                self.assertEqual(response["errors"], [])
                app.command("setText", text="editor remains usable")
                self.assertEqual(app.command("getText")["text"], "editor remains usable")


if __name__ == "__main__":
    unittest.main()
