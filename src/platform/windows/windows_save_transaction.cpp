#include "windows_save_transaction.h"

#include <atomic>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace classic_notepad {
namespace {

std::atomic<std::uint64_t> gNextSaveFileId{1};
std::atomic<bool> gReopenFaultInjected{false};
std::atomic<bool> gDestructiveReplacementFaultInjected{false};
constexpr int kFileRenameInfoExClass = 22;
constexpr DWORD kFileRenameReplaceIfExists = 0x00000001;
constexpr DWORD kFileRenamePosixSemantics = 0x00000002;

struct FileRenameInfoExBuffer {
    DWORD flags;
    HANDLE rootDirectory;
    DWORD fileNameLength;
    WCHAR fileName[1];
};

std::wstring WindowsErrorMessage(DWORD errorCode)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    if (length == 0 || buffer == nullptr) return L"Unknown Windows error.";
    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

std::wstring BuildTemporaryPath(const std::wstring& destinationPath, int attempt)
{
    return destinationPath + L".classic-notepad-save-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(gNextSaveFileId.fetch_add(1)) + L"-" +
        std::to_wstring(attempt) + L".tmp";
}

bool MoveDestinationAsideForTest(const std::wstring& destinationPath, DWORD& errorCode)
{
    constexpr int kAttempts = 64;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const std::wstring backupPath = destinationPath + L".classic-notepad-test-original-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(attempt) + L".bak";
        if (MoveFileExW(destinationPath.c_str(), backupPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
            errorCode = ERROR_SUCCESS;
            return true;
        }
        errorCode = GetLastError();
        if (errorCode != ERROR_FILE_EXISTS && errorCode != ERROR_ALREADY_EXISTS) return false;
    }
    errorCode = ERROR_FILE_EXISTS;
    return false;
}

bool GetFileIdentity(const std::wstring& path, DWORD& volume, DWORD& indexHigh, DWORD& indexLow)
{
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL okay = GetFileInformationByHandle(file, &info);
    CloseHandle(file);
    if (!okay) return false;
    volume = info.dwVolumeSerialNumber;
    indexHigh = info.nFileIndexHigh;
    indexLow = info.nFileIndexLow;
    return true;
}

bool SameFileIdentity(
    const std::wstring& path,
    DWORD expectedVolume,
    DWORD expectedIndexHigh,
    DWORD expectedIndexLow)
{
    DWORD volume = 0;
    DWORD indexHigh = 0;
    DWORD indexLow = 0;
    return GetFileIdentity(path, volume, indexHigh, indexLow) && volume == expectedVolume &&
        indexHigh == expectedIndexHigh && indexLow == expectedIndexLow;
}

bool RenameWithPosixSemantics(
    const std::wstring& temporaryPath,
    const std::wstring& destinationPath,
    DWORD& errorCode)
{
    HANDLE file = CreateFileW(temporaryPath.c_str(), DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errorCode = GetLastError();
        return false;
    }

    const std::size_t fileNameBytes = destinationPath.size() * sizeof(wchar_t);
    const std::size_t headerBytes = offsetof(FileRenameInfoExBuffer, fileName);
    const std::size_t maximumBufferBytes = std::min<std::size_t>(
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()),
        (std::numeric_limits<std::size_t>::max)());
    if (headerBytes > maximumBufferBytes - sizeof(wchar_t) ||
        fileNameBytes > maximumBufferBytes - headerBytes - sizeof(wchar_t) ||
        fileNameBytes > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
        CloseHandle(file);
        errorCode = ERROR_FILENAME_EXCED_RANGE;
        return false;
    }
    std::vector<std::uint8_t> buffer(headerBytes + fileNameBytes + sizeof(wchar_t));
    auto* renameInfo = reinterpret_cast<FileRenameInfoExBuffer*>(buffer.data());
    renameInfo->flags = kFileRenameReplaceIfExists | kFileRenamePosixSemantics;
    renameInfo->rootDirectory = nullptr;
    renameInfo->fileNameLength = static_cast<DWORD>(fileNameBytes);
    if (fileNameBytes != 0U) {
        std::memcpy(renameInfo->fileName, destinationPath.data(), fileNameBytes);
    }
    renameInfo->fileName[destinationPath.size()] = L'\0';

    const BOOL okay = SetFileInformationByHandle(file,
        static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileRenameInfoExClass), renameInfo,
        static_cast<DWORD>(buffer.size()));
    errorCode = okay ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    return okay != FALSE;
}

} // namespace

WindowsSaveTransaction::~WindowsSaveTransaction()
{
    if (output_ != INVALID_HANDLE_VALUE) CloseHandle(output_);
}

bool WindowsSaveTransaction::Create(const std::wstring& destinationPath)
{
    diagnostics_ = {};
    errorMessage_.clear();
    temporaryPath_.clear();
    if (WindowsSaveFaultRequested(L"create")) {
        SetFailure(SaveFailureStage::Create, ERROR_ACCESS_DENIED);
        return false;
    }
    constexpr int kCreateAttempts = 64;
    for (int attempt = 0; attempt < kCreateAttempts; ++attempt) {
        temporaryPath_ = BuildTemporaryPath(destinationPath, attempt);
        output_ = CreateFileW(temporaryPath_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (output_ != INVALID_HANDLE_VALUE) {
            ownsTemporary_ = true;
            return true;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            SetFailure(SaveFailureStage::Create, error);
            temporaryPath_.clear();
            return false;
        }
    }
    SetFailure(SaveFailureStage::Create, ERROR_FILE_EXISTS);
    temporaryPath_.clear();
    return false;
}

bool WindowsSaveTransaction::Write(const std::uint8_t* bytes, std::size_t length)
{
    if (output_ == INVALID_HANDLE_VALUE) {
        SetFailure(SaveFailureStage::Write, ERROR_INVALID_HANDLE);
        return false;
    }
    std::size_t offset = 0;
    while (offset < length) {
        if (!writeFaultInjected_ && WindowsSaveFaultRequested(L"write")) {
            writeFaultInjected_ = true;
            SetFailure(SaveFailureStage::Write, ERROR_WRITE_FAULT);
            return false;
        }
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            length - offset, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(output_, bytes + offset, request, &written, nullptr)) {
            SetFailure(SaveFailureStage::Write, GetLastError());
            return false;
        }
        if (written == 0U) {
            SetFailure(SaveFailureStage::Write, ERROR_WRITE_FAULT);
            return false;
        }
        offset += written;
    }
    return true;
}

bool WindowsSaveTransaction::Flush()
{
    if (output_ == INVALID_HANDLE_VALUE) {
        SetFailure(SaveFailureStage::Flush, ERROR_INVALID_HANDLE);
        return false;
    }
    bool flushed = false;
    if (WindowsSaveFaultRequested(L"flush")) {
        SetFailure(SaveFailureStage::Flush, ERROR_WRITE_FAULT);
    } else if (!FlushFileBuffers(output_)) {
        SetFailure(SaveFailureStage::Flush, GetLastError());
    } else {
        flushed = true;
    }
    const BOOL closed = CloseHandle(output_);
    output_ = INVALID_HANDLE_VALUE;
    if (!closed && flushed) {
        flushed = false;
        SetFailure(SaveFailureStage::Flush, GetLastError());
    }
    if (!flushed) return false;
    diagnostics_.failureStage = SaveFailureStage::None;
    diagnostics_.win32Error = ERROR_SUCCESS;
    diagnostics_.outputComplete = true;
    diagnostics_.committed = false;
    diagnostics_.recoveryPath = temporaryPath_;
    return true;
}

bool WindowsSaveTransaction::Commit(const std::wstring& destinationPath)
{
    if (output_ != INVALID_HANDLE_VALUE && !Flush()) return false;
    const bool committed = ReplaceWindowsSaveFile(temporaryPath_, destinationPath, diagnostics_, errorMessage_);
    if (committed) ownsTemporary_ = false;
    return committed;
}

void WindowsSaveTransaction::SetReopenFailure(DWORD errorCode, const std::wstring& committedPath)
{
    diagnostics_.failureStage = SaveFailureStage::Reopen;
    diagnostics_.win32Error = errorCode;
    diagnostics_.outputComplete = true;
    diagnostics_.committed = true;
    diagnostics_.recoveryPath = committedPath;
    errorMessage_ = FormatWindowsSaveError(diagnostics_, L"The saved file was committed, but reopening it for editing failed.");
}

void WindowsSaveTransaction::DeleteTemporary()
{
    if (output_ != INVALID_HANDLE_VALUE) {
        CloseHandle(output_);
        output_ = INVALID_HANDLE_VALUE;
    }
    if (ownsTemporary_ && !temporaryPath_.empty()) DeleteFileW(temporaryPath_.c_str());
    ownsTemporary_ = false;
    diagnostics_.outputComplete = false;
    diagnostics_.recoveryPath.clear();
}

void WindowsSaveTransaction::SetFailure(SaveFailureStage stage, DWORD errorCode)
{
    diagnostics_.failureStage = stage;
    diagnostics_.win32Error = errorCode;
    diagnostics_.committed = false;
    diagnostics_.outputComplete = false;
    diagnostics_.recoveryPath.clear();
    errorMessage_ = FormatWindowsSaveError(diagnostics_, L"The save failed");
}

bool ReplaceWindowsSaveFile(
    const std::wstring& temporaryPath,
    const std::wstring& destinationPath,
    SaveDiagnostics& diagnostics,
    std::wstring& errorMessage)
{
    DWORD tempVolume = 0;
    DWORD tempIndexHigh = 0;
    DWORD tempIndexLow = 0;
    const bool haveIdentity = GetFileIdentity(temporaryPath, tempVolume, tempIndexHigh, tempIndexLow);
    const DWORD destinationAttributes = GetFileAttributesW(destinationPath.c_str());
    bool injectLateReplaceError = false;
    if (destinationAttributes != INVALID_FILE_ATTRIBUTES &&
        (destinationAttributes & FILE_ATTRIBUTE_READONLY) != 0U) {
        diagnostics.failureStage = SaveFailureStage::Replace;
        diagnostics.win32Error = ERROR_ACCESS_DENIED;
        diagnostics.outputComplete = true;
        diagnostics.committed = false;
        diagnostics.recoveryPath = temporaryPath;
        errorMessage = FormatWindowsSaveError(diagnostics, L"The read-only destination could not be replaced.");
        return false;
    }
    const bool injectReplacementFailure = WindowsSaveFaultRequested(L"replace");
    const bool injectDestinationMovedFailure =
        WindowsSaveFaultRequested(L"replace-destination-moved");
    if (injectReplacementFailure) {
        SetLastError(ERROR_ACCESS_DENIED);
    } else if (injectDestinationMovedFailure && destinationAttributes != INVALID_FILE_ATTRIBUTES) {
        DWORD moveError = ERROR_SUCCESS;
        if (MoveDestinationAsideForTest(destinationPath, moveError)) {
            // Materialize the destructive state with real filesystem calls:
            // the old destination is retained under a test backup name, then
            // ReplaceFileW is invoked after its destination has vanished.
            if (ReplaceFileW(destinationPath.c_str(), temporaryPath.c_str(), nullptr, 0, nullptr, nullptr)) {
                diagnostics.failureStage = SaveFailureStage::None;
                diagnostics.win32Error = ERROR_SUCCESS;
                diagnostics.outputComplete = true;
                diagnostics.committed = true;
                diagnostics.recoveryPath.clear();
                errorMessage.clear();
                return true;
            }
            SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
        } else {
            SetLastError(moveError);
        }
    } else if (destinationAttributes != INVALID_FILE_ATTRIBUTES &&
        ReplaceFileW(destinationPath.c_str(), temporaryPath.c_str(), nullptr, 0, nullptr, nullptr)) {
        injectLateReplaceError = WindowsSaveFaultRequested(L"replace-after-commit");
        if (!injectLateReplaceError) {
            diagnostics.failureStage = SaveFailureStage::None;
            diagnostics.win32Error = ERROR_SUCCESS;
            diagnostics.outputComplete = true;
            diagnostics.committed = true;
            diagnostics.recoveryPath.clear();
            errorMessage.clear();
            return true;
        }
        // Deterministically exercise the ambiguous late-error path after the
        // real replacement has already moved the complete temp to destination.
        SetLastError(ERROR_UNABLE_TO_REMOVE_REPLACED);
    }

    DWORD error = GetLastError();
    if (haveIdentity && SameFileIdentity(destinationPath, tempVolume, tempIndexHigh, tempIndexLow)) {
        diagnostics.failureStage = SaveFailureStage::None;
        diagnostics.win32Error = ERROR_SUCCESS;
        diagnostics.outputComplete = true;
        diagnostics.committed = true;
        diagnostics.recoveryPath = destinationPath;
        errorMessage.clear();
        return true;
    }
    const bool destinationMissing = GetFileAttributesW(destinationPath.c_str()) == INVALID_FILE_ATTRIBUTES;
    const bool temporaryStillExists = GetFileAttributesW(temporaryPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool preserveDestructiveRecovery = error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 &&
        destinationMissing && temporaryStillExists;
    if (!preserveDestructiveRecovery && !injectReplacementFailure && !injectLateReplaceError &&
        RenameWithPosixSemantics(temporaryPath, destinationPath, error)) {
        diagnostics.failureStage = SaveFailureStage::None;
        diagnostics.win32Error = ERROR_SUCCESS;
        diagnostics.outputComplete = true;
        diagnostics.committed = true;
        diagnostics.recoveryPath.clear();
        errorMessage.clear();
        return true;
    }
    if (!preserveDestructiveRecovery && !injectReplacementFailure && !injectLateReplaceError &&
        (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER)) {
        if (MoveFileExW(temporaryPath.c_str(), destinationPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            diagnostics.failureStage = SaveFailureStage::None;
            diagnostics.win32Error = ERROR_SUCCESS;
            diagnostics.outputComplete = true;
            diagnostics.committed = true;
            diagnostics.recoveryPath.clear();
            errorMessage.clear();
            return true;
        }
        error = GetLastError();
    }
    diagnostics.failureStage = SaveFailureStage::Replace;
    diagnostics.win32Error = error;
    diagnostics.committed = false;
    diagnostics.outputComplete = true;
    const DWORD attributes = GetFileAttributesW(temporaryPath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        diagnostics.recoveryPath = temporaryPath;
    } else if (haveIdentity && SameFileIdentity(destinationPath, tempVolume, tempIndexHigh, tempIndexLow)) {
        // The rename reached the destination even though Windows returned an
        // error. Report the committed state accurately instead of encouraging
        // the caller to retry a save that has already replaced the file.
        diagnostics.failureStage = SaveFailureStage::None;
        diagnostics.win32Error = ERROR_SUCCESS;
        diagnostics.committed = true;
        diagnostics.recoveryPath = destinationPath;
        errorMessage.clear();
        return true;
    } else {
        diagnostics.recoveryPath.clear();
    }
    errorMessage = FormatWindowsSaveError(diagnostics, L"The complete save output could not replace the destination.");
    return false;
}

std::wstring FormatWindowsSaveError(const SaveDiagnostics& diagnostics, const std::wstring& action)
{
    std::wstring message = action;
    if (diagnostics.failureStage != SaveFailureStage::None) {
        message += L" Stage: ";
        message += SaveFailureStageName(diagnostics.failureStage);
        message += L". Windows error ";
        message += std::to_wstring(diagnostics.win32Error);
        message += L" (";
        message += WindowsErrorMessage(diagnostics.win32Error);
        message += L").";
    }
    if (diagnostics.committed) {
        message += L" The output was committed";
        if (!diagnostics.recoveryPath.empty()) message += L" at: " + diagnostics.recoveryPath;
        message += L".";
    } else if (diagnostics.outputComplete && !diagnostics.recoveryPath.empty()) {
        message += L" Complete recoverable output: " + diagnostics.recoveryPath;
    } else if (diagnostics.outputComplete) {
        message += L" The complete output is no longer available at its temporary path.";
    }
    return message;
}

const wchar_t* SaveFailureStageName(SaveFailureStage stage)
{
    switch (stage) {
    case SaveFailureStage::Create: return L"create";
    case SaveFailureStage::Prepare: return L"prepare";
    case SaveFailureStage::Write: return L"write";
    case SaveFailureStage::Flush: return L"flush";
    case SaveFailureStage::Replace: return L"replace";
    case SaveFailureStage::Reopen: return L"reopen";
    case SaveFailureStage::None: return L"none";
    }
    return L"unknown";
}

bool WindowsSaveFaultRequested(const wchar_t* stage)
{
    wchar_t configured[32]{};
    if (GetEnvironmentVariableW(L"CLASSIC_NOTEPAD_TEST_SAVE_FAULT", configured,
            static_cast<DWORD>(std::size(configured))) == 0U) {
        return false;
    }
    if (std::wstring(configured) != stage) return false;
    if (std::wstring(stage) == L"reopen") return !gReopenFaultInjected.exchange(true);
    if (std::wstring(stage) == L"replace-destination-moved") {
        return !gDestructiveReplacementFaultInjected.exchange(true);
    }
    return true;
}

} // namespace classic_notepad
