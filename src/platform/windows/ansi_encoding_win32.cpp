#include "ansi_encoding.h"

#include "win32_platform.h"

#include <windows.h>

#include <cstring>
#include <limits>
#include <utility>

namespace classic_notepad {
namespace {

bool FitsInInt(std::size_t value)
{
    return value <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

bool IsWellFormedUnicode(const std::wstring& text)
{
    if constexpr (sizeof(wchar_t) == 2U) {
        for (std::size_t index = 0; index < text.size(); ++index) {
            const std::uint32_t unit = static_cast<std::uint16_t>(text[index]);
            if (unit >= 0xD800U && unit <= 0xDBFFU) {
                if (index + 1U >= text.size()) return false;
                const std::uint32_t low = static_cast<std::uint16_t>(text[++index]);
                if (low < 0xDC00U || low > 0xDFFFU) return false;
            } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
                return false;
            }
        }
        return true;
    } else {
        for (wchar_t character : text) {
            const std::uint32_t codePoint = static_cast<std::uint32_t>(character);
            if (codePoint > 0x10FFFFU || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) return false;
        }
        return true;
    }
}

} // namespace

bool DecodeAnsiBytes(const std::uint8_t* data, std::size_t size, std::wstring& text)
{
    return DecodeAnsiBytesForCodePage(data, size, GetACP(), text);
}

bool DecodeAnsiBytesForCodePage(
    const std::uint8_t* data,
    std::size_t size,
    std::uint32_t codePageValue,
    std::wstring& text)
{
    text.clear();
    if (size == 0U) {
        return true;
    }

    if (data == nullptr || codePageValue == 0U || !FitsInInt(size)) {
        return false;
    }

    const UINT codePage = static_cast<UINT>(codePageValue);
    const bool codePageIsUtf8 = codePage == CP_UTF8;
    const DWORD decodeFlags = codePageIsUtf8 ? MB_ERR_INVALID_CHARS : 0U;
    const int byteCount = static_cast<int>(size);
    const int wideLength = MultiByteToWideChar(
        codePage,
        decodeFlags,
        reinterpret_cast<const char*>(data),
        byteCount,
        nullptr,
        0);

    if (wideLength <= 0) {
        return false;
    }

    std::wstring decoded(static_cast<std::size_t>(wideLength), L'\0');
    const int convertedLength = MultiByteToWideChar(
        codePage,
        decodeFlags,
        reinterpret_cast<const char*>(data),
        byteCount,
        decoded.data(),
        wideLength);

    if (convertedLength != wideLength) return false;

    std::vector<std::uint8_t> roundTrip(size);
    BOOL usedDefaultChar = FALSE;
    const DWORD encodeFlags = codePageIsUtf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    const int encodedLength = WideCharToMultiByte(
        codePage,
        encodeFlags,
        decoded.data(),
        wideLength,
        reinterpret_cast<char*>(roundTrip.data()),
        byteCount,
        codePageIsUtf8 ? nullptr : "?",
        codePageIsUtf8 ? nullptr : &usedDefaultChar);
    if (encodedLength != byteCount || usedDefaultChar ||
        std::memcmp(roundTrip.data(), data, size) != 0) {
        return false;
    }

    text = std::move(decoded);
    return true;
}

bool EncodeAnsiText(const std::wstring& text, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
{
    return EncodeAnsiTextForCodePage(text, GetACP(), bytes, errorMessage);
}

bool EncodeAnsiTextForCodePage(
    const std::wstring& text,
    std::uint32_t codePageValue,
    std::vector<std::uint8_t>& bytes,
    std::wstring& errorMessage)
{
    bytes.clear();
    if (text.empty()) {
        return true;
    }

    if (!IsWellFormedUnicode(text)) {
        errorMessage = L"The document contains malformed Unicode and cannot be saved as ANSI text.";
        return false;
    }

    if (!FitsInInt(text.size())) {
        errorMessage = L"The document is too large to encode as ANSI text.";
        return false;
    }

    if (codePageValue == 0U) {
        errorMessage = L"The active ANSI code page is unavailable.";
        return false;
    }
    const UINT codePage = static_cast<UINT>(codePageValue);
    const bool codePageIsUtf8 = codePage == CP_UTF8;
    const DWORD conversionFlags = codePageIsUtf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL usedDefaultChar = FALSE;
    const int wideCount = static_cast<int>(text.size());
    const int byteLength = WideCharToMultiByte(
        codePage,
        conversionFlags,
        text.c_str(),
        wideCount,
        nullptr,
        0,
        codePageIsUtf8 ? nullptr : "?",
        codePageIsUtf8 ? nullptr : &usedDefaultChar);

    if (byteLength <= 0 || usedDefaultChar) {
        errorMessage =
            L"The document contains characters that cannot be saved in the system ANSI code page. "
            L"Use Save As in a later encoding-aware build, or remove those characters before saving this ANSI file.";
        return false;
    }

    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(byteLength));
    usedDefaultChar = FALSE;
    const int convertedLength = WideCharToMultiByte(
        codePage,
        conversionFlags,
        text.c_str(),
        wideCount,
        reinterpret_cast<char*>(encoded.data()),
        byteLength,
        codePageIsUtf8 ? nullptr : "?",
        codePageIsUtf8 ? nullptr : &usedDefaultChar);

    if (convertedLength != byteLength || usedDefaultChar) {
        errorMessage = L"The document contains characters that cannot be saved in the system ANSI code page.";
        return false;
    }

    std::wstring roundTrip(text.size(), L'\0');
    const int roundTripLength = MultiByteToWideChar(
        codePage,
        codePageIsUtf8 ? MB_ERR_INVALID_CHARS : 0U,
        reinterpret_cast<const char*>(encoded.data()),
        byteLength,
        roundTrip.data(),
        static_cast<int>(roundTrip.size()));
    if (roundTripLength != wideCount || roundTrip != text) {
        errorMessage = L"The document cannot be represented losslessly in the system ANSI code page.";
        return false;
    }

    bytes = std::move(encoded);
    return true;
}

} // namespace classic_notepad
