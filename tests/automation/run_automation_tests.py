import argparse
import hashlib
import json
import os
import sys
import unittest
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable


MANIFEST_NAME = "test_suite_manifest.json"
SUITE_NAMES = ("Ordinary", "Stress", "Failure", "Full")
OUTCOME_PRIORITY = {
    "running": 0,
    "passed": 1,
    "expectedFailure": 2,
    "skipped": 3,
    "unexpectedSuccess": 4,
    "failed": 5,
    "error": 6,
}


def _flatten(suite: unittest.TestSuite) -> Iterable[unittest.TestCase]:
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from _flatten(item)
        else:
            yield item


def _discover(test_dir: Path, pattern: str) -> dict[str, unittest.TestCase]:
    suite = unittest.defaultTestLoader.discover(str(test_dir), pattern=pattern)
    tests = list(_flatten(suite))
    by_id = {test.id(): test for test in tests}
    if len(by_id) != len(tests):
        raise ValueError("unittest discovery produced duplicate test IDs")
    return by_id


def _load_suite_manifest(path: Path, discovered_full: set[str]) -> dict[str, list[str]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read suite manifest {path}: {error}") from error

    if not isinstance(document, dict):
        raise ValueError("suite manifest root must be an object")
    if document.get("schemaVersion") != 1 or document.get("discoveryPattern") != "test_*.py":
        raise ValueError("suite manifest has an unsupported schema or discovery pattern")
    raw_suites = document.get("suites")
    if not isinstance(raw_suites, dict) or set(raw_suites) != set(SUITE_NAMES):
        raise ValueError("suite manifest must define Ordinary, Stress, Failure, and Full")

    suites: dict[str, list[str]] = {}
    for name in SUITE_NAMES:
        ids = raw_suites[name]
        if not isinstance(ids, list) or any(not isinstance(test_id, str) for test_id in ids):
            raise ValueError(f"suite {name} must be an array of test IDs")
        if len(set(ids)) != len(ids):
            raise ValueError(f"suite {name} contains duplicate test IDs")
        suites[name] = ids

    full_ids = suites["Full"]
    if set(full_ids) != discovered_full or len(full_ids) != len(discovered_full):
        missing = sorted(discovered_full - set(full_ids))
        extra = sorted(set(full_ids) - discovered_full)
        raise ValueError(f"Full manifest differs from current discovery; missing={missing}; extra={extra}")

    partition = suites["Ordinary"] + suites["Stress"] + suites["Failure"]
    if len(partition) != len(set(partition)):
        raise ValueError("Ordinary, Stress, and Failure suites must be disjoint")
    if set(partition) != discovered_full or len(partition) != len(discovered_full):
        missing = sorted(discovered_full - set(partition))
        extra = sorted(set(partition) - discovered_full)
        raise ValueError(f"suite partition differs from Full; missing={missing}; extra={extra}")
    return suites


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


class TrackingTextTestResult(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.outcomes: dict[str, dict[str, str]] = {}

    def startTest(self, test):
        super().startTest(test)
        self.outcomes.setdefault(test.id(), {"testId": test.id(), "status": "running"})

    def _record(self, test, status: str, detail: str = "") -> None:
        outcome = self.outcomes.setdefault(test.id(), {"testId": test.id()})
        current_status = outcome.get("status", "running")
        if OUTCOME_PRIORITY[status] >= OUTCOME_PRIORITY.get(current_status, -1):
            outcome["status"] = status
        if detail and outcome.get("status") == status:
            outcome["detail"] = detail

    def addSuccess(self, test):
        super().addSuccess(test)
        self._record(test, "passed")

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self._record(test, "failed", f"{err[0].__name__}: {err[1]}")

    def addError(self, test, err):
        super().addError(test, err)
        self._record(test, "error", f"{err[0].__name__}: {err[1]}")

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self._record(test, "skipped", str(reason))

    def addExpectedFailure(self, test, err):
        super().addExpectedFailure(test, err)
        self._record(test, "expectedFailure", f"{err[0].__name__}: {err[1]}")

    def addUnexpectedSuccess(self, test):
        super().addUnexpectedSuccess(test)
        self._record(test, "unexpectedSuccess")

    def addSubTest(self, test, subtest, err):
        super().addSubTest(test, subtest, err)
        if err is not None:
            failure_exception = getattr(test, "failureException", AssertionError)
            status = "failed" if issubclass(err[0], failure_exception) else "error"
            self._record(test, status, f"{err[0].__name__}: {err[1]}")


def _summarize_outcomes(selected_count: int, outcomes: list[dict[str, str]]) -> dict[str, int]:
    return {
        "selected": selected_count,
        "executed": len(outcomes),
        "passed": sum(item.get("status") == "passed" for item in outcomes),
        "failed": sum(item.get("status") == "failed" for item in outcomes),
        "errors": sum(item.get("status") == "error" for item in outcomes),
        "skipped": sum(item.get("status") == "skipped" for item in outcomes),
        "expectedFailures": sum(item.get("status") == "expectedFailure" for item in outcomes),
        "unexpectedSuccesses": sum(item.get("status") == "unexpectedSuccess" for item in outcomes),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Classic Notepad semantic automation tests.")
    parser.add_argument("--binary", help="Path to ClassicNotepad.exe, ClassicNotepadGtk, or ClassicNotepadMac.")
    parser.add_argument("--platform", required=True, choices=("windows", "linux", "macos"))
    parser.add_argument(
        "--pattern",
        default="test_*.py",
        help="unittest discovery pattern relative to tests/automation.",
    )
    parser.add_argument(
        "--suite",
        choices=SUITE_NAMES,
        default="Full",
        help="manifest-defined suite to run (direct callers default to Full).",
    )
    parser.add_argument(
        "--manifest",
        help="suite manifest path (defaults to tests/automation/test_suite_manifest.json).",
    )
    parser.add_argument(
        "--json-results",
        help="write structured outcomes and source/binary hashes to this JSON file.",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="validate and list selected test IDs without launching the app.",
    )
    parser.add_argument(
        "--visible",
        action="store_true",
        help="Show the app window while the automation suite runs.",
    )
    parser.add_argument(
        "--visible-delay",
        type=float,
        default=0.5,
        help="Seconds to pause after each automation command when --visible is used.",
    )
    args = parser.parse_args()

    test_dir = Path(__file__).resolve().parent
    repo_root = test_dir.parents[1]
    manifest_path = Path(args.manifest).resolve() if args.manifest else test_dir / MANIFEST_NAME
    try:
        discovered_full = _discover(test_dir, "test_*.py")
        manifest = _load_suite_manifest(manifest_path, set(discovered_full))
        discovered_requested = discovered_full if args.pattern == "test_*.py" else _discover(test_dir, args.pattern)
    except (ImportError, OSError, ValueError) as error:
        parser.error(str(error))

    selected_ids = [test_id for test_id in manifest[args.suite] if test_id in discovered_requested]
    if not selected_ids:
        parser.error(f"suite {args.suite} and pattern {args.pattern!r} selected no tests")
    if args.list:
        for test_id in selected_ids:
            print(test_id)
        print(f"suite={args.suite} selected={len(selected_ids)} full={len(discovered_full)}")
        return 0

    if not args.binary:
        parser.error("--binary is required unless --list is used")
    binary = Path(args.binary).resolve()
    if not binary.exists():
        parser.error(f"binary does not exist: {binary}")

    temp_root = repo_root / "build" / "automation-tests"
    temp_root.mkdir(parents=True, exist_ok=True)
    os.environ["CLASSIC_NOTEPAD_AUTOMATION_BINARY"] = str(binary)
    os.environ["CLASSIC_NOTEPAD_AUTOMATION_PLATFORM"] = args.platform
    os.environ["CLASSIC_NOTEPAD_AUTOMATION_TEMP_ROOT"] = str(temp_root)
    if args.visible:
        os.environ["CLASSIC_NOTEPAD_AUTOMATION_VISIBLE"] = "1"
        os.environ["CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY"] = str(max(0.0, args.visible_delay))
    else:
        os.environ.pop("CLASSIC_NOTEPAD_AUTOMATION_VISIBLE", None)
        os.environ.pop("CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY", None)

    sys.path.insert(0, str(test_dir))
    suite = unittest.TestSuite(discovered_requested[test_id] for test_id in selected_ids)
    manifest_bytes = manifest_path.read_bytes()
    started = datetime.now(timezone.utc)
    print(f"Running suite {args.suite}: {len(selected_ids)} tests (Full discovery: {len(discovered_full)}).")
    result = unittest.TextTestRunner(verbosity=2, resultclass=TrackingTextTestResult).run(suite)
    exit_code = 0 if result.wasSuccessful() else 1
    outcomes = list(result.outcomes.values())
    counts = _summarize_outcomes(len(selected_ids), outcomes)
    structured = {
        "schemaVersion": 1,
        "suite": args.suite,
        "pattern": args.pattern,
        "startedUtc": started.isoformat(),
        "completedUtc": datetime.now(timezone.utc).isoformat(),
        "exitCode": exit_code,
        "manifestPath": str(manifest_path),
        "manifestSha256": hashlib.sha256(manifest_bytes).hexdigest().upper(),
        "runnerPath": str(Path(__file__).resolve()),
        "runnerSha256": _sha256(Path(__file__).resolve()),
        "binaryPath": str(binary),
        "binarySha256": _sha256(binary),
        "selectedIds": selected_ids,
        "executedIds": list(result.outcomes),
        "outcomes": outcomes,
        "counts": counts,
    }
    if args.json_results:
        result_path = Path(args.json_results).resolve()
        structured["resultPath"] = str(result_path)
        try:
            result_path.parent.mkdir(parents=True, exist_ok=True)
            result_path.write_text(json.dumps(structured, indent=2) + "\n", encoding="utf-8")
            print(f"Structured result: {result_path}")
        except OSError as error:
            print(f"Could not write structured result {result_path}: {error}", file=sys.stderr)
            exit_code = 1
            structured["exitCode"] = exit_code
    print("AUTOMATION_RESULT_JSON=" + json.dumps(structured, separators=(",", ":")))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
