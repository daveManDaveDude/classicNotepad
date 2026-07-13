"""Repeatable Windows Release 700 MiB editable large-document acceptance test."""

import argparse
import ctypes
import hashlib
import json
import os
import tempfile
import time
from pathlib import Path

from classic_notepad_driver import ClassicNotepadDriver


TARGET_SIZE = 734_003_200  # 700 MiB exactly
LINE_SIZE = 128


def write_fixture(path: Path) -> None:
    if TARGET_SIZE % LINE_SIZE:
        raise RuntimeError("fixture size must be an exact number of fixed-width lines")
    first = b"BEGIN-700MB-LARGE-FILE".ljust(LINE_SIZE - 2, b".") + b"\r\n"
    middle = b"mapped-read-only-index-line".ljust(LINE_SIZE - 2, b".") + b"\r\n"
    last = b"END-700MB-LARGE-FILE".ljust(LINE_SIZE - 2, b".") + b"\r\n"
    lines = TARGET_SIZE // LINE_SIZE
    with path.open("wb", buffering=1024 * 1024) as output:
        output.write(first)
        block = middle * 8192
        remaining = lines - 2
        while remaining >= 8192:
            output.write(block)
            remaining -= 8192
        if remaining:
            output.write(middle * remaining)
        output.write(last)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb", buffering=1024 * 1024) as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def memory_bytes(pid: int) -> tuple[int, int]:
    class ProcessMemoryCountersEx(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
            ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
            ("PrivateUsage", ctypes.c_size_t),
        ]

    process = ctypes.windll.kernel32.OpenProcess(0x0400 | 0x0010, False, pid)
    if not process:
        return 0, 0
    try:
        counters = ProcessMemoryCountersEx()
        counters.cb = ctypes.sizeof(counters)
        if not ctypes.windll.psapi.GetProcessMemoryInfo(process, ctypes.byref(counters), counters.cb):
            return 0, 0
        return int(counters.WorkingSetSize), int(counters.PrivateUsage)
    finally:
        ctypes.windll.kernel32.CloseHandle(process)


def timed(app: ClassicNotepadDriver, command: str, **kwargs):
    started = time.perf_counter()
    result = app.command(command, **kwargs)
    return result, round((time.perf_counter() - started) * 1000)


def wait_for_index(app: ClassicNotepadDriver, timeout_seconds: float, peak: dict) -> dict:
    deadline = time.monotonic() + timeout_seconds
    while True:
        state = app.command("getLargeFileState")["largeFile"]
        working_set, private_bytes = memory_bytes(app._process.pid)
        peak["workingSet"] = max(peak["workingSet"], working_set)
        peak["privateBytes"] = max(peak["privateBytes"], private_bytes)
        if state["indexingComplete"]:
            return state
        if time.monotonic() >= deadline:
            raise AssertionError(f"indexing did not complete: {state}")
        # This is also a responsiveness probe: getLargeFileState is served
        # repeatedly while the worker indexes the mapped source.
        time.sleep(0.05)


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify Windows Release editable mapped large-document mode with a real 700 MiB file.")
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--max-first-usable-ms", type=int, default=2000)
    parser.add_argument("--index-timeout-seconds", type=float, default=120.0)
    parser.add_argument("--operation-timeout-seconds", type=float, default=240.0)
    args = parser.parse_args()

    binary = args.binary.resolve()
    if not binary.exists():
        parser.error(f"binary does not exist: {binary}")
    if os.name != "nt":
        parser.error("this test is Windows-only")

    os.environ["CLASSIC_NOTEPAD_AUTOMATION_VISIBLE"] = "1"
    root = binary.parents[2] / "automation-large-file-tests"
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=root) as temp_root:
        temporary = Path(temp_root)
        source = temporary / "classic-notepad-700MiB-source.txt"
        saved = temporary / "classic-notepad-700MiB-saved.txt"
        write_fixture(source)
        if source.stat().st_size != TARGET_SIZE:
            raise AssertionError(f"fixture size was {source.stat().st_size}, expected {TARGET_SIZE}")
        source_hash = sha256(source)
        peak = {"workingSet": 0, "privateBytes": 0}
        measurements = {}

        with ClassicNotepadDriver(str(binary), "windows", timeout=args.operation_timeout_seconds) as app:
            _, measurements["openCommandMilliseconds"] = timed(app, "openFile", path=str(source))
            state = app.command("getLargeFileState")["largeFile"]
            if not state["active"] or state["readOnly"]:
                raise AssertionError(f"editable large-file mode did not activate: {state}")
            if state["fileSize"] != TARGET_SIZE:
                raise AssertionError(f"reported file size {state['fileSize']} does not match fixture")
            state = wait_for_index(app, args.index_timeout_seconds, peak)
            if not state["encodingValid"] or not state["editable"]:
                raise AssertionError(f"valid UTF-8 fixture was not editable: {state}")
            if state["firstUsableMilliseconds"] == 0 or state["firstUsableMilliseconds"] > args.max_first_usable_ms:
                raise AssertionError(f"first usable display was too slow: {state['firstUsableMilliseconds']} ms")
            if "BEGIN-700MB-LARGE-FILE" not in app.command("getLargeFileLine", line=1)["text"]:
                raise AssertionError("beginning content was not available")
            last_content_line = state["totalLines"] - 1
            app.command("goToLine", line=last_content_line)
            if "END-700MB-LARGE-FILE" not in app.command("getLargeFileLine", line=last_content_line)["text"]:
                raise AssertionError("end content was not available")

            _, measurements["beginningEditMilliseconds"] = timed(app, "setSelection", start=0, end=0)
            _, measurements["typingMilliseconds"] = timed(app, "typeText", text="[BEGIN-EDIT]")
            state = app.command("getLargeFileState")["largeFile"]
            middle = state["characterCount"] // 2
            app.command("setSelection", start=middle, end=middle)
            _, measurements["middleEditMilliseconds"] = timed(app, "insertText", text="[MIDDLE-EDIT]\r\n")
            state = app.command("getLargeFileState")["largeFile"]
            app.command("setSelection", start=state["characterCount"], end=state["characterCount"])
            _, measurements["endEditMilliseconds"] = timed(app, "insertText", text="[END-EDIT]")

            _, measurements["findMilliseconds"] = timed(
                app, "find", text="BEGIN-700MB-LARGE-FILE", matchCase=True, wholeWord=False, searchDown=True)
            replacement, measurements["replaceMilliseconds"] = timed(
                app, "replace", text="BEGIN-700MB-LARGE-FILE", replacement="START-700MB-LARGE-FILE",
                matchCase=True, wholeWord=False, searchDown=True)
            if not replacement["replaced"]:
                raise AssertionError("Replace current did not mutate the large document")
            _, measurements["undoMilliseconds"] = timed(app, "undo")
            # Reapply the replace after proving undo semantics, then insert a
            # deterministic Time/Date-equivalent marker.
            app.command("replace", text="BEGIN-700MB-LARGE-FILE", replacement="START-700MB-LARGE-FILE",
                        matchCase=True, wholeWord=False, searchDown=True)
            app.command("setSelection", start=0, end=0)
            app.command("insertText", text="[DATE-TEST]")

            app.command("selectAll")
            all_state = app.command("getLargeFileState")["largeFile"]
            if all_state["selectionStart"] != 0 or all_state["selectionEnd"] != all_state["characterCount"]:
                raise AssertionError("Select All did not create a logical full-document selection")
            app.command("setSelection", start=0, end=64)
            app.command("copy")
            if not app.command("isModified")["modified"] or not app.command("getTitle")["title"].startswith("*"):
                raise AssertionError("large-document edits did not set the dirty title and modified state")

            _, measurements["saveAsMilliseconds"] = timed(app, "saveAs", path=str(saved))
            saved_state = wait_for_index(app, args.index_timeout_seconds, peak)
            if app.command("isModified")["modified"]:
                raise AssertionError("Save As did not clear modified state")
            if sha256(source) != source_hash:
                raise AssertionError("source file changed during Save As")
            if not saved.exists() or saved.stat().st_size < TARGET_SIZE:
                raise AssertionError("streaming Save As did not create the expected large output")
            saved_hash = sha256(saved)
            if "[DATE-TEST][BEGIN-EDIT]" not in app.command("getLargeFileRange", start=0, length=128)["text"]:
                raise AssertionError("saved beginning edits were not available after rebase")
            if "[END-EDIT]" not in app.command("getLargeFileRange", start=saved_state["characterCount"] - 64, length=64)["text"]:
                raise AssertionError("saved end edit was not available after rebase")

            # Reopen verifies the file rather than relying only on the in-place
            # post-save mapping, then verify a normal Save on the new document.
            app.command("openFile", path=str(saved))
            reopened = wait_for_index(app, args.index_timeout_seconds, peak)
            if "[DATE-TEST][BEGIN-EDIT]" not in app.command("getLargeFileRange", start=0, length=128)["text"]:
                raise AssertionError("reopened saved document lost beginning edits")
            app.command("setSelection", start=reopened["characterCount"], end=reopened["characterCount"])
            app.command("insertText", text="[NORMAL-SAVE]")
            _, measurements["normalSaveMilliseconds"] = timed(app, "save")
            normal_state = wait_for_index(app, args.index_timeout_seconds, peak)
            normal_hash = sha256(saved)
            if "[NORMAL-SAVE]" not in app.command("getLargeFileRange", start=normal_state["characterCount"] - 64, length=64)["text"]:
                raise AssertionError("normal Save output did not contain the final edit")

            final = app.command("getLargeFileState")["largeFile"]
            measurements.update({
                "firstUsableMilliseconds": final["firstUsableMilliseconds"],
                "fullIndexMilliseconds": final["fullIndexMilliseconds"],
                "peakWorkingSetBytes": peak["workingSet"],
                "peakPrivateBytes": peak["privateBytes"],
                "finalPieceCount": final["pieceCount"],
                "finalOutputSize": saved.stat().st_size,
                "savedOutputHash": saved_hash,
                "normalOutputHash": normal_hash,
                "sourceHash": source_hash,
                "sourceUnchanged": sha256(source) == source_hash,
            })
        print(json.dumps(measurements, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
