#!/usr/bin/env python3
"""Opt-in Windows measurements for the ordinary native EDIT control.

The large paste cases use the production WM_PASTE handler with a file-backed,
controlled Unicode provider. No system or user clipboard is opened or changed.
"""

import argparse
import collections
import ctypes
import hashlib
import json
import os
import platform
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime, timezone
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
AUTOMATION_DIR = REPO_ROOT / "tests" / "automation"
sys.path.insert(0, str(AUTOMATION_DIR))
from classic_notepad_driver import ClassicNotepadDriver  # noqa: E402


SIZES = (270 * 1024, 1 * 1024 * 1024, 4 * 1024 * 1024, 7 * 1024 * 1024, 16 * 1024 * 1024)
LONG_ROW_UNITS = (1_000, 4_000, 8_000, 16_000, 32_767, 32_768, 65_535, 65_536, 65_537,
                  131_072, 262_144, 300_000, 1_048_576)
SHAPES = ("one-row", "four-row", "many-row")
SB_BOTTOM = 7


def utf16_units(text):
    return len(text.encode("utf-16-le")) // 2


def normalize_paste_text(text):
    return re.sub(r"\r\n|\r|\n", "\r\n", text)


def write_utf16(path, text):
    path.write_bytes(text.encode("utf-16-le"))


def sha256_utf16(text):
    return hashlib.sha256(text.encode("utf-16-le")).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git_text(*args):
    return subprocess.check_output(["git", *args], cwd=REPO_ROOT, text=True).strip()


def source_identity(binary):
    source_paths = (
        "src/platform/windows/app.cpp",
        "src/platform/windows/app.h",
        "src/platform/windows/automation.cpp",
        "tests/windows/measure_native_editor_limits.py",
    )
    status = git_text("status", "--porcelain")
    return {
        "baseRevision": git_text("rev-parse", "HEAD"),
        "workingTreeDirty": bool(status),
        "binarySha256": sha256_file(binary),
        "sourceFileSha256": {path: sha256_file(REPO_ROOT / path) for path in source_paths},
    }


class MemorySampler:
    """Sample whole-process and named-phase memory every 10 ms."""

    class CountersEx(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_ulong),
            ("PageFaultCount", ctypes.c_ulong),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
            ("PrivateUsage", ctypes.c_size_t),
        ]

    def __init__(self, process, private_cap_bytes):
        self.process = process
        self.private_cap_bytes = private_cap_bytes
        self.whole_process_peak_private_bytes = 0
        self.whole_process_peak_working_set_bytes = 0
        self.cap_hit = threading.Event()
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.active_phase = None
        self.phase_results = {}
        self.query = None
        self.thread = threading.Thread(target=self._sample, daemon=True)

    def start(self):
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        self.query = psapi.GetProcessMemoryInfo
        self.query.argtypes = [ctypes.c_void_p, ctypes.POINTER(self.CountersEx), ctypes.c_ulong]
        self.query.restype = ctypes.c_int
        self.psapi = psapi
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.thread.join(timeout=2.0)

    def _read_counters(self):
        process_handle = ctypes.c_void_p(int(self.process._handle))
        counters = self.CountersEx()
        counters.cb = ctypes.sizeof(counters)
        if not self.query(process_handle, ctypes.byref(counters), counters.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        return int(counters.PrivateUsage), int(counters.WorkingSetSize)

    def begin_phase(self, name):
        baseline_private, baseline_working_set = self._read_counters()
        with self.lock:
            if self.active_phase is not None:
                raise RuntimeError(f"memory phase {self.active_phase['name']} is still active")
            phase = {
                "name": name,
                "baselinePrivateBytes": baseline_private,
                "baselineWorkingSetBytes": baseline_working_set,
                "sampledPeakPrivateBytes": baseline_private,
                "sampledPeakWorkingSetBytes": baseline_working_set,
                "samples": 0,
            }
            self.active_phase = phase
        return phase

    def end_phase(self, name):
        ending_private, ending_working_set = self._read_counters()
        with self.lock:
            if self.active_phase is None or self.active_phase["name"] != name:
                raise RuntimeError(f"memory phase {name} is not active")
            phase = self.active_phase
            phase["endPrivateBytes"] = ending_private
            phase["endWorkingSetBytes"] = ending_working_set
            phase["peakPrivateBytes"] = max(
                phase["sampledPeakPrivateBytes"], phase["baselinePrivateBytes"], ending_private)
            phase["peakWorkingSetBytes"] = max(
                phase["sampledPeakWorkingSetBytes"], phase["baselineWorkingSetBytes"], ending_working_set)
            self.active_phase = None
            self.phase_results[name] = dict(phase)
            return dict(phase)

    def _sample(self):
        while not self.stop_event.is_set() and self.process.poll() is None:
            try:
                private_bytes, working_set_bytes = self._read_counters()
            except OSError:
                return
            with self.lock:
                self.whole_process_peak_private_bytes = max(
                    self.whole_process_peak_private_bytes, private_bytes)
                self.whole_process_peak_working_set_bytes = max(
                    self.whole_process_peak_working_set_bytes, working_set_bytes)
                if self.active_phase is not None:
                    self.active_phase["sampledPeakPrivateBytes"] = max(
                        self.active_phase["sampledPeakPrivateBytes"], private_bytes)
                    self.active_phase["sampledPeakWorkingSetBytes"] = max(
                        self.active_phase["sampledPeakWorkingSetBytes"], working_set_bytes)
                    self.active_phase["samples"] += 1
                cap_hit = self.whole_process_peak_private_bytes >= self.private_cap_bytes
            if cap_hit:
                self.cap_hit.set()
                try:
                    self.process.kill()
                except OSError:
                    pass
                return
            self.stop_event.wait(0.01)


def with_watchdog(function, seconds, process, sampler=None):
    result_queue = queue.Queue(maxsize=1)

    def invoke():
        try:
            result_queue.put((True, function()))
        except BaseException as error:  # surfaced in the caller thread
            result_queue.put((False, error))

    worker = threading.Thread(target=invoke, daemon=True)
    worker.start()
    worker.join(seconds)
    if worker.is_alive():
        try:
            process.kill()
        except OSError:
            pass
        raise TimeoutError(f"operation exceeded the {seconds:g} s watchdog")
    ok, value = result_queue.get_nowait()
    if not ok:
        raise value
    if sampler is not None and sampler.cap_hit.is_set():
        raise MemoryError(f"process private bytes reached the {sampler.private_cap_bytes} byte stop cap")
    return value


def command(app, name, watchdog, sampler=None, **payload):
    return with_watchdog(lambda: app.command(name, **payload), watchdog, app._process, sampler)


def make_shape(units, shape):
    rows = 1 if shape == "one-row" else (4 if shape == "four-row" else 1024)
    separators = rows - 1
    total = units - separators * 2  # CRLF is already in native EDIT form.
    if total < rows:
        raise ValueError(f"size {units} is too small for {shape}")
    base, extra = divmod(total, rows)
    parts = [("W" * (base + (1 if row < extra else 0))) for row in range(rows)]
    return "\r\n".join(parts)


def make_unicode_payload(units, character, line_ending):
    if line_ending == "mixed":
        delimiters = ("\r\n", "\n", "\r")
    else:
        delimiter = {"CRLF": "\r\n", "LF": "\n", "CR": "\r"}[line_ending]
        delimiters = (delimiter, delimiter, delimiter)
    separator_units = sum(utf16_units(part) for part in delimiters)
    data_units = units - separator_units
    character_units = utf16_units(character)
    if data_units % character_units:
        data_units -= data_units % character_units
    row_units = data_units // 4
    row_units += 1
    row = character * (row_units // character_units)
    # Keep the source payload at the requested code-unit count, including EOLs.
    rows = [row, row, row, row]
    while utf16_units("".join(rows[index] + delimiters[index] for index in range(3)) + rows[3]) > units:
        row = row[:-1]
        rows = [row, row, row, row]
    while utf16_units("".join(rows[index] + delimiters[index] for index in range(3)) + rows[3]) < units:
        rows[-1] += character
    return "".join(rows[index] + delimiters[index] for index in range(3)) + rows[3]


def windows_metadata(binary):
    version = platform.version()
    build = version.split(".")[-1] if version else "unknown"
    return {
        "capturedUtc": datetime.now(timezone.utc).isoformat(),
        "os": platform.platform(),
        "windowsVersion": platform.version(),
        "windowsBuild": build,
        "architecture": platform.machine(),
        "python": platform.python_version(),
        "binary": str(binary.resolve()),
        "binaryBytes": binary.stat().st_size,
    }


class Harness:
    def __init__(self, args, output_dir):
        self.args = args
        self.output_dir = output_dir
        self.fixture_dir = output_dir / "fixtures"
        self.fixture_dir.mkdir(parents=True, exist_ok=True)
        self.results_path = output_dir / "measurements.jsonl"
        self.started = time.monotonic()
        self.results = []
        self.stop_reason = None
        self.binary = Path(args.binary).resolve()
        self.metadata = windows_metadata(self.binary)
        self.metadata["sourceIdentity"] = source_identity(self.binary)

    def total_budget_check(self):
        if time.monotonic() - self.started > self.args.max_minutes * 60:
            raise TimeoutError(f"total run exceeded the {self.args.max_minutes:g} minute budget")

    def make_app(self):
        os.environ["CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY"] = "0"
        app = ClassicNotepadDriver(str(self.binary), "windows", timeout=self.args.watchdog_seconds)
        sampler = MemorySampler(app._process, int(self.args.private_cap_gib * 1024**3))
        sampler.start()
        try:
            command(app, "setAppearanceTheme", self.args.watchdog_seconds, sampler, theme=self.args.theme)
            command(app, "setWordWrap", self.args.watchdog_seconds, sampler, enabled=False)
            command(app, "setFont", self.args.watchdog_seconds, sampler, font=self.args.font)
            command(app, "resizeEditorViewport", self.args.watchdog_seconds, sampler,
                    width=self.args.viewport_width, height=self.args.viewport_height)
            app._harness_sampler = sampler
            return app, sampler
        except BaseException:
            sampler.stop()
            app.close()
            raise

    def record(self, result):
        result = {**self.metadata, **result}
        self.results.append(result)
        with self.results_path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(result, ensure_ascii=False, sort_keys=True) + "\n")
        print(json.dumps({k: result[k] for k in ("case", "repeat", "outcome", "actualPostPasteUnits",
                                                  "fullIndexTailClickStatus", "wholeProcessPeakPrivateBytes")
                          if k in result}, ensure_ascii=False))

    def save_text_and_hash(self, app, sampler, case_name, phase_prefix):
        exported = self.fixture_dir / f"actual-{case_name}.utf16le"
        export_phase = f"{phase_prefix}:export"
        sampler.begin_phase(export_phase)
        started = time.monotonic()
        response = command(app, "writeCurrentTextUtf16File", self.args.watchdog_seconds, sampler,
                           path=str(exported))
        export_command_wall_ms = (time.monotonic() - started) * 1000.0
        export_phase_memory = sampler.end_phase(export_phase)
        hash_phase = f"{phase_prefix}:hash"
        sampler.begin_phase(hash_phase)
        hash_started = time.monotonic()
        digest = sha256_file(exported)
        hash_ms = (time.monotonic() - hash_started) * 1000.0
        hash_phase_memory = sampler.end_phase(hash_phase)
        return {
            "response": response,
            "sha256": digest,
            "exportCommandWallMs": export_command_wall_ms,
            "exportOperationMs": response["exportOperationMicroseconds"] / 1000.0,
            "hashMs": hash_ms,
            "exportAndHashWallMs": export_command_wall_ms + hash_ms,
            "phaseMemory": {"export": export_phase_memory, "hash": hash_phase_memory},
        }

    def wait_width(self, app, sampler, initial_state, phase_name):
        sampler.begin_phase(phase_name)
        started = time.monotonic()
        state = initial_state
        polls = 0
        deadline = started + self.args.watchdog_seconds
        while state["measurementPending"]:
            self.total_budget_check()
            if time.monotonic() >= deadline:
                raise TimeoutError("native width indexing did not settle before its watchdog")
            state = command(app, "getEditorWidthState", self.args.watchdog_seconds, sampler)["editorWidth"]
            polls += 1
            if state["measurementPending"]:
                time.sleep(0.02)
        phase_memory = sampler.end_phase(phase_name)
        return state, (time.monotonic() - started) * 1000.0, polls, phase_memory

    def run_paste_sample(self, case_name, text, repeat, insertion, replace_units):
        self.total_budget_check()
        payload_path = self.fixture_dir / f"payload-{case_name}.utf16le"
        base_path = self.fixture_dir / f"base-{case_name}.utf16le"
        fixture_started = time.monotonic()
        write_utf16(payload_path, text)
        baseline = "A" * 140_002
        write_utf16(base_path, baseline)
        fixture_preparation_ms = (time.monotonic() - fixture_started) * 1000.0
        expected_baseline_hash = sha256_utf16(baseline)
        normalized = normalize_paste_text(text)
        expected = baseline[:insertion] + normalized + baseline[insertion + replace_units:]
        expected_hash = sha256_utf16(expected)
        expected_units = utf16_units(expected)
        expected_undo_units = utf16_units(baseline)
        expected_selection = {"start": insertion + utf16_units(normalized),
                              "end": insertion + utf16_units(normalized)}
        case_key = f"{case_name}-r{repeat}-p{insertion}-x{replace_units}"
        app, sampler = self.make_app()
        try:
            setup_response = command(app, "setTextFromUtf16File", self.args.watchdog_seconds, sampler,
                                     path=str(base_path))
            command(app, "setSelection", self.args.watchdog_seconds, sampler,
                    start=insertion, end=insertion + replace_units)
            paste_phase = f"{case_key}:paste"
            sampler.begin_phase(paste_phase)
            started = time.monotonic()
            paste_response = command(app, "pasteThroughProductionHandlerFromUtf16File",
                                     self.args.watchdog_seconds, sampler, path=str(payload_path))
            paste_command_wall_ms = (time.monotonic() - started) * 1000.0
            paste_phase_memory = sampler.end_phase(paste_phase)
            if paste_response["outcome"] != "success":
                raise RuntimeError(f"production paste outcome was {paste_response['outcome']}")
            if paste_response["nativeNotification"] != 0:
                raise RuntimeError(f"native EDIT reported notification {paste_response['nativeNotification']}")
            actual_selection = paste_response["selection"]
            if actual_selection != expected_selection:
                raise RuntimeError(
                    f"post-paste selection was {actual_selection}, expected collapsed {expected_selection}")

            state, poll_ms, polls, width_phase_memory = self.wait_width(
                app, sampler, paste_response["editorWidth"], f"{case_key}:widthSettlement")

            output = self.save_text_and_hash(app, sampler, case_key, f"{case_key}:postPaste")
            output_response = output["response"]
            actual_units = output_response["storedUnits"]
            actual_hash = output["sha256"]
            if actual_units != expected_units or actual_hash != expected_hash:
                raise RuntimeError("post-paste UTF-16LE integrity check failed")

            sampler.begin_phase(f"{case_key}:undo")
            undo_started = time.monotonic()
            command(app, "undo", self.args.watchdog_seconds, sampler)
            undo_ms = (time.monotonic() - undo_started) * 1000.0
            undo_phase_memory = sampler.end_phase(f"{case_key}:undo")
            undone = self.save_text_and_hash(app, sampler, case_key + "-undo", f"{case_key}:undoResult")
            undone_response = undone["response"]
            actual_undo_units = undone_response["storedUnits"]
            actual_undo_hash = undone["sha256"]
            if actual_undo_units != expected_undo_units or actual_undo_hash != expected_baseline_hash:
                raise RuntimeError("one Undo did not restore the exact baseline text")

            self.record({
                "case": case_key,
                "kind": "large-paste",
                "repeat": repeat,
                "payloadShape": case_name,
                "payloadSourceUnits": utf16_units(text),
                "payloadNormalizedUnits": utf16_units(normalized),
                "logicalRowsExpected": normalized.count("\r\n") + 1,
                "insertionUtf16Unit": insertion,
                "replacementUtf16Units": replace_units,
                "outcome": paste_response["outcome"],
                "textHashSha256Utf16Le": actual_hash,
                "undoHashSha256Utf16Le": actual_undo_hash,
                "expectedPostPasteUnits": expected_units,
                "actualPostPasteUnits": actual_units,
                "expectedPostPasteSha256Utf16Le": expected_hash,
                "actualPostPasteSha256Utf16Le": actual_hash,
                "expectedUndoUnits": expected_undo_units,
                "actualUndoUnits": actual_undo_units,
                "expectedUndoSha256Utf16Le": expected_baseline_hash,
                "actualUndoSha256Utf16Le": actual_undo_hash,
                "expectedPostPasteSelection": expected_selection,
                "actualPostPasteSelection": actual_selection,
                "exactContent": True,
                "oneUndoExact": True,
                "fixturePreparationMs": fixture_preparation_ms,
                "providerFileReadMs": paste_response["sourceFileReadMicroseconds"] / 1000.0,
                "providerSetupMs": paste_response["providerSetupMicroseconds"] / 1000.0,
                "productionDispatchMs": paste_response["productionDispatchMicroseconds"] / 1000.0,
                "providerCleanupMs": paste_response["providerCleanupMicroseconds"] / 1000.0,
                "pasteHelperWallMs": paste_response["pasteHelperWallMicroseconds"] / 1000.0,
                "pasteCommandWallMs": paste_command_wall_ms,
                "initialWidthStateReadMs": paste_response["initialWidthStateReadMicroseconds"] / 1000.0,
                "widthSettlementAfterInitialStateMs": poll_ms,
                "widthSettlementPolls": polls,
                "jsonAndHelperResidualMs": max(0.0, paste_command_wall_ms -
                    (paste_response["sourceFileReadMicroseconds"] +
                     paste_response["pasteHelperWallMicroseconds"] +
                     paste_response["initialWidthStateReadMicroseconds"]) / 1000.0),
                "postPasteExportCommandWallMs": output["exportCommandWallMs"],
                "postPasteExportOperationMs": output["exportOperationMs"],
                "postPasteHashMs": output["hashMs"],
                "postPasteExportAndHashWallMs": output["exportAndHashWallMs"],
                "undoMs": undo_ms,
                "undoExportCommandWallMs": undone["exportCommandWallMs"],
                "undoExportOperationMs": undone["exportOperationMs"],
                "undoHashMs": undone["hashMs"],
                "undoExportAndHashWallMs": undone["exportAndHashWallMs"],
                "setTextFileReadMs": setup_response["sourceFileReadMicroseconds"] / 1000.0,
                "setTextCallMs": setup_response["setTextMicroseconds"] / 1000.0,
                "textLimit": state["editTextLimit"],
                "logicalRows": normalized.count("\r\n") + 1,
                "editDisplayRows": state["editDisplayLineCount"],
                "maximumLineLength": state["maximumEditDisplayLineLength"],
                "nativeScrollInfo": {"min": state["nativeHorizontalScrollMinimum"],
                                     "max": state["nativeHorizontalScrollMaximum"],
                                     "page": state["nativeHorizontalScrollPage"],
                                     "position": state["nativeHorizontalOffset"],
                                     "effectiveMaxOffset": state["nativeHorizontalMaximumOffset"]},
                "documentWidthEstimatePixels": state["documentWidth"],
                "wholeProcessPeakPrivateBytes": sampler.whole_process_peak_private_bytes,
                "wholeProcessPeakWorkingSetBytes": sampler.whole_process_peak_working_set_bytes,
                "memorySamplingIntervalMs": 10,
                "phaseMemory": {
                    "pasteCommand": paste_phase_memory,
                    "widthSettlement": width_phase_memory,
                    "postPasteExport": output["phaseMemory"]["export"],
                    "postPasteHash": output["phaseMemory"]["hash"],
                    "undo": undo_phase_memory,
                    "undoExport": undone["phaseMemory"]["export"],
                    "undoHash": undone["phaseMemory"]["hash"],
                },
                "config": self.config_from_state(state, app),
            })
        finally:
            sampler.stop()
            app.close()

    def config_from_state(self, state, app):
        return {
            "theme": self.args.theme,
            "font": self.args.font,
            "dpi": state.get("windowDpi", 0),
            "viewport": [state.get("viewportWidth", 0), state.get("viewportHeight", self.args.viewport_height)],
            "wrap": False,
            "wordWrapState": app.command("getWordWrap")["enabled"],
            "statusBarVisible": app.command("getStatusBarVisible")["visible"],
        }

    @staticmethod
    def scroll_route_record(state, visible):
        return {
            "scrollInfo": {"min": state["nativeHorizontalScrollMinimum"],
                           "max": state["nativeHorizontalScrollMaximum"],
                           "page": state["nativeHorizontalScrollPage"],
                           "position": state["nativeHorizontalOffset"],
                           "effectiveMaxOffset": state["nativeHorizontalMaximumOffset"]},
            "firstVisibleDisplayLine": state["verticalFirstVisibleLine"],
            "endCharacterIndex": state["charPositionCharacterIndex"],
            "endCaretDisplayLine": state["caretDisplayLine"],
            "endCharacterPosition": [state["charPositionX"], state["charPositionY"]],
            "endCharacterPositionAvailable": state["charPositionAvailable"],
            "lastCharacterCoordinateInViewport": visible,
            "emCharFromPosAvailable": state["hitTestAvailable"],
            "emCharFromPosRawLowWord": state["hitTestCharacterLowWord"] if state["hitTestAvailable"] else None,
            "emCharFromPosRawHighWord": state["hitTestLineHighWord"] if state["hitTestAvailable"] else None,
            "emCharFromPosCharacterWordWidthBits": 16,
            "fullIndexHitTestStatus": "inconclusive_raw_loword_only",
        }

    def run_long_row_sample(self, units, repeat):
        self.total_budget_check()
        text = "W" * units
        case_name = f"long-row-{units}-r{repeat}"
        path = self.fixture_dir / f"{case_name}.utf16le"
        write_utf16(path, text)
        app, sampler = self.make_app()
        try:
            set_started = time.monotonic()
            setup = command(app, "setTextFromUtf16File", self.args.watchdog_seconds, sampler,
                            path=str(path))
            set_elapsed = (time.monotonic() - set_started) * 1000.0
            first_state_response = command(app, "getEditorWidthState", self.args.watchdog_seconds, sampler)
            state, settle_ms, polls, width_phase_memory = self.wait_width(
                app, sampler, first_state_response["editorWidth"], f"{case_name}:widthSettlement")
            output = self.save_text_and_hash(app, sampler, case_name, f"{case_name}:integrity")
            output_response = output["response"]
            actual_hash = output["sha256"]
            expected_hash = sha256_utf16(text)
            if output_response["storedUnits"] != units or actual_hash != expected_hash:
                raise RuntimeError("long-row UTF-16LE integrity check failed")

            focus = command(app, "focusEditor", self.args.watchdog_seconds, sampler)
            if not focus["focused"]:
                raise RuntimeError("automation window could not focus the native EDIT control")
            command(app, "setSelection", self.args.watchdog_seconds, sampler, start=units, end=units)
            command(app, "scrollEditorByUnits", self.args.watchdog_seconds, sampler,
                    horizontalUnits=1_000_000, verticalLines=0)
            line_scroll_state = command(app, "getEditorWidthState", self.args.watchdog_seconds,
                                        sampler)["editorWidth"]
            command(app, "sendEditorHorizontalScroll", self.args.watchdog_seconds, sampler,
                    scrollCommand=4, trackPosition=min(state["nativeHorizontalMaximumOffset"], 65_535))
            horizontal_state = command(app, "getEditorWidthState", self.args.watchdog_seconds, sampler)["editorWidth"]
            selection = command(app, "getSelection", self.args.watchdog_seconds, sampler)["selection"]
            if selection != {"start": units, "end": units}:
                raise RuntimeError(f"native EDIT did not retain the end-caret position: {selection}")
            command(app, "sendEditorVerticalScroll", self.args.watchdog_seconds, sampler,
                    scrollCommand=SB_BOTTOM, trackPosition=0)
            vertical_state = command(app, "getEditorWidthState", self.args.watchdog_seconds, sampler)["editorWidth"]
            def route_visible(route_state):
                return bool(
                    route_state["charPositionAvailable"] and
                    0 <= route_state["charPositionX"] < route_state["viewportWidth"] and
                    0 <= route_state["charPositionY"] < route_state["viewportHeight"] and
                    route_state["caretDisplayLine"] >= route_state["verticalFirstVisibleLine"] and
                    route_state["caretDisplayLine"] <
                    route_state["verticalFirstVisibleLine"] + route_state["verticalPage"])

            line_scroll_visible = route_visible(line_scroll_state)
            thumb_scroll_visible = route_visible(horizontal_state)
            horizontal_caret_visible = line_scroll_visible or thumb_scroll_visible
            vertical_caret_visible = route_visible(vertical_state)
            click_probe = {
                "action": "native WM_LBUTTONDOWN/WM_LBUTTONUP at EM_POSFROMCHAR(last character) x+1",
                "point": None,
                "clickDelivered": False,
                "expectedLastCharacterIndex": max(0, units - 1),
                "actualSelection": None,
                "selectionIsCollapsed": False,
                "fullWidthCaretIndex": None,
                "status": "inconclusive_coordinate_not_in_viewport",
            }
            if vertical_state["charPositionAvailable"] and \
                    0 <= vertical_state["charPositionX"] + 1 < vertical_state["viewportWidth"] and \
                    0 <= vertical_state["charPositionY"] < vertical_state["viewportHeight"]:
                click_x = vertical_state["charPositionX"] + 1
                click_y = vertical_state["charPositionY"]
                click_response = command(app, "clickEditorClientPoint", self.args.watchdog_seconds, sampler,
                                         x=click_x, y=click_y)
                click_selection = click_response["selection"]
                collapsed = click_selection["start"] == click_selection["end"]
                click_at_tail = collapsed and units - 1 <= click_selection["start"] <= units
                click_probe = {
                    **click_probe,
                    "point": [click_x, click_y],
                    "clickDelivered": True,
                    "actualSelection": click_selection,
                    "selectionIsCollapsed": collapsed,
                    "fullWidthCaretIndex": click_selection["start"] if collapsed else None,
                    "status": "full_width_caret_at_tail_character"
                        if click_at_tail else "inconclusive_click_selection_not_at_tail",
                }
            unit_advance = max(vertical_state["fontMaximumAdvance"], vertical_state["horizontalScrollUnit"] * 8)
            c1_estimate = min(2_147_483_647, units * unit_advance)
            self.record({
                "case": case_name,
                "kind": "long-row",
                "repeat": repeat,
                "payloadSourceUnits": units,
                "payloadNormalizedUnits": units,
                "logicalRowsExpected": 1,
                "storedUnits": output_response["storedUnits"],
                "textHashSha256Utf16Le": actual_hash,
                "exactContent": True,
                "setTextFileReadMs": setup["sourceFileReadMicroseconds"] / 1000.0,
                "setTextCallMs": setup["setTextMicroseconds"] / 1000.0,
                "setTextCommandWallMs": set_elapsed,
                "widthSettlementMs": settle_ms,
                "widthSettlementPolls": polls,
                "exactTextExportCommandWallMs": output["exportCommandWallMs"],
                "exactTextExportOperationMs": output["exportOperationMs"],
                "exactTextHashMs": output["hashMs"],
                "exactTextExportAndHashWallMs": output["exportAndHashWallMs"],
                "editTextLimit": vertical_state["editTextLimit"],
                "logicalRows": 1,
                "editDisplayRows": vertical_state["editDisplayLineCount"],
                "maximumLineLength": vertical_state["maximumEditDisplayLineLength"],
                "documentWidthEstimatePixels": vertical_state["documentWidth"],
                "c1ConservativeWidthEstimatePixels": c1_estimate,
                "c1ConservativeAdvancePerUnitPixels": unit_advance,
                "horizontalRoute": {
                    "emLineScroll": self.scroll_route_record(line_scroll_state, line_scroll_visible),
                    "sbThumbPositionAtNativeMax": self.scroll_route_record(
                        horizontal_state, thumb_scroll_visible),
                },
                "verticalRoute": {
                    "action": "select end, then SB_BOTTOM",
                    **self.scroll_route_record(vertical_state, vertical_caret_visible),
                    "verticalPage": vertical_state["verticalPage"],
                },
                "endSelection": selection,
                "editorFocusedForNavigationProbe": focus["focused"],
                "fullIndexClickProbe": click_probe,
                "fullIndexTailClickStatus": click_probe["status"],
                "caretPositionAvailable": vertical_state["caretPositionAvailable"],
                "caretX": vertical_state["caretX"],
                "lastCharacterCoordinateVisibleAfterEndSelectionAndScrollRight": horizontal_caret_visible,
                "lastCharacterCoordinateVisibleAfterVerticalBottom": vertical_caret_visible,
                "wholeProcessPeakPrivateBytes": sampler.whole_process_peak_private_bytes,
                "wholeProcessPeakWorkingSetBytes": sampler.whole_process_peak_working_set_bytes,
                "memorySamplingIntervalMs": 10,
                "phaseMemory": {
                    "widthSettlement": width_phase_memory,
                    "integrityExport": output["phaseMemory"]["export"],
                    "integrityHash": output["phaseMemory"]["hash"],
                },
                "config": self.config_from_state(vertical_state, app),
            })
        finally:
            sampler.stop()
            app.close()

    def run(self):
        self.results_path.parent.mkdir(parents=True, exist_ok=True)
        if self.results_path.exists():
            self.results_path.unlink()
        self.run_long_row_ladder()
        if not self.args.long_row_only:
            self.run_paste_ladder()
            self.run_unicode_and_eol_cases()
        self.write_summary()

    def run_long_row_ladder(self):
        for units in self.args.long_row_lengths or LONG_ROW_UNITS:
            for repeat in range(1, self.args.repeats + 1):
                try:
                    self.run_long_row_sample(units, repeat)
                except BaseException as error:
                    self.stop_reason = f"long-row escalation stopped at {units} units, repeat {repeat}: {error}"
                    return

    def run_paste_ladder(self):
        if self.stop_reason:
            return
        for size in SIZES:
            level_failed = False
            for shape in SHAPES:
                payload = make_shape(size, shape)
                if utf16_units(payload) != size:
                    raise AssertionError(f"{shape} fixture missed its declared {size} UTF-16 units")
                for repeat in range(1, self.args.repeats + 1):
                    try:
                        self.run_paste_sample(
                            f"paste-{size}-{shape}", payload, repeat, insertion=70_001, replace_units=3)
                    except BaseException as error:
                        self.stop_reason = f"paste escalation stopped at {size} units ({shape}), repeat {repeat}: {error}"
                        level_failed = True
                        break
                if level_failed:
                    break
            if level_failed:
                return

    def run_unicode_and_eol_cases(self):
        if self.stop_reason:
            return
        cases = []
        for character_name, character in (("bmp", "漢"), ("emoji", "😀")):
            cases.append((f"unicode-{character_name}-270Ki", make_unicode_payload(270 * 1024, character, "CRLF")))
        for ending in ("CRLF", "LF", "CR", "mixed"):
            cases.append((f"eol-{ending.lower()}-270Ki", make_unicode_payload(270 * 1024, "E", ending)))
        # Exercise insertion at the beginning and end in addition to the >65K replacement
        # used by every large paste case.
        for insertion, replace_units, label in ((0, 0, "beginning"), (140_002, 0, "end")):
            cases.append((f"position-{label}-emoji", "🙂", insertion, replace_units))
        for item in cases:
            if len(item) == 2:
                case_name, payload = item
                position, replace_units = 70_001, 3
            else:
                case_name, payload, position, replace_units = item
            for repeat in range(1, self.args.repeats + 1):
                try:
                    self.run_paste_sample(case_name, payload, repeat, position, replace_units)
                except BaseException as error:
                    self.stop_reason = f"Unicode/EOL/position qualification stopped at {case_name}, repeat {repeat}: {error}"
                    return

    def write_summary(self):
        all_viewports = collections.Counter()
        viewport_by_kind = collections.defaultdict(collections.Counter)
        sample_kinds = collections.Counter()
        for result in self.results:
            kind = result.get("kind", "unknown")
            sample_kinds[kind] += 1
            viewport = result.get("config", {}).get("viewport", [0, 0])
            viewport_key = f"{viewport[0]}x{viewport[1]}"
            all_viewports[viewport_key] += 1
            viewport_by_kind[kind][viewport_key] += 1
        whole_process_peak_private = max(
            (result.get("wholeProcessPeakPrivateBytes", 0) for result in self.results), default=0)
        whole_process_peak_working_set = max(
            (result.get("wholeProcessPeakWorkingSetBytes", 0) for result in self.results), default=0)
        summary = {
            "metadata": self.metadata,
            "method": {
                "productionPastePath": "file-backed controlled Unicode provider -> production WM_PASTE handler",
                "clipboard": "system/user clipboard untouched; no Winsta0 or real clipboard acquisition",
                "storedText": "exported from native EDIT to UTF-16LE without BOM, then SHA-256 hashed",
                "timings": "fixture preparation, provider file read, provider setup, exact synchronous WM_PASTE dispatch, provider cleanup, width-state read/settlement, export operation, hash, and Undo recorded separately",
                "freshProcessPerSample": True,
                "repeatsRequested": self.args.repeats,
                "memorySampling": {
                    "api": "GetProcessMemoryInfo PrivateUsage and WorkingSetSize",
                    "intervalMs": 10,
                    "phaseBaselinesAndPeaks": "current-process snapshots at phase boundaries plus peaks observed by the 10 ms sampler",
                    "wholeProcessPeakPrivateBytes": whole_process_peak_private,
                    "wholeProcessPeakWorkingSetBytes": whole_process_peak_working_set,
                    "stopCapScope": "whole application-process lifetime for each fresh sample",
                },
                "stopCaps": {
                    "watchdogSeconds": self.args.watchdog_seconds,
                    "privateBytes": int(self.args.private_cap_gib * 1024**3),
                    "totalMinutes": self.args.max_minutes,
                },
            },
            "config": {
                "theme": self.args.theme,
                "font": self.args.font,
                "viewportRequested": [self.args.viewport_width, self.args.viewport_height],
                "wrap": False,
                "textLimit": "native default; measured per sample using EM_GETLIMITTEXT",
            },
            "samples": len(self.results),
            "sampleKinds": dict(sample_kinds),
            "actualViewportDistribution": {
                "allSamples": dict(all_viewports),
                "byKind": {kind: dict(counts) for kind, counts in viewport_by_kind.items()},
            },
            "stopReason": self.stop_reason,
            "measurementsJsonl": str(self.results_path.resolve()),
        }
        path = self.output_dir / "summary.json"
        path.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(json.dumps(summary, indent=2, ensure_ascii=False))


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default=os.environ.get("CLASSIC_NOTEPAD_AUTOMATION_BINARY"),
                        help="ClassicNotepad.exe built with the Windows automation harness")
    parser.add_argument("--output-dir", type=Path,
                        default=REPO_ROOT / "build" / "native-editor-limit-measurements")
    parser.add_argument("--watchdog-seconds", type=float, default=90.0)
    parser.add_argument("--private-cap-gib", type=float, default=1.0)
    parser.add_argument("--max-minutes", type=float, default=30.0)
    parser.add_argument("--theme", choices=("light", "dark"), default="light")
    parser.add_argument("--font", default="Consolas 11")
    parser.add_argument("--viewport-width", type=int, default=1000)
    parser.add_argument("--viewport-height", type=int, default=700)
    parser.add_argument("--long-row-only", action="store_true",
                        help="run only the long-row extent ladder")
    parser.add_argument("--long-row-lengths", type=int, nargs="+", default=None,
                        help="override long-row UTF-16 lengths for a focused run")
    parser.add_argument("--repeats", type=int, default=3,
                        help="fresh-process repeats per case (default: 3)")
    args = parser.parse_args()
    if not args.binary:
        parser.error("pass --binary or set CLASSIC_NOTEPAD_AUTOMATION_BINARY")
    if sys.platform != "win32":
        parser.error("this native EDIT harness can run only on Windows")
    if not Path(args.binary).is_file():
        parser.error(f"binary does not exist: {args.binary}")
    if args.watchdog_seconds <= 0 or args.private_cap_gib <= 0 or args.max_minutes <= 0 or args.repeats <= 0:
        parser.error("watchdog, private-byte cap, total budget, and repeat count must be positive")
    return args


def main():
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    try:
        harness = Harness(args, args.output_dir.resolve())
        harness.run()
    except BaseException as error:
        print(f"measurement harness stopped: {error}", file=sys.stderr)
        return 2
    return 0 if not harness.stop_reason else 1


if __name__ == "__main__":
    raise SystemExit(main())
