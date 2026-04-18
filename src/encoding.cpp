#include "encoding.h"

#include <windows.h>

#include <limits>

namespace classic_notepad {
namespace {

bool FitsInInt(std::size_t value)
{
    return value <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

bool DecodeMultiByte(UINT codePage, DWORD flags, const std::uint8_t* data, std::size_t size, std::wstring& text)
{
    text.clear();
    if (size == 0U) {
        return true;
    }

    if (!FitsInInt(size)) {
        return false;
    }

    const int byteCount = static_cast<int>(size);
    const int wideLength = MultiByteToWideChar(
        codePage,
        flags,
        reinterpret_cast<const char*>(data),
        byteCount,
        nullptr,
        0);

    if (wideLength <= 0) {
        return false;
    }

    text.resize(static_cast<std::size_t>(wideLength));
    const int convertedLength = MultiByteToWideChar(
        codePage,
        flags,
        reinterpret_cast<const char*>(data),
        byteCount,
        text.data(),
        wideLength);

    return convertedLength == wideLength;
}

bool DecodeUtf16Le(const std::uint8_t* data, std::size_t size, std::wstring& text)
{
    text.clear();
    if ((size % 2U) != 0U) {
        return false;
    }

    text.reserve(size / 2U);
    for (std::size_t index = 0; index < size; index += 2U) {
        const auto value = static_cast<wchar_t>(data[index] | (data[index + 1U] << 8U));
        text.push_back(value);
    }

    return true;
}

bool EncodeUtf8(const std::wstring& text, bool includeBom, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
{
    bytes.clear();
    if (includeBom) {
        bytes.push_back(0xEFU);
        bytes.push_back(0xBBU);
        bytes.push_back(0xBFU);
    }

    if (text.empty()) {
        return true;
    }

    if (!FitsInInt(text.size())) {
        errorMessage = L"The document is too large to encode as UTF-8.";
        return false;
    }

    const int wideCount = static_cast<int>(text.size());
    const int byteLength = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.c_str(),
        wideCount,
        nullptr,
        0,
        nullptr,
        nullptr);

    if (byteLength <= 0) {
        errorMessage = L"The document contains invalid Unicode text and cannot be saved as UTF-8.";
        return false;
    }

    const std::size_t offset = bytes.size();
    bytes.resize(offset + static_cast<std::size_t>(byteLength));

    const int convertedLength = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.c_str(),
        wideCount,
        reinterpret_cast<char*>(bytes.data() + offset),
        byteLength,
        nullptr,
        nullptr);

    if (convertedLength != byteLength) {
        errorMessage = L"The document could not be encoded as UTF-8.";
        return false;
    }

    return true;
}

bool EncodeUtf16Le(const std::wstring& text, std::vector<std::uint8_t>& bytes)
{
    bytes.clear();
    bytes.reserve((text.size() * 2U) + 2U);
    bytes.push_back(0xFFU);
    bytes.push_back(0xFEU);

    for (const wchar_t character : text) {
        const auto value = static_cast<unsigned int>(character);
        bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    }

    return true;
}

bool EncodeAnsi(const std::wstring& text, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
{
    bytes.clear();
    if (text.empty()) {
        return true;
    }

    if (!FitsInInt(text.size())) {
        errorMessage = L"The document is too large to encode as ANSI text.";
        return false;
    }

    BOOL usedDefaultChar = FALSE;
    const int wideCount = static_cast<int>(text.size());
    const int byteLength = WideCharToMultiByte(
        CP_ACP,
        WC_NO_BEST_FIT_CHARS,
        text.c_str(),
        wideCount,
        nullptr,
        0,
        nullptr,
        &usedDefaultChar);

    if (byteLength <= 0 || usedDefaultChar) {
        errorMessage =
            L"The document contains characters that cannot be saved in the system ANSI code page. "
            L"Use Save As in a later encoding-aware build, or remove those characters before saving this ANSI file.";
        return false;
    }

    bytes.resize(static_cast<std::size_t>(byteLength));
    usedDefaultChar = FALSE;
    const int convertedLength = WideCharToMultiByte(
        CP_ACP,
        WC_NO_BEST_FIT_CHARS,
        text.c_str(),
        wideCount,
        reinterpret_cast<char*>(bytes.data()),
        byteLength,
        nullptr,
        &usedDefaultChar);

    if (convertedLength != byteLength || usedDefaultChar) {
        errorMessage = L"The document contains characters that cannot be saved in the system ANSI code page.";
        return false;
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
        if (!DecodeMultiByte(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + 3U, bytes.size() - 3U, result.text)) {
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

    if (DecodeMultiByte(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), bytes.size(), result.text)) {
        result.encoding = TextEncoding::Utf8NoBom;
        return true;
    }

    result.encoding = TextEncoding::Ansi;
    if (!DecodeMultiByte(CP_ACP, 0, bytes.data(), bytes.size(), result.text)) {
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
        return EncodeUtf16Le(text, bytes);
    case TextEncoding::Ansi:
        return EncodeAnsi(text, bytes, errorMessage);
    default:
        errorMessage = L"Unknown text encoding.";
        return false;
    }
}

} // namespace classic_notepad
