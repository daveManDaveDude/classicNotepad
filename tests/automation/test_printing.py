import tempfile
import unittest
from pathlib import Path

from automation_context import automation_binary, automation_platform, automation_temp_root
from classic_notepad_driver import ClassicNotepadDriver


class PrintingTests(unittest.TestCase):
    def test_page_setup_and_print_sink_include_text_font_and_margins(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            sink_path = Path(temp_root) / "print-sink.txt"

            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                capabilities = app.command("getCapabilities")["capabilities"]
                if not capabilities["printing"]:
                    self.skipTest("printing automation is not available on this platform")

                app.command("setText", text="first line\r\nsecond line")
                app.command("setFont", font="Consolas 14")

                if capabilities["pageSetup"]:
                    response = app.command("pageSetup", left=1000, top=1250, right=1000, bottom=1250)
                    self.assertEqual(
                        response["margins"],
                        {"left": 1000, "top": 1250, "right": 1000, "bottom": 1250},
                    )

                response = app.command("printToTestSink", path=str(sink_path))
                self.assertEqual(response["pages"], 1)
                self.assertEqual(response["font"], app.command("getFont")["font"])

            sink_text = sink_path.read_text(encoding="utf-8")
            self.assertIn("Classic Notepad Print Sink", sink_text)
            self.assertIn(f"Platform: {automation_platform()}", sink_text)
            self.assertIn("Font: Consolas 14", sink_text)
            self.assertIn("Margins (thousandths inch): 1000,1250,1000,1250", sink_text)
            self.assertIn("first line", sink_text)
            self.assertIn("second line", sink_text)

    def test_empty_document_print_sink_is_valid(self):
        with tempfile.TemporaryDirectory(dir=automation_temp_root()) as temp_root:
            sink_path = Path(temp_root) / "empty-print-sink.txt"

            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                capabilities = app.command("getCapabilities")["capabilities"]
                if not capabilities["printing"]:
                    self.skipTest("printing automation is not available on this platform")

                response = app.command("printToTestSink", path=str(sink_path))
                self.assertEqual(response["pages"], 1)

            sink_text = sink_path.read_text(encoding="utf-8")
            self.assertIn("Pages: 1", sink_text)
            self.assertIn("--- Page 1 ---", sink_text)


if __name__ == "__main__":
    unittest.main()
