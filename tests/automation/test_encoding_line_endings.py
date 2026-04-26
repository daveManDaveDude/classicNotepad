import tempfile
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


def utf16le_with_bom(text: str) -> bytes:
    return b"\xff\xfe" + text.encode("utf-16-le")


class EncodingLineEndingTests(unittest.TestCase):
    def test_open_save_preserves_supported_encodings_and_line_endings(self):
        cases = [
            (
                "utf8_bom_crlf.txt",
                b"\xef\xbb\xbfalpha\r\nbeta",
                "alpha\r\nbeta",
                "UTF-8 with BOM",
                "Windows (CRLF)",
                b"\xef\xbb\xbfalpha\r\nbeta",
            ),
            (
                "utf8_lf.txt",
                b"alpha\nbeta",
                "alpha\r\nbeta",
                "UTF-8",
                "Unix (LF)",
                b"alpha\nbeta",
            ),
            (
                "utf8_cr.txt",
                b"alpha\rbeta",
                "alpha\r\nbeta",
                "UTF-8",
                "Macintosh (CR)",
                b"alpha\rbeta",
            ),
            (
                "utf16le_crlf.txt",
                utf16le_with_bom("alpha\r\nbeta"),
                "alpha\r\nbeta",
                "UTF-16 LE",
                "Windows (CRLF)",
                utf16le_with_bom("alpha\r\nbeta"),
            ),
        ]

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            temp_path = Path(temp_root)
            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                for file_name, original_bytes, expected_text, encoding, line_ending, expected_saved in cases:
                    path = temp_path / file_name
                    path.write_bytes(original_bytes)

                    with self.subTest(file_name=file_name):
                        app.command("openFile", path=str(path))
                        self.assertEqual(app.command("getText")["text"], expected_text)

                        metadata = app.command("getDocumentMetadata")["metadata"]
                        self.assertEqual(metadata["encoding"], encoding)
                        self.assertEqual(metadata["lineEnding"], line_ending)

                        app.command("save")
                        self.assertEqual(path.read_bytes(), expected_saved)

    def test_mixed_line_endings_report_mixed_and_save_dominant_style(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "mixed.txt"
            path.write_bytes(b"one\ntwo\nthree\r\nfour")

            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                app.command("openFile", path=str(path))

                metadata = app.command("getDocumentMetadata")["metadata"]
                self.assertEqual(metadata["encoding"], "UTF-8")
                self.assertEqual(metadata["lineEnding"], "Mixed")
                self.assertEqual(metadata["saveLineEnding"], "Unix (LF)")

                app.command("save")
                self.assertEqual(path.read_bytes(), b"one\ntwo\nthree\nfour")


if __name__ == "__main__":
    unittest.main()
