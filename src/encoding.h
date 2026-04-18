#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace classic_notepad {

enum class TextEncoding {
    Utf8NoBom,
    Utf8Bom,
    Utf16LeBom,
    Ansi
};

struct DecodeTextResult {
    std::wstring text;
    TextEncoding encoding = TextEncoding::Utf8NoBom;
};

bool DecodeTextBytes(
    const std::vector<std::uint8_t>& bytes,
    DecodeTextResult& result,
    std::wstring& errorMessage);

bool EncodeTextBytes(
    const std::wstring& text,
    TextEncoding encoding,
    std::vector<std::uint8_t>& bytes,
    std::wstring& errorMessage);

} // namespace classic_notepad
