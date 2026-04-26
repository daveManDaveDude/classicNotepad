#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace classic_notepad {

bool ReadFileBytes(const std::wstring& path, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage);
bool WriteFileBytesAtomically(
    const std::wstring& path,
    const std::vector<std::uint8_t>& bytes,
    std::wstring& errorMessage);

} // namespace classic_notepad
