#pragma once

#include "file_io.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace classic_notepad {

class WindowsSaveTransaction {
public:
    WindowsSaveTransaction() = default;
    ~WindowsSaveTransaction();

    WindowsSaveTransaction(const WindowsSaveTransaction&) = delete;
    WindowsSaveTransaction& operator=(const WindowsSaveTransaction&) = delete;

    bool Create(const std::wstring& destinationPath);
    bool Write(const std::uint8_t* bytes, std::size_t length);
    bool Flush();
    bool Commit(const std::wstring& destinationPath);
    void SetReopenFailure(DWORD errorCode, const std::wstring& committedPath);
    void DeleteTemporary();

    const SaveDiagnostics& Diagnostics() const { return diagnostics_; }
    SaveDiagnostics& Diagnostics() { return diagnostics_; }
    const std::wstring& TemporaryPath() const { return temporaryPath_; }
    const std::wstring& ErrorMessage() const { return errorMessage_; }
    HANDLE Handle() const { return output_; }

private:
    void SetFailure(SaveFailureStage stage, DWORD errorCode);

    HANDLE output_ = INVALID_HANDLE_VALUE;
    std::wstring temporaryPath_;
    std::wstring errorMessage_;
    SaveDiagnostics diagnostics_;
    bool writeFaultInjected_ = false;
    bool ownsTemporary_ = false;
};

bool ReplaceWindowsSaveFile(
    const std::wstring& temporaryPath,
    const std::wstring& destinationPath,
    SaveDiagnostics& diagnostics,
    std::wstring& errorMessage);

std::wstring FormatWindowsSaveError(
    const SaveDiagnostics& diagnostics,
    const std::wstring& action);

const wchar_t* SaveFailureStageName(SaveFailureStage stage);
bool WindowsSaveFaultRequested(const wchar_t* stage);

} // namespace classic_notepad
