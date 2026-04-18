#pragma once

#include <cstddef>
#include <string>

namespace classic_notepad {

struct TextRange {
    std::size_t start = 0;
    std::size_t length = 0;
};

bool ExpandWordRangeAt(const std::wstring& text, std::size_t index, TextRange& range);
bool RangesOverlap(std::size_t firstStart, std::size_t firstLength, std::size_t secondStart, std::size_t secondLength);

} // namespace classic_notepad
