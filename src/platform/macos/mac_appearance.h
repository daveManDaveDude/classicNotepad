#pragma once

#include "appearance.h"

#include <string>

#ifdef __OBJC__
@class NSApplication;
@class NSTextView;
@class NSWindow;
#else
class NSApplication;
class NSTextView;
class NSWindow;
#endif

namespace classic_notepad::macos {

struct MacAppearanceConfiguration {
    classic_notepad::AppearanceTheme theme = classic_notepad::AppearanceTheme::System;
    bool darkMode = false;
    std::string effectiveAppearanceName;
};

classic_notepad::AppearanceTheme AppearanceThemeFromEnvironment();
MacAppearanceConfiguration ApplyApplicationAppearance(NSApplication* application, classic_notepad::AppearanceTheme theme);
MacAppearanceConfiguration ApplyWindowAppearance(NSWindow* window, classic_notepad::AppearanceTheme theme);
void ConfigurePlainTextViewAppearance(NSTextView* textView);

} // namespace classic_notepad::macos
