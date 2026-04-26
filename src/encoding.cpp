#include "encoding.h"

#include "ansi_encoding.h"

#include <cstdint>

namespace classic_notepad {
namespace {

bool AppendCodePointToWide(std::uint32_t codePoint, std::wstring& text)
{
    if (codePoint > 0x10FFFFU || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
        return false;
    }

    if constexpr (sizeof(wchar_t) == 2U) {
        if (codePoint <= 0xFFFFU) {
            text.push_back(static_cast<wchar_t>(codePoint));
            return true;
        }

        const std::uint32_t adjusted = codePoint - 0x10000U;
        text.push_back(static_cast<wchar_t>(0xD800U + (adjusted >> 10U)));
        text.push_back(static_cast<wchar_t>(0xDC00U + (adjusted & 0x3FFU)));
        return true;
    } else {
        text.push_back(static_cast<wchar_t>(codePoint));
        return true;
    }
}

bool ReadWideCodePoint(const std::wstring& text, std::size_t& index, std::uint32_t& codePoint)
{
    if (index >= text.size()) {
        return false;
    }

    if constexpr (sizeof(wchar_t) == 2U) {
        const std::uint32_t first = static_cast<std::uint16_t>(text[index++]);
        if (first >= 0xD800U && first <= 0xDBFFU) {
            if (index >= text.size()) {
                return false;
            }

            const std::uint32_t second = static_cast<std::uint16_t>(text[index++]);
            if (second < 0xDC00U || second > 0xDFFFU) {
                return false;
            }

            codePoint = 0x10000U + (((first - 0xD800U) << 10U) | (second - 0xDC00U));
            return true;
        }

        if (first >= 0xDC00U && first <= 0xDFFFU) {
            return false;
        }

        codePoint = first;
        return true;
    } else {
        codePoint = static_cast<std::uint32_t>(text[index++]);
        return codePoint <= 0x10FFFFU && (codePoint < 0xD800U || codePoint > 0xDFFFU);
    }
}

bool DecodeUtf8(const std::uint8_t* data, std::size_t size, std::wstring& text)
{
    text.clear();
    text.reserve(size);

    for (std::size_t index = 0; index < size;) {
        const std::uint8_t lead = data[index++];
        std::uint32_t codePoint = 0;
        std::uint32_t minimum = 0;
        int continuationCount = 0;

        if (lead <= 0x7FU) {
            codePoint = lead;
        } else if (lead >= 0xC2U && lead <= 0xDFU) {
            codePoint = lead & 0x1FU;
            minimum = 0x80U;
            continuationCount = 1;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            codePoint = lead & 0x0FU;
            minimum = 0x800U;
            continuationCount = 2;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            codePoint = lead & 0x07U;
            minimum = 0x10000U;
            continuationCount = 3;
        } else {
            return false;
        }

        for (int offset = 0; offset < continuationCount; ++offset) {
            if (index >= size || (data[index] & 0xC0U) != 0x80U) {
                return false;
            }

            codePoint = (codePoint << 6U) | (data[index] & 0x3FU);
            ++index;
        }

        if (codePoint < minimum || !AppendCodePointToWide(codePoint, text)) {
            return false;
        }
    }

    return true;
}

void AppendUtf8CodePoint(std::uint32_t codePoint, std::vector<std::uint8_t>& bytes)
{
    if (codePoint <= 0x7FU) {
        bytes.push_back(static_cast<std::uint8_t>(codePoint));
    } else if (codePoint <= 0x7FFU) {
        bytes.push_back(static_cast<std::uint8_t>(0xC0U | (codePoint >> 6U)));
        bytes.push_back(static_cast<std::uint8_t>(0x80U | (codePoint & 0x3FU)));
    } else if (codePoint <= 0xFFFFU) {
        bytes.push_back(static_cast<std::uint8_t>(0xE0U | (codePoint >> 12U)));
        bytes.push_back(static_cast<std::uint8_t>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        bytes.push_back(static_cast<std::uint8_t>(0x80U | (codePoint & 0x3FU)));
    } else {
        bytes.push_back(static_cast<std::uint8_t>(0xF0U | (codePoint >> 18U)));
        bytes.push_back(static_cast<std::uint8_t>(0x80U | ((codePoint >> 12U) & 0x3FU)));
        bytes.push_back(static_cast<std::uint8_t>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        bytes.push_back(static_cast<std::uint8_t>(0x80U | (codePoint & 0x3FU)));
    }
}

bool EncodeUtf8(const std::wstring& text, bool includeBom, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
{
    bytes.clear();
    if (includeBom) {
        bytes.push_back(0xEFU);
        bytes.push_back(0xBBU);
        bytes.push_back(0xBFU);
    }

    std::size_t index = 0;
    while (index < text.size()) {
        std::uint32_t codePoint = 0;
        if (!ReadWideCodePoint(text, index, codePoint)) {
            errorMessage = L"The document contains invalid Unicode text and cannot be saved as UTF-8.";
            return false;
        }

        AppendUtf8CodePoint(codePoint, bytes);
    }

    return true;
}

bool DecodeUtf16Le(const std::uint8_t* data, std::size_t size, std::wstring& text)
{
    text.clear();
    if ((size % 2U) != 0U) {
        return false;
    }

    text.reserve(size / 2U);
    for (std::size_t index = 0; index < size; index += 2U) {
        const std::uint32_t first = data[index] | (static_cast<std::uint32_t>(data[index + 1U]) << 8U);

        if constexpr (sizeof(wchar_t) == 2U) {
            text.push_back(static_cast<wchar_t>(first));
        } else {
            if (first >= 0xD800U && first <= 0xDBFFU) {
                if (index + 3U >= size) {
                    return false;
                }

                const std::uint32_t second =
                    data[index + 2U] | (static_cast<std::uint32_t>(data[index + 3U]) << 8U);
                if (second < 0xDC00U || second > 0xDFFFU) {
                    return false;
                }

                const std::uint32_t codePoint =
                    0x10000U + (((first - 0xD800U) << 10U) | (second - 0xDC00U));
                text.push_back(static_cast<wchar_t>(codePoint));
                index += 2U;
            } else if (first >= 0xDC00U && first <= 0xDFFFU) {
                return false;
            } else {
                text.push_back(static_cast<wchar_t>(first));
            }
        }
    }

    return true;
}

bool EncodeUtf16Le(const std::wstring& text, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
{
    bytes.clear();
    bytes.reserve((text.size() * 2U) + 2U);
    bytes.push_back(0xFFU);
    bytes.push_back(0xFEU);

    std::size_t index = 0;
    while (index < text.size()) {
        std::uint32_t codePoint = 0;
        if (!ReadWideCodePoint(text, index, codePoint)) {
            errorMessage = L"The document contains invalid Unicode text and cannot be saved as UTF-16 LE.";
            return false;
        }

        if (codePoint <= 0xFFFFU) {
            bytes.push_back(static_cast<std::uint8_t>(codePoint & 0xFFU));
            bytes.push_back(static_cast<std::uint8_t>((codePoint >> 8U) & 0xFFU));
        } else {
            const std::uint32_t adjusted = codePoint - 0x10000U;
            const std::uint32_t high = 0xD800U + (adjusted >> 10U);
            const std::uint32_t low = 0xDC00U + (adjusted & 0x3FFU);
            bytes.push_back(static_cast<std::uint8_t>(high & 0xFFU));
            bytes.push_back(static_cast<std::uint8_t>((high >> 8U) & 0xFFU));
            bytes.push_back(static_cast<std::uint8_t>(low & 0xFFU));
            bytes.push_back(static_cast<std::uint8_t>((low >> 8U) & 0xFFU));
        }
    }

    return true;
}

} // namespace

bool DecodeTextBytes(
    const std::vector<std::uint8_t>& bytes,
    DecodeTextResult& result,
    std::wstring& errorMessage)
{
    result = {};
    errorMessage.clear();

    if (bytes.size() >= 3U && bytes[0] == 0xEFU && bytes[1] == 0xBBU && bytes[2] == 0xBFU) {
        result.encoding = TextEncoding::Utf8Bom;
        if (!DecodeUtf8(bytes.data() + 3U, bytes.size() - 3U, result.text)) {
            errorMessage = L"The file has a UTF-8 BOM but could not be decoded as UTF-8.";
            return false;
        }
        return true;
    }

    if (bytes.size() >= 2U && bytes[0] == 0xFFU && bytes[1] == 0xFEU) {
        result.encoding = TextEncoding::Utf16LeBom;
        if (!DecodeUtf16Le(bytes.data() + 2U, bytes.size() - 2U, result.text)) {
            errorMessage = L"The file has a UTF-16 LE BOM but contains an incomplete character.";
            return false;
        }
        return true;
    }

    if (DecodeUtf8(bytes.data(), bytes.size(), result.text)) {
        result.encoding = TextEncoding::Utf8NoBom;
        return true;
    }

    result.encoding = TextEncoding::Ansi;
    if (!DecodeAnsiBytes(bytes.data(), bytes.size(), result.text)) {
        errorMessage = L"The file could not be decoded as text.";
        return false;
    }

    return true;
}

bool EncodeTextBytes(
    const std::wstring& text,
    TextEncoding encoding,
    std::vector<std::uint8_t>& bytes,
    std::wstring& errorMessage)
{
    errorMessage.clear();

    switch (encoding) {
    case TextEncoding::Utf8NoBom:
        return EncodeUtf8(text, false, bytes, errorMessage);
    case TextEncoding::Utf8Bom:
        return EncodeUtf8(text, true, bytes, errorMessage);
    case TextEncoding::Utf16LeBom:
        return EncodeUtf16Le(text, bytes, errorMessage);
    case TextEncoding::Ansi:
        return EncodeAnsiText(text, bytes, errorMessage);
    default:
        errorMessage = L"Unknown text encoding.";
        return false;
    }
}

} // namespace classic_notepad
