#include "windows_menu_text.h"

#include <cwctype>

namespace classic_notepad::windows {

std::wstring StripMenuMnemonics(const std::wstring& text)
{
    std::wstring stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'&') {
            if (index + 1U < text.size() && text[index + 1U] == L'&') {
                stripped.push_back(L'&');
                ++index;
            }
            continue;
        }

        stripped.push_back(text[index]);
    }

    return stripped;
}

wchar_t FindMenuMnemonic(const std::wstring& text)
{
    for (std::size_t index = 0; index + 1U < text.size(); ++index) {
        if (text[index] == L'&' && text[index + 1U] != L'&') {
            return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(text[index + 1U])));
        }

        if (text[index] == L'&') {
            ++index;
        }
    }

    return L'\0';
}

void SplitMenuItemText(const std::wstring& text, std::wstring& label, std::wstring& accelerator)
{
    const std::size_t separator = text.find(L'\t');
    if (separator == std::wstring::npos) {
        label = StripMenuMnemonics(text);
        accelerator.clear();
        return;
    }

    label = StripMenuMnemonics(text.substr(0, separator));
    accelerator = StripMenuMnemonics(text.substr(separator + 1U));
}

} // namespace classic_notepad::windows
