#pragma once

#include "encoding.h"
#include "line_endings.h"

#include <cstddef>
#include <string>

namespace classic_notepad {

std::wstring FormatEncoding(TextEncoding encoding);
std::wstring FormatLineEnding(LineEndingStyle lineEnding);
std::wstring FormatNumberWithSeparators(std::size_t value);
std::wstring FormatCharacterCount(std::size_t count);
std::size_t CountStatusCharacters(const std::wstring& text);

} // namespace classic_notepad
