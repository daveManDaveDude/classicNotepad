#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace classic_notepad {

enum class SaveFailureStage {
    None,
    Create,
    Prepare,
    Write,
    Flush,
    Replace,
    Reopen
};

struct SaveDiagnostics {
    SaveFailureStage failureStage = SaveFailureStage::None;
    std::uint32_t win32Error = 0;
    bool outputComplete = false;
    bool committed = false;
    std::wstring recoveryPath;
};

bool ReadFileBytes(const std::wstring& path, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage);
bool WriteFileBytesAtomically(
    const std::wstring& path,
    const std::vector<std::uint8_t>& bytes,
    std::wstring& errorMessage,
    SaveDiagnostics* diagnostics = nullptr);

} // namespace classic_notepad
