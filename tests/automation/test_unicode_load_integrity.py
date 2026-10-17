import os
import tempfile
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


class UnicodeLoadIntegrityTests(unittest.TestCase):
    @staticmethod
    def _write_mapped_candidate(path):
        line = b"mapped metadata allocation boundary".ljust(126, b".") + b"\r\n"
        block = line * 8192
        with path.open("wb") as output:
            for _ in range(49):
                output.write(block)

    @staticmethod
    def _wait_for_large_index(app):
        import time

        deadline = time.monotonic() + 60.0
        state = app.command("getLargeFileState")["largeFile"]
        while not state["indexingComplete"] and time.monotonic() < deadline:
            time.sleep(0.05)
            state = app.command("getLargeFileState")["largeFile"]
        if not state["indexingComplete"]:
            raise AssertionError("large-file indexing did not complete")
        return state

    @unittest.skipUnless(automation_platform() == "windows", "Windows mapped metadata publication")
    def test_failed_mapped_adoption_preserves_ordinary_document(self):
        previous_failure = os.environ.get("CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE")
        os.environ["CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE"] = "mapped-adoption"
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                prior_path = root / "ordinary-prior.txt"
                mapped_path = root / "mapped-candidate.txt"
                self._write_mapped_candidate(mapped_path)
                prior_path.write_bytes(b"saved ordinary document")

                with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=90.0) as app:
                    app.command("setText", text="saved ordinary document")
                    app.command("saveAs", path=str(prior_path))
                    app.command("setText", text="dirty ordinary document survives")
                    app.command("setSelection", start=6, end=14)
                    app.command("setWordWrap", enabled=True)

                    def snapshot():
                        return {
                            "text": app.command("getText")["text"],
                            "metadata": app.command("getDocumentMetadata")["metadata"],
                            "modified": app.command("isModified")["modified"],
                            "selection": app.command("getSelection")["selection"],
                            "word_wrap": app.command("getWordWrap")["enabled"],
                            "large": app.command("getLargeFileState")["largeFile"],
                        }

                    before = snapshot()
                    response = app.try_command("openFile", path=str(mapped_path))
                    self.assertFalse(response.get("ok", False), response)
                    self.assertEqual(snapshot(), before)
                    app.command("insertText", text="EDIT")
                    start, end = before["selection"]["start"], before["selection"]["end"]
                    self.assertEqual(
                        app.command("getText")["text"],
                        before["text"][:start] + "EDIT" + before["text"][end:],
                    )
                    self.assertTrue(app.command("isModified")["modified"])
        finally:
            if previous_failure is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE"] = previous_failure

    @unittest.skipUnless(automation_platform() == "windows", "Windows mapped metadata publication")
    def test_failed_mapped_adoption_preserves_dirty_mapped_document(self):
        previous_failure = os.environ.get("CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE")
        os.environ["CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE"] = "mapped-adoption-after-first"
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                prior_path = root / "mapped-prior.txt"
                candidate_path = root / "mapped-replacement.txt"
                self._write_mapped_candidate(prior_path)
                self._write_mapped_candidate(candidate_path)

                with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=120.0) as app:
                    app.command("openFile", path=str(prior_path))
                    prior_state = self._wait_for_large_index(app)
                    self.assertTrue(prior_state["active"])
                    app.command("setSelection", start=0, end=0)
                    app.command("insertText", text="dirty ")
                    app.command("setSelection", start=2, end=6)
                    app.command("setWordWrap", enabled=True)

                    def snapshot():
                        large = app.command("getLargeFileState")["largeFile"]
                        return {
                            "metadata": app.command("getDocumentMetadata")["metadata"],
                            "modified": app.command("isModified")["modified"],
                            "selection": app.command("getSelection")["selection"],
                            "large": {
                                key: large[key]
                                for key in (
                                    "active", "modified", "indexingComplete", "encodingValid", "fileSize",
                                    "characterCount", "caret", "selectionStart", "selectionEnd", "wordWrap",
                                    "generation", "pieceCount", "insertedBufferBytes",
                                )
                            },
                            "prefix": app.command("getLargeFileRange", start=0, length=64)["text"],
                        }

                    before = snapshot()
                    self.assertTrue(before["modified"])
                    self.assertEqual(before["metadata"]["path"], str(prior_path))
                    response = app.try_command("openFile", path=str(candidate_path))
                    self.assertFalse(response.get("ok", False), response)
                    self.assertEqual(snapshot(), before)

                    app.command("insertText", text="EDIT")
                    expected_prefix = before["prefix"][:2] + "EDIT" + before["prefix"][6:]
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=64)["text"], expected_prefix)
                    self.assertTrue(app.command("isModified")["modified"])
        finally:
            if previous_failure is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE"] = previous_failure

    @unittest.skipUnless(automation_platform() == "windows", "Windows allocation-failure recovery")
    def test_postpublication_allocation_failure_reports_success_and_keeps_new_mapped_document(self):
        previous_failure = os.environ.get("CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE")
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                prior_path = root / "mapped-prior.txt"
                candidate_path = root / "mapped-new.txt"
                self._write_mapped_candidate(prior_path)
                self._write_mapped_candidate(candidate_path)
                with candidate_path.open("r+b") as output:
                    output.write(b"NEW")
                for stage in (
                    "mapped-presentation-bad-alloc", "mapped-presentation-length-error",
                    "mapped-focus-bad-alloc", "mapped-focus-length-error",
                    "mapped-paint-bad-alloc", "mapped-paint-length-error",
                ):
                    with self.subTest(stage=stage):
                        os.environ["CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE"] = stage
                        with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=120.0) as app:
                            app.command("openFile", path=str(prior_path))
                            app.command("setSelection", start=0, end=0)
                            app.command("insertText", text="dirty prior ")
                            self.assertTrue(app.command("isModified")["modified"])
                            response = app.try_command("openFile", path=str(candidate_path))
                            self.assertTrue(response.get("ok", False), response)
                            state = app.command("getLargeFileState")["largeFile"]
                            self.assertTrue(state["active"])
                            self.assertTrue(state["indexingComplete"])
                            self.assertTrue(state["encodingValid"])
                            self.assertTrue(state["editable"])
                            self.assertFalse(state["readOnly"])
                            self.assertFalse(state["modified"])
                            expected_callback_failures = 0 if "presentation" in stage else 1
                            self.assertEqual(state["callbackAllocationFailures"], expected_callback_failures)
                            self.assertFalse(app.command("isModified")["modified"])
                            self.assertEqual(state["fileSize"], candidate_path.stat().st_size)
                            metadata = app.command("getDocumentMetadata")["metadata"]
                            self.assertEqual(metadata["path"], str(candidate_path))
                            self.assertEqual(metadata["displayName"], candidate_path.name)
                            self.assertTrue(metadata["hasPath"])
                            self.assertEqual(metadata["encoding"], "UTF-8")
                            self.assertEqual(metadata["lineEnding"], "Windows (CRLF)")
                            self.assertEqual(metadata["saveLineEnding"], "Windows (CRLF)")
                            prefix = app.command("getLargeFileRange", start=0, length=64)["text"]
                            self.assertTrue(prefix.startswith("NEW"), prefix)
                            app.command("setSelection", start=0, end=0)
                            app.command("sendLargeFileText", text="EDIT ")
                            self.assertEqual(app.command("getLargeFileRange", start=0, length=69)["text"], "EDIT " + prefix)
                            self.assertTrue(app.command("isModified")["modified"])
                            edited_state = app.command("getLargeFileState")["largeFile"]
                            self.assertTrue(edited_state["modified"])
                            self.assertEqual(edited_state["callbackAllocationFailures"], expected_callback_failures)
                            self.assertEqual(app.command("getDocumentMetadata")["metadata"]["path"], str(candidate_path))
        finally:
            if previous_failure is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE"] = previous_failure

    @unittest.skipUnless(automation_platform() == "windows", "Windows allocation-failure recovery")
    def test_allocation_failures_preserve_document_and_leave_editing_usable(self):
        previous_failure = os.environ.get("CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE")
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                prior_path = root / "allocation-prior.txt"
                replacement_path = root / "allocation-replacement.txt"
                prior_path.write_bytes(b"initial")
                replacement_path.write_bytes(b"replacement")

                for stage in ("file-preparation", "after-adoption"):
                    with self.subTest(stage=stage):
                        os.environ["CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE"] = stage
                        with ClassicNotepadDriver(
                            automation_binary(), automation_platform(), timeout=30.0
                        ) as app:
                            app.command("setText", text="saved baseline")
                            app.command("saveAs", path=str(prior_path))
                            app.command("setText", text="dirty baseline survives allocation failure")
                            app.command("setSelection", start=6, end=14)
                            app.command("setWordWrap", enabled=True)

                            def snapshot():
                                return {
                                    "text": app.command("getText")["text"],
                                    "metadata": app.command("getDocumentMetadata")["metadata"],
                                    "modified": app.command("isModified")["modified"],
                                    "selection": app.command("getSelection")["selection"],
                                    "word_wrap": app.command("getWordWrap")["enabled"],
                                    "large_active": app.command("getLargeFileState")["largeFile"]["active"],
                                }

                            before = snapshot()
                            self.assertTrue(before["modified"])
                            self.assertEqual(before["metadata"]["path"], str(prior_path))
                            response = app.try_command("openFile", path=str(replacement_path))
                            self.assertFalse(response.get("ok", False), response)
                            self.assertEqual(snapshot(), before)

                            if stage == "after-adoption":
                                app.command("setWordWrap", enabled=False)
                                self.assertEqual(snapshot(), before)

                            app.command("insertText", text="!")
                            edited = app.command("getText")["text"]
                            start = before["selection"]["start"]
                            end = before["selection"]["end"]
                            self.assertEqual(edited, before["text"][:start] + "!" + before["text"][end:])
                            self.assertTrue(app.command("isModified")["modified"])
        finally:
            if previous_failure is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE"] = previous_failure

    @unittest.skipUnless(automation_platform() == "windows", "Windows large-file viewport")
    def test_large_viewport_keeps_supplementary_scalar_whole_at_64k_unit_limit(self):
        file_size = 49 * 1024 * 1024
        prefix_units = 65535
        scalar = b"\xf0\x9f\x98\x80"
        fill = b"x" * (1024 * 1024)

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            root = Path(temp_root)
            source_path = root / "viewport-scalar-boundary.txt"
            with source_path.open("wb") as output:
                output.write(b"x" * prefix_units)
                output.write(scalar)
                remaining = file_size - prefix_units - len(scalar)
                while remaining:
                    chunk = fill[:min(len(fill), remaining)]
                    output.write(chunk)
                    remaining -= len(chunk)

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=60.0) as app:
                app.command("openFile", path=str(source_path))
                state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])
                self.assertTrue(state["encodingValid"])
                self.assertEqual(state["fileSize"], file_size)

                visible = app.command("getLargeFileVisibleText")
                self.assertEqual(visible["start"], 0)
                self.assertTrue(
                    visible["text"].endswith("\U0001f600"),
                    "the 64 Ki-unit viewport limit must include the complete scalar at its boundary",
                )

    @unittest.skipUnless(automation_platform() == "windows", "Windows edit-control adoption")
    def test_failed_open_and_wrap_recreation_preserve_the_current_document(self):
        previous_failure = os.environ.get("CLASSIC_NOTEPAD_TEST_EDITOR_ADOPTION_FAILURE")
        os.environ["CLASSIC_NOTEPAD_TEST_EDITOR_ADOPTION_FAILURE"] = "partial"
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                saved_path = root / "prior.txt"
                nul_path = root / "embedded-nul.txt"
                utf16_path = root / "malformed-utf16.txt"
                replacement_path = root / "replacement.txt"
                saved_path.write_bytes(b"original")
                nul_path.write_bytes(b"new\x00text")
                utf16_path.write_bytes(b"\xff\xfe\x00\xd8")
                replacement_path.write_bytes(b"replacement text")

                with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=30.0) as app:
                    app.command("setText", text="active document")
                    app.command("saveAs", path=str(saved_path))
                    app.command("setText", text="dirty active document")
                    app.command("setSelection", start=3, end=8)

                    def snapshot():
                        return {
                            "text": app.command("getText")["text"],
                            "metadata": app.command("getDocumentMetadata")["metadata"],
                            "modified": app.command("isModified")["modified"],
                            "selection": app.command("getSelection")["selection"],
                            "word_wrap": app.command("getWordWrap")["enabled"],
                            "large_active": app.command("getLargeFileState")["largeFile"]["active"],
                        }

                    before = snapshot()
                    self.assertTrue(before["modified"])
                    self.assertEqual(before["metadata"]["path"], str(saved_path))

                    for path in (nul_path, utf16_path, replacement_path):
                        with self.subTest(path=path.name):
                            response = app.try_command("openFile", path=str(path))
                            self.assertFalse(response.get("ok", False), response)
                            self.assertEqual(snapshot(), before)

                    app.command("setWordWrap", enabled=not before["word_wrap"])
                    self.assertEqual(snapshot(), before)
        finally:
            if previous_failure is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_EDITOR_ADOPTION_FAILURE", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_EDITOR_ADOPTION_FAILURE"] = previous_failure

    @unittest.skipUnless(automation_platform() == "windows", "Windows explicit ANSI code-page fallback")
    def test_malformed_ansi_fallback_preserves_the_current_document(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            root = Path(temp_root)
            saved_path = root / "ansi-prior.txt"
            malformed_utf8_acp = root / "malformed-utf8-acp.txt"
            malformed_sjis = root / "malformed-shift-jis.txt"
            saved_path.write_bytes(b"initial")
            malformed_utf8_acp.write_bytes(b"\xc3(")
            malformed_sjis.write_bytes(b"\x82")

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=30.0) as app:
                app.command("setText", text="prior saved document")
                app.command("saveAs", path=str(saved_path))
                app.command("setText", text="dirty document survives malformed ANSI")
                app.command("setSelection", start=4, end=12)
                app.command("setWordWrap", enabled=True)

                def snapshot():
                    return {
                        "text": app.command("getText")["text"],
                        "metadata": app.command("getDocumentMetadata")["metadata"],
                        "modified": app.command("isModified")["modified"],
                        "selection": app.command("getSelection")["selection"],
                        "word_wrap": app.command("getWordWrap")["enabled"],
                        "large_active": app.command("getLargeFileState")["largeFile"]["active"],
                    }

                before = snapshot()
                self.assertTrue(before["modified"])
                self.assertTrue(before["word_wrap"])
                for path, code_page in ((malformed_utf8_acp, 65001), (malformed_sjis, 932)):
                    with self.subTest(path=path.name, code_page=code_page):
                        response = app.try_command(
                            "openFile", path=str(path), ansiCodePage=code_page)
                        self.assertFalse(response.get("ok", False), response)
                        self.assertEqual(snapshot(), before)

    @unittest.skipUnless(automation_platform() == "windows", "Windows mapped-load validation")
    def test_late_large_file_nul_failure_retains_the_previous_dirty_document(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            root = Path(temp_root)
            prior_path = root / "prior-large-load.txt"
            invalid_large_path = root / "late-nul-large.txt"
            prior_path.write_bytes(b"original")
            chunk = b"x" * (1024 * 1024)
            with invalid_large_path.open("wb") as output:
                for _ in range(49):
                    output.write(chunk)
                output.seek(-1, os.SEEK_END)
                output.write(b"\x00")

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=60.0) as app:
                app.command("setText", text="large candidate must not replace this")
                app.command("saveAs", path=str(prior_path))
                app.command("setText", text="dirty prior document")
                app.command("setSelection", start=2, end=7)
                before = {
                    "text": app.command("getText")["text"],
                    "metadata": app.command("getDocumentMetadata")["metadata"],
                    "modified": app.command("isModified")["modified"],
                    "selection": app.command("getSelection")["selection"],
                    "large_active": app.command("getLargeFileState")["largeFile"]["active"],
                }

                response = app.try_command("openFile", path=str(invalid_large_path))
                self.assertFalse(response.get("ok", False), response)
                self.assertEqual(app.command("getText")["text"], before["text"])
                self.assertEqual(app.command("getDocumentMetadata")["metadata"], before["metadata"])
                self.assertEqual(app.command("isModified")["modified"], before["modified"])
                self.assertEqual(app.command("getSelection")["selection"], before["selection"])
                self.assertEqual(app.command("getLargeFileState")["largeFile"]["active"], before["large_active"])


if __name__ == "__main__":
    unittest.main()
