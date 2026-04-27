import os
import unittest
from contextlib import contextmanager

from automation_context import automation_binary, automation_platform
from classic_notepad_driver import ClassicNotepadDriver


@contextmanager
def theme_override(value):
    previous = os.environ.get("CLASSIC_NOTEPAD_THEME")
    if value is None:
        os.environ.pop("CLASSIC_NOTEPAD_THEME", None)
    else:
        os.environ["CLASSIC_NOTEPAD_THEME"] = value

    try:
        yield
    finally:
        if previous is None:
            os.environ.pop("CLASSIC_NOTEPAD_THEME", None)
        else:
            os.environ["CLASSIC_NOTEPAD_THEME"] = previous


@contextmanager
def visible_automation_window():
    previous_visible = os.environ.get("CLASSIC_NOTEPAD_AUTOMATION_VISIBLE")
    previous_delay = os.environ.get("CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY")
    os.environ["CLASSIC_NOTEPAD_AUTOMATION_VISIBLE"] = "1"
    os.environ["CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY"] = "0.05"
    try:
        yield
    finally:
        if previous_visible is None:
            os.environ.pop("CLASSIC_NOTEPAD_AUTOMATION_VISIBLE", None)
        else:
            os.environ["CLASSIC_NOTEPAD_AUTOMATION_VISIBLE"] = previous_visible

        if previous_delay is None:
            os.environ.pop("CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY", None)
        else:
            os.environ["CLASSIC_NOTEPAD_AUTOMATION_STEP_DELAY"] = previous_delay


class AppearanceThemeTests(unittest.TestCase):
    def test_theme_override_is_reported_in_capabilities(self):
        for theme in ("dark", "light", "system"):
            with self.subTest(theme=theme), theme_override(theme):
                with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                    capabilities = app.command("getCapabilities")["capabilities"]
                    self.assertEqual(capabilities["appearanceTheme"], theme)
                    self.assertIn(capabilities["effectiveAppearance"], ("dark", "light"))
                    self.assertIsInstance(capabilities["darkMode"], bool)

                    if theme == "dark" and not capabilities["highContrast"]:
                        self.assertTrue(capabilities["darkMode"])
                        self.assertEqual(capabilities["effectiveAppearance"], "dark")
                    if theme == "light":
                        self.assertFalse(capabilities["darkMode"])
                        self.assertEqual(capabilities["effectiveAppearance"], "light")

    def test_invalid_theme_override_falls_back_to_system(self):
        with theme_override("not-a-theme"):
            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                appearance = app.command("getAppearance")["appearance"]
                self.assertEqual(appearance["theme"], "system")
                self.assertIn(appearance["effectiveAppearance"], ("dark", "light"))

    def test_appearance_theme_can_change_in_automation(self):
        with theme_override("system"):
            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                dark = app.command("setAppearanceTheme", theme="dark")["appearance"]
                self.assertEqual(dark["theme"], "dark")
                if not dark["highContrast"]:
                    self.assertTrue(dark["darkMode"])
                    self.assertEqual(dark["effectiveAppearance"], "dark")

                light = app.command("setAppearanceTheme", theme="light")["appearance"]
                self.assertEqual(light["theme"], "light")
                self.assertFalse(light["darkMode"])
                self.assertEqual(light["effectiveAppearance"], "light")

                system = app.command("setAppearanceTheme", theme="system")["appearance"]
                self.assertEqual(system["theme"], "system")

                invalid = app.try_command("setAppearanceTheme", theme="sepia")
                self.assertFalse(invalid["ok"])

    def test_linux_appearance_menu_dismisses_after_theme_selection(self):
        if automation_platform() != "linux":
            self.skipTest("GTK menu popover dismissal is Linux-specific")

        with theme_override("light"), visible_automation_window():
            with ClassicNotepadDriver(automation_binary(), automation_platform()) as app:
                view = app.command("activateMenuLabel", label="View")
                self.assertTrue(view["activated"])
                self.assertGreater(view["openPopovers"], 0)

                appearance = app.command("activateMenuLabel", label="Appearance")
                self.assertTrue(appearance["activated"])
                self.assertGreaterEqual(appearance["openPopovers"], 1)

                dark = app.command("activateMenuLabel", label="Dark")
                self.assertTrue(dark["activated"])
                self.assertEqual(dark["appearance"]["theme"], "dark")
                self.assertEqual(dark["openPopovers"], 0)

                self.assertEqual(app.command("getOpenMenuPopoverCount")["openPopovers"], 0)


if __name__ == "__main__":
    unittest.main()
