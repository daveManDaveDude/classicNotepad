#pragma once

#include <array>
#include <cstdint>

namespace classic_notepad {

// Fixed-size tab-aware summary for text widths measured in UTF-16 code units.
// Prefix widths are indexed by the incoming eight-column tab-stop residue.
struct LargeFileWidthSummary {
    std::array<std::uint64_t, 8> prefixColumns{};
    std::array<std::uint8_t, 8> prefixResidues{{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}};
    std::uint64_t suffixColumns = 0;
    std::uint8_t suffixResidue = 0;
    std::uint64_t maximumLineColumns = 0;
    bool hasLineBreak = false;
};

void AppendLargeTextWidthCharacter(
    LargeFileWidthSummary& summary,
    bool& prefixOpen,
    wchar_t character) noexcept;

LargeFileWidthSummary CombineLargeTextWidthSummaries(
    const LargeFileWidthSummary& left,
    const LargeFileWidthSummary& right) noexcept;

} // namespace classic_notepad
