import time
import unittest

from automation_context import automation_binary, automation_platform
from classic_notepad_driver import ClassicNotepadDriver


def wait_for_width_index(app, timeout=8.0):
    deadline = time.monotonic() + timeout
    state = app.command("getEditorWidthState")["editorWidth"]
    while state["measurementPending"] and time.monotonic() < deadline:
        time.sleep(0.02)
        state = app.command("getEditorWidthState")["editorWidth"]
    if state["measurementPending"]:
        raise AssertionError(f"Width index did not finish its bounded rebuild: {state}")
    if (state["lastBatchDiscoveredDisplayLines"] > 96 or
            state["lastBatchPublishedDisplayLines"] > 96 or
            state["lastBatchDiscoveredTextUnits"] + state["lastBatchMeasuredTextUnits"] > 64 * 1024 or
            state["maximumBatchDiscoveredDisplayLines"] > 96 or
            state["maximumBatchPublishedDisplayLines"] > 96 or
            state["maximumBatchTextUnits"] > 64 * 1024):
        raise AssertionError(f"Width-index batch exceeded its work budget: {state}")
    return state


def assert_reachable_horizontal_anchor(test_case, state, anchor_pixels):
    tolerance = max(1, state["horizontalScrollUnit"])
    test_case.assertLessEqual(abs(state["horizontalAnchorPixels"] - anchor_pixels), tolerance, state)
    if state["nativeHorizontalScrollInfoAvailable"]:
        test_case.assertLessEqual(
            abs(state["nativeHorizontalOffset"] - anchor_pixels), tolerance, state
        )
        test_case.assertLessEqual(
            abs(state["customHorizontalOffsetPixels"] - state["nativeHorizontalOffset"]),
            tolerance,
            state,
        )


@unittest.skipUnless(automation_platform() == "windows", "Windows EDIT horizontal extent regression")
class WindowsEditorHorizontalExtentTests(unittest.TestCase):
    def test_below_threshold_offscreen_wide_row_survives_layout_changes(self):
        wide_row_index = 3200
        rows = ["i" * 32 if index % 2 == 0 else ("W" * 8 + "i" * 24) for index in range(4000)]
        rows[wide_row_index] = "W" * 6000
        text = "\r\n".join(rows)
        wide_row_start = sum(len(row) + 2 for row in rows[:wide_row_index])

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setAppearanceTheme", theme="light")
            app.command("setFont", font="Arial 11")
            app.command("setText", text=text)
            ready = wait_for_width_index(app)
            self.assertLess(len(text), 256 * 1024)
            self.assertTrue(ready["horizontalScrollBarVisible"], ready)
            self.assertGreater(ready["documentWidth"], ready["viewportWidth"], ready)

            app.command("setSelection", start=0, end=0)
            top = app.command("getEditorWidthState")["editorWidth"]
            app.command("setSelection", start=wide_row_start, end=wide_row_start)
            at_wide_row = app.command("getEditorWidthState")["editorWidth"]
            app.command("setSelection", start=0, end=0)
            back_at_top = app.command("getEditorWidthState")["editorWidth"]
            for state in (top, at_wide_row, back_at_top):
                self.assertTrue(state["horizontalScrollBarVisible"], state)
                self.assertEqual(state["documentWidth"], ready["documentWidth"], state)
                self.assertEqual(
                    state["documentHorizontalRangeMaximumOffset"],
                    ready["documentHorizontalRangeMaximumOffset"], state
                )

            app.command("sendEditorHorizontalScroll", scrollCommand=1)  # SB_LINERIGHT
            anchor_state = app.command("getEditorWidthState")["editorWidth"]
            anchor = anchor_state["horizontalAnchorPixels"]
            self.assertGreater(anchor, 0, anchor_state)
            self.assertEqual(anchor_state["nativeHorizontalOffset"], 8, anchor_state)
            self.assertEqual(anchor_state["horizontalScrollUnit"], 7, anchor_state)
            self.assertLessEqual(
                abs(anchor_state["customHorizontalOffsetPixels"] - anchor_state["nativeHorizontalOffset"]),
                anchor_state["horizontalScrollUnit"], anchor_state
            )
            assert_reachable_horizontal_anchor(self, anchor_state, anchor)
            generation = ready["generation"]
            batches = ready["measurementBatches"]
            for width, theme in ((980, "dark"), (720, "light"), (1050, "dark"), (820, "light")):
                resized = app.command("resizeEditorViewport", width=width, height=720)["editorWidth"]
                self.assertTrue(resized["horizontalScrollBarVisible"], resized)
                self.assertEqual(resized["documentWidth"], ready["documentWidth"], resized)
                self.assertEqual(
                    resized["documentHorizontalRangeMaximumOffset"],
                    ready["documentHorizontalRangeMaximumOffset"], resized
                )
                assert_reachable_horizontal_anchor(self, resized, anchor)
                app.command("setAppearanceTheme", theme=theme)
                state = app.command("getEditorWidthState")["editorWidth"]
                self.assertTrue(state["horizontalScrollBarVisible"], state)
                self.assertEqual(state["documentWidth"], ready["documentWidth"], state)
                self.assertEqual(
                    state["documentHorizontalRangeMaximumOffset"],
                    ready["documentHorizontalRangeMaximumOffset"], state
                )
                assert_reachable_horizontal_anchor(self, state, anchor)
                self.assertEqual(state["generation"], generation, state)
                self.assertEqual(state["measurementBatches"], batches, state)

    def test_offscreen_width_survives_scroll_resize_theme_and_shrinks_after_delete(self):
        wide_row = "W" * 300000
        short_rows = ["short row with more text"] * 12000
        text = "\r\n".join([wide_row, *short_rows])
        width_sample = "W" * 128

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setAppearanceTheme", theme="light")
            app.command("setText", text=width_sample)
            sample = wait_for_width_index(app)
            app.command("setText", text=text)
            ready = wait_for_width_index(app)
            self.assertGreater(len(text), 256 * 1024)
            self.assertTrue(ready["horizontalScrollBarVisible"], ready)
            self.assertGreater(ready["documentWidth"], ready["viewportWidth"], ready)
            self.assertGreater(ready["conservativeLineEstimates"], 0, ready)
            self.assertGreaterEqual(
                ready["documentWidth"], sample["documentWidth"] * len(wide_row) // len(width_sample), ready
            )
            self.assertGreater(ready["editDisplayLineCount"], len(short_rows), ready)
            self.assertLess(ready["maximumEditDisplayLineLength"], len(wide_row), ready)

            app.command("setSelection", start=len(text), end=len(text))
            scrolled = app.command("getEditorWidthState")["editorWidth"]
            self.assertTrue(scrolled["horizontalScrollBarVisible"], scrolled)

            app.command("sendEditorHorizontalScroll", scrollCommand=7)  # SB_RIGHT
            at_right = app.command("getEditorWidthState")["editorWidth"]
            self.assertGreater(at_right["horizontalOffset"], 0, at_right)
            self.assertGreaterEqual(
                at_right["nativeHorizontalOffset"], at_right["nativeHorizontalMaximumOffset"] - 1, at_right
            )
            self.assertEqual(at_right["horizontalOffset"], at_right["nativeHorizontalOffset"], at_right)
            self.assertTrue(at_right["nativeHorizontalScrollInfoAvailable"], at_right)
            # Track EDIT's live range separately from the document-level C1 estimate.
            self.assertGreater(
                at_right["documentHorizontalRangeMaximumOffset"],
                at_right["nativeHorizontalMaximumOffset"], at_right
            )
            self.assertGreater(at_right["horizontalMaximumOffset"], at_right["nativeHorizontalMaximumOffset"], at_right)
            app.command("sendEditorHorizontalScroll", scrollCommand=6)  # SB_LEFT
            app.command("sendEditorHorizontalScroll", scrollCommand=1)  # SB_LINERIGHT
            anchor_state = app.command("getEditorWidthState")["editorWidth"]
            anchor = anchor_state["horizontalAnchorPixels"]
            self.assertGreater(anchor, 0, anchor_state)

            stable_generation = anchor_state["generation"]
            stable_batches = anchor_state["measurementBatches"]
            for width, theme in ((980, "dark"), (720, "light"), (1050, "dark"), (820, "light")):
                app.command("resizeEditorViewport", width=width, height=720)
                app.command("setAppearanceTheme", theme=theme)
                state = app.command("getEditorWidthState")["editorWidth"]
                self.assertTrue(state["horizontalScrollBarVisible"], state)
                assert_reachable_horizontal_anchor(self, state, anchor)
                self.assertEqual(state["generation"], stable_generation, state)
                self.assertEqual(state["measurementBatches"], stable_batches, state)

            app.command("setSelection", start=0, end=len(wide_row) + 2)
            app.command("deleteSelection")
            shrunk = wait_for_width_index(app)
            self.assertFalse(shrunk["horizontalScrollBarVisible"], shrunk)
            self.assertEqual(shrunk["horizontalMaximumOffset"], 0, shrunk)

            app.command("undo")
            restored = wait_for_width_index(app)
            self.assertTrue(restored["horizontalScrollBarVisible"], restored)
            self.assertGreater(restored["documentWidth"], restored["viewportWidth"], restored)

    def test_local_typing_updates_only_affected_row_and_proportional_metrics(self):
        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setAppearanceTheme", theme="dark")
            app.command("setFont", font="Arial 11")
            app.command("setText", text="i" * 800)
            narrow = wait_for_width_index(app)
            app.command("setText", text="W" * 800)
            wide = wait_for_width_index(app)
            self.assertGreater(wide["documentWidth"], narrow["documentWidth"], wide)

            before = wide["measuredDisplayLines"]
            before_local_lines = wide["localEditMeasuredDisplayLines"]
            before_local_units = wide["localEditMeasuredTextUnits"]
            app.command("setSelection", start=800, end=800)
            app.command("insertText", text="X")
            after = app.command("getEditorWidthState")["editorWidth"]
            self.assertLessEqual(after["measuredDisplayLines"] - before, 2, after)
            self.assertEqual(after["localEditMeasuredDisplayLines"] - before_local_lines, 1, after)
            self.assertLessEqual(after["localEditMeasuredTextUnits"] - before_local_units, 64 * 1024, after)
            self.assertEqual(after["generation"], wide["generation"], after)
            self.assertFalse(after["measurementPending"], after)

            app.command("setText", text="W" * 201)
            without_tabs = wait_for_width_index(app)
            app.command("setText", text=("W\t" * 100) + "W")
            tabbed = wait_for_width_index(app)
            self.assertGreater(tabbed["documentWidth"], without_tabs["documentWidth"], tabbed)
            self.assertGreater(tabbed["documentWidth"], tabbed["viewportWidth"], tabbed)

    def test_font_change_rejects_pending_long_row_batches(self):
        long_row = "W" * 300000
        width_sample = "W" * 128

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setAppearanceTheme", theme="light")
            app.command("setFont", font="Arial 11")
            app.command("setText", text=long_row)
            pending = app.command("getEditorWidthState")["editorWidth"]
            self.assertTrue(pending["measurementPending"], pending)
            self.assertGreater(pending["editDisplayLineCount"], 96, pending)

            app.command("setFont", font="Consolas 14")
            restarted = app.command("getEditorWidthState")["editorWidth"]
            self.assertGreater(restarted["generation"], pending["generation"], restarted)
            self.assertTrue(restarted["measurementPending"], restarted)
            ready = wait_for_width_index(app, timeout=20.0)
            self.assertEqual(ready["generation"], restarted["generation"], ready)
            self.assertLessEqual(ready["lastBatchDiscoveredDisplayLines"], 96, ready)
            self.assertLessEqual(ready["lastBatchPublishedDisplayLines"], 96, ready)
            self.assertLessEqual(
                ready["lastBatchDiscoveredTextUnits"] + ready["lastBatchMeasuredTextUnits"], 64 * 1024, ready
            )
            self.assertLessEqual(ready["maximumBatchDiscoveredDisplayLines"], 96, ready)
            self.assertLessEqual(ready["maximumBatchPublishedDisplayLines"], 96, ready)
            self.assertLessEqual(ready["maximumBatchTextUnits"], 64 * 1024, ready)
            settled_width = ready["documentWidth"]
            time.sleep(0.15)
            settled = app.command("getEditorWidthState")["editorWidth"]
            self.assertEqual(settled["generation"], restarted["generation"], settled)
            self.assertEqual(settled["documentWidth"], settled_width, settled)

            app.command("setText", text=width_sample)
            consolas_sample = wait_for_width_index(app)
            self.assertGreaterEqual(
                settled_width, consolas_sample["documentWidth"] * len(long_row) // len(width_sample), settled
            )

    def test_192_display_line_finalization_gets_an_extra_batch(self):
        long_row = "W" * 196000

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setAppearanceTheme", theme="light")
            app.command("setFont", font="Arial 11")
            app.command("setText", text=long_row)
            pending = app.command("getEditorWidthState")["editorWidth"]
            self.assertEqual(pending["editDisplayLineCount"], 192, pending)
            self.assertTrue(pending["measurementPending"], pending)

            ready = wait_for_width_index(app, timeout=3.0)
            self.assertFalse(ready["measurementPending"], ready)
            self.assertLessEqual(ready["maximumBatchDiscoveredDisplayLines"], 96, ready)
            self.assertLessEqual(ready["maximumBatchPublishedDisplayLines"], 96, ready)
            self.assertLessEqual(ready["maximumBatchTextUnits"], 64 * 1024, ready)
            self.assertGreaterEqual(ready["measurementBatches"], 3, ready)
            self.assertGreater(ready["documentWidth"], ready["viewportWidth"], ready)

    def test_edit_invalidates_pending_long_row_rebuild(self):
        long_row = "W" * 300000
        inserted = "W" * 8192
        width_sample = "W" * 128

        with ClassicNotepadDriver(automation_binary(), "windows") as app:
            app.command("setAppearanceTheme", theme="light")
            app.command("setFont", font="Arial 11")
            app.command("setText", text=width_sample)
            sample = wait_for_width_index(app)
            app.command("setText", text=long_row)
            pending = app.command("getEditorWidthState")["editorWidth"]
            self.assertTrue(pending["measurementPending"], pending)
            self.assertGreater(pending["editDisplayLineCount"], 96, pending)

            app.command("setSelection", start=len(long_row), end=len(long_row))
            app.command("insertText", text=inserted)
            restarted = app.command("getEditorWidthState")["editorWidth"]
            self.assertGreater(restarted["generation"], pending["generation"], restarted)
            self.assertTrue(restarted["measurementPending"], restarted)

            ready = wait_for_width_index(app, timeout=20.0)
            self.assertEqual(ready["generation"], restarted["generation"], ready)
            self.assertLessEqual(ready["maximumBatchDiscoveredDisplayLines"], 96, ready)
            self.assertLessEqual(ready["maximumBatchPublishedDisplayLines"], 96, ready)
            self.assertLessEqual(ready["maximumBatchTextUnits"], 64 * 1024, ready)
            self.assertEqual(app.command("getText")["text"], long_row + inserted)
            self.assertGreaterEqual(
                ready["documentWidth"], sample["documentWidth"] * (len(long_row) + len(inserted)) // len(width_sample), ready
            )
            settled_width = ready["documentWidth"]
            time.sleep(0.15)
            settled = app.command("getEditorWidthState")["editorWidth"]
            self.assertEqual(settled["generation"], restarted["generation"], settled)
            self.assertEqual(settled["documentWidth"], settled_width, settled)


if __name__ == "__main__":
    unittest.main()
