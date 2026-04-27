#include "mac_appearance.h"

#import <AppKit/AppKit.h>

#include <cstdlib>

namespace classic_notepad::macos {
namespace {

std::string StringFromNSString(NSString* value)
{
    if (value == nil) {
        return {};
    }

    const char* utf8 = [value UTF8String];
    return utf8 == nullptr ? std::string() : std::string(utf8);
}

NSAppearance* AppearanceForTheme(classic_notepad::AppearanceTheme theme)
{
    switch (theme) {
    case classic_notepad::AppearanceTheme::Light:
        return [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    case classic_notepad::AppearanceTheme::Dark:
        return [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    case classic_notepad::AppearanceTheme::System:
    default:
        return nil;
    }
}

MacAppearanceConfiguration ConfigurationFromEffectiveAppearance(
    classic_notepad::AppearanceTheme theme,
    NSAppearance* effectiveAppearance)
{
    MacAppearanceConfiguration configuration;
    configuration.theme = theme;

    NSArray<NSAppearanceName>* names = @[
        NSAppearanceNameDarkAqua,
        NSAppearanceNameAqua
    ];
    NSAppearanceName bestMatch = [effectiveAppearance bestMatchFromAppearancesWithNames:names];
    configuration.darkMode = [bestMatch isEqualToString:NSAppearanceNameDarkAqua];
    configuration.effectiveAppearanceName = StringFromNSString(bestMatch);
    return configuration;
}

} // namespace

classic_notepad::AppearanceTheme AppearanceThemeFromEnvironment()
{
    const char* value = std::getenv(classic_notepad::kAppearanceThemeEnvironmentVariable);
    return value == nullptr
        ? classic_notepad::AppearanceTheme::System
        : classic_notepad::ParseAppearanceThemeOrSystem(value);
}

MacAppearanceConfiguration ApplyApplicationAppearance(
    NSApplication* application,
    classic_notepad::AppearanceTheme theme)
{
    if (application == nil) {
        return ConfigurationFromEffectiveAppearance(theme, [NSApp effectiveAppearance]);
    }

    [application setAppearance:AppearanceForTheme(theme)];
    return ConfigurationFromEffectiveAppearance(theme, [application effectiveAppearance]);
}

MacAppearanceConfiguration ApplyWindowAppearance(NSWindow* window, classic_notepad::AppearanceTheme theme)
{
    if (window == nil) {
        return ConfigurationFromEffectiveAppearance(theme, [NSApp effectiveAppearance]);
    }

    [window setAppearance:AppearanceForTheme(theme)];
    return ConfigurationFromEffectiveAppearance(theme, [window effectiveAppearance]);
}

void ConfigurePlainTextViewAppearance(NSTextView* textView)
{
    if (textView == nil) {
        return;
    }

    [textView setDrawsBackground:YES];
    [textView setBackgroundColor:[NSColor textBackgroundColor]];
    [textView setTextColor:[NSColor textColor]];
    [textView setInsertionPointColor:[NSColor textColor]];
}

} // namespace classic_notepad::macos
