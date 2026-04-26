import tempfile
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


class FileWorkflowTests(unittest.TestCase):
    def test_command_line_file_opens_in_automation_mode(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "argument.txt"
            path.write_bytes(b"from argument")

            with ClassicNotepadDriver(automation_binary(), automation_platform(), str(path)) as app:
                self.assertEqual(app.command("getText")["text"], "from argument")
                self.assertEqual(app.command("getTitle")["title"], "argument.txt - Classic Notepad")
                self.assertFalse(app.command("isModified")["modified"])

    def test_command_line_missing_file_creates_document_for_path(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "created-from-argument.txt"

            with ClassicNotepadDriver(automation_binary(), automation_platform(), str(path)) as app:
                self.assertEqual(app.command("getText")["text"], "")
                self.assertEqual(app.command("getTitle")["title"], "created-from-argument.txt - Classic Notepad")
                self.assertFalse(app.command("isModified")["modified"])

                metadata = app.command("getDocumentMetadata")["metadata"]
                self.assertTrue(metadata["hasPath"])
                self.assertEqual(metadata["displayName"], "created-from-argument.txt")

                app.command("save")
                self.assertEqual(path.read_bytes(), b"")

    def test_new_type_save_as_save_and_open(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            temp_path = Path(temp_root)
            note_path = temp_path / "note.txt"
            opened_path = temp_path / "opened.txt"
            opened_path.write_bytes(b"opened\r\nfile")

            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                capabilities = app.command("getCapabilities")["capabilities"]
                self.assertEqual(capabilities["platform"], automation_platform())

                self.assertEqual(app.command("getTitle")["title"], "Untitled - Classic Notepad")
                self.assertFalse(app.command("isModified")["modified"])

                app.command("setText", text="hello\r\nworld")
                self.assertTrue(app.command("isModified")["modified"])
                self.assertEqual(app.command("getTitle")["title"], "*Untitled - Classic Notepad")

                app.command("saveAs", path=str(note_path))
                self.assertEqual(note_path.read_bytes(), b"hello\r\nworld")
                self.assertFalse(app.command("isModified")["modified"])
                self.assertEqual(app.command("getTitle")["title"], "note.txt - Classic Notepad")

                app.command("setText", text="changed\r\nagain")
                app.command("save")
                self.assertEqual(note_path.read_bytes(), b"changed\r\nagain")
                self.assertFalse(app.command("isModified")["modified"])

                app.command("openFile", path=str(opened_path))
                self.assertEqual(app.command("getText")["text"], "opened\r\nfile")
                self.assertEqual(app.command("getTitle")["title"], "opened.txt - Classic Notepad")
                self.assertFalse(app.command("isModified")["modified"])


if __name__ == "__main__":
    unittest.main()
