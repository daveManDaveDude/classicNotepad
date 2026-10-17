#pragma once

#include "large_text_width_summary.h"

#include <cstdint>
#include <string>

namespace classic_notepad {

// Fixed-size summary of a text span used by LargeDocument's sequence tree.
// Positions are UTF-16 code units; displayed-character counts use the status
// semantics where a surrogate pair and CRLF each count as one character.
struct LargeTextSummary {
    std::uint64_t characters = 0;
    std::uint64_t displayedCharacters = 0;
    bool displayedCharactersKnown = false;
    std::uint64_t lineBreaks = 0;
    bool empty = true;
    wchar_t first = 0;
    wchar_t last = 0;
    LargeFileWidthSummary width;
};

LargeTextSummary CombineLargeTextSummaries(
    const LargeTextSummary& left,
    const LargeTextSummary& right);

LargeTextSummary SummarizeLargeText(const std::wstring& text);

} // namespace classic_notepad
