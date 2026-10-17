import hashlib
import ctypes
import os
import tempfile
import time
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


def utf16_units(value: str) -> int:
    return len(value.encode("utf-16-le", errors="surrogatepass")) // 2


def _save_gate_name(process_id: int, suffix: str) -> str:
    return rf"Local\ClassicNotepadA2SaveGate-{process_id}-{suffix}"


def _wait_save_gate(process_id: int, suffix: str, timeout: float = 30.0) -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenEventW.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p)
    kernel32.OpenEventW.restype = ctypes.c_void_p
    kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    kernel32.WaitForSingleObject.restype = ctypes.c_uint32
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.restype = ctypes.c_int
    deadline = time.monotonic() + timeout
    handle = None
    while time.monotonic() < deadline:
        handle = kernel32.OpenEventW(0x00100000, 0, _save_gate_name(process_id, suffix))
        if handle:
            break
        time.sleep(0.025)
    if not handle:
        raise AssertionError(f"timed out opening Save gate event {suffix}")
    try:
        remaining = max(0, int((deadline - time.monotonic()) * 1000))
        if kernel32.WaitForSingleObject(handle, remaining) != 0:
            raise AssertionError(f"timed out waiting for Save gate event {suffix}")
    finally:
        kernel32.CloseHandle(handle)


def _release_save_gate(process_id: int, timeout: float = 30.0) -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenEventW.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p)
    kernel32.OpenEventW.restype = ctypes.c_void_p
    kernel32.SetEvent.argtypes = (ctypes.c_void_p,)
    kernel32.SetEvent.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.restype = ctypes.c_int
    deadline = time.monotonic() + timeout
    handle = None
    while time.monotonic() < deadline:
        handle = kernel32.OpenEventW(0x0002, 0, _save_gate_name(process_id, "proceed"))
        if handle:
            break
        time.sleep(0.025)
    if not handle:
        raise AssertionError("timed out opening Save gate proceed event")
    try:
        if not kernel32.SetEvent(handle):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel32.CloseHandle(handle)


def _operation_gate_name(process_id: int, operation: str, suffix: str) -> str:
    return rf"Local\ClassicNotepadA2OperationGate-{process_id}-{operation}-{suffix}"


def _wait_operation_gate(process_id: int, operation: str, suffix: str = "ready", timeout: float = 30.0) -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenEventW.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p)
    kernel32.OpenEventW.restype = ctypes.c_void_p
    kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    kernel32.WaitForSingleObject.restype = ctypes.c_uint32
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.restype = ctypes.c_int
    deadline = time.monotonic() + timeout
    handle = None
    while time.monotonic() < deadline:
        handle = kernel32.OpenEventW(0x00100000, 0, _operation_gate_name(process_id, operation, suffix))
        if handle:
            break
        time.sleep(0.025)
    if not handle:
        raise AssertionError(f"timed out opening {operation} gate event")
    try:
        remaining = max(0, int((deadline - time.monotonic()) * 1000))
        if kernel32.WaitForSingleObject(handle, remaining) != 0:
            raise AssertionError(f"timed out waiting for {operation} gate event")
    finally:
        kernel32.CloseHandle(handle)


def _release_operation_gate(process_id: int, operation: str, timeout: float = 30.0) -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenEventW.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p)
    kernel32.OpenEventW.restype = ctypes.c_void_p
    kernel32.SetEvent.argtypes = (ctypes.c_void_p,)
    kernel32.SetEvent.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.restype = ctypes.c_int
    deadline = time.monotonic() + timeout
    handle = None
    while time.monotonic() < deadline:
        handle = kernel32.OpenEventW(0x0002, 0, _operation_gate_name(process_id, operation, "proceed"))
        if handle:
            break
        time.sleep(0.025)
    if not handle:
        raise AssertionError(f"timed out opening {operation} gate proceed event")
    try:
        if not kernel32.SetEvent(handle):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel32.CloseHandle(handle)


def _adoption_gate_name(process_id: int, operation: str, suffix: str) -> str:
    return rf"Local\ClassicNotepadA4AdoptionGate-{process_id}-{operation}-{suffix}"


def _wait_adoption_gate(process_id: int, operation: str, timeout: float = 30.0) -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenEventW.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p)
    kernel32.OpenEventW.restype = ctypes.c_void_p
    kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    kernel32.WaitForSingleObject.restype = ctypes.c_uint32
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.restype = ctypes.c_int
    deadline = time.monotonic() + timeout
    handle = None
    while time.monotonic() < deadline:
        handle = kernel32.OpenEventW(0x00100000, 0, _adoption_gate_name(process_id, operation, "ready"))
        if handle:
            break
        time.sleep(0.025)
    if not handle:
        raise AssertionError(f"timed out opening {operation} adoption gate event")
    try:
        remaining = max(0, int((deadline - time.monotonic()) * 1000))
        if kernel32.WaitForSingleObject(handle, remaining) != 0:
            raise AssertionError(f"timed out waiting for {operation} adoption gate event")
    finally:
        kernel32.CloseHandle(handle)


def _release_adoption_gate(process_id: int, operation: str, timeout: float = 30.0) -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenEventW.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p)
    kernel32.OpenEventW.restype = ctypes.c_void_p
    kernel32.SetEvent.argtypes = (ctypes.c_void_p,)
    kernel32.SetEvent.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.restype = ctypes.c_int
    deadline = time.monotonic() + timeout
    handle = None
    while time.monotonic() < deadline:
        handle = kernel32.OpenEventW(0x0002, 0, _adoption_gate_name(process_id, operation, "proceed"))
        if handle:
            break
        time.sleep(0.025)
    if not handle:
        raise AssertionError(f"timed out opening {operation} adoption gate proceed event")
    try:
        if not kernel32.SetEvent(handle):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel32.CloseHandle(handle)


class LargeFileModeTests(unittest.TestCase):
    @unittest.skipUnless(automation_platform() == "windows", "mapped spelling context targeting is Windows-only")
    def test_mapped_context_target_uses_full_offsets_for_far_right_and_wrapped_rows(self):
        body_bytes = 50 * 1024 * 1024
        marker_offset = 100000
        tail = " café 漢字 😀\tWiWide\tteh " + "x" * 40 + " END"
        tail_bytes = tail.encode("utf-8")
        tail_target = body_bytes + len(tail[:tail.index("teh")].encode("utf-16-le")) // 2
        first_target = marker_offset + 1

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            root = Path(temp_root)
            source_path = root / "mapped-context-target.txt"
            block = b"x" * (1024 * 1024)
            with source_path.open("wb") as output:
                remaining = body_bytes
                while remaining:
                    count = min(remaining, len(block))
                    output.write(block[:count])
                    remaining -= count
                output.seek(marker_offset)
                output.write(b" teh ")
                output.seek(body_bytes)
                output.write(tail_bytes)

            with ClassicNotepadDriver(automation_binary(), "windows", timeout=180.0) as app:
                if not app.command("getCapabilities")["capabilities"]["spellCheck"]:
                    self.skipTest("Spell backend is unavailable; a current mapped spelling batch is required.")
                app.command("openFile", path=str(source_path))
                deadline = time.monotonic() + 120.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.05)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"], state)
                self.assertTrue(state["active"], state)
                self.assertGreater(tail_target, 65535)

                def select_resolve(position, keyboard=False):
                    app.command("setSelection", start=position, end=position)
                    deadline = time.monotonic() + 8.0
                    last = None
                    while time.monotonic() < deadline:
                        if keyboard:
                            last = app.try_command("resolveSpellingContextTarget", keyboard=True)
                            if last.get("ok") and last.get("found"):
                                return None, last
                        else:
                            point = app.try_command("getVisibleTextPositionPoint", position=position + 1)
                            if point.get("ok"):
                                last = app.command("resolveSpellingContextTarget", x=point["x"], y=point["y"])
                                if last["found"]:
                                    return point, last
                            else:
                                last = {"pointError": point.get("error"),
                                    "state": app.command("getLargeFileState")["largeFile"]}
                        time.sleep(0.05)
                    self.fail(f"No mapped context target for {position}: {last}")

                def resolve_visible(position):
                    deadline = time.monotonic() + 8.0
                    last = None
                    while time.monotonic() < deadline:
                        point = app.try_command("getVisibleTextPositionPoint", position=position + 1)
                        if point.get("ok"):
                            last = app.command("resolveSpellingContextTarget", x=point["x"], y=point["y"])
                            if last["found"]:
                                return point, last
                        else:
                            last = {"pointError": point.get("error"),
                                "state": app.command("getLargeFileState")["largeFile"]}
                        time.sleep(0.05)
                    self.fail(f"No mapped context target in the measured visible segment at {position}: {last}")

                first_point, first = select_resolve(first_target)
                self.assertEqual((first["start"], first["length"], first["text"]),
                    (first_target, 3, "teh"), first)
                app.command("resizeLargeFileViewport", width=1200, height=700)
                stale_resize = app.command("resolveSpellingContextTarget",
                    x=first_point["x"], y=first_point["y"])
                self.assertFalse(stale_resize["found"], stale_resize)
                first_point, first = select_resolve(first_target)
                self.assertEqual((first["start"], first["text"]), (first_target, "teh"), first)
                keyboard_point, keyboard = select_resolve(first_target + 3, keyboard=True)
                self.assertIsNone(keyboard_point)
                self.assertEqual((keyboard["start"], keyboard["length"], keyboard["text"]),
                    (first_target, 3, "teh"), keyboard)
                app.command("sendEditorContextMenu", keyboard=True)

                app.command("setWordWrap", enabled=False)
                app.command("sendLargeFileHorizontalScroll", scrollCommand=7)  # SB_RIGHT
                tail_point, distant = resolve_visible(tail_target)
                self.assertEqual((distant["start"], distant["length"], distant["text"]),
                    (tail_target, 3, "teh"), distant)
                self.assertGreater(distant["start"], 65535)
                target_lookup_before = {
                    "state": app.command("getLargeFileState")["largeFile"],
                    "selection": app.command("getSelection")["selection"],
                    "modified": app.command("isModified")["modified"],
                    "metadata": app.command("getDocumentMetadata")["metadata"],
                }
                repeated_target = app.command("resolveSpellingContextTarget",
                    x=tail_point["x"], y=tail_point["y"])
                target_lookup_after = {
                    "state": app.command("getLargeFileState")["largeFile"],
                    "selection": app.command("getSelection")["selection"],
                    "modified": app.command("isModified")["modified"],
                    "metadata": app.command("getDocumentMetadata")["metadata"],
                }
                self.assertEqual((repeated_target["start"], repeated_target["text"]),
                    (tail_target, "teh"), repeated_target)
                for key in ("caret", "topLine", "horizontalOffset", "selectionStart", "selectionEnd"):
                    self.assertEqual(target_lookup_after["state"][key], target_lookup_before["state"][key],
                        (key, target_lookup_before["state"], target_lookup_after["state"]))
                self.assertEqual(target_lookup_after["selection"], target_lookup_before["selection"])
                self.assertEqual(target_lookup_after["modified"], target_lookup_before["modified"])
                self.assertEqual(target_lookup_after["metadata"], target_lookup_before["metadata"])
                blank_point = app.command("getVisibleTextPositionPoint", position=tail_target + 3)
                self.assertFalse(app.command("resolveSpellingContextTarget",
                    x=blank_point["x"], y=blank_point["y"])["found"])
                state_before = app.command("getLargeFileState")["largeFile"]
                selection_before = app.command("getSelection")["selection"]
                metadata_before = app.command("getDocumentMetadata")["metadata"]
                self.assertFalse(app.command("resolveSpellingContextTarget", x=-1, y=-1)["found"])
                state_after = app.command("getLargeFileState")["largeFile"]
                self.assertEqual((state_after["caret"], state_after["topLine"], state_after["horizontalOffset"],
                    state_after["selectionStart"], state_after["selectionEnd"]),
                    (state_before["caret"], state_before["topLine"], state_before["horizontalOffset"],
                    state_before["selectionStart"], state_before["selectionEnd"]))
                self.assertEqual(app.command("getSelection")["selection"], selection_before)
                self.assertEqual(app.command("getDocumentMetadata")["metadata"], metadata_before)

                # The same word is checked after a font/theme layout change;
                # the resolver must use the rebuilt measured viewport.
                app.command("setFont", font="Arial 11")
                stale_font = app.command("resolveSpellingContextTarget",
                    x=tail_point["x"], y=tail_point["y"])
                self.assertFalse(stale_font["found"], stale_font)
                app.command("setAppearanceTheme", theme="dark")
                app.command("sendLargeFileHorizontalScroll", scrollCommand=7)  # SB_RIGHT
                point, themed = resolve_visible(tail_target)
                self.assertEqual((themed["start"], themed["text"]), (tail_target, "teh"), themed)
                target_revision = (
                    themed["sessionId"], themed["contentRevision"], themed["layoutRevision"])
                visible_windows = app.command("getLargeFileVisibleWindows")["windows"]
                self.assertTrue(any(
                    (window["sessionId"], window["contentRevision"], window["layoutRevision"])
                    == target_revision and window["start"] <= tail_target <
                    window["start"] + utf16_units(window["text"])
                    for window in visible_windows
                ), visible_windows[:2])
                popup_before = {
                    "state": app.command("getLargeFileState")["largeFile"],
                    "selection": app.command("getSelection")["selection"],
                    "modified": app.command("isModified")["modified"],
                    "metadata": app.command("getDocumentMetadata")["metadata"],
                    "text": app.command("getLargeFileRange", start=tail_target - 1, length=6)["text"],
                }
                app.command("sendEditorContextMenu", x=point["x"], y=point["y"])
                popup_after = {
                    "state": app.command("getLargeFileState")["largeFile"],
                    "selection": app.command("getSelection")["selection"],
                    "modified": app.command("isModified")["modified"],
                    "metadata": app.command("getDocumentMetadata")["metadata"],
                    "text": app.command("getLargeFileRange", start=tail_target - 1, length=6)["text"],
                }
                before_state = popup_before["state"]
                after_state = popup_after["state"]
                for key in ("caret", "topLine", "horizontalOffset", "selectionStart", "selectionEnd"):
                    self.assertEqual(after_state[key], before_state[key], (key, before_state, after_state))
                self.assertEqual(popup_after["selection"], popup_before["selection"])
                self.assertEqual(popup_after["modified"], popup_before["modified"])
                self.assertEqual(popup_after["metadata"], popup_before["metadata"])
                self.assertEqual(popup_after["text"], popup_before["text"])
                app.command("setAppearanceTheme", theme="light")
                app.command("setFont", font="Consolas 11")

                # Wrapping changes the measured row segment and remains targetable
                # at a document offset beyond the native 16-bit hit-test range.
                app.command("invokeWordWrapMenuCommand")
                stale_wrap = app.command("resolveSpellingContextTarget",
                    x=point["x"], y=point["y"])
                self.assertFalse(stale_wrap["found"], stale_wrap)
                wrapped_point, wrapped = select_resolve(first_target)
                self.assertEqual((wrapped["start"], wrapped["text"]), (first_target, "teh"), wrapped)
                self.assertTrue(app.command("sendEditorContextMenu", x=wrapped_point["x"],
                    y=wrapped_point["y"]))

                # Same-size content edits immediately invalidate the old tagged
                # window before a later spelling pass can publish a replacement.
                app.command("setSelection", start=tail_target, end=tail_target + 3)
                app.command("insertText", text="the")
                stale = app.command("resolveSpellingContextTarget", keyboard=True)
                self.assertFalse(stale["found"], stale)

    @unittest.skipUnless(automation_platform() == "windows", "mapped line-ending metadata is Windows-only")
    def test_mapped_save_as_and_rebase_preserve_detected_line_ending_metadata(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            root = Path(temp_root)
            source_path = root / "mixed-utf8-bom-source.txt"
            synchronous_save_as = root / "mixed-sync-save-as.txt"
            asynchronous_save_as = root / "mixed-async-save-as.txt"
            body_bytes = 48 * 1024 * 1024
            body_block = b"x" * (8 * 1024 * 1024)
            source_tail = b"\nlf-one\nlf-two\r\ncrlf-one\rcr-only"
            saved_tail = b"\nlf-one\nlf-two\ncrlf-one\ncr-only"
            with source_path.open("wb") as output:
                output.write(b"\xef\xbb\xbf")
                for _ in range(body_bytes // len(body_block)):
                    output.write(body_block)
                output.write(source_tail)

            def expected_digest(tail: bytes) -> bytes:
                digest = hashlib.sha256()
                digest.update(b"\xef\xbb\xbf")
                for _ in range(body_bytes // len(body_block)):
                    digest.update(body_block)
                digest.update(tail)
                return digest.digest()

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=120.0) as app:
                app.command("openFile", path=str(source_path))
                deadline = time.monotonic() + 90.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.025)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])

                metadata = app.command("getDocumentMetadata")["metadata"]
                self.assertEqual(metadata["encoding"], "UTF-8 with BOM")
                self.assertEqual(metadata["lineEnding"], "Mixed")
                self.assertEqual(metadata["saveLineEnding"], "Unix (LF)")

                end = state["characterCount"]
                app.command("setSelection", start=end, end=end)
                app.command("insertText", text="X")
                app.command("saveAs", path=str(synchronous_save_as))
                self.assertEqual(hashlib.sha256(synchronous_save_as.read_bytes()).digest(),
                    expected_digest(saved_tail + b"X"))
                metadata = app.command("getDocumentMetadata")["metadata"]
                self.assertEqual(metadata["path"], str(synchronous_save_as))
                self.assertEqual(metadata["encoding"], "UTF-8 with BOM")
                self.assertEqual(metadata["lineEnding"], "Mixed")
                self.assertEqual(metadata["saveLineEnding"], "Unix (LF)")

                deadline = time.monotonic() + 90.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.025)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])
                end = state["characterCount"]
                app.command("setSelection", start=end, end=end)
                app.command("insertText", text="Y")

                app.command("startLargeFileSave", path=str(asynchronous_save_as))
                deadline = time.monotonic() + 120.0
                state = app.command("getLargeFileState")["largeFile"]
                while state["operationInProgress"] and time.monotonic() < deadline:
                    time.sleep(0.025)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["operationCompleted"])
                self.assertTrue(state["operationSucceeded"], state["operationError"])
                self.assertEqual(hashlib.sha256(asynchronous_save_as.read_bytes()).digest(),
                    expected_digest(saved_tail + b"XY"))
                metadata = app.command("getDocumentMetadata")["metadata"]
                self.assertEqual(metadata["path"], str(asynchronous_save_as))
                self.assertEqual(metadata["encoding"], "UTF-8 with BOM")
                self.assertEqual(metadata["lineEnding"], "Mixed")
                self.assertEqual(metadata["saveLineEnding"], "Unix (LF)")

                state = app.command("getLargeFileState")["largeFile"]
                end = state["characterCount"]
                app.command("setSelection", start=end, end=end)
                app.command("insertText", text="Z")
                app.command("save")
                deadline = time.monotonic() + 90.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.025)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])
                self.assertEqual(hashlib.sha256(asynchronous_save_as.read_bytes()).digest(),
                    expected_digest(saved_tail + b"XYZ"))
                metadata = app.command("getDocumentMetadata")["metadata"]
                self.assertEqual(metadata["encoding"], "UTF-8 with BOM")
                self.assertEqual(metadata["lineEnding"], "Mixed")
                self.assertEqual(metadata["saveLineEnding"], "Unix (LF)")

                app.command("openFile", path=str(asynchronous_save_as))
                metadata = app.command("getDocumentMetadata")["metadata"]
                self.assertEqual(metadata["encoding"], "UTF-8 with BOM")
                self.assertEqual(metadata["lineEnding"], "Unix (LF)")
                self.assertEqual(metadata["saveLineEnding"], "Unix (LF)")

    @unittest.skipUnless(automation_platform() == "windows", "Windows immutable large-file snapshot source")
    def test_async_find_snapshot_blocks_external_source_mutation(self):
        previous_gate = os.environ.get("CLASSIC_NOTEPAD_TEST_OPERATION_GATE")
        os.environ["CLASSIC_NOTEPAD_TEST_OPERATION_GATE"] = "find"
        process_id = None
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                source_path = root / "immutable-snapshot-source.txt"
                prefix = b"abcdefgh\r\n"
                line = b"0123456789abcdef" * 255 + b"\r\n"
                with source_path.open("wb") as output:
                    output.write(prefix)
                    while output.tell() + len(line) <= 52 * 1024 * 1024:
                        output.write(line)

                with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=60.0) as app:
                    app.command("openFile", path=str(source_path))
                    deadline = time.monotonic() + 45.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while not state["indexingComplete"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["indexingComplete"])
                    app.command("startLargeFileFind", text="MUTATION", matchCase=True)
                    process_id = app._process.pid
                    _wait_operation_gate(process_id, "find")

                    # The Find worker now owns its cloned snapshot. The source
                    # open must reject a writer instead of allowing the mapped
                    # bytes to change underneath that snapshot.
                    with self.assertRaises(PermissionError):
                        with source_path.open("r+b", buffering=0) as writer:
                            writer.seek(0)
                            writer.write(b"MUTATION")
                            writer.flush()
                            os.fsync(writer.fileno())

                    with source_path.open("rb") as source:
                        self.assertEqual(source.read(8), b"abcdefgh")
                    _release_operation_gate(process_id, "find")

                    deadline = time.monotonic() + 30.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while state["operationInProgress"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["operationCompleted"])
                    self.assertTrue(state["operationSucceeded"])
                    self.assertEqual(state["operationResultCount"], 0)
                    app.command("newDocument")
        finally:
            if process_id is not None:
                try:
                    _release_operation_gate(process_id, "find", timeout=0.25)
                except (AssertionError, OSError):
                    pass
            if previous_gate is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_OPERATION_GATE", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_OPERATION_GATE"] = previous_gate

    @unittest.skipUnless(automation_platform() == "windows", "Windows stale Save As completion adoption")
    def test_stale_async_save_as_writes_snapshot_without_cleaning_newer_edits(self):
        previous_gate = os.environ.get("CLASSIC_NOTEPAD_TEST_ADOPTION_GATE")
        os.environ["CLASSIC_NOTEPAD_TEST_ADOPTION_GATE"] = "save"
        process_id = None
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                source_path = root / "save-as-source.txt"
                destination_path = root / "save-as-captured.txt"
                expected_path = root / "save-as-expected.txt"
                prefix = b"CRLF\r\nCR\rlf\n"
                line = b"0123456789abcdef" * 255 + b"\n"
                original_digest = hashlib.sha256()
                line_count = 0
                with source_path.open("wb") as output:
                    output.write(prefix)
                    original_digest.update(prefix)
                    while output.tell() + len(line) <= 52 * 1024 * 1024:
                        output.write(line)
                        original_digest.update(line)
                        line_count += 1
                with expected_path.open("wb") as expected:
                    expected.write(b"X")
                    expected.write(b"CRLF\nCR\nlf\n")
                    for _ in range(line_count):
                        expected.write(line)
                expected_digest = hashlib.sha256(expected_path.read_bytes()).digest()

                with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=60.0) as app:
                    app.command("openFile", path=str(source_path))
                    deadline = time.monotonic() + 45.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while not state["indexingComplete"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["indexingComplete"])
                    self.assertEqual(state["maximumLineColumns"], 4080)
                    metadata = app.command("getDocumentMetadata")["metadata"]
                    self.assertEqual(metadata["lineEnding"], "Mixed")
                    self.assertEqual(metadata["saveLineEnding"], "Unix (LF)")
                    app.command("sendLargeFileText", text="X")
                    state = app.command("getLargeFileState")["largeFile"]
                    self.assertEqual(state["maximumLineColumns"], 4080)
                    app.command("startLargeFileSave", path=str(destination_path))
                    process_id = app._process.pid
                    _wait_adoption_gate(process_id, "save")
                    self.assertTrue(app.command("getLargeFileState")["largeFile"]["operationInProgress"])
                    app.command("sendLargeFileText", text="Y")
                    _release_adoption_gate(process_id, "save")

                    deadline = time.monotonic() + 45.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while state["operationInProgress"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["operationCompleted"])
                    self.assertTrue(state["operationSucceeded"])
                    self.assertTrue(state["modified"], "the later edit must remain dirty after Save As")
                    self.assertEqual(hashlib.sha256(destination_path.read_bytes()).digest(), expected_digest)
                    self.assertEqual(hashlib.sha256(source_path.read_bytes()).digest(), original_digest.digest())
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=2)["text"], "XY")
                    self.assertIn("unsaved", state["operationError"].lower())
                    metadata = app.command("getDocumentMetadata")["metadata"]
                    self.assertEqual(metadata["path"], str(destination_path))
                    self.assertEqual(metadata["lineEnding"], "Mixed")
                    self.assertEqual(metadata["saveLineEnding"], "Unix (LF)")
        finally:
            if process_id is not None:
                try:
                    _release_adoption_gate(process_id, "save", timeout=0.25)
                except (AssertionError, OSError):
                    pass
            if previous_gate is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_ADOPTION_GATE", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_ADOPTION_GATE"] = previous_gate

    @unittest.skipUnless(automation_platform() == "windows", "Windows asynchronous completion adoption")
    def test_async_results_reject_changed_selection_and_document_before_adoption(self):
        previous_gate = os.environ.get("CLASSIC_NOTEPAD_TEST_ADOPTION_GATE")
        os.environ["CLASSIC_NOTEPAD_TEST_ADOPTION_GATE"] = "all"
        process_id = None
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                source_path = root / "completion-source.txt"
                line = b"0123456789abcdef" * 255 + b"\r\n"
                original_digest = hashlib.sha256()
                with source_path.open("wb") as output:
                    while output.tell() + len(line) <= 52 * 1024 * 1024:
                        output.write(line)
                        original_digest.update(line)
                    tail = b"T" * 4070 + b"target\r\n"
                    output.write(tail)
                    original_digest.update(tail)

                with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=60.0) as app:
                    app.command("openFile", path=str(source_path))
                    deadline = time.monotonic() + 45.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while not state["indexingComplete"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["indexingComplete"])
                    self.assertEqual(state["maximumLineColumns"], 4080)
                    original_line_count = state["totalLines"]

                    synchronous = app.command(
                        "replaceAll", text="target", replacement="ABCDEFGHIJKL",
                        matchCase=True, wholeWord=False,
                    )
                    self.assertEqual(synchronous["count"], 1)
                    state = app.command("getLargeFileState")["largeFile"]
                    self.assertEqual(state["maximumLineColumns"], 4082)
                    app.command("undo")
                    state = app.command("getLargeFileState")["largeFile"]
                    self.assertEqual(state["maximumLineColumns"], 4080)
                    self.assertEqual(state["totalLines"], original_line_count)
                    self.assertFalse(state["modified"])

                    # Keep an earlier edit in history while the asynchronous
                    # Replace All snapshot is prepared and adopted.
                    app.command("setSelection", start=0, end=0)
                    app.command("sendLargeFileText", text="P")
                    before_async = app.command("getLargeFileState")["largeFile"]
                    self.assertEqual(before_async["maximumLineColumns"], 4081)
                    self.assertEqual(before_async["totalLines"], original_line_count)
                    self.assertTrue(before_async["modified"])
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=3)["text"], "P01")

                    asynchronous = app.command(
                        "startLargeFileReplaceAll", text="target", replacement="ABCDEFGHIJKLMNOPQRST",
                        matchCase=True, wholeWord=False,
                    )["largeFile"]
                    process_id = app._process.pid
                    self.assertTrue(asynchronous["operationInProgress"])
                    _wait_adoption_gate(process_id, "replace")
                    self.assertEqual(
                        app.command("getLargeFileState")["largeFile"]["maximumLineColumns"], 4081
                    )
                    _release_adoption_gate(process_id, "replace")
                    deadline = time.monotonic() + 30.0
                    while asynchronous["operationInProgress"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        asynchronous = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(asynchronous["operationCompleted"])
                    self.assertTrue(asynchronous["operationSucceeded"], asynchronous["operationError"])
                    self.assertEqual(asynchronous["maximumLineColumns"], 4090)
                    self.assertEqual(asynchronous["totalLines"], original_line_count)
                    self.assertTrue(asynchronous["modified"])
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=3)["text"], "P01")
                    app.command("undo")
                    after_replace_undo = app.command("getLargeFileState")["largeFile"]
                    self.assertEqual(after_replace_undo["maximumLineColumns"], 4081)
                    self.assertEqual(after_replace_undo["totalLines"], original_line_count)
                    self.assertTrue(after_replace_undo["modified"])
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=3)["text"], "P01")
                    tail_range = app.command(
                        "getLargeFileRange", start=after_replace_undo["characterCount"] - 8, length=6
                    )
                    self.assertEqual(tail_range["text"], "target")

                    # A second Undo must reverse the edit that preceded the
                    # adopted Replace All snapshot and restore the saved state.
                    app.command("undo")
                    after_prior_undo = app.command("getLargeFileState")["largeFile"]
                    self.assertEqual(after_prior_undo["maximumLineColumns"], 4080)
                    self.assertEqual(after_prior_undo["totalLines"], original_line_count)
                    self.assertFalse(after_prior_undo["modified"])
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=3)["text"], "012")
                    tail_range = app.command(
                        "getLargeFileRange", start=after_prior_undo["characterCount"] - 8, length=6
                    )
                    self.assertEqual(tail_range["text"], "target")

                    cancelled = app.command(
                        "startLargeFileReplaceAll", text="target", replacement="0123456789",
                        matchCase=True, wholeWord=False,
                    )["largeFile"]
                    _wait_adoption_gate(process_id, "replace")
                    app.command("cancelLargeFileOperation")
                    _release_adoption_gate(process_id, "replace")
                    deadline = time.monotonic() + 30.0
                    while cancelled["operationInProgress"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        cancelled = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(cancelled["operationCompleted"])
                    self.assertFalse(cancelled["operationSucceeded"])
                    self.assertEqual(cancelled["maximumLineColumns"], 4080)

                    app.command("setSelection", start=0, end=0)
                    app.command("sendLargeFileText", text="X")
                    state = app.command("getLargeFileState")["largeFile"]
                    self.assertEqual(state["maximumLineColumns"], 4081)

                    app.command("startLargeFileFind", text="target", matchCase=True)
                    process_id = app._process.pid
                    _wait_adoption_gate(process_id, "find")
                    self.assertTrue(app.command("getLargeFileState")["largeFile"]["operationInProgress"])
                    app.command("setSelection", start=1, end=2)
                    _release_adoption_gate(process_id, "find")

                    deadline = time.monotonic() + 30.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while state["operationInProgress"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["operationCompleted"])
                    self.assertFalse(state["operationSucceeded"])
                    self.assertEqual(app.command("getSelection")["selection"], {"start": 1, "end": 2})
                    self.assertTrue(app.command("isModified")["modified"])
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=1)["text"], "X")

                    app.command("startLargeFileReplaceAll", text="target", replacement="R" * 30, matchCase=True)
                    _wait_adoption_gate(process_id, "replace")
                    self.assertEqual(
                        app.command("getLargeFileState")["largeFile"]["maximumLineColumns"], 4081
                    )
                    app.command("sendLargeFileText", text="Y")
                    changed_selection = app.command("getSelection")["selection"]
                    self.assertEqual(
                        app.command("getLargeFileState")["largeFile"]["maximumLineColumns"], 4081
                    )
                    _release_adoption_gate(process_id, "replace")

                    deadline = time.monotonic() + 30.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while state["operationInProgress"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["operationCompleted"])
                    self.assertFalse(state["operationSucceeded"])
                    self.assertEqual(app.command("getSelection")["selection"], changed_selection)
                    self.assertTrue(app.command("isModified")["modified"])
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=2)["text"], "XY")
                    self.assertEqual(
                        app.command("getLargeFileState")["largeFile"]["maximumLineColumns"], 4081
                    )
                    tail_range = app.command("getLargeFileRange", start=state["characterCount"] - 8, length=6)
                    self.assertEqual(tail_range["text"], "target")
                    self.assertEqual(hashlib.sha256(source_path.read_bytes()).digest(), original_digest.digest())
        finally:
            if process_id is not None:
                for operation in ("find", "replace"):
                    try:
                        _release_adoption_gate(process_id, operation, timeout=0.25)
                    except (AssertionError, OSError):
                        pass
            if previous_gate is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_ADOPTION_GATE", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_ADOPTION_GATE"] = previous_gate

    @unittest.skipUnless(automation_platform() == "windows", "Windows asynchronous Save sharing failure")
    def test_async_save_sharing_failure_keeps_ui_responsive_and_complete_recovery(self):
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.argtypes = (ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p)
        kernel32.CreateFileW.restype = ctypes.c_void_p
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        kernel32.CloseHandle.restype = ctypes.c_int
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            root = Path(temp_root)
            source_path = root / "sharing-source.txt"
            destination_path = root / "sharing-destination.txt"
            line = b"0123456789abcdef" * 255 + b"\r\n"
            original_digest = hashlib.sha256()
            expected_digest = hashlib.sha256(b"X")
            with source_path.open("wb") as output:
                while output.tell() + len(line) <= 54 * 1024 * 1024:
                    output.write(line)
                    original_digest.update(line)
                    expected_digest.update(line)
            destination_path.write_bytes(b"old destination")

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=45.0) as app:
                app.command("openFile", path=str(source_path))
                deadline = time.monotonic() + 30.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.02)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])
                app.command("sendLargeFileText", text="X")
                # A real external destination handle permits reads/writes but
                # denies delete sharing, so production ReplaceFileW must fail.
                # The source itself is protected against new writers while the
                # document and any snapshots retain their captured bytes.
                handle = kernel32.CreateFileW(str(destination_path), 0xC0000000, 0x3, None, 3, 0, None)
                if handle == ctypes.c_void_p(-1).value:
                    raise ctypes.WinError(ctypes.get_last_error())
                try:
                    app.command("startLargeFileSave", path=str(destination_path))
                    deadline = time.monotonic() + 30.0
                    query_times = []
                    while True:
                        started = time.monotonic()
                        state = app.command("getLargeFileState")["largeFile"]
                        elapsed = time.monotonic() - started
                        query_times.append(elapsed)
                        self.assertLess(elapsed, 1.0, "UI query stalled during destination sharing failure")
                        if not state["operationInProgress"]:
                            break
                        self.assertLess(time.monotonic(), deadline, "Save did not finish")
                        time.sleep(0.01)
                    self.assertTrue(state["operationCompleted"])
                    self.assertFalse(state["operationSucceeded"])
                    self.assertTrue(state["active"])
                    self.assertTrue(state["modified"])
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=1)["text"], "X")
                    recovery = list(root.glob("sharing-destination.txt.classic-notepad-save-*.tmp"))
                    self.assertEqual(len(recovery), 1)
                    self.assertIn(str(recovery[0]), state["operationError"])
                    self.assertEqual(state["saveFailureStage"], "replace")
                    self.assertEqual(state["saveWin32Error"], 32)
                    self.assertTrue(state["saveOutputComplete"])
                    self.assertFalse(state["saveCommitted"])
                    self.assertEqual(state["saveRecoveryPath"], str(recovery[0]))
                    self.assertEqual(hashlib.sha256(recovery[0].read_bytes()).hexdigest(), expected_digest.hexdigest())
                    self.assertEqual(hashlib.sha256(source_path.read_bytes()).hexdigest(), original_digest.hexdigest())
                    print(f"Sharing failure: {len(query_times)} UI queries; slowest {max(query_times):.3f}s; complete recovery retained")
                finally:
                    kernel32.CloseHandle(handle)
                app.command("newDocument")

    @unittest.skipUnless(automation_platform() == "windows", "Windows asynchronous Save transaction diagnostics")
    def test_async_save_create_write_flush_failures_report_structured_diagnostics(self):
        previous_fault = os.environ.get("CLASSIC_NOTEPAD_TEST_SAVE_FAULT")
        try:
            line = b"0123456789abcdef" * 255 + b"\n"
            for stage, win32_error in (("create", 5), ("write", 29), ("flush", 29)):
                with self.subTest(stage=stage), tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                    root = Path(temp_root)
                    source_path = root / f"async-{stage}-source.txt"
                    destination_path = root / f"async-{stage}-destination.txt"
                    with source_path.open("wb") as output:
                        while output.tell() + len(line) <= 50 * 1024 * 1024:
                            output.write(line)
                    original_hash = hashlib.sha256(source_path.read_bytes()).hexdigest()
                    os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = stage
                    with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=45.0) as app:
                        app.command("openFile", path=str(source_path))
                        deadline = time.monotonic() + 30.0
                        state = app.command("getLargeFileState")["largeFile"]
                        while not state["indexingComplete"] and time.monotonic() < deadline:
                            time.sleep(0.02)
                            state = app.command("getLargeFileState")["largeFile"]
                        self.assertTrue(state["indexingComplete"])
                        metadata_before = app.command("getDocumentMetadata")["metadata"]
                        self.assertEqual(metadata_before["lineEnding"], "Unix (LF)")
                        self.assertEqual(metadata_before["saveLineEnding"], "Unix (LF)")
                        app.command("sendLargeFileText", text="X")
                        app.command("startLargeFileSave", path=str(destination_path))

                        deadline = time.monotonic() + 30.0
                        state = app.command("getLargeFileState")["largeFile"]
                        while state["operationInProgress"] and time.monotonic() < deadline:
                            time.sleep(0.02)
                            state = app.command("getLargeFileState")["largeFile"]
                        self.assertTrue(state["operationCompleted"])
                        self.assertFalse(state["operationSucceeded"])
                        self.assertEqual(state["saveFailureStage"], stage)
                        self.assertEqual(state["saveWin32Error"], win32_error)
                        self.assertFalse(state["saveOutputComplete"])
                        self.assertFalse(state["saveCommitted"])
                        self.assertFalse(state["saveRecoveryPath"])
                        self.assertIn(f"Stage: {stage}.", state["operationError"])
                        self.assertIn(f"Windows error {win32_error}", state["operationError"])
                        self.assertTrue(state["active"])
                        self.assertTrue(state["modified"])
                        metadata_after = app.command("getDocumentMetadata")["metadata"]
                        self.assertEqual(metadata_after["path"], str(source_path))
                        self.assertEqual(metadata_after["encoding"], metadata_before["encoding"])
                        self.assertEqual(metadata_after["lineEnding"], metadata_before["lineEnding"])
                        self.assertEqual(metadata_after["saveLineEnding"], metadata_before["saveLineEnding"])
                        self.assertEqual(app.command("getLargeFileRange", start=0, length=1)["text"], "X")
                        self.assertEqual(hashlib.sha256(source_path.read_bytes()).hexdigest(), original_hash)
                        self.assertFalse(destination_path.exists())
                        self.assertEqual(list(root.glob(f"async-{stage}-destination.txt.classic-notepad-save-*.tmp")), [])
                        app.command("newDocument")
        finally:
            if previous_fault is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_SAVE_FAULT", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = previous_fault

    @unittest.skipUnless(automation_platform() == "windows", "Windows destructive asynchronous Save recovery")
    def test_async_save_keeps_recovery_when_destination_was_displaced(self):
        previous_fault = os.environ.get("CLASSIC_NOTEPAD_TEST_SAVE_FAULT")
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                source_path = root / "displaced-async-source.txt"
                destination_path = root / "displaced-async-destination.txt"
                line = b"0123456789abcdef" * 255 + b"\r\n"
                with source_path.open("wb") as output:
                    while output.tell() + len(line) <= 50 * 1024 * 1024:
                        output.write(line)
                destination_path.write_bytes(b"previous destination")
                original_hash = hashlib.sha256(source_path.read_bytes()).hexdigest()
                expected = hashlib.sha256(b"X")
                with source_path.open("rb") as source:
                    for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
                        expected.update(chunk)
                expected_hash = expected.hexdigest()
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = "replace-destination-moved"

                with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=45.0) as app:
                    app.command("openFile", path=str(source_path))
                    deadline = time.monotonic() + 30.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while not state["indexingComplete"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["indexingComplete"])
                    app.command("sendLargeFileText", text="X")

                    app.command("startLargeFileSave", path=str(destination_path))
                    deadline = time.monotonic() + 30.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while state["operationInProgress"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["operationCompleted"])
                    self.assertFalse(state["operationSucceeded"])
                    self.assertFalse(destination_path.exists())
                    backup = list(root.glob("displaced-async-destination.txt.classic-notepad-test-original-*.bak"))
                    recovery = Path(state["saveRecoveryPath"])
                    self.assertEqual(len(backup), 1)
                    self.assertEqual(backup[0].read_bytes(), b"previous destination")
                    self.assertTrue(recovery.is_file())
                    self.assertEqual(hashlib.sha256(recovery.read_bytes()).hexdigest(), expected_hash)
                    self.assertEqual(state["saveFailureStage"], "replace")
                    self.assertEqual(state["saveWin32Error"], 1177)
                    self.assertTrue(state["saveOutputComplete"])
                    self.assertFalse(state["saveCommitted"])
                    self.assertIn(str(recovery), state["operationError"])
                    self.assertTrue(state["active"])
                    self.assertTrue(state["modified"])
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=1)["text"], "X")
                    self.assertEqual(hashlib.sha256(source_path.read_bytes()).hexdigest(), original_hash)

                    app.command("startLargeFileSave", path=str(destination_path))
                    deadline = time.monotonic() + 30.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while state["operationInProgress"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["operationCompleted"])
                    self.assertTrue(state["operationSucceeded"])
                    self.assertEqual(hashlib.sha256(destination_path.read_bytes()).hexdigest(), expected_hash)
                    self.assertEqual(hashlib.sha256(recovery.read_bytes()).hexdigest(), expected_hash)
                    self.assertFalse(state["modified"])
                    self.assertTrue(state["saveCommitted"])
        finally:
            if previous_fault is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_SAVE_FAULT", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = previous_fault

    @unittest.skipUnless(automation_platform() == "windows", "Windows large-file operation coordination")
    def test_busy_find_and_replace_reject_transition_and_allow_later_new(self):
        previous_gate = os.environ.get("CLASSIC_NOTEPAD_TEST_OPERATION_GATE")
        try:
            for operation in ("find", "replace"):
                with self.subTest(operation=operation), tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                    root = Path(temp_root)
                    source_path = root / f"busy-{operation}.txt"
                    expected_path = root / "expected.txt"
                    line = b"0123456789abcdef" * 255 + b"\r\n"
                    target_size = 52 * 1024 * 1024
                    with source_path.open("wb") as output:
                        while output.tell() + len(line) <= target_size:
                            output.write(line)
                    with expected_path.open("wb") as output, source_path.open("rb") as source:
                        output.write(b"X")
                        while chunk := source.read(8 * 1024 * 1024):
                            output.write(chunk)
                    expected_hash = hashlib.sha256(expected_path.read_bytes()).hexdigest()

                    os.environ["CLASSIC_NOTEPAD_TEST_OPERATION_GATE"] = operation
                    with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=45.0) as app:
                        app.command("openFile", path=str(source_path))
                        deadline = time.monotonic() + 30.0
                        state = app.command("getLargeFileState")["largeFile"]
                        while not state["indexingComplete"] and time.monotonic() < deadline:
                            time.sleep(0.02)
                            state = app.command("getLargeFileState")["largeFile"]
                        self.assertTrue(state["indexingComplete"])
                        app.command("sendLargeFileText", text="X")
                        if operation == "find":
                            app.command("startLargeFileFind", text="A2-absent-search-term", matchCase=True)
                            expected_name = "Find"
                        else:
                            app.command("startLargeFileReplaceAll", text="A2-absent-search-term",
                                        replacement="replacement", matchCase=True)
                            expected_name = "Replace All"
                        process_id = app._process.pid
                        _wait_operation_gate(process_id, operation)
                        state = app.command("getLargeFileState")["largeFile"]
                        self.assertTrue(state["operationInProgress"])
                        self.assertEqual(state["operationName"], expected_name)

                        # An active whole-document operation must reject the
                        # queued document transition without discarding edits.
                        app.command("requestNewTransition", saveChanges=True)
                        state = app.command("getLargeFileState")["largeFile"]
                        self.assertTrue(state["active"])
                        self.assertTrue(state["modified"])
                        self.assertTrue(state["operationInProgress"])
                        _release_operation_gate(process_id, operation)

                        deadline = time.monotonic() + 30.0
                        state = app.command("getLargeFileState")["largeFile"]
                        while state["operationInProgress"] and time.monotonic() < deadline:
                            time.sleep(0.02)
                            state = app.command("getLargeFileState")["largeFile"]
                        self.assertTrue(state["operationCompleted"])
                        self.assertTrue(state["active"])
                        self.assertTrue(state["modified"])
                        self.assertEqual(app.command("getLargeFileRange", start=0, length=1)["text"], "X")

                        # A fresh transition request after completion must work.
                        app.command("requestNewTransition", saveChanges=True)
                        deadline = time.monotonic() + 45.0
                        state = app.command("getLargeFileState")["largeFile"]
                        while state["active"] and time.monotonic() < deadline:
                            time.sleep(0.02)
                            state = app.command("getLargeFileState")["largeFile"]
                        self.assertFalse(state["active"])
                        self.assertEqual(hashlib.sha256(source_path.read_bytes()).hexdigest(), expected_hash)
                        app.command("newDocument")
        finally:
            if previous_gate is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_OPERATION_GATE", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_OPERATION_GATE"] = previous_gate

    @unittest.skipUnless(automation_platform() == "windows", "Windows large-file save coordination")
    def test_cancelled_queued_async_save_keeps_edits_and_allows_a_later_transition(self):
        previous_gate = os.environ.get("CLASSIC_NOTEPAD_TEST_SAVE_GATE")
        os.environ["CLASSIC_NOTEPAD_TEST_SAVE_GATE"] = "1"
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                source_path = root / "cancel-queued-transition.txt"
                line = b"0123456789abcdef" * 255 + b"\r\n"
                target_size = 52 * 1024 * 1024
                original_digest = hashlib.sha256()
                with source_path.open("wb") as output:
                    while output.tell() + len(line) <= target_size:
                        output.write(line)
                        original_digest.update(line)
                original_hash = original_digest.hexdigest()

                with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=45.0) as app:
                    app.command("openFile", path=str(source_path))
                    deadline = time.monotonic() + 30.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while not state["indexingComplete"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["indexingComplete"])
                    app.command("sendLargeFileText", text="X")

                    # This automation entry point goes through the real New
                    # transition coordinator, with its dirty prompt answered
                    # Yes so the Save is pending before it can be cancelled.
                    app.command("requestNewTransition", saveChanges=True)
                    process_id = app._process.pid
                    _wait_save_gate(process_id, "ready")
                    operation = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(operation["operationInProgress"])
                    self.assertEqual(operation["operationName"], "Save")
                    app.command("cancelLargeFileOperation")
                    _release_save_gate(process_id)

                    deadline = time.monotonic() + 30.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while state["operationInProgress"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertFalse(state["operationInProgress"])
                    self.assertTrue(state["operationCompleted"])
                    self.assertFalse(state["operationSucceeded"])
                    self.assertTrue(state["active"])
                    self.assertTrue(state["modified"])
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=1)["text"], "X")
                    self.assertEqual(hashlib.sha256(source_path.read_bytes()).hexdigest(), original_hash)

                    # A canceled queued transition must not poison the next
                    # request. The second request saves and then transitions.
                    app.command("requestNewTransition", saveChanges=True)
                    _wait_save_gate(process_id, "ready")
                    _release_save_gate(process_id)
                    deadline = time.monotonic() + 45.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while state["active"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertFalse(state["active"], "the later New transition did not complete")
                    self.assertEqual(app.command("getText")["text"], "")
                    self.assertFalse(app.command("isModified")["modified"])
                    self.assertEqual(source_path.read_bytes()[:1], b"X")
        finally:
            if previous_gate is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_SAVE_GATE", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_GATE"] = previous_gate

    @unittest.skipUnless(automation_platform() == "windows", "Windows synchronous large-file Save recovery")
    def test_synchronous_large_file_save_faults_keep_dirty_source_readable(self):
        previous_fault = os.environ.get("CLASSIC_NOTEPAD_TEST_SAVE_FAULT")
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                for stage in ("write", "flush", "replace", "reopen"):
                    with self.subTest(stage=stage):
                        source_path = root / f"sync-{stage}-source.txt"
                        temporary_path = root / f"sync-{stage}-expected.txt"
                        line = b"0123456789abcdef" * 255 + b"\n"
                        target_size = 52 * 1024 * 1024
                        with source_path.open("wb") as output:
                            while output.tell() + len(line) <= target_size:
                                output.write(line)
                        with temporary_path.open("wb") as output, source_path.open("rb") as source:
                            output.write(b"X")
                            while chunk := source.read(8 * 1024 * 1024):
                                output.write(chunk)

                        original_hash = hashlib.sha256(source_path.read_bytes()).hexdigest()
                        expected_hash = hashlib.sha256(temporary_path.read_bytes()).hexdigest()
                        os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = stage
                        with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=45.0) as app:
                            app.command("openFile", path=str(source_path))
                            deadline = time.monotonic() + 30.0
                            state = app.command("getLargeFileState")["largeFile"]
                            while not state["indexingComplete"] and time.monotonic() < deadline:
                                time.sleep(0.02)
                                state = app.command("getLargeFileState")["largeFile"]
                            self.assertTrue(state["indexingComplete"])
                            metadata_before = app.command("getDocumentMetadata")["metadata"]
                            self.assertEqual(metadata_before["lineEnding"], "Unix (LF)")
                            self.assertEqual(metadata_before["saveLineEnding"], "Unix (LF)")
                            app.command("setSelection", start=0, end=0)
                            app.command("insertText", text="X")
                            response = app.try_command("save")
                            self.assertFalse(response.get("ok", False), response)

                            state = app.command("getLargeFileState")["largeFile"]
                            self.assertTrue(state["modified"])
                            metadata_after = app.command("getDocumentMetadata")["metadata"]
                            self.assertEqual(metadata_after["path"], str(source_path))
                            self.assertEqual(metadata_after["encoding"], metadata_before["encoding"])
                            self.assertEqual(metadata_after["lineEnding"], metadata_before["lineEnding"])
                            self.assertEqual(metadata_after["saveLineEnding"], metadata_before["saveLineEnding"])
                            self.assertEqual(state["saveFailureStage"], stage, f"{response!r}; {state!r}")
                            self.assertEqual(state["saveWin32Error"], {
                                "write": 29, "flush": 29, "replace": 5, "reopen": 110
                            }[stage])
                            if stage == "reopen":
                                self.assertIn("after opening the committed output", response["error"])
                            if stage in ("write", "flush"):
                                self.assertFalse(state["saveOutputComplete"])
                                self.assertFalse(state["saveCommitted"])
                                self.assertFalse(state["saveRecoveryPath"])
                            elif stage == "replace":
                                self.assertTrue(state["saveOutputComplete"])
                                self.assertFalse(state["saveCommitted"])
                                self.assertTrue(Path(state["saveRecoveryPath"]).is_file())
                            else:
                                self.assertTrue(state["saveOutputComplete"])
                                self.assertTrue(state["saveCommitted"])
                                self.assertEqual(state["saveRecoveryPath"], str(source_path))
                            self.assertEqual(app.command("getLargeFileRange", start=0, length=1)["text"], "X")
                            if stage == "reopen":
                                self.assertEqual(hashlib.sha256(source_path.read_bytes()).hexdigest(), expected_hash)
                            else:
                                self.assertEqual(hashlib.sha256(source_path.read_bytes()).hexdigest(), original_hash)
                            if stage == "replace":
                                recovery = list(root.glob("sync-replace-source.txt.classic-notepad-save-*.tmp"))
                                self.assertEqual(len(recovery), 1)
                                self.assertEqual(hashlib.sha256(recovery[0].read_bytes()).hexdigest(), expected_hash)
                            if stage == "reopen":
                                app.command("setSelection", start=state["characterCount"], end=state["characterCount"])
                                app.command("insertText", text="Y")
                                app.command("save")
                                retry_digest = hashlib.sha256()
                                with temporary_path.open("rb") as expected_file:
                                    for chunk in iter(lambda: expected_file.read(8 * 1024 * 1024), b""):
                                        retry_digest.update(chunk)
                                retry_digest.update(b"Y")
                                self.assertEqual(hashlib.sha256(source_path.read_bytes()).hexdigest(), retry_digest.hexdigest())
                                self.assertFalse(app.command("isModified")["modified"])
                                self.assertTrue(app.command("getLargeFileState")["largeFile"]["saveCommitted"])
                            if stage in ("write", "flush"):
                                self.assertEqual(list(root.glob(f"sync-{stage}-source.txt.classic-notepad-save-*.tmp")), [])
                            app.command("newDocument")
        finally:
            if previous_fault is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_SAVE_FAULT", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = previous_fault

    @unittest.skipUnless(automation_platform() == "windows", "Windows destructive synchronous Save recovery")
    def test_synchronous_save_keeps_recovery_when_destination_was_displaced(self):
        previous_fault = os.environ.get("CLASSIC_NOTEPAD_TEST_SAVE_FAULT")
        try:
            with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
                root = Path(temp_root)
                source_path = root / "displaced-sync-source.txt"
                line = b"0123456789abcdef" * 255 + b"\r\n"
                with source_path.open("wb") as output:
                    while output.tell() + len(line) <= 52 * 1024 * 1024:
                        output.write(line)
                original_hash = hashlib.sha256(source_path.read_bytes()).hexdigest()
                expected = hashlib.sha256(b"X")
                with source_path.open("rb") as source:
                    for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
                        expected.update(chunk)
                expected_hash = expected.hexdigest()
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = "replace-destination-moved"

                with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=45.0) as app:
                    app.command("openFile", path=str(source_path))
                    deadline = time.monotonic() + 30.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while not state["indexingComplete"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["indexingComplete"])
                    app.command("setSelection", start=0, end=0)
                    app.command("insertText", text="X")
                    response = app.try_command("save")
                    self.assertFalse(response.get("ok", False), response)

                    state = app.command("getLargeFileState")["largeFile"]
                    self.assertFalse(source_path.exists())
                    backup = list(root.glob("displaced-sync-source.txt.classic-notepad-test-original-*.bak"))
                    recovery = Path(state["saveRecoveryPath"])
                    self.assertEqual(len(backup), 1)
                    self.assertEqual(hashlib.sha256(backup[0].read_bytes()).hexdigest(), original_hash)
                    self.assertTrue(recovery.is_file())
                    self.assertEqual(hashlib.sha256(recovery.read_bytes()).hexdigest(), expected_hash)
                    self.assertTrue(state["active"])
                    self.assertTrue(state["modified"])
                    self.assertEqual(state["saveFailureStage"], "replace")
                    self.assertEqual(state["saveWin32Error"], 1177)
                    self.assertTrue(state["saveOutputComplete"])
                    self.assertFalse(state["saveCommitted"])
                    self.assertIn(str(recovery), response["error"])
                    self.assertEqual(app.command("getLargeFileRange", start=0, length=1)["text"], "X")

                    app.command("save")
                    self.assertEqual(hashlib.sha256(source_path.read_bytes()).hexdigest(), expected_hash)
                    self.assertEqual(hashlib.sha256(recovery.read_bytes()).hexdigest(), expected_hash)
                    self.assertFalse(app.command("isModified")["modified"])
                    self.assertTrue(app.command("getLargeFileState")["largeFile"]["saveCommitted"])
        finally:
            if previous_fault is None:
                os.environ.pop("CLASSIC_NOTEPAD_TEST_SAVE_FAULT", None)
            else:
                os.environ["CLASSIC_NOTEPAD_TEST_SAVE_FAULT"] = previous_fault

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
    def test_large_document_is_not_published_until_full_validation_completes(self):
        line = b"source-target".ljust(126, b".") + b"\r\n"
        block = line * 8192
        size = 192 * 1024 * 1024
        self.assertEqual(size % len(block), 0)

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            root = Path(temp_root)
            source_path = root / "incomplete-source.txt"
            save_as_path = root / "saved-copy.txt"

            def file_hash(path: Path) -> str:
                digest = hashlib.sha256()
                with path.open("rb") as source:
                    for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
                        digest.update(chunk)
                return digest.hexdigest()

            source_hash = hashlib.sha256()
            edited_hash = hashlib.sha256()
            edited_hash.update(b"XX")
            with source_path.open("wb") as output:
                for _ in range(size // len(block)):
                    output.write(block)
                    source_hash.update(block)
                    edited_hash.update(block)
            expected_source_hash = source_hash.hexdigest()
            expected_saved_hash = edited_hash.hexdigest()

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=120.0) as app:
                app.command("openFile", path=str(source_path))
                state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["editable"])
                self.assertTrue(state["indexingComplete"])
                self.assertTrue(state["encodingValid"])
                self.assertEqual(state["indexedBytes"], state["fileSize"])

                # A successfully published large view is complete before any
                # whole-document operation or edit can act on it.
                app.command("setSelection", start=0, end=0)
                app.command("insertText", text="X")
                app.command("setSelection", start=0, end=1)
                app.command("copy")
                app.command("setSelection", start=1, end=1)
                app.command("paste")
                self.assertEqual(app.command("getLargeFileRange", start=0, length=2)["text"], "XX")

                self.assertTrue(app.command("getLargeFileState")["largeFile"]["modified"])
                self.assertEqual(source_path.stat().st_size, size)
                self.assertEqual(source_hash.hexdigest(), expected_source_hash)
                self.assertEqual(file_hash(source_path), expected_source_hash)

                state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])
                self.assertTrue(state["encodingValid"])

                app.command("saveAs", path=str(save_as_path))
                self.assertEqual(file_hash(save_as_path), expected_saved_hash)

                deadline = time.monotonic() + 120.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.02)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])
                end = state["characterCount"]
                app.command("setSelection", start=end, end=end)
                app.command("insertText", text="Y")
                app.command("save")
                expected_saved_hash_with_tail_edit = edited_hash.copy()
                expected_saved_hash_with_tail_edit.update(b"Y")
                self.assertEqual(
                    file_hash(save_as_path),
                    expected_saved_hash_with_tail_edit.hexdigest(),
                )

    @unittest.skipUnless(automation_platform() == "windows", "Windows bounded original-piece save")
    def test_bounded_long_row_with_bom_unicode_and_crlf_seam_saves_exactly(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            root = Path(temp_root)
            source_path = root / "bounded-long-row-utf8-bom.txt"
            save_as_path = root / "bounded-long-row-saved.txt"
            repeated_blocks = 767
            seam_fill_bytes = repeated_blocks * (64 * 1024) + 65531
            block = b"b" * (64 * 1024)
            with source_path.open("wb") as output:
                output.write(b"\xef\xbb\xbf")
                output.write(b"a" * 65535)
                output.write("😀".encode("utf-8"))
                remaining = seam_fill_bytes
                while remaining >= len(block):
                    output.write(block)
                    remaining -= len(block)
                output.write(b"b" * remaining)
                # The builder's second piece ends with CR at its 64 KiB
                # boundary; LF therefore begins the next piece.
                output.write(b"\r\n")
                output.write(b"tail")
            self.assertGreaterEqual(source_path.stat().st_size, 48 * 1024 * 1024)

            def file_hash(path: Path) -> str:
                digest = hashlib.sha256()
                with path.open("rb") as source:
                    for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
                        digest.update(chunk)
                return digest.hexdigest()

            expected_hash = file_hash(source_path)
            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=120.0) as app:
                app.command("openFile", path=str(source_path))
                deadline = time.monotonic() + 90.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.025)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])
                self.assertTrue(state["encodingValid"])
                self.assertTrue(state["editable"])
                payload_bytes = source_path.stat().st_size - 3
                minimum_piece_count = (payload_bytes + (64 * 1024) - 1) // (64 * 1024)
                self.assertGreaterEqual(state["pieceCount"], minimum_piece_count)
                self.assertGreaterEqual(state["pieceCount"], 700)

                app.command("saveAs", path=str(save_as_path))
                self.assertEqual(file_hash(save_as_path), expected_hash)

                empty_path = root / "empty.txt"
                empty_saved_path = root / "empty-saved.txt"
                empty_path.write_bytes(b"")
                app.command("openFile", path=str(empty_path))
                app.command("saveAs", path=str(empty_saved_path))
                self.assertEqual(empty_saved_path.read_bytes(), b"")

    @unittest.skipUnless(automation_platform() == "windows", "large-file mode is Windows-only")
    def test_invalid_large_file_never_becomes_save_ready(self):
        line = b"valid-prefix".ljust(126, b".") + b"\r\n"
        block = line * 8192
        size = 56 * 1024 * 1024

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            root = Path(temp_root)
            source_path = root / "invalid-source.txt"
            save_as_path = root / "invalid-save-as.txt"
            async_path = root / "invalid-async-save.txt"
            source_hash = hashlib.sha256()
            with source_path.open("wb") as output:
                for _ in range(size // len(block)):
                    output.write(block)
                    source_hash.update(block)
                truncated_scalar = b"\xf0\x9f\x98"
                output.write(truncated_scalar)
                source_hash.update(truncated_scalar)
            expected_source_hash = source_hash.hexdigest()

            def file_hash(path: Path) -> str:
                digest = hashlib.sha256()
                with path.open("rb") as source:
                    for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
                        digest.update(chunk)
                return digest.hexdigest()

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=120.0) as app:
                response = app.try_command("openFile", path=str(source_path))
                self.assertFalse(response.get("ok", False), response)
                self.assertFalse(app.command("getLargeFileState")["largeFile"]["active"])
                self.assertFalse(async_path.exists())
                self.assertFalse(save_as_path.exists())
                self.assertEqual(file_hash(source_path), expected_source_hash)

    @unittest.skipUnless(automation_platform() == "windows", "large-file mode is Windows-only")
    def test_large_file_scroll_extents_follow_edits_and_undo(self):
        line = b"baseline-row".ljust(1022, b".") + b"\r\n"
        line_count = ((48 * 1024 * 1024) + len(line) - 1) // len(line)

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "edited-scroll-extents.txt"
            with path.open("wb") as output:
                block = line * 8192
                remaining = line_count
                while remaining >= 8192:
                    output.write(block)
                    remaining -= 8192
                output.write(line * remaining)

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=10.0) as app:
                app.command("openFile", path=str(path))
                state = app.command("getLargeFileState")["largeFile"]
                deadline = time.monotonic() + 30.0
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.05)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])
                self.assertEqual(state["maximumLineColumns"], 1022)
                baseline_lines = state["totalLines"]

                def assert_scroll_info(current):
                    legal_thumb_maximum = max(
                        0, current["verticalScrollRangeMaximum"] - current["verticalScrollPage"] + 1
                    )
                    self.assertLessEqual(current["verticalScrollPosition"], legal_thumb_maximum)
                    self.assertEqual(
                        current["verticalScrollRangeMaximum"] + 1 - current["verticalScrollPage"],
                        current["maximumTopLine"],
                    )
                    self.assertLessEqual(current["topLine"], current["maximumTopLine"])

                assert_scroll_info(state)
                wide_line = "W" * 1500 + "\r\n"
                app.command("setSelection", start=0, end=0)
                app.command("insertText", text=wide_line)
                state = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(state["totalLines"], baseline_lines + 1)
                self.assertEqual(state["maximumLineColumns"], 1500)
                assert_scroll_info(state)

                # A second equal maximum keeps the measured range when the
                # first widest row is removed.
                duplicate_start = state["characterCount"]
                app.command("setSelection", start=duplicate_start, end=duplicate_start)
                app.command("insertText", text=wide_line)
                state = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(state["totalLines"], baseline_lines + 2)
                self.assertEqual(state["maximumLineColumns"], 1500)
                assert_scroll_info(state)
                app.command("setSelection", start=0, end=len(wide_line))
                app.command("deleteSelection")
                state = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(state["totalLines"], baseline_lines + 1)
                self.assertEqual(state["maximumLineColumns"], 1500)
                assert_scroll_info(state)
                app.command("undo")
                state = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(state["totalLines"], baseline_lines + 2)
                self.assertEqual(state["maximumLineColumns"], 1500)
                assert_scroll_info(state)

                # Removing the final remaining widest row shrinks immediately;
                # Undo restores both its row and horizontal extent.
                second_start = state["characterCount"] - len(wide_line)
                app.command("setSelection", start=second_start, end=state["characterCount"])
                app.command("deleteSelection")
                state = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(state["totalLines"], baseline_lines + 1)
                self.assertEqual(state["maximumLineColumns"], 1500)
                assert_scroll_info(state)
                app.command("setSelection", start=0, end=len(wide_line))
                app.command("deleteSelection")
                state = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(state["totalLines"], baseline_lines)
                self.assertEqual(state["maximumLineColumns"], 1022)
                assert_scroll_info(state)
                app.command("undo")
                state = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(state["maximumLineColumns"], 1500)
                assert_scroll_info(state)
                app.command("setSelection", start=0, end=len(wide_line))
                app.command("deleteSelection")

                # Exercise row insertion/deletion at the middle and end too.
                middle_line = baseline_lines // 2
                app.command("goToLine", line=middle_line)
                middle = app.command("getLargeFileState")["largeFile"]["caret"]
                app.command("insertText", text="MID\r\n")
                state = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(state["totalLines"], baseline_lines + 1)
                assert_scroll_info(state)
                app.command("setSelection", start=middle, end=middle + 5)
                app.command("deleteSelection")
                state = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(state["totalLines"], baseline_lines)
                assert_scroll_info(state)

                end = state["characterCount"]
                app.command("setSelection", start=end, end=end)
                app.command("insertText", text="END\r\n")
                state = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(state["totalLines"], baseline_lines + 1)
                assert_scroll_info(state)
                app.command("setSelection", start=end, end=end + 5)
                app.command("deleteSelection")
                state = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(state["totalLines"], baseline_lines)
                assert_scroll_info(state)

                # Bottom and final thumb positions must stop at the last full
                # page, and remain coherent after font/viewport changes.
                bottom = app.command("sendLargeFileVerticalScroll", scrollCommand=7)["largeFile"]
                self.assertEqual(bottom["topLine"], bottom["maximumTopLine"])
                self.assertEqual(bottom["verticalScrollPosition"],
                    bottom["verticalScrollRangeMaximum"] - bottom["verticalScrollPage"] + 1)
                thumb = bottom["verticalScrollRangeMaximum"] - bottom["verticalScrollPage"] + 1
                tracked = app.command(
                    "sendLargeFileVerticalScroll", scrollCommand=5, trackPosition=thumb
                )["largeFile"]
                self.assertEqual(tracked["topLine"], tracked["maximumTopLine"])
                app.command("sendLargeFileVerticalScroll", scrollCommand=6)  # SB_TOP
                wheel = app.command("sendLargeFileWheel", delta=-120)["largeFile"]
                self.assertGreater(wheel["topLine"], 0)
                assert_scroll_info(wheel)
                app.command("setSelection", start=0, end=0)
                paged = app.command("sendLargeFileKey", key=0x22)["largeFile"]  # Page Down
                self.assertGreater(paged["caret"], 0)
                assert_scroll_info(paged)

                old_page = paged["verticalScrollPage"]
                app.command("setFont", font="Consolas 24")
                resized = app.command("resizeLargeFileViewport", width=700, height=200)["largeFile"]
                assert_scroll_info(resized)
                self.assertNotEqual(resized["verticalScrollPage"], old_page)
                bottom_after_resize = app.command(
                    "sendLargeFileVerticalScroll", scrollCommand=7
                )["largeFile"]
                self.assertEqual(bottom_after_resize["topLine"], bottom_after_resize["maximumTopLine"])
                assert_scroll_info(bottom_after_resize)

    @unittest.skipUnless(automation_platform() == "windows", "wrapped continuation scrolling is Windows-only")
    def test_wrapped_page_down_reaches_final_logical_row_and_survives_layout_changes(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "wrapped-continuation-scroll.txt"
            block = b"x" * (1024 * 1024)
            with path.open("wb") as output:
                for _ in range(48):
                    output.write(block)

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=10.0) as app:
                app.command("openFile", path=str(path))
                state = app.command("getLargeFileState")["largeFile"]
                deadline = time.monotonic() + 45.0
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.05)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"])

                wrapped_text = "A" * 1000 + "\r\n" + "B" * 1000
                app.command("setSelection", start=0, end=state["characterCount"])
                app.command("insertText", text=wrapped_text)
                self.assertEqual(app.command("getLargeFileState")["largeFile"]["totalLines"], 2)
                app.command("setSelection", start=0, end=0)
                app.command("invokeWordWrapMenuCommand")
                # The real Format command performs normal window layout, so
                # apply the test viewport after the command before measuring
                # wrapped continuation paging.
                small_view = app.command("resizeLargeFileViewport", width=400, height=120)["largeFile"]
                self.assertEqual(small_view["viewportWidth"], 400, small_view)
                self.assertEqual(small_view["viewportHeight"], 120, small_view)

                def current_wrap_anchor():
                    windows = app.command("getLargeFileVisibleWindows")["windows"]
                    current = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(windows)
                    self.assertGreater(current["wrappedContinuationRows"], 0, (current, windows))
                    return current["topLine"], windows[0]["start"], current

                line_starts = [0, 1002]
                anchors = [current_wrap_anchor()]
                self.assertEqual(anchors[0][0], 0)
                self.assertEqual(anchors[0][1], line_starts[0])
                self.assertGreater(anchors[0][2]["visibleLineCount"], 2)
                for _ in range(8):
                    app.command("sendLargeFileVerticalScroll", scrollCommand=3)  # SB_PAGEDOWN
                    anchors.append(current_wrap_anchor())

                anchor_starts = [start for _, start, _ in anchors]
                anchor_lines = [line for line, _, _ in anchors]
                self.assertEqual(anchor_starts, sorted(anchor_starts), anchors)
                self.assertEqual(anchor_lines, sorted(anchor_lines), anchors)
                first_row_starts = [start for line, start, _ in anchors if line == 0]
                self.assertEqual(first_row_starts[0], line_starts[0])
                self.assertEqual(first_row_starts, sorted(set(first_row_starts)))
                second_row_anchors = [(line, start) for line, start, _ in anchors if line == 1]
                self.assertTrue(second_row_anchors, anchors)
                self.assertTrue(all(start >= line_starts[1] for _, start in second_row_anchors), anchors)

                final_line, final_start, _ = anchors[-1]
                self.assertEqual(final_line, 1, anchors)
                self.assertGreaterEqual(final_start, line_starts[1], anchors)

                app.command("setFont", font="Consolas 16")
                app.command("getLargeFileVisibleWindows")
                after_font = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(after_font["topLine"], 1, after_font)
                self.assertGreaterEqual(
                    app.command("getLargeFileVisibleWindows")["windows"][0]["start"],
                    line_starts[1],
                )
                app.command("resizeLargeFileViewport", width=360, height=140)
                resized_windows = app.command("getLargeFileVisibleWindows")["windows"]
                after_resize = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(after_resize["topLine"], 1, after_resize)
                self.assertTrue(resized_windows)
                self.assertGreaterEqual(resized_windows[0]["start"], line_starts[1])

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
                app.command("invokeWordWrapMenuCommand")
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

    @unittest.skipUnless(automation_platform() == "windows", "large-file viewport mapping is Windows-only")
    def test_large_horizontal_viewport_mapping_is_bounded_and_shared(self):
        prefix = b"x" * 65535
        local_utf8 = "\U0001f600\tWiWide\tsegment".encode("utf-8")
        tail = b"RIGHT-SIDE-END"
        block = b"x" * (1024 * 1024)

        def write_large_utf8_row(path: Path, size: int) -> None:
            remaining = size - len(prefix) - len(local_utf8) - len(tail)
            self.assertGreater(remaining, 0)
            with path.open("wb") as output:
                output.write(prefix)
                output.write(local_utf8)
                while remaining:
                    chunk = min(remaining, len(block))
                    output.write(block[:chunk])
                    remaining -= chunk
                output.write(tail)

        def write_four_large_utf16_rows(path: Path, row_units: int) -> None:
            fill = (b"x\x00") * (1024 * 1024)
            with path.open("wb") as output:
                output.write(b"\xff\xfe")
                for row_number in range(4):
                    prefix_text = "x" * 65535 + "\U0001f600\tWiWide\t"
                    local = prefix_text.encode("utf-16-le")
                    marker = f"ROW-{row_number}".encode("utf-16-le")
                    used_units = len(local) // 2 + len(marker) // 2
                    remaining_units = row_units - used_units
                    self.assertGreater(remaining_units, 0)
                    output.write(local)
                    while remaining_units:
                        chunk_units = min(remaining_units, len(fill) // 2)
                        output.write(fill[:chunk_units * 2])
                        remaining_units -= chunk_units
                    output.write(marker)
                    if row_number != 3:
                        output.write("\r\n".encode("utf-16-le"))

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            root = Path(temp_root)
            smaller = root / "single-row-50MiB.txt"
            larger = root / "single-row-76MiB.txt"
            four_rows = root / "four-utf16-rows.txt"
            tab_heavy = root / "single-row-tabs-50MiB.txt"
            unequal_rows = root / "unequal-rows-38MiB-14MiB.txt"
            write_large_utf8_row(smaller, 50 * 1024 * 1024)
            write_large_utf8_row(larger, 76 * 1024 * 1024)
            write_four_large_utf16_rows(four_rows, 7 * 1024 * 1024)
            tab_block = b"\t" * (1024 * 1024)
            with tab_heavy.open("wb") as output:
                remaining_tabs = 50 * 1024 * 1024
                while remaining_tabs:
                    count = min(remaining_tabs, len(tab_block))
                    output.write(tab_block[:count])
                    remaining_tabs -= count
                output.write(b"TAIL-END")
            with unequal_rows.open("wb") as output:
                first_row = b"A" * (1024 * 1024)
                remaining_first = 38 * 1024 * 1024
                while remaining_first:
                    count = min(remaining_first, len(first_row))
                    output.write(first_row[:count])
                    remaining_first -= count
                output.write(b"\r\n")
                second_row = b"B" * (1024 * 1024)
                remaining_second = 14 * 1024 * 1024
                while remaining_second:
                    count = min(remaining_second, len(second_row))
                    output.write(second_row[:count])
                    remaining_second -= count

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=180.0) as app:
                def open_and_wait(path: Path):
                    app.command("openFile", path=str(path))
                    deadline = time.monotonic() + 150.0
                    state = app.command("getLargeFileState")["largeFile"]
                    while not state["indexingComplete"] and time.monotonic() < deadline:
                        time.sleep(0.05)
                        state = app.command("getLargeFileState")["largeFile"]
                    self.assertTrue(state["indexingComplete"])
                    self.assertTrue(state["encodingValid"])
                    return state

                def assert_caret_uses_visible_mapping():
                    windows = app.command("getLargeFileVisibleWindows")["windows"]
                    state = app.command("getLargeFileState")["largeFile"]
                    position = state["caret"]
                    self.assertTrue(any(window["start"] <= position <=
                        window["start"] + len(window["text"]) for window in windows),
                        {"caret": position, "windows": windows})
                    rectangle = state["caretRectangle"]
                    self.assertGreaterEqual(rectangle["left"], 0, state)
                    self.assertLess(rectangle["left"] + 2, state["viewportWidth"], state)
                    self.assertGreaterEqual(rectangle["top"], 0, state)
                    self.assertLess(rectangle["bottom"], state["viewportHeight"], state)
                    return state, windows

                open_and_wait(smaller)
                small_windows = app.command("getLargeFileVisibleWindows")["windows"]
                self.assertEqual(len(small_windows), 1)
                small_work = app.command("getLargeFileState")["largeFile"]
                self.assertGreater(small_work["viewportLastBuildDecodedCharacters"], 0)
                self.assertLess(small_work["viewportLastBuildDecodedCharacters"], 8192)
                self.assertLess(small_work["viewportLastBuildMeasuredCharacters"], 8192)

                large_state = open_and_wait(larger)
                large_windows = app.command("getLargeFileVisibleWindows")["windows"]
                self.assertEqual(len(large_windows), 1)
                large_work = app.command("getLargeFileState")["largeFile"]
                self.assertLess(large_work["viewportLastBuildDecodedCharacters"], 8192)
                self.assertLess(large_work["viewportLastBuildMeasuredCharacters"], 8192)
                self.assertLessEqual(abs(
                    large_work["viewportLastBuildDecodedCharacters"] -
                    small_work["viewportLastBuildDecodedCharacters"]), 16)

                # Repeated viewport reads reuse the same row mapping. Horizontal
                # scrolling invalidates it and causes one bounded rebuild.
                rebuilds = large_work["viewportCacheRebuilds"]
                hits = large_work["viewportCacheHits"]
                app.command("getLargeFileVisibleWindows")
                reused = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(reused["viewportCacheRebuilds"], rebuilds)
                self.assertGreater(reused["viewportCacheHits"], hits)
                app.command("sendLargeFileHorizontalScroll", scrollCommand=7)  # SB_RIGHT
                right_windows = app.command("getLargeFileVisibleWindows")["windows"]
                right_state = app.command("getLargeFileState")["largeFile"]
                self.assertGreater(right_state["viewportCacheRebuilds"], rebuilds)
                self.assertGreater(right_state["horizontalMaximumOffset"], 0, right_state)
                self.assertEqual(right_state["horizontalOffset"],
                    right_state["horizontalMaximumOffset"], right_state)
                self.assertGreater(right_state["viewportLastLogicalAnchor"], 65536, right_state)
                self.assertGreater(right_state["viewportLastSegmentStart"], 65536)
                self.assertLess(right_state["viewportLastBuildDecodedCharacters"], 8192)
                self.assertLess(right_state["viewportLastBuildMeasuredCharacters"], 8192)
                self.assertEqual(right_windows[0]["start"], right_state["viewportLastSegmentStart"])
                self.assertTrue(right_windows[0]["text"].endswith("RIGHT-SIDE-END"))

                # The real child-window click must resolve through that same map;
                # the caret should land at the measured boundary near the click.
                click_x = min(100, max(20, large_state["viewportWidth"] // 3))
                click = app.command("sendLargeFileClick", x=click_x, y=8)["largeFile"]
                self.assertGreater(click["caret"], 65536)
                self.assertLessEqual(abs(click["caretRectangle"]["left"] - click_x), 20)
                click, click_windows = assert_caret_uses_visible_mapping()
                clicked_position = click["caret"]
                unchanged = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(unchanged["viewportCacheRebuilds"], right_state["viewportCacheRebuilds"])
                right_arrow = app.command("sendLargeFileKey", key=0x27)["largeFile"]
                self.assertEqual(right_arrow["caret"], clicked_position + 1)
                right_arrow, _ = assert_caret_uses_visible_mapping()
                left_arrow = app.command("sendLargeFileKey", key=0x25)["largeFile"]
                self.assertEqual(left_arrow["caret"], clicked_position)
                left_arrow, _ = assert_caret_uses_visible_mapping()

                # Resize, font, theme, and wrap transitions rebuild the mapping.
                old_width = large_state["viewportWidth"]
                old_height = large_state["viewportHeight"]
                before_resize = app.command("getLargeFileState")["largeFile"]["viewportCacheRebuilds"]
                app.command("resizeLargeFileViewport", width=max(320, old_width - 120),
                    height=max(240, old_height - 80))
                app.command("getLargeFileVisibleWindows")
                resized = app.command("getLargeFileState")["largeFile"]
                self.assertEqual(resized["viewportWidth"], max(320, old_width - 120))
                self.assertGreater(resized["viewportCacheRebuilds"], before_resize)
                app.command("resizeLargeFileViewport", width=old_width, height=old_height)
                before_font = app.command("getLargeFileState")["largeFile"]["viewportCacheRebuilds"]
                app.command("setFont", font="Arial 11")
                app.command("getLargeFileVisibleWindows")
                self.assertGreater(app.command("getLargeFileState")["largeFile"]["viewportCacheRebuilds"], before_font)
                before_theme = app.command("getLargeFileState")["largeFile"]["viewportCacheRebuilds"]
                app.command("setAppearanceTheme", theme="dark")
                app.command("getLargeFileVisibleWindows")
                self.assertGreater(app.command("getLargeFileState")["largeFile"]["viewportCacheRebuilds"], before_theme)
                app.command("setAppearanceTheme", theme="light")
                large_tail = app.command("getLargeFileState")["largeFile"]["characterCount"]
                app.command("setSelection", start=large_tail, end=large_tail)
                one_row_before_state = app.command("getLargeFileState")["largeFile"]
                one_row_before_wrap = app.command("getLargeFileVisibleWindows")["windows"]
                before_wrap = app.command("getLargeFileState")["largeFile"]["viewportCacheRebuilds"]
                app.command("invokeWordWrapMenuCommand")
                pre_read_wrap_state = app.command("getLargeFileState")["largeFile"]
                visible_response = app.try_command("getLargeFileVisibleWindows")
                self.assertTrue(visible_response.get("ok", False), (pre_read_wrap_state, visible_response))
                one_row_wrapped_windows = visible_response["windows"]
                wrapped = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(wrapped["wordWrap"])
                self.assertEqual(wrapped["caret"], large_tail)
                self.assertEqual(wrapped["selectionStart"], large_tail)
                self.assertEqual(wrapped["selectionEnd"], large_tail)
                self.assertEqual(wrapped["characterCount"], one_row_before_state["characterCount"])
                self.assertEqual(wrapped["generation"], one_row_before_state["generation"])
                self.assertEqual(wrapped["modified"], one_row_before_state["modified"])
                self.assertGreater(wrapped["viewportVisualRows"], 0)
                self.assertGreater(wrapped["viewportLastBuildMeasuredCharacters"],
                    wrapped["viewportLastBuildDecodedCharacters"])
                self.assertLessEqual(wrapped["viewportLastBuildDecodedCharacters"], 64 * 1024)
                self.assertLess(wrapped["viewportLastBuildMeasuredCharacters"], 4 * 1024 * 1024)
                self.assertTrue(one_row_wrapped_windows)
                self.assertTrue("".join(window["text"] for window in one_row_wrapped_windows)
                    .endswith("RIGHT-SIDE-END"), one_row_wrapped_windows)
                self.assertEqual(one_row_wrapped_windows[0]["sessionId"], one_row_before_wrap[0]["sessionId"])
                self.assertEqual(one_row_wrapped_windows[0]["contentRevision"], one_row_before_wrap[0]["contentRevision"])
                self.assertGreater(one_row_wrapped_windows[0]["layoutRevision"], one_row_before_wrap[0]["layoutRevision"])
                self.assertGreater(app.command("getLargeFileState")["largeFile"]["viewportCacheRebuilds"], before_wrap)
                before_unwrap = app.command("getLargeFileState")["largeFile"]["viewportCacheRebuilds"]
                app.command("invokeWordWrapMenuCommand")
                app.command("getLargeFileVisibleWindows")
                self.assertGreater(app.command("getLargeFileState")["largeFile"]["viewportCacheRebuilds"], before_unwrap)
                app.command("setFont", font="Consolas 11")

                # Four UTF-16 rows each exceed the old 65,536-unit edge. The
                # spelling diagnostic exposes separate absolute windows.
                four_state = open_and_wait(four_rows)
                self.assertEqual(four_state["totalLines"], 4)
                left_windows = app.command("getLargeFileVisibleWindows")["windows"]
                app.command("sendLargeFileHorizontalScroll", scrollCommand=7)
                windows = app.command("getLargeFileVisibleWindows")["windows"]
                self.assertEqual(len(windows), 4)
                spelling_revision = (
                    windows[0]["sessionId"],
                    windows[0]["contentRevision"],
                    windows[0]["layoutRevision"],
                )
                self.assertTrue(all(
                    (window["sessionId"], window["contentRevision"], window["layoutRevision"])
                    == spelling_revision for window in windows
                ))
                self.assertEqual(spelling_revision[0], left_windows[0]["sessionId"])
                self.assertEqual(spelling_revision[1], left_windows[0]["contentRevision"])
                self.assertGreater(spelling_revision[2], left_windows[0]["layoutRevision"])
                self.assertTrue(all(
                    window["eligibleStart"] <= window["eligibleEnd"] and
                    window["eligibleEnd"] <= len(window["text"].encode("utf-16-le")) // 2 and
                    len(window["text"].encode("utf-16-le")) // 2 <= 96 * 1024 for window in windows
                ))
                self.assertLessEqual(
                    sum(len(window["text"].encode("utf-16-le")) // 2 for window in windows),
                    96 * 1024,
                )
                starts = [window["start"] for window in windows]
                self.assertEqual(len(set(starts)), 4)
                self.assertTrue(all(start > 65536 for start in starts))
                self.assertTrue(all(len(window["text"]) < 8192 for window in windows))
                self.assertTrue(windows[3]["text"].endswith("ROW-3"))
                self.assertGreater(starts[1] - starts[0], 1024 * 1024)

                app.command("sendLargeFileClick", x=100, y=8)  # focus the child so the caret row is measured
                app.command("goToLine", line=4)
                app.command("sendLargeFileHorizontalScroll", scrollCommand=7)
                last_row_windows = app.command("getLargeFileVisibleWindows")["windows"]
                self.assertTrue(any("ROW-3" in window["text"] for window in last_row_windows))
                last_row_start = starts[3]
                last_row_state = app.command("getLargeFileState")["largeFile"]
                visible_row = 3 - last_row_state["topLine"]
                self.assertGreaterEqual(visible_row, 0)
                self.assertLess(visible_row, last_row_state["visibleLineCount"])
                line_height = max(1, last_row_state["viewportHeight"] //
                    last_row_state["visibleLineCount"])
                last_row_y = visible_row * line_height + 1
                self.assertLess(last_row_y, last_row_state["viewportHeight"])
                click = app.command("sendLargeFileClick", x=100, y=last_row_y)["largeFile"]
                self.assertGreater(click["caret"], last_row_start)
                self.assertLessEqual(abs(click["caretRectangle"]["left"] - 100), 20)
                click, _ = assert_caret_uses_visible_mapping()
                dragged = app.command("sendLargeFileDrag", startX=80, startY=last_row_y,
                    endX=160, endY=last_row_y)["largeFile"]
                self.assertGreater(dragged["selectionStart"], last_row_start)
                self.assertGreater(dragged["selectionEnd"], dragged["selectionStart"])
                selected_start = dragged["selectionStart"]
                selected_end = dragged["selectionEnd"]
                dragged, _ = assert_caret_uses_visible_mapping()
                app.command("sendLargeFileText", text="EDIT")
                self.assertEqual(
                    app.command("getLargeFileRange", start=selected_start, length=4)["text"], "EDIT")
                assert_caret_uses_visible_mapping()
                app.command("sendLargeFileKey", key=0x5A, control=True)
                self.assertNotEqual(
                    app.command("getLargeFileRange", start=selected_start, length=4)["text"], "EDIT")
                assert_caret_uses_visible_mapping()

                # UTF-16 scalar and tab boundaries are navigated at an absolute
                # position near the old fixed-prefix cutoff.
                scalar_start = 65535
                app.command("setSelection", start=scalar_start, end=scalar_start)
                local_windows = app.command("getLargeFileVisibleWindows")["windows"]
                scalar_window = next(window for window in local_windows
                    if window["start"] <= scalar_start < window["start"] + len(window["text"]))
                self.assertTrue(scalar_window["text"][scalar_start - scalar_window["start"]:].startswith("\U0001f600\t"))
                after_scalar = app.command("sendLargeFileKey", key=0x27)["largeFile"]
                self.assertEqual(after_scalar["caret"], scalar_start + 2)
                app.command("setSelection", start=scalar_start + 3,
                    end=scalar_start + 3)
                app.command("sendLargeFileText", text="A\tW")
                before_w = app.command("sendLargeFileKey", key=0x25)["largeFile"]
                after_a = app.command("sendLargeFileKey", key=0x25)["largeFile"]
                self.assertGreater(before_w["caretRectangle"]["left"] - after_a["caretRectangle"]["left"], 20)
                app.command("sendLargeFileKey", key=0x5A, control=True)
                four_row_end = app.command("getLargeFileState")["largeFile"]["characterCount"]
                app.command("setSelection", start=four_row_end, end=four_row_end)
                four_row_before_state = app.command("getLargeFileState")["largeFile"]
                end, _ = assert_caret_uses_visible_mapping()
                self.assertEqual(end["caret"], four_row_end)
                four_row_before_wrap = app.command("getLargeFileVisibleWindows")["windows"]
                app.command("invokeWordWrapMenuCommand")
                four_row_wrapped = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(four_row_wrapped["wordWrap"], four_row_wrapped)
                self.assertEqual(four_row_wrapped["caret"], four_row_end)
                self.assertEqual(four_row_wrapped["selectionStart"], four_row_end)
                self.assertEqual(four_row_wrapped["selectionEnd"], four_row_end)
                self.assertEqual(four_row_wrapped["characterCount"], four_row_end)
                self.assertEqual(four_row_wrapped["generation"], four_row_before_state["generation"])
                self.assertEqual(four_row_wrapped["modified"], four_row_before_state["modified"])
                four_row_tail_windows = app.command("getLargeFileVisibleWindows")["windows"]
                self.assertTrue(four_row_tail_windows)
                self.assertTrue("".join(window["text"] for window in four_row_tail_windows)
                    .endswith("ROW-3"),
                    four_row_tail_windows)
                self.assertEqual(four_row_tail_windows[0]["sessionId"], four_row_before_wrap[0]["sessionId"])
                self.assertEqual(four_row_tail_windows[0]["contentRevision"],
                    four_row_before_wrap[0]["contentRevision"])
                self.assertGreater(four_row_tail_windows[0]["layoutRevision"],
                    four_row_before_wrap[0]["layoutRevision"])
                app.command("invokeWordWrapMenuCommand")
                self.assertFalse(app.command("getLargeFileState")["largeFile"]["wordWrap"])

                # A tab-dense skipped prefix makes visual pixel width much
                # larger than UTF-16 length. The logical scroll anchor still
                # has to reach and map the actual tail without measuring the
                # skipped prefix.
                open_and_wait(tab_heavy)
                app.command("sendLargeFileHorizontalScroll", scrollCommand=7)
                tab_windows = app.command("getLargeFileVisibleWindows")["windows"]
                self.assertEqual(len(tab_windows), 1)
                self.assertTrue(tab_windows[0]["text"].endswith("TAIL-END"))
                tab_right = app.command("getLargeFileState")["largeFile"]
                self.assertLess(tab_right["viewportLastBuildDecodedCharacters"], 8192)
                self.assertEqual(tab_right["horizontalOffset"], tab_right["horizontalMaximumOffset"])
                self.assertGreater(tab_right["viewportLastLogicalAnchor"], 65536)
                tab_click = app.command("sendLargeFileClick", x=100, y=8)["largeFile"]
                self.assertLessEqual(abs(tab_click["caretRectangle"]["left"] - 100), 20)
                assert_caret_uses_visible_mapping()
                app.command("sendLargeFileKey", key=0x23)  # End
                tab_end, tab_end_windows = assert_caret_uses_visible_mapping()
                self.assertEqual(tab_end["caret"], tab_end["characterCount"])
                self.assertTrue(tab_end_windows[0]["text"].endswith("TAIL-END"))
                tail_position = tab_end["caret"]
                app.command("sendLargeFileText", text="EDIT")
                edited_tail, _ = assert_caret_uses_visible_mapping()
                self.assertEqual(edited_tail["caret"], tail_position + 4)
                self.assertEqual(app.command("getLargeFileRange", start=tail_position, length=4)["text"], "EDIT")
                app.command("sendLargeFileKey", key=0x5A, control=True)
                undone_tail, _ = assert_caret_uses_visible_mapping()
                self.assertEqual(undone_tail["caret"], tail_position)

                # Different line lengths share the same global scroll fraction,
                # while each row's local segment and caret geometry stay valid.
                unequal_state = open_and_wait(unequal_rows)
                self.assertEqual(unequal_state["totalLines"], 2)
                app.command("sendLargeFileHorizontalScroll", scrollCommand=7)
                app.command("goToLine", line=2)
                app.command("sendLargeFileKey", key=0x23)  # End of second row
                second_end, second_row_windows = assert_caret_uses_visible_mapping()
                self.assertEqual(second_end["caret"], second_end["characterCount"])
                self.assertTrue(any("B" in window["text"] for window in second_row_windows))
                app.command("sendLargeFileKey", key=0x24)  # Home of second row
                second_home, _ = assert_caret_uses_visible_mapping()
                self.assertGreater(second_home["caret"], 38 * 1024 * 1024)
                app.command("sendLargeFileKey", key=0x23)  # End of second row
                second_end, second_row_windows = assert_caret_uses_visible_mapping()
                self.assertEqual(second_end["caret"], second_end["characterCount"])
                second_visible_row = 1 - second_home["topLine"]
                self.assertGreaterEqual(second_visible_row, 0)
                self.assertLess(second_visible_row, second_home["visibleLineCount"])
                second_line_height = max(1, second_home["viewportHeight"] //
                    second_home["visibleLineCount"])
                second_row_y = second_visible_row * second_line_height + 1
                unequal_click = app.command("sendLargeFileClick", x=100, y=second_row_y)["largeFile"]
                self.assertLessEqual(abs(unequal_click["caretRectangle"]["left"] - 100), 20)
                assert_caret_uses_visible_mapping()
                dragged = app.command("sendLargeFileDrag", startX=80, startY=second_row_y,
                    endX=160, endY=second_row_y)["largeFile"]
                self.assertGreater(dragged["selectionStart"], 38 * 1024 * 1024)
                self.assertGreater(dragged["selectionEnd"], dragged["selectionStart"])
                assert_caret_uses_visible_mapping()
                unequal_edit_position = dragged["selectionStart"]
                app.command("sendLargeFileText", text="EDIT")
                self.assertEqual(app.command("getLargeFileRange", start=unequal_edit_position,
                    length=4)["text"], "EDIT")
                assert_caret_uses_visible_mapping()
                app.command("sendLargeFileKey", key=0x5A, control=True)
                assert_caret_uses_visible_mapping()


    @unittest.skipUnless(automation_platform() == "windows", "wrapped large-file resize is Windows-only")
    def test_wrapped_unicode_eof_caret_stays_visible_after_resize(self):
        prefix_bytes = 48 * 1024 * 1024
        suffix = "漢\t😀" * 600
        suffix_utf16 = suffix.encode("utf-16-le", errors="surrogatepass")
        expected_units = prefix_bytes + len(suffix_utf16) // 2

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "wrapped-unicode-eof.txt"
            block = b"x" * (1024 * 1024)
            with path.open("wb") as output:
                remaining = prefix_bytes
                while remaining:
                    count = min(remaining, len(block))
                    output.write(block[:count])
                    remaining -= count
                output.write(suffix.encode("utf-8"))

            with ClassicNotepadDriver(automation_binary(), automation_platform(), timeout=180.0) as app:
                app.command("openFile", path=str(path))
                deadline = time.monotonic() + 120.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.05)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"], state)
                self.assertEqual(state["characterCount"], expected_units)
                self.assertEqual(state["fileSize"], prefix_bytes + len(suffix.encode("utf-8")))

                app.command("setFont", font="Consolas 11")
                app.command("setSelection", start=expected_units, end=expected_units)
                app.command("invokeWordWrapMenuCommand")
                before = app.command("getLargeFileState")["largeFile"]
                before_windows = app.command("getLargeFileVisibleWindows")["windows"]
                self.assertTrue(before["wordWrap"], before)
                self.assertEqual(before["caret"], expected_units)
                self.assertEqual((before["selectionStart"], before["selectionEnd"]),
                    (expected_units, expected_units))
                self.assertTrue(before_windows)
                before_metadata = app.command("getDocumentMetadata")["metadata"]

                app.command("resizeLargeFileViewport", width=400, height=160)
                after = app.command("getLargeFileState")["largeFile"]
                windows = app.command("getLargeFileVisibleWindows")["windows"]
                selection = app.command("getSelection")["selection"]
                metadata = app.command("getDocumentMetadata")["metadata"]

                self.assertTrue(windows)
                self.assertEqual((after["viewportWidth"], after["viewportHeight"]), (400, 160), after)
                caret_rectangle = after["caretRectangle"]
                self.assertGreaterEqual(caret_rectangle["left"], 0, after)
                self.assertGreaterEqual(caret_rectangle["top"], 0, after)
                self.assertLessEqual(caret_rectangle["right"], after["viewportWidth"], after)
                self.assertLessEqual(caret_rectangle["bottom"], after["viewportHeight"], after)
                self.assertEqual(after["caret"], expected_units, after)
                self.assertEqual((after["selectionStart"], after["selectionEnd"]),
                    (expected_units, expected_units), after)
                self.assertEqual((selection["start"], selection["end"]),
                    (expected_units, expected_units), selection)
                self.assertEqual(after["characterCount"], before["characterCount"])
                self.assertEqual(after["generation"], before["generation"])
                self.assertEqual(after["modified"], before["modified"])
                self.assertFalse(after["modified"])
                self.assertEqual(metadata, before_metadata)
                self.assertGreater(windows[0]["layoutRevision"], before_windows[0]["layoutRevision"])
                self.assertTrue(all(
                    (window["sessionId"], window["contentRevision"], window["layoutRevision"])
                    == (windows[0]["sessionId"], windows[0]["contentRevision"], windows[0]["layoutRevision"])
                    for window in windows
                ))
                self.assertTrue(windows[-1]["text"].endswith("漢\t😀"), windows[-2:])
                self.assertLessEqual(after["viewportLastBuildDecodedCharacters"], 64 * 1024, after)
                self.assertLess(after["viewportLastBuildMeasuredCharacters"], 4 * 1024 * 1024, after)
                self.assertLessEqual(
                    sum(utf16_units(window["text"]) for window in windows), 96 * 1024, windows[-2:])

                def unit_at(position):
                    if position < prefix_bytes:
                        return ord("x")
                    offset = (position - prefix_bytes) * 2
                    return int.from_bytes(suffix_utf16[offset:offset + 2], "little")

                def source_slice(start, count):
                    end = start + count
                    pieces = []
                    prefix_end = min(end, prefix_bytes)
                    if start < prefix_end:
                        pieces.append((b"x\x00") * (prefix_end - start))
                    tail_start = max(start, prefix_bytes)
                    if tail_start < end:
                        tail_end = end - prefix_bytes
                        pieces.append(suffix_utf16[(tail_start - prefix_bytes) * 2:tail_end * 2])
                    return b"".join(pieces)

                def assert_scalar_boundary(position):
                    if not 0 < position < expected_units:
                        return
                    previous = unit_at(position - 1)
                    current = unit_at(position)
                    self.assertFalse(0xD800 <= previous <= 0xDBFF and 0xDC00 <= current <= 0xDFFF,
                        position)

                total_units = 0
                for window in windows:
                    start = window["start"]
                    text = window["text"]
                    count = utf16_units(text)
                    end = start + count
                    self.assertGreaterEqual(start, 0)
                    self.assertLessEqual(end, expected_units)
                    self.assertEqual(text.encode("utf-16-le", errors="surrogatepass"),
                        source_slice(start, count), window)
                    assert_scalar_boundary(start)
                    assert_scalar_boundary(end)
                    for eligible in (window["eligibleStart"], window["eligibleEnd"]):
                        self.assertGreaterEqual(eligible, 0)
                        self.assertLessEqual(eligible, count)
                        assert_scalar_boundary(start + eligible)
                    total_units += count
                self.assertLessEqual(total_units, 96 * 1024)

                # An explicit scroll away from EOF keeps its anchor through a
                # later resize; resizing must not force an already offscreen
                # caret back into view.
                app.command("sendLargeFileVerticalScroll", scrollCommand=6)  # SB_TOP
                top_windows = app.command("getLargeFileVisibleWindows")["windows"]
                self.assertTrue(top_windows)
                self.assertEqual(top_windows[0]["start"], 0, top_windows[:2])
                self.assertTrue(top_windows[0]["text"].startswith("x"), top_windows[:2])
                app.command("resizeLargeFileViewport", width=420, height=170)
                scrolled_state = app.command("getLargeFileState")["largeFile"]
                scrolled_windows = app.command("getLargeFileVisibleWindows")["windows"]
                self.assertEqual(scrolled_state["caret"], expected_units, scrolled_state)
                self.assertEqual(scrolled_state["viewportLastSegmentStart"], 0, scrolled_state)
                self.assertTrue(scrolled_windows[0]["text"].startswith("x"), scrolled_windows[:2])
if __name__ == "__main__":
    unittest.main()
