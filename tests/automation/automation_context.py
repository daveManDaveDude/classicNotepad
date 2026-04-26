import os
from pathlib import Path


def automation_binary() -> str:
    value = os.environ.get("CLASSIC_NOTEPAD_AUTOMATION_BINARY")
    if not value:
        raise RuntimeError("CLASSIC_NOTEPAD_AUTOMATION_BINARY is not set.")
    return str(Path(value))


def automation_platform() -> str:
    return os.environ.get("CLASSIC_NOTEPAD_AUTOMATION_PLATFORM", "windows")


def automation_temp_root() -> str:
    value = os.environ.get("CLASSIC_NOTEPAD_AUTOMATION_TEMP_ROOT")
    if value:
        root = Path(value)
    else:
        root = Path(__file__).resolve().parents[2] / "build" / "automation-tests"

    root.mkdir(parents=True, exist_ok=True)
    return str(root)
