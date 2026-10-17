#include "large_text_summary.h"

#include <cstddef>

namespace classic_notepad {
namespace {

bool IsHighSurrogate(wchar_t character)
{
    return character >= 0xD800 && character <= 0xDBFF;
}

bool IsLowSurrogate(wchar_t character)
{
    return character >= 0xDC00 && character <= 0xDFFF;
}

} // namespace

LargeTextSummary CombineLargeTextSummaries(
    const LargeTextSummary& left,
    const LargeTextSummary& right)
{
    if (left.empty) return right;
    if (right.empty) return left;
    LargeTextSummary combined;
    combined.empty = false;
    combined.first = left.first;
    combined.last = right.last;
    combined.characters = left.characters + right.characters;
    combined.displayedCharactersKnown = left.displayedCharactersKnown && right.displayedCharactersKnown;
    if (combined.displayedCharactersKnown) {
        combined.displayedCharacters = left.displayedCharacters + right.displayedCharacters;
    }
    combined.lineBreaks = left.lineBreaks + right.lineBreaks;
    if (left.last == L'\r' && right.first == L'\n') {
        --combined.lineBreaks;
        if (combined.displayedCharactersKnown) --combined.displayedCharacters;
    }
    if (combined.displayedCharactersKnown && IsHighSurrogate(left.last) && IsLowSurrogate(right.first)) {
        --combined.displayedCharacters;
    }
    combined.width = CombineLargeTextWidthSummaries(left.width, right.width);
    return combined;
}

LargeTextSummary SummarizeLargeText(const std::wstring& text)
{
    LargeTextSummary summary;
    summary.characters = static_cast<std::uint64_t>(text.size());
    summary.displayedCharacters = summary.characters;
    summary.displayedCharactersKnown = true;
    summary.empty = text.empty();
    if (text.empty()) return summary;
    summary.first = text.front();
    summary.last = text.back();
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            ++summary.lineBreaks;
            if (index + 1U < text.size() && text[index + 1U] == L'\n') {
                ++index;
                --summary.displayedCharacters;
            }
        } else if (text[index] == L'\n') {
            ++summary.lineBreaks;
        }
        if (IsHighSurrogate(text[index]) && index + 1U < text.size() && IsLowSurrogate(text[index + 1U])) {
            ++index;
            --summary.displayedCharacters;
        }
    }
    bool prefixOpen = true;
    for (wchar_t character : text) {
        AppendLargeTextWidthCharacter(summary.width, prefixOpen, character);
    }
    return summary;
}

} // namespace classic_notepad
