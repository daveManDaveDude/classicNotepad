#include "platform/macos/mac_appearance.h"
#include "platform/macos/mac_spelling.h"

#import <AppKit/AppKit.h>

#include <iostream>

int main()
{
    const std::string language = classic_notepad::macos::SelectBritishEnglishSpellLanguage();
    const classic_notepad::AppearanceTheme theme = classic_notepad::macos::AppearanceThemeFromEnvironment();
    const classic_notepad::macos::MacAppearanceConfiguration appearance =
        classic_notepad::macos::ApplyApplicationAppearance(NSApp, theme);
    std::cout << "macOS British English spelling language: "
              << (language.empty() ? "<missing>" : language)
              << '\n';
    std::cout << "macOS appearance theme: "
              << classic_notepad::AppearanceThemeName(appearance.theme)
              << ", effective: "
              << (appearance.effectiveAppearanceName.empty() ? "<unknown>" : appearance.effectiveAppearanceName)
              << '\n';
    return 0;
}
