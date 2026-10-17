import json
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER_PATH = REPO_ROOT / "tests" / "automation" / "run_automation_tests.py"
MANIFEST_PATH = REPO_ROOT / "tests" / "automation" / "test_suite_manifest.json"
GATE_PATH = REPO_ROOT / "scripts" / "verify-windows-release.ps1"
POWERSHELL = shutil.which("powershell") or shutil.which("pwsh")


class WindowsReleaseVerificationWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.name != "nt" or not POWERSHELL:
            raise unittest.SkipTest("PowerShell process workflow tests require Windows.")

    def test_manifest_matches_current_discovery_and_exact_partition(self):
        sys.path.insert(0, str(RUNNER_PATH.parent))
        try:
            import run_automation_tests

            discovered = run_automation_tests._discover(RUNNER_PATH.parent, "test_*.py")
            manifest = run_automation_tests._load_suite_manifest(MANIFEST_PATH, set(discovered))
        finally:
            sys.path.remove(str(RUNNER_PATH.parent))

        self.assertEqual(len(discovered), 120)
        self.assertEqual({name: len(ids) for name, ids in manifest.items()}, {
            "Ordinary": 39,
            "Stress": 45,
            "Failure": 36,
            "Full": 120,
        })
        self.assertEqual(set(manifest["Full"]), set(discovered))
        self.assertEqual(manifest["Full"], list(discovered))
        partition = manifest["Ordinary"] + manifest["Stress"] + manifest["Failure"]
        self.assertEqual(len(partition), len(set(partition)))
        self.assertEqual(set(partition), set(discovered))

    def test_manifest_validation_rejects_missing_ids_and_overlapping_suites(self):
        sys.path.insert(0, str(RUNNER_PATH.parent))
        try:
            import run_automation_tests

            discovered = run_automation_tests._discover(RUNNER_PATH.parent, "test_*.py")
            document = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
            with tempfile.TemporaryDirectory(prefix="classic-notepad-manifest-") as temporary:
                manifest_path = Path(temporary) / "manifest.json"
                document["suites"]["Full"].pop()
                manifest_path.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "Full manifest differs"):
                    run_automation_tests._load_suite_manifest(manifest_path, set(discovered))

                document = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
                document["suites"]["Stress"].append(document["suites"]["Ordinary"][0])
                manifest_path.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "must be disjoint"):
                    run_automation_tests._load_suite_manifest(manifest_path, set(discovered))
        finally:
            sys.path.remove(str(RUNNER_PATH.parent))

    def test_runner_lists_exact_suite_counts_and_keeps_full_default(self):
        expected = {"Ordinary": 39, "Stress": 45, "Failure": 36, "Full": 120}
        manifest_ids = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))["suites"]["Full"]
        for suite, count in expected.items():
            with self.subTest(suite=suite):
                command = [
                    sys.executable,
                    str(RUNNER_PATH),
                    "--platform", "windows",
                    "--suite", suite,
                    "--list",
                ]
                result = subprocess.run(command, cwd=REPO_ROOT, text=True, capture_output=True, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"suite={suite} selected={count} full=120", result.stdout)
                if suite == "Full":
                    listed_ids = result.stdout.splitlines()[:-1]
                    self.assertEqual(listed_ids, manifest_ids)

        default = subprocess.run(
            [sys.executable, str(RUNNER_PATH), "--platform", "windows", "--list"],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(default.returncode, 0, default.stderr)
        self.assertIn("suite=Full selected=120 full=120", default.stdout)
        self.assertEqual(default.stdout.splitlines()[:-1], manifest_ids)

    def test_runner_rejects_empty_selected_suite_even_with_exact_partition(self):
        document = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        document["suites"]["Stress"] = document["suites"]["Ordinary"] + document["suites"]["Stress"]
        document["suites"]["Ordinary"] = []
        with tempfile.TemporaryDirectory(prefix="classic-notepad-empty-suite-") as temporary:
            manifest_path = Path(temporary) / "manifest.json"
            manifest_path.write_text(json.dumps(document), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(RUNNER_PATH), "--platform", "windows", "--suite", "Ordinary",
                 "--manifest", str(manifest_path), "--list"],
                cwd=REPO_ROOT, text=True, capture_output=True, check=False,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("selected no tests", result.stderr)
        self.assertNotIn("Ran 0 tests", result.stdout + result.stderr)

    def test_subtest_outcomes_use_unittest_classification_and_error_precedence(self):
        sys.path.insert(0, str(RUNNER_PATH.parent))
        try:
            import run_automation_tests
        finally:
            sys.path.remove(str(RUNNER_PATH.parent))

        class AssertionSubTest(unittest.TestCase):
            def runTest(self):
                with self.subTest(kind="assertion"):
                    self.assertEqual(1, 2)

        class RuntimeSubTest(unittest.TestCase):
            def runTest(self):
                with self.subTest(kind="runtime"):
                    raise RuntimeError("subTest runtime failure")

        class MixedSubTest(unittest.TestCase):
            def runTest(self):
                with self.subTest(kind="assertion"):
                    self.assertEqual("left", "right")
                with self.subTest(kind="runtime"):
                    raise RuntimeError("mixed runtime failure")

        suite = unittest.TestSuite([AssertionSubTest(), RuntimeSubTest(), MixedSubTest()])
        stream = io.StringIO()
        result = unittest.TextTestRunner(
            stream=stream, verbosity=0, resultclass=run_automation_tests.TrackingTextTestResult
        ).run(suite)
        self.assertFalse(result.wasSuccessful())
        self.assertEqual(len(result.failures), 2)
        self.assertEqual(len(result.errors), 2)

        outcomes = list(result.outcomes.values())
        outcome_by_class = {item["testId"].split(".")[-2]: item["status"] for item in outcomes}
        self.assertEqual(outcome_by_class, {
            "AssertionSubTest": "failed",
            "RuntimeSubTest": "error",
            "MixedSubTest": "error",
        })
        self.assertEqual(len(outcomes), 3)
        self.assertEqual([item["status"] for item in outcomes], ["failed", "error", "error"])
        counts = run_automation_tests._summarize_outcomes(3, outcomes)
        self.assertEqual(counts, {
            "selected": 3, "executed": 3, "passed": 0, "failed": 1, "errors": 2,
            "skipped": 0, "expectedFailures": 0, "unexpectedSuccesses": 0,
        })

    def _make_fixture(self, failure_stage=None, failure_code=0):
        temporary = tempfile.TemporaryDirectory(prefix="classic-notepad-e10-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name) / "project"
        (root / "scripts").mkdir(parents=True)
        (root / "tests" / "automation").mkdir(parents=True)
        shutil.copy2(GATE_PATH, root / "scripts" / GATE_PATH.name)
        (root / "tests" / "automation" / "run_automation_tests.py").write_text("# fake runner target\n", encoding="utf-8")
        shutil.copy2(MANIFEST_PATH, root / "tests" / "automation" / "test_suite_manifest.json")
        (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.20)\nproject(Fake LANGUAGES CXX)\n", encoding="utf-8")
        version_bytes = b"1.2.3\r\n"
        (root / "VERSION").write_bytes(version_bytes)
        tool_script = root / "fake_tool.py"
        tool_script.write_text(
            """import datetime, hashlib, json, os, pathlib, sys
kind, *args = sys.argv[1:]
if os.environ.get('FAKE_STDERR_STAGE') == kind:
    sys.stderr.write(os.environ.get('FAKE_STDERR_TEXT', 'fake progress on stderr') + '\\n')
    sys.stderr.flush()
if os.environ.get('FAKE_MUTATE_VERSION') == kind:
    pathlib.Path(os.environ['FAKE_VERSION_PATH']).write_bytes(b'9.9.9\\n')
def event(stage):
    log = os.environ.get('FAKE_EVENT_LOG')
    if log:
        with open(log, 'a', encoding='utf-8') as handle:
            handle.write(json.dumps({'tool': kind, 'stage': stage, 'args': args}) + '\\n')
def fail(stage):
    if os.environ.get('FAKE_FAILURE_STAGE') == stage:
        return int(os.environ.get('FAKE_FAILURE_CODE', '1'))
    return 0
if kind == 'vswhere' and args == ['-?']:
    event('version')
    print('vswhere version 3.1.7')
    raise SystemExit(0)
if kind == 'vswhere' and args and args[-2:] == ['-format', 'json']:
    event('inventory')
    majors = os.environ.get('FAKE_VSWHERE_MAJORS', '17').split(',')
    versions = {'18': ('C:/Fake/VS2026', '18.0.0'), '17': ('C:/Fake/VS2022', '17.10.0'),
                '17b': ('C:/Fake/VS2022-BuildTools', '17.9.0')}
    print(json.dumps([{'installationPath': versions[major][0], 'installationVersion': versions[major][1]}
                      for major in majors]))
    raise SystemExit(0)
if kind == 'cmake' and args == ['--help']:
    event('help')
    code = fail('help')
    if code: raise SystemExit(code)
    print('Generators: Visual Studio 18 2026; Visual Studio 17 2022')
    raise SystemExit(0)
if kind == 'cmake' and args == ['--version']:
    print('cmake version 3.30.0')
    raise SystemExit(0)
if kind == 'cmake' and args and args[0] == '-S':
    event('configure')
    code = fail('configure')
    if code: raise SystemExit(code)
    values = {args[i]: args[i+1] for i in range(0, len(args)-1) if args[i] in ('-S','-B','-G','-A')}
    if values['-G'] != 'Visual Studio 17 2022': raise SystemExit(44)
    instance = {'Visual Studio 18 2026': 'C:/Fake/VS2026', 'Visual Studio 17 2022': 'C:/Fake/VS2022'}[values['-G']]
    build = pathlib.Path(values['-B'])
    build.mkdir(parents=True, exist_ok=True)
    (build / 'CMakeCache.txt').write_text('\\n'.join([
        f"CMAKE_HOME_DIRECTORY:INTERNAL={values['-S']}",
        f"CMAKE_CACHEFILE_DIR:INTERNAL={values['-B']}",
        f"CMAKE_GENERATOR:INTERNAL={values['-G']}",
        f"CMAKE_GENERATOR_INSTANCE:INTERNAL={instance}",
        f"CMAKE_GENERATOR_PLATFORM:INTERNAL={values['-A']}",
        'CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release;RelWithDebInfo;MinSizeRel',
        ''
    ]), encoding='utf-8')
    raise SystemExit(0)
if kind == 'cmake' and args and args[0] == '--build':
    event('build')
    code = fail('build')
    if code: raise SystemExit(code)
    build = pathlib.Path(args[1])
    output = build / 'Release' / 'ClassicNotepad.exe'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(b'fake release executable')
    raise SystemExit(0)
if kind == 'ctest':
    if args == ['--version']:
        print('ctest version 3.30.0')
        raise SystemExit(0)
    event('ctest')
    raise SystemExit(fail('ctest'))
if kind == 'python':
    if args == ['--version']:
        print('Python 3.12.0')
        raise SystemExit(0)
    event('automation')
    code = fail('automation')
    runner, *runner_args = args
    options = {runner_args[i]: runner_args[i+1] for i in range(0, len(runner_args)-1) if runner_args[i].startswith('--')}
    manifest_path = pathlib.Path(runner).parent / 'test_suite_manifest.json'
    manifest_bytes = manifest_path.read_bytes()
    suite = options['--suite']
    selected = json.loads(manifest_bytes)['suites'][suite]
    binary = pathlib.Path(options['--binary'])
    runner_path = pathlib.Path(runner)
    result_path = pathlib.Path(options['--json-results'])
    outcomes = [{'testId': test_id, 'status': ('error' if code and index == 0 else 'passed')}
                for index, test_id in enumerate(selected)]
    result = {
        'schemaVersion': 1, 'suite': suite, 'pattern': 'test_*.py',
        'startedUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'completedUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'exitCode': code, 'manifestPath': str(manifest_path.resolve()),
        'manifestSha256': hashlib.sha256(manifest_bytes).hexdigest().upper(),
        'runnerPath': str(runner_path.resolve()),
        'runnerSha256': hashlib.sha256(runner_path.read_bytes()).hexdigest().upper(),
        'binaryPath': str(binary.resolve()),
        'binarySha256': hashlib.sha256(binary.read_bytes()).hexdigest().upper(),
        'resultPath': str(result_path.resolve()), 'selectedIds': selected,
        'executedIds': selected, 'outcomes': outcomes,
        'counts': {'selected': len(selected), 'executed': len(selected),
                   'passed': len(selected) - (1 if code else 0), 'failed': 0,
                   'errors': 1 if code else 0, 'skipped': 0,
                   'expectedFailures': 0, 'unexpectedSuccesses': 0}
    }
    result_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.write_text(json.dumps(result), encoding='utf-8')
    raise SystemExit(code)
raise SystemExit(64)
""",
            encoding="utf-8",
        )
        wrappers = {}
        for tool in ("cmake", "ctest", "python", "vswhere"):
            wrapper = root / f"{tool}.cmd"
            wrapper.write_text(
                f'@echo off\r\n"{sys.executable}" "{tool_script}" {tool} %*\r\nexit /b %ERRORLEVEL%\r\n',
                encoding="ascii",
            )
            wrappers[tool] = wrapper
        event_log = Path(temporary.name) / "events.jsonl"
        environment = os.environ.copy()
        environment["FAKE_EVENT_LOG"] = str(event_log)
        if failure_stage:
            environment["FAKE_FAILURE_STAGE"] = failure_stage
            environment["FAKE_FAILURE_CODE"] = str(failure_code)
        command = [
            POWERSHELL,
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", str(root / "scripts" / GATE_PATH.name),
            "-Suite", "Full",
            "-CMakePath", str(wrappers["cmake"]),
            "-CTestPath", str(wrappers["ctest"]),
            "-PythonPath", str(wrappers["python"]),
            "-VsWherePath", str(wrappers["vswhere"]),
        ]
        return temporary, root, version_bytes, event_log, environment, command

    def _run_gate(self, fixture, extra_environment=None, extra_arguments=None):
        _, root, _, _, environment, command = fixture
        environment = environment.copy()
        if extra_environment:
            environment.update(extra_environment)
        if extra_arguments:
            command = command + extra_arguments
        return subprocess.run(command, cwd=root, env=environment, text=True, capture_output=True, check=False)

    def _create_junction(self, link, target):
        quote = lambda value: "'" + str(value).replace("'", "''") + "'"
        command = f"New-Item -ItemType Junction -Path {quote(link)} -Target {quote(target)} | Out-Null"
        result = subprocess.run([POWERSHELL, "-NoProfile", "-Command", command], text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def _remove_junction(self, link):
        quote = "'" + str(link).replace("'", "''") + "'"
        subprocess.run([POWERSHELL, "-NoProfile", "-Command", f"Remove-Item -LiteralPath {quote} -Force"],
                       text=True, capture_output=True, check=False)

    def _read_log_text(self, path):
        raw = Path(path).read_bytes()
        try:
            return raw.decode("utf-8-sig")
        except UnicodeDecodeError:
            return raw.decode("utf-16")

    def _latest_record(self, root):
        records = sorted((root / "build" / "windows-release-verification-evidence").glob("*.json"))
        self.assertTrue(records, "gate did not write a result record")
        return json.loads(records[-1].read_text(encoding="utf-8-sig"))

    def test_gate_success_reuses_compatible_cache_and_preserves_version(self):
        fixture = self._make_fixture()
        temporary, root, version_bytes, event_log, _, _ = fixture
        first = self._run_gate(fixture)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        build = root / "build" / "windows-release-verification"
        marker = build / "user-cache-marker.txt"
        marker.write_text("retain", encoding="utf-8")

        second = self._run_gate(fixture)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertTrue(marker.exists())
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        events = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(sum(event["stage"] == "configure" for event in events), 1)
        record = self._latest_record(root)
        self.assertEqual(record["status"], "passed")
        self.assertEqual(record["versionSha256Before"], record["versionSha256After"])
        self.assertEqual(record["binarySha256"], __import__("hashlib").sha256((build / "Release" / "ClassicNotepad.exe").read_bytes()).hexdigest().upper())
        self.assertEqual(record["generator"], "Visual Studio 17 2022")
        self.assertEqual(record["toolVersions"]["vswhere"]["version"], "vswhere version 3.1.7")
        self.assertEqual(record["manifestSha256"], __import__("hashlib").sha256(
            (root / "tests" / "automation" / "test_suite_manifest.json").read_bytes()).hexdigest().upper())
        self.assertTrue(record["automationValidation"]["valid"])
        self.assertEqual(record["automationValidation"]["counts"]["executed"], 120)
        automation = json.loads(Path(record["automationResultsJson"]).read_text(encoding="utf-8"))
        self.assertEqual(automation["resultPath"], record["automationResultsJson"])
        self.assertEqual(automation["executedIds"], automation["selectedIds"])
        self.assertEqual(automation["counts"]["passed"], 120)
        for step in record["steps"]:
            self.assertTrue(Path(step["stdoutLog"]).exists(), step["name"])
            self.assertTrue(Path(step["stderrLog"]).exists(), step["name"])

    def test_cmake_help_preflight_preserves_exit_code_and_logs(self):
        fixture = self._make_fixture("help", 23)
        _, root, version_bytes, _, _, _ = fixture
        result = self._run_gate(fixture)
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        record = self._latest_record(root)
        help_step = next(step for step in record["steps"] if step["name"] == "cmake-generator-help")
        self.assertEqual(help_step["exitCode"], 23)
        self.assertTrue(Path(help_step["stdoutLog"]).exists())
        self.assertTrue(Path(help_step["stderrLog"]).exists())

    def test_native_stderr_is_logged_without_aborting_and_nonzero_exit_is_preserved(self):
        success_fixture = self._make_fixture()
        _, success_root, _, _, _, _ = success_fixture
        success = self._run_gate(success_fixture, extra_environment={
            "FAKE_STDERR_STAGE": "ctest",
            "FAKE_STDERR_TEXT": "ctest progress stream",
        })
        self.assertEqual(success.returncode, 0, success.stdout + success.stderr)
        success_record = self._latest_record(success_root)
        ctest_step = next(step for step in success_record["steps"] if step["name"] == "ctest-release")
        self.assertIn("ctest progress stream", self._read_log_text(ctest_step["stderrLog"]))

        failing_fixture = self._make_fixture("ctest", 17)
        _, failing_root, _, _, _, _ = failing_fixture
        failing = self._run_gate(failing_fixture, extra_environment={
            "FAKE_STDERR_STAGE": "ctest",
            "FAKE_STDERR_TEXT": "ctest failing progress stream",
        })
        self.assertEqual(failing.returncode, 17, failing.stdout + failing.stderr)
        failing_record = self._latest_record(failing_root)
        ctest_step = next(step for step in failing_record["steps"] if step["name"] == "ctest-release")
        self.assertIn("ctest failing progress stream", self._read_log_text(ctest_step["stderrLog"]))

    def test_gate_preserves_child_exit_codes_and_version_on_each_failure_stage(self):
        for stage, code in (("configure", 7), ("build", 8), ("ctest", 9), ("automation", 11)):
            with self.subTest(stage=stage):
                fixture = self._make_fixture(stage, code)
                _, root, version_bytes, _, _, _ = fixture
                result = self._run_gate(fixture)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
                record = self._latest_record(root)
                self.assertEqual(record["exitCode"], code)
                self.assertEqual(record["status"], "failed")
                self.assertEqual(record["versionSha256Before"], record["versionSha256After"])

    def test_missing_tool_preserves_version_and_records_failure(self):
        fixture = self._make_fixture()
        _, root, version_bytes, _, _, _ = fixture
        missing = str(root / "missing-cmake.exe")
        fixture[5][fixture[5].index("-CMakePath") + 1] = missing
        result = self._run_gate(fixture)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        record = self._latest_record(root)
        self.assertEqual(record["status"], "failed")
        self.assertEqual(record["versionSha256Before"], record["versionSha256After"])

    def test_finally_restores_version_mutated_by_a_failing_child_and_keeps_child_code(self):
        fixture = self._make_fixture("ctest", 13)
        _, root, version_bytes, _, _, _ = fixture
        result = self._run_gate(fixture, extra_environment={
            "FAKE_MUTATE_VERSION": "ctest",
            "FAKE_VERSION_PATH": str(root / "VERSION"),
        })
        self.assertEqual(result.returncode, 13, result.stdout + result.stderr)
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        record = self._latest_record(root)
        self.assertTrue(record["versionRestored"])
        self.assertEqual(record["versionSha256Before"], record["versionSha256After"])
        self.assertEqual(record["exitCode"], 13)

    def test_successful_child_version_mutation_fails_gate_after_restoration(self):
        fixture = self._make_fixture()
        _, root, version_bytes, _, _, _ = fixture
        result = self._run_gate(fixture, extra_environment={
            "FAKE_MUTATE_VERSION": "ctest",
            "FAKE_VERSION_PATH": str(root / "VERSION"),
        })
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        record = self._latest_record(root)
        self.assertTrue(record["versionUnexpectedMutation"])
        self.assertTrue(record["versionRestored"])
        self.assertEqual(record["versionSha256Before"], record["versionSha256After"])
        self.assertIn("unexpectedly changed VERSION", record["failure"])

    def test_build_junction_escape_is_rejected_without_touching_external_target(self):
        fixture = self._make_fixture()
        temporary, root, version_bytes, event_log, _, _ = fixture
        target = Path(temporary.name) / "external-build"
        target.mkdir()
        marker = target / "keep.txt"
        marker.write_text("untouched", encoding="utf-8")
        build_link = root / "build" / "windows-release-verification"
        build_link.parent.mkdir(parents=True)
        self._create_junction(build_link, target)
        self.addCleanup(self._remove_junction, build_link)

        result = self._run_gate(fixture)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "untouched")
        self.assertEqual([path.name for path in target.iterdir()], ["keep.txt"])
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        self.assertFalse(event_log.exists() and event_log.stat().st_size)
        record = self._latest_record(root)
        self.assertIn("reparse point", record["failure"])

    def test_cached_release_junction_is_rejected_before_external_writes(self):
        fixture = self._make_fixture()
        temporary, root, version_bytes, event_log, _, _ = fixture
        build = root / "build" / "windows-release-verification"
        build.mkdir(parents=True)
        cache = build / "CMakeCache.txt"
        cache.write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={root}\n"
            f"CMAKE_CACHEFILE_DIR:INTERNAL={build}\n"
            "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\n"
            "CMAKE_GENERATOR_INSTANCE:INTERNAL=C:/Fake/VS2022\n"
            "CMAKE_GENERATOR_PLATFORM:INTERNAL=x64\n"
            "CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release;RelWithDebInfo;MinSizeRel\n",
            encoding="utf-8",
        )
        cache_before = cache.read_bytes()
        target = Path(temporary.name) / "external-release"
        target.mkdir()
        marker = target / "keep.txt"
        marker.write_text("untouched", encoding="utf-8")
        release_link = build / "Release"
        self._create_junction(release_link, target)
        self.addCleanup(self._remove_junction, release_link)

        result = self._run_gate(fixture)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(cache.read_bytes(), cache_before)
        self.assertEqual(marker.read_text(encoding="utf-8"), "untouched")
        self.assertEqual([path.name for path in target.iterdir()], ["keep.txt"])
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        self.assertFalse(event_log.exists())
        record = self._latest_record(root)
        self.assertIn("descendant reparse point", record["failure"])

    def test_cached_generator_must_match_installed_advertised_generator(self):
        fixture = self._make_fixture()
        _, root, version_bytes, event_log, _, _ = fixture
        first = self._run_gate(fixture)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        cache = root / "build" / "windows-release-verification" / "CMakeCache.txt"
        compatible_cache = cache.read_text(encoding="utf-8")
        self.assertIn("CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022", compatible_cache)
        self.assertIn("CMAKE_GENERATOR_INSTANCE:INTERNAL=C:/Fake/VS2022", compatible_cache)
        cache.write_text(compatible_cache.replace(
            "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022",
            "CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026",
        ), encoding="utf-8")
        stale_cache = cache.read_bytes()
        events_before = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]

        second = self._run_gate(fixture)
        self.assertNotEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(cache.read_bytes(), stale_cache)
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        events_after = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(sum(event["stage"] == "configure" for event in events_after),
                         sum(event["stage"] == "configure" for event in events_before))
        self.assertEqual(sum(event["stage"] == "build" for event in events_after),
                         sum(event["stage"] == "build" for event in events_before))
        record = self._latest_record(root)
        self.assertIn("has no installed Visual Studio instance", record["failure"])

    def test_compatible_cached_generator_is_reused_even_when_not_default(self):
        fixture = self._make_fixture()
        _, root, _, event_log, _, _ = fixture
        first = self._run_gate(fixture)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        cache = root / "build" / "windows-release-verification" / "CMakeCache.txt"
        self.assertIn("CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022", cache.read_text(encoding="utf-8"))
        events_before = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]

        second = self._run_gate(fixture, extra_environment={"FAKE_VSWHERE_MAJORS": "18,17"})
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        events_after = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(sum(event["stage"] == "configure" for event in events_after),
                         sum(event["stage"] == "configure" for event in events_before))
        record = self._latest_record(root)
        self.assertEqual(record["status"], "passed")
        self.assertEqual(record["generator"], "Visual Studio 17 2022")

    def test_missing_cached_instance_requires_unambiguous_resolution(self):
        fixture = self._make_fixture()
        _, root, version_bytes, event_log, _, _ = fixture
        first = self._run_gate(fixture)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        cache = root / "build" / "windows-release-verification" / "CMakeCache.txt"
        cache_text = cache.read_text(encoding="utf-8")
        cache.write_text("\n".join(
            line for line in cache_text.splitlines()
            if not line.startswith("CMAKE_GENERATOR_INSTANCE:")
        ) + "\n", encoding="utf-8")

        unambiguous = self._run_gate(fixture)
        self.assertEqual(unambiguous.returncode, 0, unambiguous.stdout + unambiguous.stderr)
        cache_without_instance = cache.read_bytes()
        events_before = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]

        ambiguous = self._run_gate(fixture, extra_environment={"FAKE_VSWHERE_MAJORS": "18,17,17b"})
        self.assertNotEqual(ambiguous.returncode, 0, ambiguous.stdout + ambiguous.stderr)
        self.assertEqual(cache.read_bytes(), cache_without_instance)
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        events_after = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(sum(event["stage"] == "configure" for event in events_after),
                         sum(event["stage"] == "configure" for event in events_before))
        self.assertEqual(sum(event["stage"] == "build" for event in events_after),
                         sum(event["stage"] == "build" for event in events_before))
        record = self._latest_record(root)
        self.assertIn("has no recorded instance and resolves to 2 installed instances", record["failure"])

    def test_cached_generator_instance_must_be_in_installed_inventory(self):
        fixture = self._make_fixture()
        _, root, version_bytes, event_log, _, _ = fixture
        first = self._run_gate(fixture)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        cache = root / "build" / "windows-release-verification" / "CMakeCache.txt"
        compatible_cache = cache.read_text(encoding="utf-8")
        self.assertIn("CMAKE_GENERATOR_INSTANCE:INTERNAL=C:/Fake/VS2022", compatible_cache)
        cache.write_text(compatible_cache.replace(
            "CMAKE_GENERATOR_INSTANCE:INTERNAL=C:/Fake/VS2022",
            "CMAKE_GENERATOR_INSTANCE:INTERNAL=C:/External/VS2022",
        ), encoding="utf-8")
        stale_cache = cache.read_bytes()
        events_before = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]

        second = self._run_gate(fixture)
        self.assertNotEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(cache.read_bytes(), stale_cache)
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        events_after = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(sum(event["stage"] == "configure" for event in events_after),
                         sum(event["stage"] == "configure" for event in events_before))
        self.assertEqual(sum(event["stage"] == "build" for event in events_after),
                         sum(event["stage"] == "build" for event in events_before))
        record = self._latest_record(root)
        self.assertIn("is not present in the installed instance inventory", record["failure"])

    def test_evidence_junction_escape_is_rejected_without_external_writes(self):
        fixture = self._make_fixture()
        temporary, root, version_bytes, event_log, _, _ = fixture
        target = Path(temporary.name) / "external-evidence"
        target.mkdir()
        marker = target / "keep.txt"
        marker.write_text("untouched", encoding="utf-8")
        evidence_link = root / "build" / "windows-release-verification-evidence"
        evidence_link.parent.mkdir(parents=True)
        self._create_junction(evidence_link, target)
        self.addCleanup(self._remove_junction, evidence_link)

        result = self._run_gate(fixture)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "untouched")
        self.assertEqual([path.name for path in target.iterdir()], ["keep.txt"])
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        self.assertFalse(event_log.exists())
        self.assertFalse((root / "build" / "windows-release-verification").exists())

    def test_incompatible_cache_is_retained_without_running_tools(self):
        fixture = self._make_fixture()
        _, root, version_bytes, event_log, _, _ = fixture
        build = root / "build" / "windows-release-verification"
        build.mkdir(parents=True)
        cache = build / "CMakeCache.txt"
        cache.write_text(
            "CMAKE_HOME_DIRECTORY:INTERNAL=C:/wrong/source\n"
            f"CMAKE_CACHEFILE_DIR:INTERNAL={build}\n"
            "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\n"
            "CMAKE_GENERATOR_PLATFORM:INTERNAL=x64\n",
            encoding="utf-8",
        )
        cache_before = cache.read_bytes()
        marker = build / "keep.txt"
        marker.write_text("keep", encoding="utf-8")

        result = self._run_gate(fixture)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(cache.read_bytes(), cache_before)
        self.assertEqual(marker.read_text(encoding="utf-8"), "keep")
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        events = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]
        self.assertEqual([event["stage"] for event in events], ["version", "inventory", "help"])
        record = self._latest_record(root)
        self.assertEqual(record["status"], "failed")

    def test_nonempty_build_path_without_cache_is_retained(self):
        fixture = self._make_fixture()
        _, root, version_bytes, event_log, _, _ = fixture
        build = root / "build" / "windows-release-verification"
        build.mkdir(parents=True)
        marker = build / "unrecognized-user-file.txt"
        marker.write_text("keep", encoding="utf-8")

        result = self._run_gate(fixture)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(marker.exists())
        self.assertEqual(marker.read_text(encoding="utf-8"), "keep")
        self.assertEqual((root / "VERSION").read_bytes(), version_bytes)
        events = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]
        self.assertEqual([event["stage"] for event in events], ["version", "inventory", "help"])
        record = self._latest_record(root)
        self.assertEqual(record["status"], "failed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
