#pragma once

#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <string_view>

namespace classic_notepad {

struct TextUnitRange {
    std::size_t start = 0;
    std::size_t end = 0;
};

inline bool IsHighSurrogateUnit(wchar_t value) noexcept
{
    const std::uint32_t unit = static_cast<std::uint32_t>(value);
    return unit >= 0xD800U && unit <= 0xDBFFU;
}

inline bool IsLowSurrogateUnit(wchar_t value) noexcept
{
    const std::uint32_t unit = static_cast<std::uint32_t>(value);
    return unit >= 0xDC00U && unit <= 0xDFFFU;
}

inline std::size_t NextScalarBoundary(std::wstring_view text, std::size_t position) noexcept
{
    if (position >= text.size()) return text.size();
    if (IsHighSurrogateUnit(text[position]) && position + 1U < text.size() &&
        IsLowSurrogateUnit(text[position + 1U])) return position + 2U;
    return position + 1U;
}

inline std::size_t PreviousScalarBoundary(std::wstring_view text, std::size_t position) noexcept
{
    if (position == 0U) return 0U;
    position = position > text.size() ? text.size() : position;
    if (position >= 2U && IsHighSurrogateUnit(text[position - 2U]) &&
        IsLowSurrogateUnit(text[position - 1U])) return position - 2U;
    return position - 1U;
}

// Returns the collapsed Backspace/Delete range in text-local UTF-16 units.
// CRLF is treated as one deletion target even when the caret sits between its
// two stored units. A surrogate pair is likewise returned as one scalar span.
inline bool CollapsedDeleteRange(
    std::wstring_view text,
    std::size_t caret,
    bool backspace,
    TextUnitRange& range) noexcept
{
    range = {caret, caret};
    if (caret > text.size()) return false;
    if (caret > 0U && caret < text.size() && IsHighSurrogateUnit(text[caret - 1U]) &&
        IsLowSurrogateUnit(text[caret])) {
        range = {caret - 1U, caret + 1U};
        return true;
    }

    if (backspace) {
        if (caret == 0U) return true;
        const std::size_t previous = PreviousScalarBoundary(text, caret);
        if (text[previous] == L'\n' && previous > 0U && text[previous - 1U] == L'\r') {
            range.start = previous - 1U;
        } else if (text[previous] == L'\r' && caret < text.size() && text[caret] == L'\n') {
            range.start = previous;
            range.end = caret + 1U;
        } else {
            range.start = previous;
        }
        return true;
    }

    if (caret == text.size()) return true;
    const std::size_t next = NextScalarBoundary(text, caret);
    range.end = next;
    if (text[caret] == L'\r' && next < text.size() && text[next] == L'\n') {
        range.end = next + 1U;
    } else if (text[caret] == L'\n' && caret > 0U && text[caret - 1U] == L'\r') {
        range.start = caret - 1U;
    }
    return true;
}

inline bool IsWordCharacterUnit(wchar_t character) noexcept
{
    return std::iswalnum(static_cast<wint_t>(character)) != 0 || character == L'_';
}

} // namespace classic_notepad
