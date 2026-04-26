#include "appearance.h"

#include <string>

namespace classic_notepad {
namespace {

char ToAsciiLower(char value)
{
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

std::string TrimAndLower(std::string_view value)
{
    std::size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first && (value[last - 1U] == ' ' || value[last - 1U] == '\t' || value[last - 1U] == '\r' || value[last - 1U] == '\n')) {
        --last;
    }

    std::string normalized;
    normalized.reserve(last - first);
    for (std::size_t index = first; index < last; ++index) {
        normalized.push_back(ToAsciiLower(value[index]));
    }
    return normalized;
}

} // namespace

std::optional<AppearanceTheme> TryParseAppearanceTheme(std::string_view value)
{
    const std::string normalized = TrimAndLower(value);
    if (normalized == "system") {
        return AppearanceTheme::System;
    }

    if (normalized == "light") {
        return AppearanceTheme::Light;
    }

    if (normalized == "dark") {
        return AppearanceTheme::Dark;
    }

    return std::nullopt;
}

AppearanceTheme ParseAppearanceThemeOrSystem(std::string_view value)
{
    return TryParseAppearanceTheme(value).value_or(AppearanceTheme::System);
}

const char* AppearanceThemeName(AppearanceTheme theme)
{
    switch (theme) {
    case AppearanceTheme::Light:
        return "light";
    case AppearanceTheme::Dark:
        return "dark";
    case AppearanceTheme::System:
    default:
        return "system";
    }
}

const wchar_t* AppearanceThemeLabel(AppearanceTheme theme)
{
    switch (theme) {
    case AppearanceTheme::Light:
        return L"Light";
    case AppearanceTheme::Dark:
        return L"Dark";
    case AppearanceTheme::System:
    default:
        return L"System";
    }
}

bool ResolveDarkMode(AppearanceTheme theme, bool systemPrefersDark, bool highContrast)
{
    if (highContrast) {
        return false;
    }

    switch (theme) {
    case AppearanceTheme::Light:
        return false;
    case AppearanceTheme::Dark:
        return true;
    case AppearanceTheme::System:
    default:
        return systemPrefersDark;
    }
}

} // namespace classic_notepad
