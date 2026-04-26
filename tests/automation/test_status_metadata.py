import tempfile
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


class StatusMetadataTests(unittest.TestCase):
    def test_title_dirty_marker_and_status_metadata(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "status.txt"

            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                metadata = app.command("getDocumentMetadata")["metadata"]
                self.assertFalse(metadata["hasPath"])
                self.assertEqual(metadata["displayName"], "Untitled")
                self.assertEqual(metadata["encoding"], "UTF-8")
                self.assertEqual(metadata["lineEnding"], "Windows (CRLF)")

                status = app.command("getStatusText")["statusText"]
                self.assertIn("Ln 1, Col 1", status)
                self.assertIn("0 characters", status)
                self.assertIn("Windows (CRLF)", status)
                self.assertIn("UTF-8", status)

                app.command("setText", text="abc\r\ndef")
                self.assertEqual(app.command("getTitle")["title"], "*Untitled - Classic Notepad")
                self.assertTrue(app.command("isModified")["modified"])

                status = app.command("getStatusText")["statusText"]
                self.assertIn("7 characters", status)

                app.command("saveAs", path=str(path))
                self.assertEqual(app.command("getTitle")["title"], "status.txt - Classic Notepad")
                self.assertFalse(app.command("isModified")["modified"])

                metadata = app.command("getDocumentMetadata")["metadata"]
                self.assertTrue(metadata["hasPath"])
                self.assertEqual(metadata["displayName"], "status.txt")
                self.assertEqual(metadata["encoding"], "UTF-8")
                self.assertEqual(metadata["lineEnding"], "Windows (CRLF)")

    def test_status_updates_after_opening_existing_file(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "lf.txt"
            path.write_bytes(b"first\nsecond")

            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                app.command("openFile", path=str(path))

                status = app.command("getStatusText")["statusText"]
                self.assertIn("Ln 1, Col 1", status)
                self.assertIn("12 characters", status)
                self.assertIn("Unix (LF)", status)
                self.assertIn("UTF-8", status)


if __name__ == "__main__":
    unittest.main()
