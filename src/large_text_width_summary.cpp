#include "large_text_width_summary.h"

#include <algorithm>
#include <limits>

namespace classic_notepad {
namespace {

std::uint64_t SaturatingWidthAdd(std::uint64_t left, std::uint64_t right) noexcept
{
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    return right > maximum - left ? maximum : left + right;
}

} // namespace

void AppendLargeTextWidthCharacter(
    LargeFileWidthSummary& summary,
    bool& prefixOpen,
    wchar_t character) noexcept
{
    if (character == L'\r' || character == L'\n') {
        summary.maximumLineColumns = std::max(summary.maximumLineColumns, summary.suffixColumns);
        summary.suffixColumns = 0U;
        summary.suffixResidue = 0U;
        summary.hasLineBreak = true;
        prefixOpen = false;
        return;
    }

    if (prefixOpen) {
        for (std::size_t residue = 0; residue < summary.prefixColumns.size(); ++residue) {
            const std::uint64_t advance = character == L'\t'
                ? 8U - summary.prefixResidues[residue] : 1U;
            summary.prefixColumns[residue] = SaturatingWidthAdd(summary.prefixColumns[residue], advance);
            summary.prefixResidues[residue] = static_cast<std::uint8_t>(
                (summary.prefixResidues[residue] + advance) % 8U);
        }
    }

    const std::uint64_t advance = character == L'\t' ? 8U - summary.suffixResidue : 1U;
    summary.suffixColumns = SaturatingWidthAdd(summary.suffixColumns, advance);
    summary.suffixResidue = static_cast<std::uint8_t>((summary.suffixResidue + advance) % 8U);
    summary.maximumLineColumns = std::max(summary.maximumLineColumns, summary.suffixColumns);
}

LargeFileWidthSummary CombineLargeTextWidthSummaries(
    const LargeFileWidthSummary& left,
    const LargeFileWidthSummary& right) noexcept
{
    LargeFileWidthSummary combined;
    combined.hasLineBreak = left.hasLineBreak || right.hasLineBreak;
    for (std::size_t residue = 0; residue < combined.prefixColumns.size(); ++residue) {
        if (left.hasLineBreak) {
            combined.prefixColumns[residue] = left.prefixColumns[residue];
            combined.prefixResidues[residue] = left.prefixResidues[residue];
        } else {
            const std::uint8_t rightInput = left.prefixResidues[residue];
            combined.prefixColumns[residue] = SaturatingWidthAdd(
                left.prefixColumns[residue], right.prefixColumns[rightInput]);
            combined.prefixResidues[residue] = right.prefixResidues[rightInput];
        }
    }

    if (right.hasLineBreak) {
        combined.suffixColumns = right.suffixColumns;
        combined.suffixResidue = right.suffixResidue;
    } else {
        const std::uint8_t rightInput = left.suffixResidue;
        combined.suffixColumns = SaturatingWidthAdd(
            left.suffixColumns, right.prefixColumns[rightInput]);
        combined.suffixResidue = right.prefixResidues[rightInput];
    }

    const std::uint64_t joinedLine = SaturatingWidthAdd(
        left.suffixColumns, right.prefixColumns[left.suffixResidue]);
    combined.maximumLineColumns = std::max({
        left.maximumLineColumns, right.maximumLineColumns, joinedLine});
    return combined;
}

} // namespace classic_notepad
