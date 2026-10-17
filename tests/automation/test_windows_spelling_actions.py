import tempfile
import time
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


E_FAIL = 2147500037
E_ACCESSDENIED = 2147942405
EN_ERRSPACE = 0x0500
EN_MAXTEXT = 0x0501


def utf16_units(text: str) -> int:
    return len(text.encode("utf-16-le")) // 2


def configure_backend(app, **fields):
    app.command("configureSpellingTestBackend", **fields)


def native_target(app, text: str, start: int):
    app.command("setSelection", start=start, end=start)
    app.command("refreshSpellCheckNow")
    if start > 65535:
        app.command("sendEditorVerticalScroll", scrollCommand=7)  # SB_BOTTOM
    deadline = time.monotonic() + 5.0
    target = None
    while time.monotonic() < deadline:
        point = app.try_command("getVisibleTextPositionPoint", position=start + 1)
        if point.get("ok"):
            target = app.command("resolveSpellingContextTarget", x=point["x"], y=point["y"])
            if target["found"]:
                break
        time.sleep(0.05)
    if not target or not target.get("found"):
        raise AssertionError(f"No native spelling target at {start}: {target}")
    if target["start"] != start or target["text"] != text:
        raise AssertionError(f"Unexpected target: {target}")
    return {key: target[key] for key in (
        "sessionId", "contentRevision", "layoutRevision", "start", "length", "text",
    )}


@unittest.skipUnless(automation_platform() == "windows", "Windows spelling actions use the native and mapped engines")
class WindowsSpellingActionTests(unittest.TestCase):
    def test_check_suggest_and_action_outcomes_distinguish_empty_unavailable_and_failure(self):
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            configure_backend(app, available=True, misspelledWord="teh", suggestions="the|ten",
                checkStatus="failure", checkHresult=E_FAIL)
            failed_check = app.command("checkSpelling", text="teh")
            self.assertEqual((failed_check["status"], failed_check["hresult"], failed_check["errors"]),
                ("failure", E_FAIL, []))

            configure_backend(app, available=True, checkStatus="success", misspelledWord="teh",
                failCheckAfter=0)
            partial_check = app.command("checkSpelling", text="teh teh")
            self.assertEqual(partial_check["status"], "failure")
            self.assertEqual(partial_check["hresult"], E_FAIL)
            self.assertEqual(partial_check["errors"], [])

            configure_backend(app, available=True, misspelledWord="teh", suggestStatus="failure",
                suggestHresult=E_FAIL)
            failed_suggest = app.command("suggestSpelling", word="teh")
            self.assertEqual((failed_suggest["status"], failed_suggest["hresult"],
                failed_suggest["suggestions"]), ("failure", E_FAIL, []))

            configure_backend(app, available=True, suggestions="the|ten", failSuggestAfter=1)
            partial_suggest = app.command("suggestSpelling", word="teh")
            self.assertEqual((partial_suggest["status"], partial_suggest["hresult"],
                partial_suggest["suggestions"]), ("failure", E_FAIL, []))

            configure_backend(app, available=True)
            clean = app.command("checkSpelling", text="ordinary text")
            no_suggestions = app.command("suggestSpelling", word="ordinary")
            self.assertEqual((clean["status"], clean["errors"]), ("success", []))
            self.assertEqual((no_suggestions["status"], no_suggestions["suggestions"]), ("success", []))

            configure_backend(app, available=False)
            unavailable = app.command("checkSpelling", text="teh")
            unavailable_suggest = app.command("suggestSpelling", word="teh")
            self.assertEqual(unavailable["status"], "unavailable")
            self.assertFalse(unavailable["available"])
            self.assertEqual((unavailable_suggest["status"], unavailable_suggest["suggestions"]),
                ("unavailable", []))
            app.command("setText", text="editing remains available")
            self.assertEqual(app.command("getText")["text"], "editing remains available")
            app.command("setSelection", start=3, end=3)
            self.assertTrue(app.command("sendEditorContextMenu", keyboard=True)["ok"])

    def test_ignore_once_suppresses_one_occurrence_and_is_revision_scoped(self):
        prefix = "okay\r\n" * 12000
        source = prefix + "teh / teh"
        first = utf16_units(prefix)
        second = first + utf16_units("teh / ")
        self.assertGreater(first, 65535)

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setWordWrap", enabled=False)
            app.command("setText", text=source)
            app.command("setSelection", start=first, end=first)
            configure_backend(app, available=True, misspelledWord="teh", suggestions="the")
            target = native_target(app, "teh", first)
            ignored = app.command("applySpellingAction", action="ignoreOnce", **target)
            self.assertEqual((ignored["status"], ignored["changed"], ignored["partial"]),
                ("success", False, False))
            self.assertEqual(app.command("getSpellingTestBackendCalls")["ignore"], 0,
                "Ignore Once must not call the word-wide backend Ignore method.")

            annotations = app.command("getSpellingAnnotations")["annotations"]
            self.assertEqual([(item["start"], item["length"]) for item in annotations], [(second, 3)])

            app.command("setAppearanceTheme", theme="dark")
            app.command("refreshSpellCheckNow")
            annotations = app.command("getSpellingAnnotations")["annotations"]
            self.assertEqual([(item["start"], item["length"]) for item in annotations], [(second, 3)])

            end = utf16_units(source)
            app.command("setSelection", start=end, end=end)
            app.command("insertText", text="!")
            app.command("refreshSpellCheckNow")
            annotations = app.command("getSpellingAnnotations")["annotations"]
            self.assertEqual([(item["start"], item["length"]) for item in annotations],
                [(first, 3), (second, 3)])
            app.command("undo")
            app.command("refreshSpellCheckNow")
            annotations = app.command("getSpellingAnnotations")["annotations"]
            self.assertEqual([(item["start"], item["length"]) for item in annotations],
                [(first, 3), (second, 3)])

    def test_backend_ignore_and_dictionary_add_report_success_failure_and_dry_run(self):
        source = "teh / teh"
        first = 0
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text=source)
            app.command("setSelection", start=first, end=first)
            configure_backend(app, available=True, misspelledWord="teh", suggestions="the")
            target = native_target(app, "teh", first)

            dry_run = app.command("addSpelling", word="teh", dryRun=True)
            self.assertEqual((dry_run["status"], dry_run["dryRun"], dry_run["persisted"]),
                ("success", True, False))
            unbound_add = app.command("addSpelling", word="teh", dryRun=False)
            self.assertEqual((unbound_add["status"], unbound_add["hresult"], unbound_add["persisted"]),
                ("failure", E_ACCESSDENIED, False))
            self.assertEqual(app.command("getSpellingTestBackendCalls")["add"], 0)

            added = app.command("applySpellingAction", action="addToDictionary", **target)
            self.assertEqual((added["status"], added["changed"], added["partial"]),
                ("success", True, False))
            self.assertEqual(app.command("getSpellingTestBackendCalls")["add"], 1)
            self.assertEqual(app.command("getSpellingAnnotations")["annotations"], [])
            self.assertEqual(app.command("getText")["text"], source)

            configure_backend(app, available=True, misspelledWord="teh", suggestions="the",
                addStatus="failure", addHresult=E_FAIL)
            target = native_target(app, "teh", first)
            before = app.command("getText")["text"]
            failed_add = app.command("applySpellingAction", action="addToDictionary", **target)
            self.assertEqual((failed_add["status"], failed_add["hresult"], failed_add["changed"]),
                ("failure", E_FAIL, False))
            self.assertEqual(app.command("getText")["text"], before)

            backend_ignore = app.command("ignoreSpelling", word="teh")
            self.assertEqual((backend_ignore["status"], backend_ignore["hresult"], backend_ignore["changed"]),
                ("success", 0, True))
            self.assertEqual(app.command("getSpellingAnnotations")["annotations"], [])

            configure_backend(app, available=True, misspelledWord="teh", ignoreStatus="failure",
                ignoreHresult=E_FAIL)
            failed_ignore = app.command("ignoreSpelling", word="teh")
            self.assertEqual((failed_ignore["status"], failed_ignore["hresult"]), ("failure", E_FAIL))
            self.assertEqual(len(app.command("getSpellingAnnotations")["annotations"]), 2)

    def test_stale_target_and_native_capacity_reject_without_partial_replacement(self):
        prefix = "okay\r\n" * 11000
        source = prefix + "teh"
        target_start = utf16_units(prefix)
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text=source)
            app.command("setSelection", start=target_start, end=target_start)
            configure_backend(app, available=True, misspelledWord="teh", suggestions="wonderful")
            target = native_target(app, "teh", target_start)

            app.command("setSelection", start=target_start, end=target_start + 3)
            app.command("insertText", text="the")
            stale = app.command("applySpellingAction", action="replaceSuggestion",
                suggestion="wonderful", **target)
            self.assertEqual((stale["status"], stale["changed"], stale["partial"]),
                ("failure", False, False))
            self.assertIn("changed", stale["message"].lower())
            self.assertEqual(app.command("getText")["text"], prefix + "the")
            app.command("undo")
            self.assertEqual(app.command("getText")["text"], source,
                "The failed stale action must not add an Undo record.")

            target = native_target(app, "teh", target_start)
            current_length = utf16_units(source)
            limit = app.command("setEditorTextLimit", limit=current_length)["limit"]
            self.assertGreaterEqual(limit, current_length)
            capacity = app.command("applySpellingAction", action="replaceSuggestion",
                suggestion="wonderful", **target)
            self.assertEqual((capacity["status"], capacity["changed"], capacity["partial"]),
                ("failure", False, False))
            self.assertIn("limit", capacity["message"].lower())
            self.assertEqual(app.command("getText")["text"], source)

    def test_native_replacement_changes_only_captured_repeated_occurrence_after_65535(self):
        prefix = "ordinary row\r\n" * 6000
        source = prefix + "alot / alot"
        first = utf16_units(prefix)
        self.assertGreater(first, 65535)

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setWordWrap", enabled=False)
            app.command("setText", text=source)
            configure_backend(app, available=True, misspelledWord="alot", suggestions="a lot")
            target = native_target(app, "alot", first)
            result = app.command("applySpellingAction", action="replaceSuggestion",
                suggestion="a lot", **target)
            self.assertEqual((result["status"], result["changed"], result["partial"]),
                ("success", True, False))
            self.assertEqual(app.command("getText")["text"], prefix + "a lot / alot")

            app.command("undo")
            self.assertEqual(app.command("getText")["text"], source)

    def test_native_replace_reports_unchanged_partial_and_unverifiable_outcomes(self):
        source = "alot / alot"
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setText", text=source)
            app.command("setSelection", start=utf16_units(source), end=utf16_units(source))
            app.command("typeText", text="!")
            configure_backend(app, available=True, misspelledWord="alot", suggestions="a lot")
            target = native_target(app, "alot", 0)
            app.command("setPasteFault", stage="native-errspace")
            unchanged = app.command("applySpellingAction", action="replaceSuggestion",
                suggestion="a lot", **target)
            self.assertEqual((unchanged["status"], unchanged["changed"], unchanged["partial"],
                unchanged["verification"], unchanged["nativeNotification"]),
                ("failure", False, False, "unchanged", EN_ERRSPACE))
            self.assertIn("prior undo state is preserved", unchanged["message"].lower())
            self.assertEqual(app.command("getText")["text"], source + "!")
            app.command("setPasteFault", stage="")
            app.command("undo")
            self.assertEqual(app.command("getText")["text"], source,
                "The rejected replacement must leave the prior typing Undo available.")

            target = native_target(app, "alot", 0)
            app.command("setPasteFault", stage="native-maxtext")
            partial = app.command("applySpellingAction", action="replaceSuggestion",
                suggestion="a lot", **target)
            self.assertEqual((partial["status"], partial["changed"], partial["partial"],
                partial["verification"]), ("failure", True, True, "partial"))
            self.assertEqual(partial["nativeNotification"], EN_MAXTEXT)
            self.assertIn("undo availability was not verified", partial["message"].lower())
            self.assertEqual(app.command("getText")["text"], "a lo / alot")
            app.command("setPasteFault", stage="")

            app.command("setText", text=source)
            configure_backend(app, available=True, misspelledWord="alot", suggestions="a lot")
            target = native_target(app, "alot", 0)
            app.command("setPasteFault", stage="native-spell-verification")
            unverifiable = app.command("applySpellingAction", action="replaceSuggestion",
                suggestion="a lot", **target)
            self.assertEqual((unverifiable["status"], unverifiable["changed"],
                unverifiable["partial"], unverifiable["verification"]),
                ("failure", True, False, "unavailable"))
            self.assertEqual(unverifiable["nativeNotification"], 0)
            self.assertIn("undo availability is unknown", unverifiable["message"].lower())
            self.assertEqual(app.command("getText")["text"], "a lot / alot")
            app.command("setPasteFault", stage="")
            app.command("undo")
            self.assertEqual(app.command("getText")["text"], source)

    def test_mapped_far_right_suggestion_replacement_is_exact_and_one_undo(self):
        body_bytes = 50 * 1024 * 1024
        tail = " café 漢字 😀\t" + "x" * 180 + " alot / alot END"
        tail_bytes = tail.encode("utf-8")
        first = body_bytes + utf16_units(tail[:tail.index("alot")])
        second = first + utf16_units("alot / ")
        second_after_replacement = second + utf16_units("a lot") - utf16_units("alot")

        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            path = Path(temp_root) / "spelling-actions-mapped.txt"
            block = b"x" * (1024 * 1024)
            with path.open("wb") as output:
                remaining = body_bytes
                while remaining:
                    count = min(remaining, len(block))
                    output.write(block[:count])
                    remaining -= count
                output.seek(body_bytes)
                output.write(tail_bytes)

            with ClassicNotepadDriver(automation_binary(), "windows", timeout=180.0) as app:
                app.command("openFile", path=str(path))
                deadline = time.monotonic() + 120.0
                state = app.command("getLargeFileState")["largeFile"]
                while not state["indexingComplete"] and time.monotonic() < deadline:
                    time.sleep(0.05)
                    state = app.command("getLargeFileState")["largeFile"]
                self.assertTrue(state["indexingComplete"], state)
                app.command("setWordWrap", enabled=False)
                app.command("setSelection", start=first, end=first)
                app.command("sendLargeFileHorizontalScroll", scrollCommand=7)  # SB_RIGHT
                configure_backend(app, available=True, misspelledWord="alot", suggestions="a lot")
                app.command("refreshSpellCheckNow")

                deadline = time.monotonic() + 8.0
                target = None
                last_point = None
                while time.monotonic() < deadline:
                    point = app.try_command("getVisibleTextPositionPoint", position=first + 1)
                    last_point = point
                    if point.get("ok"):
                        target = app.command("resolveSpellingContextTarget", x=point["x"], y=point["y"])
                        if target["found"]:
                            break
                    time.sleep(0.05)
                self.assertTrue(target and target.get("found"), {"target": target, "point": last_point,
                    "state": app.command("getLargeFileState")["largeFile"]})
                self.assertEqual((target["start"], target["text"]), (first, "alot"), target)
                target = {key: target[key] for key in (
                    "sessionId", "contentRevision", "layoutRevision", "start", "length", "text",
                )}
                mapped_failure = app.command("applySpellingAction", action="replaceSuggestion",
                    suggestion="a lot", injectMappedInsertFailureForTesting=True, **target)
                self.assertEqual((mapped_failure["status"], mapped_failure["changed"],
                    mapped_failure["partial"]), ("failure", False, False))
                self.assertIn("injected mapped insertion failure", mapped_failure["message"].lower())
                self.assertEqual(app.command("getLargeFileRange", start=first, length=4)["text"], "alot")
                self.assertEqual(app.command("getLargeFileRange", start=second, length=4)["text"], "alot")

                result = app.command("applySpellingAction", action="replaceSuggestion",
                    suggestion="a lot", **target)
                self.assertEqual((result["status"], result["changed"], result["partial"]),
                    ("success", True, False))
                self.assertEqual(app.command("getLargeFileRange", start=first, length=5)["text"], "a lot")
                self.assertEqual(app.command("getLargeFileRange", start=second_after_replacement, length=4)["text"], "alot")

                app.command("undo")
                self.assertEqual(app.command("getLargeFileRange", start=first, length=4)["text"], "alot")
                self.assertEqual(app.command("getLargeFileRange", start=second, length=4)["text"], "alot")


if __name__ == "__main__":
    unittest.main()
