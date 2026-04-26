import json
import os
import queue
import subprocess
import threading
import time
from pathlib import Path


class AutomationError(AssertionError):
    pass


class ClassicNotepadDriver:
    def __init__(self, binary: str, platform: str, *extra_args: str, timeout: float = 5.0):
        self.binary = str(Path(binary))
        self.platform = platform
        self.timeout = timeout
        self.step_delay = _automation_step_delay()
        self._next_id = 1
        self._stdout = queue.Queue()
        self._stderr = []
        process_args = [self.binary, "--automation"]
        if os.environ.get("CLASSIC_NOTEPAD_AUTOMATION_VISIBLE") == "1":
            process_args.append("--automation-visible")
        process_args.extend(extra_args)

        self._process = subprocess.Popen(
            process_args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        self._stdout_thread = threading.Thread(
            target=self._read_stdout,
            name="classic-notepad-automation-stdout",
            daemon=True,
        )
        self._stderr_thread = threading.Thread(
            target=self._read_stderr,
            name="classic-notepad-automation-stderr",
            daemon=True,
        )
        self._stdout_thread.start()
        self._stderr_thread.start()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    def command(self, command: str, **payload):
        request_id = self._next_id
        self._next_id += 1
        request = {"id": request_id, "command": command, **payload}

        if self._process.stdin is None:
            raise AutomationError("Automation stdin is closed.")

        try:
            self._process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
            self._process.stdin.flush()
        except BrokenPipeError as error:
            raise AutomationError(f"Automation process exited early: {self.stderr_text}") from error

        response = self._read_response(request_id)
        self._delay_after_command(command)
        if not response.get("ok", False):
            raise AutomationError(response.get("error", "Automation command failed."))
        return response

    def try_command(self, command: str, **payload):
        request_id = self._next_id
        self._next_id += 1
        request = {"id": request_id, "command": command, **payload}

        if self._process.stdin is None:
            raise AutomationError("Automation stdin is closed.")

        self._process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self._process.stdin.flush()
        response = self._read_response(request_id)
        self._delay_after_command(command)
        return response

    def close(self):
        if self._process.poll() is None:
            try:
                self.try_command("close")
            except Exception:
                pass

        if self._process.stdin is not None:
            try:
                self._process.stdin.close()
            except OSError:
                pass

        try:
            self._process.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait(timeout=self.timeout)

    @property
    def stderr_text(self) -> str:
        return "".join(self._stderr)

    def _read_stdout(self):
        assert self._process.stdout is not None
        for line in self._process.stdout:
            self._stdout.put(line)
        self._stdout.put(None)

    def _read_stderr(self):
        assert self._process.stderr is not None
        for line in self._process.stderr:
            self._stderr.append(line)

    def _read_response(self, request_id: int):
        while True:
            try:
                line = self._stdout.get(timeout=self.timeout)
            except queue.Empty as error:
                raise AutomationError(
                    f"Timed out waiting for response {request_id}. stderr:\n{self.stderr_text}"
                ) from error

            if line is None:
                raise AutomationError(
                    f"Automation process exited before response {request_id}. stderr:\n{self.stderr_text}"
                )

            try:
                response = json.loads(line)
            except json.JSONDecodeError as error:
                raise AutomationError(f"Invalid automation JSON response: {line!r}") from error

            if response.get("id") == request_id:
                return response

    def _delay_after_command(self, command: str):
        if self.step_delay > 0.0 and command != "close":
            time.sleep(self.step_delay)


def _automation_step_delay() -> float:
    value = os.environ.get("CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY", "0")
    try:
        return max(0.0, float(value))
    except ValueError:
        return 0.0
