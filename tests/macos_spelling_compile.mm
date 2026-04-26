#include "platform/macos/mac_spelling.h"

#include <iostream>

int main()
{
    const std::string language = classic_notepad::macos::SelectBritishEnglishSpellLanguage();
    std::cout << "macOS British English spelling language: "
              << (language.empty() ? "<missing>" : language)
              << '\n';
    return 0;
}
