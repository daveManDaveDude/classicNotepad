import ctypes
import os
import tempfile
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


class FileWorkflowTests(unittest.TestCase):
    @unittest.skipUnless(automation_platform() == "windows", "Windows destructive replacement recovery")
    def test_destination_displacement_keeps_ordinary_output_and_allows_retry(self):
        previous_fault = os.environ.get("CLASSIC_NOTEPAD_TEST_SAVE_FAULT")
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                source_path = root / "displaced-ordinary.txt"
                source_path.write_bytes(b"original text")
                expected = b"Xoriginal text"
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = "replace-destination-moved"
                with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                    app.command("openFile", path=str(source_path))
                    app.command("setSelection", start=0, end=0)
                    app.command("insertText", text="X")
                    response = app.try_command("save")
                    self.assertFalse(response.get("ok", False), response)

                    state = app.command("getLargeFileState")["largeFile"]
                    self.assertFalse(source_path.exists())
                    backup = list(root.glob("displaced-ordinary.txt.classic-notepad-test-original-*.bak"))
                    recovery = Path(state["saveRecoveryPath"])
                    self.assertEqual(len(backup), 1)
                    self.assertEqual(backup[0].read_bytes(), b"original text")
                    self.assertTrue(recovery.is_file())
                    self.assertEqual(recovery.read_bytes(), expected)
                    self.assertEqual(state["saveFailureStage"], "replace")
                    self.assertEqual(state["saveWin32Error"], 1177)
                    self.assertTrue(state["saveOutputComplete"])
                    self.assertFalse(state["saveCommitted"])
                    self.assertIn(str(recovery), response["error"])
                    self.assertTrue(app.command("isModified")["modified"])
                    self.assertEqual(app.command("getText")["text"], "Xoriginal text")

                    app.command("save")
                    self.assertEqual(source_path.read_bytes(), expected)
                    self.assertEqual(recovery.read_bytes(), expected)
                    self.assertFalse(app.command("isModified")["modified"])
                    state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["saveCommitted"])
                    self.assertEqual(state["saveFailureStage"], "none")
        finally:
            if previous_fault is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_SAVE_FAULT", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = previous_fault

    @unittest.skipUnless(automation_platform() == "windows", "Windows post-replacement diagnostics")
    def test_late_replace_error_is_reconciled_after_real_commit(self):
        previous_fault = os.environ.get("CLASSIC_NOTEPAD_TEST_SAVE_FAULT")
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                source_path = Path(temp_root) / "late-replace-error.txt"
                source_path.write_bytes(b"original text")
                expected = b"Xoriginal text"
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = "replace-after-commit"
                with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                    app.command("openFile", path=str(source_path))
                    app.command("setSelection", start=0, end=0)
                    app.command("insertText", text="X")
                    app.command("save")

                    state = app.command("getLargeFileState")["largeFile"]
                    self.assertEqual(source_path.read_bytes(), expected)
                    self.assertEqual(state["saveFailureStage"], "none")
                    self.assertEqual(state["saveWin32Error"], 0)
                    self.assertTrue(state["saveOutputComplete"])
                    self.assertTrue(state["saveCommitted"])
                    self.assertEqual(state["saveRecoveryPath"], str(source_path))
                    self.assertFalse(app.command("isModified")["modified"])
                    self.assertEqual(list(Path(temp_root).glob("late-replace-error.txt.classic-notepad-save-*.tmp")), [])
        finally:
            if previous_fault is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_SAVE_FAULT", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = previous_fault

    @unittest.skipUnless(automation_platform() == "windows", "Windows atomic-save diagnostics")
    def test_ordinary_save_faults_preserve_document_and_report_recovery(self):
        previous_fault = os.environ.get("CLASSIC_NOTEPAD_TEST_SAVE_FAULT")
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                for stage, win32_error in (("write", 29), ("flush", 29), ("replace", 5)):
                    with self.subTest(stage=stage):
                        source_path = root / f"ordinary-{stage}.txt"
                        source_path.write_bytes(b"original text")
                        expected_bytes = b"Xoriginal text"
                        os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = stage
                        with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                            app.command("openFile", path=str(source_path))
                            app.command("setSelection", start=0, end=0)
                            app.command("insertText", text="X")
                            response = app.try_command("save")
                            self.assertFalse(response.get("ok", False), response)

                            state = app.command("getLargeFileState")["largeFile"]
                            self.assertEqual(state["saveFailureStage"], stage)
                            self.assertEqual(state["saveWin32Error"], win32_error)
                            self.assertTrue(app.command("isModified")["modified"])
                            self.assertEqual(app.command("getText")["text"], "Xoriginal text")
                            self.assertEqual(source_path.read_bytes(), b"original text")
                            if stage == "replace":
                                self.assertTrue(state["saveOutputComplete"])
                                self.assertFalse(state["saveCommitted"])
                                recovery = Path(state["saveRecoveryPath"])
                                self.assertTrue(recovery.is_file())
                                self.assertEqual(recovery.read_bytes(), expected_bytes)
                                self.assertIn(str(recovery), response["error"])
                            else:
                                self.assertFalse(state["saveOutputComplete"])
                                self.assertFalse(state["saveRecoveryPath"])

        finally:
            if previous_fault is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_SAVE_FAULT", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = previous_fault

    @unittest.skipUnless(automation_platform() == "windows", "Windows locked-destination replacement")
    def test_ordinary_locked_destination_keeps_complete_recovery_and_can_retry(self):
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.argtypes = (ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p)
        kernel32.CreateFileW.restype = ctypes.c_void_p
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        kernel32.CloseHandle.restype = ctypes.c_int
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            source_path = Path(temp_root) / "locked-ordinary-save.txt"
            source_path.write_bytes(b"original text")
            expected = b"Xoriginal text"
            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                app.command("openFile", path=str(source_path))
                app.command("setSelection", start=0, end=0)
                app.command("insertText", text="X")
                handle = kernel32.CreateFileW(str(source_path), 0x80000000, 0x3, None, 3, 0, None)
                if handle == ctypes.c_void_p(-1).value:
                    raise ctypes.WinError(ctypes.get_last_error())
                try:
                    response = app.try_command("save")
                    self.assertFalse(response.get("ok", False), response)
                    state = app.command("getLargeFileState")["largeFile"]
                    self.assertEqual(state["saveFailureStage"], "replace")
                    self.assertEqual(state["saveWin32Error"], 32)
                    self.assertTrue(state["saveOutputComplete"])
                    self.assertFalse(state["saveCommitted"])
                    recovery = Path(state["saveRecoveryPath"])
                    self.assertTrue(recovery.is_file())
                    self.assertEqual(recovery.read_bytes(), expected)
                    self.assertEqual(source_path.read_bytes(), b"original text")
                    self.assertEqual(app.command("getText")["text"], "Xoriginal text")
                finally:
                    kernel32.CloseHandle(handle)

                app.command("save")
                self.assertEqual(source_path.read_bytes(), expected)
                state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["saveCommitted"])
                self.assertFalse(app.command("isModified")["modified"])

    @unittest.skipUnless(automation_platform() == "windows", "Windows read-only Save destination")
    def test_read_only_destination_remains_unchanged_with_recoverable_save_as_output(self):
        previous_fault = os.environ.get("CLASSIC_NOTEPAD_TEST_SAVE_FAULT")
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                destination = Path(temp_root) / "read-only-save-as.txt"
                destination.write_bytes(b"old destination")
                destination.chmod(0o444)
                os.environ.pop("CLASSIC_NOTEPAD_TEST_SAVE_FAULT", None)
                try:
                    with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                        app.command("setText", text="new complete output")
                        response = app.try_command("saveAs", path=str(destination))
                        self.assertFalse(response.get("ok", False), response)
                        state = app.command("getLargeFileState")["largeFile"]
                        self.assertEqual(state["saveFailureStage"], "replace")
                        self.assertEqual(state["saveWin32Error"], 5)
                        self.assertTrue(state["saveOutputComplete"])
                        self.assertFalse(state["saveCommitted"])
                        recovery = Path(state["saveRecoveryPath"])
                        self.assertTrue(recovery.is_file())
                        self.assertEqual(recovery.read_bytes(), b"new complete output")
                        self.assertEqual(destination.read_bytes(), b"old destination")
                        self.assertTrue(app.command("isModified")["modified"])
                        self.assertFalse(app.command("getDocumentMetadata")["metadata"]["hasPath"])
                finally:
                    destination.chmod(0o666)
        finally:
            if previous_fault is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_SAVE_FAULT", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = previous_fault

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
