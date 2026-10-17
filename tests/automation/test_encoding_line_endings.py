import hashlib
import tempfile
import time
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


def utf16le_with_bom(text: str) -> bytes:
    return b"\xff\xfe" + text.encode("utf-16-le")


def _encoding_parts(name: str):
    if name == "UTF-8":
        return b"", b"x", lambda text: text.encode("utf-8")
    if name == "UTF-8 with BOM":
        return b"\xef\xbb\xbf", b"x", lambda text: text.encode("utf-8")
    return b"\xff\xfe", b"x\x00", lambda text: text.encode("utf-16-le")


def _target_endings(text: str, style: str) -> str:
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    if style == "Windows (CRLF)":
        return normalized.replace("\n", "\r\n")
    if style == "Macintosh (CR)":
        return normalized.replace("\n", "\r")
    return normalized


def _file_digest(path: Path) -> bytes:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.digest()


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

    @unittest.skipUnless(automation_platform() == "windows", "Windows mapped-file engine matrix")
    def test_source_encoding_and_line_endings_round_trip_through_both_engines(self):
        line_ending_cases = [
            ("crlf", "\r\ncrlf-one\r\ncrlf-two", "Windows (CRLF)", "Windows (CRLF)"),
            ("lf", "\nlf-one\nlf-two", "Unix (LF)", "Unix (LF)"),
            ("cr", "\rcr-one\rcr-two", "Macintosh (CR)", "Macintosh (CR)"),
            ("mixed-dominant-lf", "\nlf-one\nlf-two\r\ncrlf-one\rcr-only", "Mixed", "Unix (LF)"),
        ]
        encodings = ("UTF-8", "UTF-8 with BOM", "UTF-16 LE")

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            root = Path(temp_root)
            case_number = 0
            with ClassicNotepadDriver(
                automation_binary(), automation_platform(), timeout=120.0
            ) as app:
                for engine, body_size_bytes in (("ordinary", 64 * 1024), ("mapped", 48 * 1024 * 1024)):
                    for encoding in encodings:
                        bom, body_unit, encode_text = _encoding_parts(encoding)
                        unit_byte_count = len(body_unit)
                        body_character_count = body_size_bytes // unit_byte_count
                        block_character_count = (1024 * 1024) // unit_byte_count
                        body_block = body_unit * block_character_count
                        full_blocks, remainder_characters = divmod(body_character_count, block_character_count)
                        body_remainder = body_unit * remainder_characters
                        cases = list(line_ending_cases)
                        if encoding == "UTF-8":
                            # Equal CRLF/LF counts preserve the ordinary policy's CRLF tie-break.
                            cases.append(("mixed-tie-crlf", "\r\ncrlf-one\nlf-one", "Mixed", "Windows (CRLF)"))

                        for case_name, source_tail, observed_style, save_style in cases:
                            with self.subTest(engine=engine, encoding=encoding, line_endings=case_name):
                                case_number += 1
                                source_path = root / f"{case_number}-{engine}-{case_name}-source.txt"
                                save_as_path = root / f"{case_number}-{engine}-{case_name}-save-as.txt"
                                encoded_source_tail = encode_text(source_tail)
                                with source_path.open("wb") as output:
                                    output.write(bom)
                                    for _ in range(full_blocks):
                                        output.write(body_block)
                                    output.write(body_remainder)
                                    output.write(encoded_source_tail)
                                if engine == "mapped":
                                    self.assertGreaterEqual(source_path.stat().st_size, 48 * 1024 * 1024)
                                else:
                                    self.assertLess(source_path.stat().st_size, 48 * 1024 * 1024)

                                def expected_digest(text_tail: str) -> bytes:
                                    digest = hashlib.sha256()
                                    digest.update(bom)
                                    for _ in range(full_blocks):
                                        digest.update(body_block)
                                    digest.update(body_remainder)
                                    digest.update(encode_text(text_tail))
                                    return digest.digest()

                                def wait_for_large_index() -> dict:
                                    deadline = time.monotonic() + 120.0
                                    state = app.command("getLargeFileState")["largeFile"]
                                    while not state["indexingComplete"] and time.monotonic() < deadline:
                                        time.sleep(0.025)
                                        state = app.command("getLargeFileState")["largeFile"]
                                    self.assertTrue(state["indexingComplete"])
                                    self.assertTrue(state["encodingValid"])
                                    return state

                                app.command("openFile", path=str(source_path))
                                position = wait_for_large_index()["characterCount"] if engine == "mapped" else len(
                                    app.command("getText")["text"]
                                )
                                metadata = app.command("getDocumentMetadata")["metadata"]
                                self.assertEqual(metadata["encoding"], encoding)
                                self.assertEqual(metadata["lineEnding"], observed_style)
                                self.assertEqual(metadata["saveLineEnding"], save_style)
                                if engine == "mapped":
                                    status = app.command("getStatusText")["statusText"]
                                    self.assertIn(encoding, status)
                                    self.assertIn(observed_style, status)

                                app.command("setSelection", start=position, end=position)
                                app.command("insertText", text="X")
                                if engine == "mapped":
                                    status = app.command("getStatusText")["statusText"]
                                    self.assertIn(encoding, status)
                                    self.assertIn(observed_style, status)
                                    app.command("undo")
                                    status = app.command("getStatusText")["statusText"]
                                    self.assertIn(encoding, status)
                                    self.assertIn(observed_style, status)
                                    metadata = app.command("getDocumentMetadata")["metadata"]
                                    self.assertEqual(metadata["encoding"], encoding)
                                    self.assertEqual(metadata["lineEnding"], observed_style)
                                    app.command("setSelection", start=position, end=position)
                                    app.command("insertText", text="X")
                                app.command("save")
                                if engine == "mapped":
                                    wait_for_large_index()
                                target_tail = _target_endings(source_tail, save_style) + "X"
                                self.assertEqual(_file_digest(source_path), expected_digest(target_tail))
                                metadata = app.command("getDocumentMetadata")["metadata"]
                                self.assertEqual(metadata["lineEnding"], observed_style)
                                self.assertEqual(metadata["saveLineEnding"], save_style)
                                if engine == "mapped":
                                    status = app.command("getStatusText")["statusText"]
                                    self.assertIn(encoding, status)
                                    self.assertIn(observed_style, status)

                                app.command("saveAs", path=str(save_as_path))
                                if engine == "mapped":
                                    wait_for_large_index()
                                self.assertEqual(_file_digest(save_as_path), expected_digest(target_tail))
                                metadata = app.command("getDocumentMetadata")["metadata"]
                                self.assertEqual(metadata["path"], str(save_as_path))
                                self.assertEqual(metadata["encoding"], encoding)
                                self.assertEqual(metadata["lineEnding"], observed_style)
                                self.assertEqual(metadata["saveLineEnding"], save_style)

                                if engine == "mapped":
                                    position = app.command("getLargeFileState")["largeFile"]["characterCount"]
                                else:
                                    position = len(app.command("getText")["text"])
                                app.command("setSelection", start=position, end=position)
                                app.command("insertText", text="Y")
                                app.command("save")
                                if engine == "mapped":
                                    wait_for_large_index()
                                target_tail += "Y"
                                self.assertEqual(_file_digest(save_as_path), expected_digest(target_tail))
                                metadata = app.command("getDocumentMetadata")["metadata"]
                                self.assertEqual(metadata["encoding"], encoding)
                                self.assertEqual(metadata["lineEnding"], observed_style)
                                self.assertEqual(metadata["saveLineEnding"], save_style)
                                if engine == "mapped":
                                    status = app.command("getStatusText")["statusText"]
                                    self.assertIn(encoding, status)
                                    self.assertIn(observed_style, status)

                                app.command("openFile", path=str(save_as_path))
                                if engine == "mapped":
                                    wait_for_large_index()
                                metadata = app.command("getDocumentMetadata")["metadata"]
                                self.assertEqual(metadata["encoding"], encoding)
                                self.assertEqual(metadata["lineEnding"], save_style)
                                self.assertEqual(metadata["saveLineEnding"], save_style)
                                if engine == "mapped":
                                    status = app.command("getStatusText")["statusText"]
                                    self.assertIn(encoding, status)
                                    self.assertIn(save_style, status)
                                self.assertEqual(_file_digest(save_as_path), expected_digest(target_tail))


if __name__ == "__main__":
    unittest.main()
