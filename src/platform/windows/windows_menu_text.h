#pragma once

#include <string>

namespace classic_notepad::windows {

std::wstring StripMenuMnemonics(const std::wstring& text);
wchar_t FindMenuMnemonic(const std::wstring& text);
void SplitMenuItemText(const std::wstring& text, std::wstring& label, std::wstring& accelerator);

} // namespace classic_notepad::windows
