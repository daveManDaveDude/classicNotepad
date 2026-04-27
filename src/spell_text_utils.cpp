#include "spell_text_utils.h"

#include <algorithm>
#include <cwctype>

namespace classic_notepad {
namespace {

bool IsCoreWordCharacter(wchar_t character)
{
    return std::iswalnum(static_cast<wint_t>(character)) != 0;
}

bool IsWordCharacterAt(const std::wstring& text, std::size_t index)
{
    if (index >= text.size()) {
        return false;
    }

    const wchar_t character = text[index];
    if (IsCoreWordCharacter(character)) {
        return true;
    }

    if (character != L'\'' && character != L'-') {
        return false;
    }

    if (index == 0U || index + 1U >= text.size()) {
        return false;
    }

    return IsCoreWordCharacter(text[index - 1U]) && IsCoreWordCharacter(text[index + 1U]);
}

} // namespace

bool ExpandWordRangeAt(const std::wstring& text, std::size_t index, TextRange& range)
{
    range = {};
    if (!IsWordCharacterAt(text, index)) {
        return false;
    }

    std::size_t start = index;
    while (start > 0U && IsWordCharacterAt(text, start - 1U)) {
        --start;
    }

    std::size_t end = index + 1U;
    while (end < text.size() && IsWordCharacterAt(text, end)) {
        ++end;
    }

    while (start < end && !IsCoreWordCharacter(text[start])) {
        ++start;
    }
    while (end > start && !IsCoreWordCharacter(text[end - 1U])) {
        --end;
    }

    if (end <= start) {
        return false;
    }

    range.start = start;
    range.length = end - start;
    return true;
}

std::vector<TextRange> FindSpellCheckWordRanges(const std::wstring& text)
{
    std::vector<TextRange> ranges;
    for (std::size_t index = 0; index < text.size();) {
        TextRange range {};
        if (ExpandWordRangeAt(text, index, range)) {
            ranges.push_back(range);
            index = range.start + range.length;
        } else {
            ++index;
        }
    }

    return ranges;
}

bool RangesOverlap(std::size_t firstStart, std::size_t firstLength, std::size_t secondStart, std::size_t secondLength)
{
    if (firstLength == 0U || secondLength == 0U) {
        return false;
    }

    if (firstStart <= secondStart) {
        return (secondStart - firstStart) < firstLength;
    }

    return (firstStart - secondStart) < secondLength;
}

} // namespace classic_notepad
