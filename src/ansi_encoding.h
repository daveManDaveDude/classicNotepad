#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace classic_notepad {

bool DecodeAnsiBytes(const std::uint8_t* data, std::size_t size, std::wstring& text);
bool EncodeAnsiText(const std::wstring& text, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage);

} // namespace classic_notepad
