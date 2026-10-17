import time
import unittest

from automation_context import automation_binary, automation_platform
from classic_notepad_driver import ClassicNotepadDriver


def utf16_units(value):
    return len(value.encode("utf-16-le", errors="surrogatepass")) // 2


def utf16_slice(value, start, end):
    encoded = value.encode("utf-16-le", errors="surrogatepass")
    return encoded[start * 2:end * 2].decode("utf-16-le", errors="surrogatepass")


def read_native_windows(app):
    windows = app.command("getEditorVisibleSpellWindows")["windows"]
    if not windows:
        raise AssertionError("Native editor returned no visible spelling windows.")
    for window in windows:
        for field in (
            "sessionId", "contentRevision", "layoutRevision", "start",
            "eligibleStart", "eligibleEnd", "text",
        ):
            if field not in window:
                raise AssertionError(f"Visible spelling window is missing {field}: {window}")
        units = utf16_units(window["text"])
        if window["eligibleStart"] > window["eligibleEnd"] or window["eligibleEnd"] > units:
            raise AssertionError(f"Invalid eligible bounds: {window}")
        if units > 96 * 1024:
            raise AssertionError(f"Visible spelling window exceeded the copy bound: {units}")
    return windows


@unittest.skipUnless(automation_platform() == "windows", "Windows native spelling-window regression")
class WindowsSpellingWindowTests(unittest.TestCase):
    def test_native_visible_windows_keep_full_coordinates_and_invalidate_by_revision(self):
        lines = [f"row-{index:04d} café 漢字 😀 misspelled segment" for index in range(7200)]
        lines[4] += " NAVWORD"
        lines[6100] += " NAVWORD"
        source = "\r\n".join(lines)

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setWordWrap", enabled=False)
            app.command("setText", text=source)

            initial = read_native_windows(app)
            initial_revision = (
                initial[0]["sessionId"],
                initial[0]["contentRevision"],
                initial[0]["layoutRevision"],
            )

            app.command("sendEditorVerticalScroll", scrollCommand=7)  # SB_BOTTOM
            distant = read_native_windows(app)
            distant_revision = (
                distant[0]["sessionId"],
                distant[0]["contentRevision"],
                distant[0]["layoutRevision"],
            )
            self.assertEqual(distant_revision[0], initial_revision[0])
            self.assertEqual(distant_revision[1], initial_revision[1])
            self.assertGreater(distant_revision[2], initial_revision[2])
            self.assertTrue(all(
                (window["sessionId"], window["contentRevision"], window["layoutRevision"])
                == distant_revision for window in distant
            ))
            self.assertTrue(any(window["start"] > 65535 for window in distant), distant[:3])
            self.assertLessEqual(sum(utf16_units(window["text"]) for window in distant), 96 * 1024)
            source_units = source.encode("utf-16-le", errors="surrogatepass")
            for window in distant:
                start = window["start"]
                expected = source_units[start * 2:(start + utf16_units(window["text"])) * 2]
                self.assertEqual(window["text"].encode("utf-16-le", errors="surrogatepass"), expected)

            app.command("goToLine", line=1)
            at_top = read_native_windows(app)[0]
            self.assertGreater(at_top["layoutRevision"], distant_revision[2])
            app.command("sendEditorVerticalScroll", scrollCommand=6)  # SB_TOP
            top_scroll = read_native_windows(app)[0]
            self.assertGreater(top_scroll["layoutRevision"], at_top["layoutRevision"])
            self.assertEqual(top_scroll["start"], 0)

            first_find = app.command("find", text="NAVWORD")
            self.assertTrue(first_find["found"], first_find)
            after_first_find = read_native_windows(app)[0]
            self.assertGreater(after_first_find["layoutRevision"], top_scroll["layoutRevision"])
            next_find = app.command("findNext")
            self.assertTrue(next_find["found"], next_find)
            after_find_next = read_native_windows(app)[0]
            self.assertGreater(after_find_next["layoutRevision"], after_first_find["layoutRevision"])

            app.command("insertText", text="X")
            after_edit = read_native_windows(app)[0]
            self.assertGreater(after_edit["contentRevision"], distant_revision[1])
            app.command("undo")
            after_undo = read_native_windows(app)[0]
            self.assertGreater(after_undo["contentRevision"], after_edit["contentRevision"])

            app.command("sendEditorVerticalScroll", scrollCommand=7)  # SB_BOTTOM
            after_scroll = read_native_windows(app)[0]
            self.assertGreater(after_scroll["layoutRevision"], after_undo["layoutRevision"])
            self.assertGreater(after_scroll["start"], 65535)

            app.command("newDocument")
            app.command("setText", text="new session 😀 text")
            after_new_document = read_native_windows(app)[0]
            self.assertGreater(after_new_document["sessionId"], after_scroll["sessionId"])

    @unittest.skipUnless(automation_platform() == "windows", "Windows context hit testing is native-only")
    def test_native_context_target_uses_full_document_positions_and_live_geometry(self):
        lines = [f"row-{index:05d} okay" for index in range(7600)]
        lines[3000] = "repeat teh distant repeated spelling target"
        lines[7571] = "repeat teh distant repeated spelling target"
        lines[7575] = "repeat teh distant repeated spelling target"
        lines[7585] = "prefix café 漢字 😀\t" + "x" * 180 + " teh suffix"
        lines[7599] = "wrapped " + "w" * 220 + " teh"
        source = "\r\n".join(lines)

        def position_of(line_number, needle, within=0):
            prefix = "\r\n".join(lines[:line_number + 1])
            local = lines[line_number].index(needle, within)
            return utf16_units(prefix) - utf16_units(lines[line_number]) + utf16_units(lines[line_number][:local])

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            capabilities = app.command("getCapabilities")["capabilities"]
            if not capabilities["spellCheck"]:
                self.skipTest("Spell backend is unavailable; a current tagged batch is required for targeting.")

            def wait_for_width_settlement(timeout=20.0):
                deadline = time.monotonic() + timeout
                state = app.command("getEditorWidthState")["editorWidth"]
                while state["measurementPending"] and time.monotonic() < deadline:
                    time.sleep(0.025)
                    state = app.command("getEditorWidthState")["editorWidth"]
                self.assertFalse(state["measurementPending"], state)

            app.command("setWordWrap", enabled=False)
            app.command("setText", text=source)
            wait_for_width_settlement()
            before_65535 = position_of(3000, "teh")
            self.assertLess(before_65535, 65535)
            app.command("sendEditorVerticalScroll", scrollCommand=4, trackPosition=3000)  # SB_THUMBPOSITION

            def select_and_resolve(position, keyboard=False):
                app.command("setSelection", start=position, end=position)
                deadline = time.monotonic() + 3.0
                while time.monotonic() < deadline:
                    if keyboard:
                        resolved = app.command("resolveSpellingContextTarget", keyboard=True)
                    else:
                        point = app.command("getVisibleTextPositionPoint", position=position + 1)
                        resolved = app.command("resolveSpellingContextTarget", x=point["x"], y=point["y"])
                    if resolved["found"]:
                        return resolved
                    time.sleep(0.05)
                self.fail(f"No current spelling context target for position {position}.")

            def assert_target_matches_current_window(target):
                revision = (target["sessionId"], target["contentRevision"], target["layoutRevision"])
                windows = read_native_windows(app)
                self.assertTrue(any(
                    (window["sessionId"], window["contentRevision"], window["layoutRevision"]) == revision and
                    window["start"] + window["eligibleStart"] <= target["start"] and
                    target["start"] + target["length"] <= window["start"] + window["eligibleEnd"]
                    for window in windows
                ), {"target": target, "windows": windows[:3]})

            early = select_and_resolve(before_65535)
            self.assertEqual((early["start"], early["length"], early["text"]),
                (before_65535, 3, "teh"), early)
            assert_target_matches_current_window(early)
            app.command("sendEditorVerticalScroll", scrollCommand=7)  # SB_BOTTOM

            first_target = position_of(7571, "teh")
            second_target = position_of(7575, "teh")
            self.assertGreater(first_target, 65535)
            self.assertGreater(second_target, first_target)

            first = select_and_resolve(first_target)
            self.assertEqual((first["start"], first["length"], first["text"]),
                (first_target, 3, "teh"), first)
            assert_target_matches_current_window(first)

            keyboard = select_and_resolve(first_target + 3, keyboard=True)
            self.assertEqual((keyboard["start"], keyboard["length"], keyboard["text"]),
                (first_target, 3, "teh"), keyboard)
            app.command("sendEditorContextMenu", keyboard=True)

            second = select_and_resolve(second_target)
            self.assertEqual((second["start"], second["length"], second["text"]),
                (second_target, 3, "teh"), second)
            assert_target_matches_current_window(second)
            lookup_before = {
                "selection": app.command("getSelection")["selection"],
                "modified": app.command("isModified")["modified"],
                "metadata": app.command("getDocumentMetadata")["metadata"],
                "viewport": app.command("getEditorWidthState")["editorWidth"],
            }
            repeated_lookup = app.command("resolveSpellingContextTarget",
                x=app.command("getVisibleTextPositionPoint", position=second_target + 1)["x"],
                y=app.command("getVisibleTextPositionPoint", position=second_target + 1)["y"])
            lookup_after = {
                "selection": app.command("getSelection")["selection"],
                "modified": app.command("isModified")["modified"],
                "metadata": app.command("getDocumentMetadata")["metadata"],
                "viewport": app.command("getEditorWidthState")["editorWidth"],
            }
            self.assertEqual((repeated_lookup["start"], repeated_lookup["text"]),
                (second_target, "teh"), repeated_lookup)
            self.assertEqual(lookup_after["selection"], lookup_before["selection"])
            self.assertEqual(lookup_after["modified"], lookup_before["modified"])
            self.assertEqual(lookup_after["metadata"], lookup_before["metadata"])
            for key in ("horizontalOffset", "verticalFirstVisibleLine", "caretPositionAvailable"):
                self.assertEqual(lookup_after["viewport"][key], lookup_before["viewport"][key],
                    (key, lookup_before["viewport"], lookup_after["viewport"]))
            blank = app.command("getVisibleTextPositionPoint", position=second_target + 3)
            self.assertFalse(app.command("resolveSpellingContextTarget", x=blank["x"], y=blank["y"])["found"])
            self.assertFalse(app.command("resolveSpellingContextTarget", x=-1, y=-1)["found"])

            # The third misspelling is preceded by a tab and wide Unicode units.
            # Horizontal scrolling must use native EDIT geometry for the target.
            target_position = position_of(7585, "teh")
            old_geometry_point = app.command(
                "getVisibleTextPositionPoint", position=second_target + 1)
            app.command("setFont", font="Consolas 12")
            stale_layout = app.command("resolveSpellingContextTarget",
                x=old_geometry_point["x"], y=old_geometry_point["y"])
            self.assertFalse(stale_layout["found"], stale_layout)
            wait_for_width_settlement()
            app.command("setSelection", start=target_position, end=target_position)
            app.command("sendEditorVerticalScroll", scrollCommand=7)  # SB_BOTTOM
            app.command("sendEditorHorizontalScroll", scrollCommand=7)  # SB_RIGHT
            point = app.command("getVisibleTextPositionPoint", position=target_position + 1)
            deadline = time.monotonic() + 3.0
            measured = app.command("resolveSpellingContextTarget", x=point["x"], y=point["y"])
            while not measured["found"] and time.monotonic() < deadline:
                time.sleep(0.05)
                measured = app.command("resolveSpellingContextTarget", x=point["x"], y=point["y"])
            self.assertTrue(measured["found"], measured)
            self.assertEqual((measured["start"], measured["text"]), (target_position, "teh"), measured)

            # A wrapped native display row has a separate full-width segment.
            # Resolve the word by its actual character cell after SB_BOTTOM.
            wrapped_target = position_of(7599, "teh")
            app.command("setWordWrap", enabled=True)
            app.command("sendEditorVerticalScroll", scrollCommand=7)  # SB_BOTTOM
            deadline = time.monotonic() + 3.0
            wrapped_result = None
            while time.monotonic() < deadline:
                wrapped_point = app.try_command("getVisibleTextPositionPoint", position=wrapped_target + 1)
                if wrapped_point.get("ok"):
                    wrapped_result = app.command("resolveSpellingContextTarget",
                        x=wrapped_point["x"], y=wrapped_point["y"])
                    if wrapped_result["found"]:
                        break
                time.sleep(0.05)
            self.assertIsNotNone(wrapped_result)
            self.assertTrue(wrapped_result["found"], wrapped_result)
            self.assertEqual((wrapped_result["start"], wrapped_result["text"]),
                (wrapped_target, "teh"), wrapped_result)

            # Theme changes rebuild native editor layout tags. Resolve again
            # from the new measured geometry and verify the same document
            # coordinates in both appearances.
            for theme in ("dark", "light"):
                app.command("setAppearanceTheme", theme=theme)
                app.command("sendEditorVerticalScroll", scrollCommand=7)  # SB_BOTTOM
                deadline = time.monotonic() + 5.0
                themed = None
                while time.monotonic() < deadline:
                    themed_point = app.try_command(
                        "getVisibleTextPositionPoint", position=wrapped_target + 1)
                    if themed_point.get("ok"):
                        themed = app.command("resolveSpellingContextTarget",
                            x=themed_point["x"], y=themed_point["y"])
                        if themed["found"]:
                            break
                    time.sleep(0.05)
                self.assertIsNotNone(themed)
                self.assertEqual((themed["start"], themed["length"], themed["text"]),
                    (wrapped_target, 3, "teh"), {"theme": theme, "target": themed})

            # A same-size replacement clears the tagged batch before a stale
            # point can be used to resolve an unrelated spelling target.
            app.command("setSelection", start=target_position, end=target_position + 3)
            app.command("insertText", text="the")
            stale = app.command("resolveSpellingContextTarget", x=point["x"], y=point["y"])
            self.assertFalse(stale["found"], stale)

    @unittest.skipUnless(automation_platform() == "windows", "Windows context popup smoke is native-only")
    def test_native_context_menu_popup_dismisses_without_mutating_document(self):
        source = "ordinary text with teh spelling issue and emoji 😀"
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            if not app.command("getCapabilities")["capabilities"]["spellCheck"]:
                self.skipTest("Spell backend is unavailable; popup smoke is capability-dependent.")
            app.command("setWordWrap", enabled=False)
            app.command("setText", text=source)
            position = utf16_units("ordinary text with ")
            app.command("setSelection", start=position, end=position)
            point = app.command("getVisibleTextPositionPoint", position=position + 1)
            deadline = time.monotonic() + 3.0
            resolved = app.command("resolveSpellingContextTarget", x=point["x"], y=point["y"])
            while not resolved["found"] and time.monotonic() < deadline:
                time.sleep(0.05)
                resolved = app.command("resolveSpellingContextTarget", x=point["x"], y=point["y"])
            self.assertTrue(resolved["found"], resolved)
            before = {
                "text": app.command("getText")["text"],
                "selection": app.command("getSelection")["selection"],
                "modified": app.command("isModified")["modified"],
                "metadata": app.command("getDocumentMetadata")["metadata"],
                "viewport": app.command("getEditorWidthState")["editorWidth"],
            }
            app.command("sendEditorContextMenu", x=point["x"], y=point["y"])
            after = {
                "text": app.command("getText")["text"],
                "selection": app.command("getSelection")["selection"],
                "modified": app.command("isModified")["modified"],
                "metadata": app.command("getDocumentMetadata")["metadata"],
                "viewport": app.command("getEditorWidthState")["editorWidth"],
            }
            self.assertEqual(after, before)

    def test_native_horizontal_scroll_extracts_a_bounded_exact_segment(self):
        source = "head-" + ("W" * 120000) + "-tail 😀"
        source_units = source.encode("utf-16-le", errors="surrogatepass")

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setWordWrap", enabled=False)
            app.command("setText", text=source)
            before = read_native_windows(app)[0]
            app.command("sendEditorHorizontalScroll", scrollCommand=7)  # SB_RIGHT
            windows = read_native_windows(app)
            self.assertTrue(all(
                window["layoutRevision"] >= before["layoutRevision"] for window in windows
            ), windows[:2])
            self.assertLessEqual(sum(utf16_units(window["text"]) for window in windows), 96 * 1024)
            self.assertTrue(any(window["start"] > 0 for window in windows), windows[:2])
            for window in windows:
                start = window["start"]
                expected = source_units[start * 2:(start + utf16_units(window["text"])) * 2]
                self.assertEqual(window["text"].encode("utf-16-le", errors="surrogatepass"), expected)

    def test_native_display_row_seam_never_splits_an_emoji_surrogate_pair(self):
        source = "A" * 1023 + "😀" + " B " * 1000
        source_bytes = source.encode("utf-16-le", errors="surrogatepass")
        source_units = [
            int.from_bytes(source_bytes[index:index + 2], "little")
            for index in range(0, len(source_bytes), 2)
        ]

        def is_high_surrogate(unit):
            return 0xD800 <= unit <= 0xDBFF

        def is_low_surrogate(unit):
            return 0xDC00 <= unit <= 0xDFFF

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setWordWrap", enabled=False)
            app.command("resizeEditorViewport", width=300, height=300)
            app.command("setText", text=source)
            app.command("setSelection", start=1025, end=1025)
            windows = read_native_windows(app)

            emoji_window_starts = []
            for window in windows:
                start = window["start"]
                text_bytes = window["text"].encode("utf-16-le", errors="surrogatepass")
                actual_units = [
                    int.from_bytes(text_bytes[index:index + 2], "little")
                    for index in range(0, len(text_bytes), 2)
                ]
                end = start + len(actual_units)
                expected_bytes = source_bytes[start * 2:end * 2]
                self.assertEqual(text_bytes, expected_bytes)
                has_emoji_pair = any(
                    left == 0xD83D and right == 0xDE00
                    for left, right in zip(actual_units, actual_units[1:])
                )
                if has_emoji_pair:
                    emoji_window_starts.append(start)

                if start > 0 and actual_units and is_high_surrogate(source_units[start - 1]):
                    self.assertFalse(is_low_surrogate(actual_units[0]), window)
                if end < len(source_units) and actual_units:
                    self.assertFalse(
                        is_high_surrogate(actual_units[-1]) and is_low_surrogate(source_units[end]),
                        window,
                    )

                for eligible_boundary in (window["eligibleStart"], window["eligibleEnd"]):
                    absolute_boundary = start + eligible_boundary
                    if 0 < absolute_boundary < len(source_units):
                        self.assertFalse(
                            is_high_surrogate(source_units[absolute_boundary - 1]) and
                            is_low_surrogate(source_units[absolute_boundary]),
                            window,
                        )

            self.assertIn(1023, emoji_window_starts, windows[:3])

    def test_native_wrapping_emits_separate_display_row_windows(self):
        lines = [f"row-{index:02d} café 漢字 😀 " + "word " * 180 for index in range(24)]
        source = "\r\n".join(lines)
        source_units = source.encode("utf-16-le", errors="surrogatepass")

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setFont", font="Consolas 11")
            app.command("resizeEditorViewport", width=560, height=500)
            app.command("setWordWrap", enabled=True)
            app.command("setText", text=source)
            windows = read_native_windows(app)

            self.assertGreater(len(windows), 1)
            self.assertTrue(all("\r" not in window["text"] and "\n" not in window["text"]
                for window in windows), windows[:3])
            self.assertTrue(all(
                (window["sessionId"], window["contentRevision"], window["layoutRevision"])
                == (windows[0]["sessionId"], windows[0]["contentRevision"], windows[0]["layoutRevision"])
                for window in windows
            ))
            copied_units = sum(utf16_units(window["text"]) for window in windows)
            self.assertLessEqual(copied_units, 96 * 1024)
            for window in windows:
                start = window["start"]
                expected = source_units[start * 2:(start + utf16_units(window["text"])) * 2]
                self.assertEqual(window["text"].encode("utf-16-le", errors="surrogatepass"), expected)

            self.assertTrue(any(
                later["start"] == earlier["start"] + utf16_units(earlier["text"])
                for earlier, later in zip(windows, windows[1:])
            ), "Wrapped display rows should remain separate windows with full contiguous document offsets.")

    def test_native_wrapped_word_fragments_are_ineligible_and_complete_word_remains_eligible(self):
        long_word = "abcdefghij" * 600
        source = long_word + "\r\nleft kept right"

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setFont", font="Consolas 11")
            app.command("resizeEditorViewport", width=300, height=500)
            app.command("setWordWrap", enabled=True)
            app.command("setText", text=source)

            wrapped = read_native_windows(app)
            self.assertGreater(len(wrapped), 1, wrapped[:3])
            self.assertTrue(any(
                later["start"] == earlier["start"] + utf16_units(earlier["text"])
                for earlier, later in zip(wrapped, wrapped[1:])
            ), "The unbroken 6,000-unit word must be split across native display-row windows.")
            self.assertTrue(all(
                window["start"] < len(long_word) and
                window["start"] + utf16_units(window["text"]) <= len(long_word) and
                window["eligibleStart"] == window["eligibleEnd"]
                for window in wrapped
            ), wrapped[:3])

            app.command("sendEditorVerticalScroll", scrollCommand=7)  # SB_BOTTOM
            complete_line = read_native_windows(app)
            kept_window = next(window for window in complete_line if "left kept right" in window["text"])
            kept_offset = kept_window["text"].index("kept")
            self.assertLessEqual(kept_window["eligibleStart"], kept_offset)
            self.assertGreaterEqual(kept_window["eligibleEnd"], kept_offset + len("kept"))

