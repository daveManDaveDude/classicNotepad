#include "ansi_encoding.h"

#include "win32_platform.h"

#include <windows.h>

#include <limits>

namespace classic_notepad {
namespace {

bool FitsInInt(std::size_t value)
{
    return value <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

} // namespace

bool DecodeAnsiBytes(const std::uint8_t* data, std::size_t size, std::wstring& text)
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
        CP_ACP,
        0,
        reinterpret_cast<const char*>(data),
        byteCount,
        nullptr,
        0);

    if (wideLength <= 0) {
        return false;
    }

    text.resize(static_cast<std::size_t>(wideLength));
    const int convertedLength = MultiByteToWideChar(
        CP_ACP,
        0,
        reinterpret_cast<const char*>(data),
        byteCount,
        text.data(),
        wideLength);

    return convertedLength == wideLength;
}

bool EncodeAnsiText(const std::wstring& text, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
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

} // namespace classic_notepad
