#include "text_metadata.h"

namespace classic_notepad {

std::wstring FormatEncoding(TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Utf8NoBom:
        return L"UTF-8";
    case TextEncoding::Utf8Bom:
        return L"UTF-8 with BOM";
    case TextEncoding::Utf16LeBom:
        return L"UTF-16 LE";
    case TextEncoding::Ansi:
        return L"ANSI";
    default:
        return L"UTF-8";
    }
}

std::wstring FormatLineEnding(LineEndingStyle lineEnding)
{
    switch (lineEnding) {
    case LineEndingStyle::Crlf:
        return L"Windows (CRLF)";
    case LineEndingStyle::Lf:
        return L"Unix (LF)";
    case LineEndingStyle::Cr:
        return L"Macintosh (CR)";
    case LineEndingStyle::Mixed:
        return L"Mixed";
    default:
        return L"Windows (CRLF)";
    }
}

std::wstring FormatNumberWithSeparators(std::size_t value)
{
    const std::wstring digits = std::to_wstring(value);
    std::wstring formatted;
    formatted.reserve(digits.size() + ((digits.size() - 1U) / 3U));

    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index > 0U && ((digits.size() - index) % 3U) == 0U) {
            formatted += L',';
        }

        formatted += digits[index];
    }

    return formatted;
}

std::wstring FormatCharacterCount(std::size_t count)
{
    std::wstring text = FormatNumberWithSeparators(count);
    text += count == 1U ? L" character" : L" characters";
    return text;
}

std::size_t CountStatusCharacters(const std::wstring& text)
{
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r' && index + 1U < text.size() && text[index + 1U] == L'\n') {
            ++index;
        } else if (
            text[index] >= 0xD800 &&
            text[index] <= 0xDBFF &&
            index + 1U < text.size() &&
            text[index + 1U] >= 0xDC00 &&
            text[index + 1U] <= 0xDFFF) {
            ++index;
        }

        ++count;
    }

    return count;
}

} // namespace classic_notepad
