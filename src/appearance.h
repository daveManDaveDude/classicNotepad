#pragma once

#include <optional>
#include <string_view>

namespace classic_notepad {

enum class AppearanceTheme {
    System,
    Light,
    Dark
};

constexpr const char* kAppearanceThemeEnvironmentVariable = "CLASSIC_NOTEPAD_THEME";

std::optional<AppearanceTheme> TryParseAppearanceTheme(std::string_view value);
AppearanceTheme ParseAppearanceThemeOrSystem(std::string_view value);
const char* AppearanceThemeName(AppearanceTheme theme);
const wchar_t* AppearanceThemeLabel(AppearanceTheme theme);
bool ResolveDarkMode(AppearanceTheme theme, bool systemPrefersDark, bool highContrast);

} // namespace classic_notepad
