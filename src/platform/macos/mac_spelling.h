#pragma once

#include "spelling.h"

#include <string>

#ifdef __OBJC__
@class NSTextView;
#else
class NSTextView;
#endif

namespace classic_notepad::macos {

struct MacSpellingConfiguration {
    classic_notepad::SpellCapability capability = classic_notepad::SpellCapability::MissingDictionary;
    std::string languageCode;
};

MacSpellingConfiguration ConfigureTextViewSpelling(NSTextView* textView);
std::string SelectBritishEnglishSpellLanguage();

} // namespace classic_notepad::macos
