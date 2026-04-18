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

bool RangesOverlap(std::size_t firstStart, std::size_t firstLength, std::size_t secondStart, std::size_t secondLength)
{
    const std::size_t firstEnd = firstStart + firstLength;
    const std::size_t secondEnd = secondStart + secondLength;
    return firstStart < secondEnd && secondStart < firstEnd;
}

} // namespace classic_notepad
