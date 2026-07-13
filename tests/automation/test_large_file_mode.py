import os
import tempfile
import time
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


class LargeFileModeTests(unittest.TestCase):
    @unittest.skipUnless(automation_platform() == "windows", "large-file mode is Windows-only")
    def test_large_file_mode_is_editable_and_stream_saved(self):
        line = b"small-large-file-test".ljust(126, b".") + b"\r\n"
        size = (48 * 1024 * 1024) + (128 * 8)
        line_count = size // len(line)

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "large-file-mode.txt"
            with path.open("wb") as output:
                output.write(b"BEGIN-LARGE-MODE".ljust(126, b".") + b"\r\n")
                block = line * 8192
                remaining = line_count - 2
                while remaining >= 8192:
                    output.write(block)
                    remaining -= 8192
                output.write(line * remaining)
                output.write(b"END-LARGE-MODE".ljust(126, b".") + b"\r\n")
            def edge_bytes() -> bytes:
                with path.open("rb") as source:
                    beginning = source.read(256)
                    source.seek(-256, 2)
                    return beginning + source.read(256)

            original = edge_bytes()
            saved_path = Path(temp_root) / "large-file-mode-saved.txt"

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=10.0) as app:
                started = time.perf_counter()
                app.command("openFile", path=str(path))
                open_elapsed = time.perf_counter() - started
                state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["active"])
                self.assertFalse(state["readOnly"])
                self.assertEqual(state["fileSize"], path.stat().st_size)
                self.assertLess(open_elapsed, 2.0)
                self.assertIn("Large File", app.command("getTitle")["title"])
                self.assertFalse(app.command("isModified")["modified"])

                deadline = time.monotonic() + 30.0
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.05)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])
                self.assertTrue(state["encodingValid"])
                self.assertTrue(state["editable"])
                self.assertGreater(state["indexedLines"], 100)
                self.assertIn("BEGIN-LARGE-MODE", app.command("getLargeFileLine", line=1)["text"])
                last_content_line = state["totalLines"] - 1
                app.command("goToLine", line=last_content_line)
                self.assertIn("END-LARGE-MODE", app.command("getLargeFileLine", line=last_content_line)["text"])

                app.command("setSelection", start=0, end=0)
                app.command("insertText", text="[BEGIN-EDIT]")
                middle = state["characterCount"] // 2
                app.command("setSelection", start=middle, end=middle)
                app.command("typeText", text="[MIDDLE-EDIT]\r\n")
                end = app.command("getLargeFileState")["largeFile"]["characterCount"]
                app.command("setSelection", start=end, end=end)
                app.command("insertText", text="[END-EDIT]")
                self.assertTrue(app.command("isModified")["modified"])
                self.assertTrue(app.command("getLargeFileState")["largeFile"]["modified"])
                self.assertTrue(app.command("getTitle")["title"].startswith("*"))

                found = app.command("find", text="BEGIN-LARGE-MODE", matchCase=True, wholeWord=False, searchDown=True)
                self.assertTrue(found["found"])
                self.assertTrue(app.command("replace", text="BEGIN-LARGE-MODE", replacement="START-LARGE-MODE", matchCase=True, wholeWord=False, searchDown=True)["replaced"])
                app.command("undo")
                app.command("setSelection", start=0, end=12)
                app.command("copy")
                app.command("deleteSelection")
                app.command("paste")

                app.command("saveAs", path=str(saved_path))
                deadline = time.monotonic() + 45.0
                saved_state = app.command("getLargeFileState")["largeFile"]
                while not saved_state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.05)
                    saved_state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(saved_state["indexingComplete"])
                self.assertFalse(app.command("isModified")["modified"])
                self.assertTrue(saved_path.exists())
                self.assertEqual(edge_bytes(), original)
                self.assertIn("[BEGIN-EDIT]", app.command("getLargeFileRange", start=0, length=128)["text"])
                self.assertIn("[END-EDIT]", app.command("getLargeFileRange", start=saved_state["characterCount"] - 64, length=64)["text"])

            self.assertEqual(edge_bytes(), original)


if __name__ == "__main__":
    unittest.main()
