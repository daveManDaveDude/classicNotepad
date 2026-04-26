import argparse
import os
import sys
import unittest
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Classic Notepad semantic automation tests.")
    parser.add_argument("--binary", required=True, help="Path to ClassicNotepad.exe or ClassicNotepadGtk.")
    parser.add_argument("--platform", required=True, choices=("windows", "linux"))
    parser.add_argument(
        "--pattern",
        default="test_*.py",
        help="unittest discovery pattern relative to tests/automation.",
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

    binary = Path(args.binary).resolve()
    if not binary.exists():
        parser.error(f"binary does not exist: {binary}")

    test_dir = Path(__file__).resolve().parent
    repo_root = test_dir.parents[1]
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
    suite = unittest.defaultTestLoader.discover(str(test_dir), pattern=args.pattern)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
