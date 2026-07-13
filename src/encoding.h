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

// Incremental counterpart used by the large-document streaming save path.
// includeBom is honored for UTF-8 BOM and UTF-16 LE; ANSI remains delegated to
// the platform encoder and is not used by the large-file viewer.
bool EncodeTextChunk(
    const std::wstring& text,
    TextEncoding encoding,
    bool includeBom,
    std::vector<std::uint8_t>& bytes,
    std::wstring& errorMessage);

} // namespace classic_notepad
