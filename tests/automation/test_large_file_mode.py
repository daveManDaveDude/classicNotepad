import hashlib
import os
import tempfile
import time
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


class LargeFileModeTests(unittest.TestCase):
    @unittest.skipUnless(automation_platform() == "macos", "macOS AppKit large-file mode")
    def test_macos_large_file_mode_diagnostics_edit_save_as_and_reopen(self):
        size = (48 * 1024 * 1024) + 4096
        line = b"macos-large-file-test".ljust(126, b".") + b"\r\n"

        with tempfile.TemporaryDirectory() as temp_root:
            root = Path(temp_root)
            source_dir = root / "source"
            destination_dir = root / "selected output \u00fcnicode"
            source_dir.mkdir()
            destination_dir.mkdir()
            source = source_dir / "mac-large-source.txt"
            destination = destination_dir / "mac saved copy.txt"
            with source.open("wb") as output:
                remaining = size
                block = line * 8192
                while remaining >= len(block):
                    output.write(block)
                    remaining -= len(block)
                while remaining >= len(line):
                    output.write(line)
                    remaining -= len(line)
                output.write(b"Z" * remaining)
            self.assertEqual(source.stat().st_size, size)

            def sha256(path: Path) -> str:
                digest = hashlib.sha256()
                with path.open("rb") as stream:
                    for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                        digest.update(chunk)
                return digest.hexdigest()

            source_hash = sha256(source)
            previous_visible = os.environ.get("CLASSIC_NOTEPAD_AUTOMATION_VISIBLE")
            os.environ["CLASSIC_NOTEPAD_AUTOMATION_VISIBLE"] = "1"
            try:
                with ClassicNotepadDriver(automation_binary(), automation_platform(), str(source), timeout=45.0) as app:
                    state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["active"])
                    self.assertTrue(state["editable"])
                    self.assertEqual(state["fileSize"], size)
                    self.assertGreater(state["indexedBytes"], 0)
                    self.assertGreater(state["pieceCount"], 0)
                    self.assertLessEqual(state["firstUsableMilliseconds"], 2000)
                    self.assertIn("macos-large", app.command(
                        "getLargeFileRange", start=0, length=64
                    )["text"])

                    app.command("setSelection", start=5, end=5)
                    app.command("insertText", text="[MAC-EDIT]")
                    self.assertTrue(app.command("getLargeFileState")["largeFile"]["modified"])
                    app.command("undo")
                    self.assertFalse(app.command("getLargeFileState")["largeFile"]["modified"])
                    app.command("setSelection", start=5, end=5)
                    app.command("insertText", text="[MAC-EDIT]")

                    # Save As is deliberately issued without waiting for the
                    # index worker, exercising the foreground completion path.
                    app.command("saveAs", path=str(destination))
                    saved_state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(saved_state["indexingComplete"])
                    self.assertFalse(saved_state["modified"])
                    self.assertEqual(app.command("getTitle")["title"], "mac saved copy.txt - Classic Notepad")
                    self.assertEqual(sha256(source), source_hash)

                    expected = hashlib.sha256()
                    with source.open("rb") as stream:
                        expected.update(stream.read(5))
                        expected.update(b"[MAC-EDIT]")
                        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                            expected.update(chunk)
                    self.assertEqual(sha256(destination), expected.hexdigest())

                    app.command("openFile", path=str(destination))
                    reopened = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(reopened["active"])
                    app.command("setSelection", start=0, end=0)
                    app.command("insertText", text="N")
                    app.command("save")
                    self.assertTrue(app.command("getLargeFileRange", start=0, length=16)["text"].startswith("Nmacos"))
                    self.assertEqual(sha256(source), source_hash)
                    self.assertEqual(list(destination_dir.glob("*.classic-notepad-save-*")), [])
            finally:
                if previous_visible is None:
                    os.environ.pop("CLASSIC_NOTEPAD_AUTOMATION_VISIBLE", None)
                else:
                    os.environ["CLASSIC_NOTEPAD_AUTOMATION_VISIBLE"] = previous_visible

    @unittest.skipUnless(automation_platform() == "linux", "Linux GTK large-file mode")
    def test_linux_large_file_mode_opens_edits_undoes_and_stream_saves(self):
        line = b"linux-large-file-test".ljust(126, b".") + b"\r\n"
        size = (48 * 1024 * 1024) + 1024
        line_count = size // len(line)

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "linux-large-file.txt"
            saved_path = Path(temp_root) / "linux-large-file-saved.txt"
            with path.open("wb") as output:
                output.write(b"BEGIN-LINUX-LARGE".ljust(126, b".") + b"\r\n")
                remaining = line_count - 2
                block = line * 8192
                while remaining >= 8192:
                    output.write(block)
                    remaining -= 8192
                output.write(line * remaining)
                output.write(b"END-LINUX-LARGE".ljust(126, b".") + b"\r\n")

            def sha256(file_path: Path) -> str:
                digest = hashlib.sha256()
                with file_path.open("rb") as source:
                    for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
                        digest.update(chunk)
                return digest.hexdigest()

            source_hash = sha256(path)
            opened = time.monotonic()
            with ClassicNotepadDriver(automation_binary(), automation_platform(), str(path), timeout=30.0) as app:
                state = app.command("getLargeFileState")["largeFile"]
                self.assertLess(time.monotonic() - opened, 2.0)
                self.assertTrue(state["active"])
                self.assertTrue(state["editable"])
                self.assertEqual(state["fileSize"], path.stat().st_size)

                deadline = time.monotonic() + 30.0
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.03)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])
                self.assertTrue(state["encodingValid"])
                self.assertEqual(state["maximumLineColumns"], 126)
                self.assertIn("BEGIN-LINUX-LARGE", app.command("getLargeFileTextRange", start=0, length=126)["text"])
                self.assertIn("END-LINUX-LARGE", app.command(
                    "getLargeFileTextRange", start=state["characterCount"] - 128, length=128
                )["text"])

                app.command("setSelection", start=5, end=5)
                app.command("insertText", text="Z")
                edited = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(edited["modified"])
                self.assertEqual(edited["caretPosition"], 6)
                self.assertEqual(app.command("getLargeFileTextRange", start=0, length=12)["text"], "BEGINZ-LINUX")
                app.command("undo")
                self.assertFalse(app.command("getLargeFileState")["largeFile"]["modified"])

                app.command("setSelection", start=5, end=5)
                app.command("insertText", text="Z")
                app.command("saveAs", path=str(saved_path))
                self.assertFalse(app.command("isModified")["modified"])
                app.command("openFile", path=str(saved_path))
                reopened = app.command("getLargeFileState")["largeFile"]
                deadline = time.monotonic() + 30.0
                while not reopened["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.03)
                    reopened = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(reopened["indexingComplete"])
                self.assertEqual(app.command("getLargeFileTextRange", start=0, length=12)["text"], "BEGINZ-LINUX")

            expected = hashlib.sha256()
            with path.open("rb") as source:
                expected.update(source.read(5))
                expected.update(b"Z")
                for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
                    expected.update(chunk)
            self.assertEqual(sha256(path), source_hash)
            self.assertEqual(sha256(saved_path), expected.hexdigest())
            self.assertEqual(list(Path(temp_root).glob("*.classic-notepad-save-*")), [])

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
                self.assertEqual(state["maximumLineColumns"], 126)
                self.assertLess(state["horizontalMaximumOffset"], 4096)
                self.assertEqual(state["horizontalOffset"], 0)
                scrolled = app.command("sendLargeFileHorizontalScroll", scrollCommand=7)["largeFile"]  # SB_RIGHT
                self.assertEqual(scrolled["horizontalOffset"], scrolled["horizontalMaximumOffset"])
                self.assertLess(scrolled["horizontalOffset"], 4096)
                reset_scroll = app.command("sendLargeFileHorizontalScroll", scrollCommand=6)["largeFile"]  # SB_LEFT
                self.assertEqual(reset_scroll["horizontalOffset"], 0)
                self.assertGreater(state["indexedLines"], 100)
                self.assertIn("BEGIN-LARGE-MODE", app.command("getLargeFileLine", line=1)["text"])
                last_content_line = state["totalLines"] - 1
                app.command("goToLine", line=last_content_line)
                self.assertIn("END-LARGE-MODE", app.command("getLargeFileLine", line=last_content_line)["text"])

                started_operation = app.command(
                    "startLargeFileFind", text="END-LARGE-MODE", matchCase=True
                )["largeFile"]
                self.assertEqual(started_operation["operationName"], "Find")
                # A second command must return while the immutable-generation
                # search is running, proving the endpoint/UI thread is not held.
                self.assertIn("Large File", app.command("getTitle")["title"])
                deadline = time.monotonic() + 10.0
                operation = app.command("getLargeFileState")["largeFile"]
                while operation["operationInProgress"] and time.monotonic() < deadline:
                    operation = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(operation["operationCompleted"])
                self.assertTrue(operation["operationSucceeded"])
                self.assertEqual(operation["operationResultCount"], 1)

                # Exercise the real child-window input path. These commands send
                # Win32 mouse/key/character messages to the custom control; they
                # do not call the document model's selection or edit methods.
                app.command("goToLine", line=1)
                click = app.command("sendLargeFileClick", x=52, y=8)["largeFile"]
                self.assertGreater(click["caret"], 0)
                self.assertLess(click["caret"], 126)
                self.assertGreater(click["caretRectangle"]["bottom"], click["caretRectangle"]["top"])
                clicked_at = click["caret"]
                app.command("sendLargeFileText", text="[CLICK]")
                self.assertEqual(
                    app.command("getLargeFileRange", start=clicked_at, length=7)["text"],
                    "[CLICK]",
                )
                app.command("sendLargeFileKey", key=0x5A, control=True)  # Ctrl+Z
                self.assertNotEqual(
                    app.command("getLargeFileRange", start=clicked_at, length=7)["text"],
                    "[CLICK]",
                )
                app.command("sendLargeFileText", text="Q")
                app.command("sendLargeFileKey", key=0x08)  # Backspace through WM_KEYDOWN.
                self.assertNotEqual(
                    app.command("getLargeFileRange", start=clicked_at, length=1)["text"], "Q"
                )
                selection = app.command("sendLargeFileKey", key=0x27, shift=True)["largeFile"]
                self.assertEqual(selection["selectionEnd"] - selection["selectionStart"], 1)
                deleted_start = selection["selectionStart"]
                deleted_text = app.command("getLargeFileRange", start=deleted_start, length=1)["text"]
                app.command("sendLargeFileKey", key=0x2E)  # Delete selected character.
                self.assertNotEqual(
                    app.command("getLargeFileRange", start=deleted_start, length=1)["text"], deleted_text
                )
                app.command("sendLargeFileKey", key=0x5A, control=True)
                dragged = app.command(
                    "sendLargeFileDrag", startX=12, startY=8, endX=92, endY=24
                )["largeFile"]
                self.assertGreater(dragged["selectionEnd"], dragged["selectionStart"])
                before_page = dragged["caret"]
                paged = app.command("sendLargeFileKey", key=0x22)["largeFile"]  # Page Down
                self.assertGreater(paged["caret"], before_page)
                self.assertGreater(paged["generation"], 0)
                self.assertIn("BEGIN-LARGE-MODE", app.command("getLargeFileRange", start=0, length=128)["text"])

                multiline = "clipboard-one\r\nclipboard-two"
                app.command("setSelection", start=0, end=0)
                app.command("insertText", text=multiline)
                app.command("setSelection", start=0, end=len(multiline))
                app.command("sendLargeFileKey", key=0x43, control=True)  # Ctrl+C via the control.
                app.command("sendLargeFileKey", key=0x5A, control=True)  # Remove staging insertion.
                app.command("setSelection", start=0, end=0)
                app.command("sendLargeFileKey", key=0x56, control=True)  # Ctrl+V through WM_PASTE.
                self.assertEqual(
                    app.command("getLargeFileRange", start=0, length=len(multiline))["text"], multiline
                )
                app.command("sendLargeFileKey", key=0x5A, control=True)
                app.command("setSelection", start=0, end=0)
                app.command("sendLargeFileText", text="\r\n")
                self.assertEqual(app.command("getLargeFileRange", start=0, length=2)["text"], "\r\n")
                app.command("sendLargeFileKey", key=0x5A, control=True)

                # Tabs must be expanded by the same bounded layout used for
                # painting, clicks and the native Win32 caret.
                app.command("setSelection", start=0, end=0)
                tab_end = app.command("sendLargeFileText", text="A\tB")["largeFile"]
                self.assertEqual(app.command("getLargeFileRange", start=0, length=3)["text"], "A\tB")
                before_b = app.command("sendLargeFileKey", key=0x25)["largeFile"]
                after_a = app.command("sendLargeFileKey", key=0x25)["largeFile"]
                self.assertGreater(
                    before_b["caretRectangle"]["left"] - after_a["caretRectangle"]["left"], 20
                )
                clicked_tab = app.command(
                    "sendLargeFileClick",
                    x=(before_b["caretRectangle"]["left"] + after_a["caretRectangle"]["left"]) // 2,
                    y=8,
                )["largeFile"]
                self.assertIn(clicked_tab["caret"], (1, 2))
                app.command("setSelection", start=0, end=3)
                app.command("sendLargeFileKey", key=0x2E)

                # Insert toggles overtype in the actual child window. Typing
                # replaces one character and a second Insert restores insert.
                app.command("setSelection", start=0, end=0)
                original_first = app.command("getLargeFileRange", start=0, length=1)["text"]
                app.command("sendLargeFileKey", key=0x2D)  # Insert => overtype.
                app.command("sendLargeFileText", text="Z")
                self.assertEqual(app.command("getLargeFileRange", start=0, length=1)["text"], "Z")
                app.command("sendLargeFileKey", key=0x5A, control=True)
                self.assertEqual(app.command("getLargeFileRange", start=0, length=1)["text"], original_first)
                app.command("sendLargeFileKey", key=0x2D)  # Restore insert mode.

                # Word wrap must alter the real custom control's bounded visual-row
                # cache; reporting only the preference is not sufficient.
                app.command("setFont", font="Consolas 24")
                app.command("setWordWrap", enabled=True)
                wrapped = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(wrapped["wordWrap"])
                self.assertGreater(wrapped["viewportVisualRows"], 0)
                self.assertGreater(wrapped["wrappedContinuationRows"], 0)
                wrapped_click = app.command("sendLargeFileClick", x=52, y=40)["largeFile"]
                wrapped_at = wrapped_click["caret"]
                app.command("sendLargeFileText", text="[WRAP]")
                self.assertEqual(
                    app.command("getLargeFileRange", start=wrapped_at, length=6)["text"], "[WRAP]"
                )
                app.command("sendLargeFileKey", key=0x5A, control=True)
                app.command("setWordWrap", enabled=False)
                self.assertFalse(app.command("getLargeFileState")["largeFile"]["wordWrap"])
                app.command("setFont", font="Consolas 11")

                if app.command("getCapabilities")["capabilities"]["spellCheck"]:
                    app.command("setSelection", start=0, end=0)
                    app.command("sendLargeFileText", text="teh ")
                    deadline = time.monotonic() + 3.0
                    spelling = app.command("getLargeFileState")["largeFile"]
                    while spelling["spellingIssueCount"] == 0 and time.monotonic() < deadline:
                        time.sleep(0.05)
                        spelling = app.command("getLargeFileState")["largeFile"]
                    self.assertGreater(spelling["spellingIssueCount"], 0)
                    app.command("sendLargeFileKey", key=0x5A, control=True)

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

                save_started = app.command("startLargeFileSave", path=str(saved_path))["largeFile"]
                self.assertTrue(save_started["operationInProgress"])
                self.assertEqual(save_started["operationName"], "Save")
                self.assertIn("Large File", app.command("getTitle")["title"])
                save_deadline = time.monotonic() + 45.0
                save_operation = app.command("getLargeFileState")["largeFile"]
                while save_operation["operationInProgress"] and time.monotonic() < save_deadline:
                    time.sleep(0.01)
                    save_operation = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(save_operation["operationCompleted"])
                self.assertTrue(save_operation["operationSucceeded"], save_operation["operationError"])
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
                app.command("setSelection", start=saved_state["characterCount"], end=saved_state["characterCount"])
                app.command("insertText", text="[NORMAL-SAVE]")
                app.command("save")
                normal_deadline = time.monotonic() + 45.0
                normal_state = app.command("getLargeFileState")["largeFile"]
                while not normal_state["indexingComplete"] and time.monotonic() < normal_deadline:
                    time.sleep(0.05)
                    normal_state = app.command("getLargeFileState")["largeFile"]
                self.assertIn("[NORMAL-SAVE]", app.command("getLargeFileRange", start=normal_state["characterCount"] - 64, length=64)["text"])

                # Replacing the active document must cancel or reject stale
                # immutable-generation work without leaving the UI thread or a
                # sibling temporary save behind.
                replace_started = app.command(
                    "startLargeFileReplaceAll",
                    text="small-large-file-test",
                    replacement="cancelled-large-file-test",
                    matchCase=True,
                    wholeWord=False,
                )["largeFile"]
                self.assertTrue(replace_started["operationInProgress"])
                cancel_started = time.perf_counter()
                app.command("newDocument")
                self.assertLess(time.perf_counter() - cancel_started, 2.0)
                self.assertFalse(app.command("getLargeFileState")["largeFile"]["active"])
                self.assertEqual(list(Path(temp_root).glob("*.classic-notepad-save-*.tmp")), [])

            self.assertEqual(edge_bytes(), original)


if __name__ == "__main__":
    unittest.main()
