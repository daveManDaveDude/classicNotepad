#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace classic_notepad {

bool DecodeAnsiBytes(const std::uint8_t* data, std::size_t size, std::wstring& text);
bool EncodeAnsiText(const std::wstring& text, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage);

#ifdef _WIN32
// Explicit code-page overloads make ANSI fallback behavior testable without
// depending on the host machine's active ANSI code page.
bool DecodeAnsiBytesForCodePage(
    const std::uint8_t* data,
    std::size_t size,
    std::uint32_t codePage,
    std::wstring& text);
bool EncodeAnsiTextForCodePage(
    const std::wstring& text,
    std::uint32_t codePage,
    std::vector<std::uint8_t>& bytes,
    std::wstring& errorMessage);
#endif

} // namespace classic_notepad
