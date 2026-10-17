#pragma once

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <limits>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace classic_notepad {

inline bool IsSearchWordUnit(wchar_t character)
{
    if (character == L'_') return true;
    if (character >= L'0' && character <= L'9') return true;
    if (character >= L'A' && character <= L'Z') return true;
    if (character >= L'a' && character <= L'z') return true;
#if defined(_WIN32)
    WORD characterType = 0;
    const wchar_t value = character;
    return GetStringTypeExW(LOCALE_USER_DEFAULT, CT_CTYPE1, &value, 1, &characterType) != FALSE &&
        (characterType & (C1_ALPHA | C1_DIGIT)) != 0;
#else
    return std::iswalnum(static_cast<wint_t>(character)) != 0;
#endif
}

inline bool SearchCodeUnitsEqual(wchar_t left, wchar_t right, bool matchCase)
{
    if (left == right || matchCase) return left == right;
    if (left >= L'A' && left <= L'Z') left = static_cast<wchar_t>(left + (L'a' - L'A'));
    if (right >= L'A' && right <= L'Z') right = static_cast<wchar_t>(right + (L'a' - L'A'));
    if (left == right) return true;
#if defined(_WIN32)
    return CompareStringOrdinal(&left, 1, &right, 1, TRUE) == CSTR_EQUAL;
#else
    return std::towlower(static_cast<wint_t>(left)) == std::towlower(static_cast<wint_t>(right));
#endif
}

inline bool IsUtf16ScalarBoundary(std::wstring_view text, std::size_t position)
{
    if (position == 0U || position >= text.size()) return position <= text.size();
    const wchar_t before = text[position - 1U];
    const wchar_t after = text[position];
    return !(before >= 0xD800 && before <= 0xDBFF && after >= 0xDC00 && after <= 0xDFFF);
}

inline bool SearchTextMatchesAt(
    std::wstring_view text,
    std::size_t position,
    std::wstring_view needle,
    bool matchCase,
    bool wholeWord)
{
    if (needle.empty() || position > text.size() || needle.size() > text.size() - position ||
        !IsUtf16ScalarBoundary(text, position) ||
        !IsUtf16ScalarBoundary(text, position + needle.size())) return false;

    bool equal = false;
#if defined(_WIN32)
    if (matchCase) {
        equal = std::equal(text.begin() + static_cast<std::ptrdiff_t>(position),
            text.begin() + static_cast<std::ptrdiff_t>(position + needle.size()), needle.begin());
    } else if (needle.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        equal = CompareStringOrdinal(
            text.data() + position,
            static_cast<int>(needle.size()),
            needle.data(),
            static_cast<int>(needle.size()),
            TRUE) == CSTR_EQUAL;
    }
#else
    equal = std::equal(text.begin() + static_cast<std::ptrdiff_t>(position),
        text.begin() + static_cast<std::ptrdiff_t>(position + needle.size()), needle.begin(),
        [matchCase](wchar_t left, wchar_t right) { return SearchCodeUnitsEqual(left, right, matchCase); });
#endif
    if (!equal || !wholeWord) return equal;
    const std::size_t end = position + needle.size();
    const bool leadingBoundary = position == 0U ||
        !IsSearchWordUnit(text[position - 1U]) || !IsSearchWordUnit(text[position]);
    const bool trailingBoundary = end == text.size() ||
        !IsSearchWordUnit(text[end - 1U]) || !IsSearchWordUnit(text[end]);
    return leadingBoundary && trailingBoundary;
}

} // namespace classic_notepad
