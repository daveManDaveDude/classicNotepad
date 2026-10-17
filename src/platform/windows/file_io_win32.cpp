#include "file_io.h"
#include "windows_save_transaction.h"

#include "win32_platform.h"

#include <windows.h>

#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace classic_notepad {
namespace {

class ScopedFileHandle {
public:
    explicit ScopedFileHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedFileHandle()
    {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;
    HANDLE Get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

bool IsInjectedPreparationAllocationFailure() noexcept
{
    wchar_t stage[64]{};
    const DWORD length = GetEnvironmentVariableW(
        L"CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE", stage, static_cast<DWORD>(std::size(stage)));
    return length > 0U && length < std::size(stage) && wcscmp(stage, L"file-preparation") == 0;
}

void SetAllocationFailureMessage(std::wstring& errorMessage) noexcept
{
    try {
        errorMessage.assign(L"The file could not be loaded because memory allocation failed.");
    } catch (...) {
        errorMessage.clear();
    }
}

std::wstring GetLastErrorMessage(DWORD errorCode)
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

    if (length == 0 || buffer == nullptr) {
        return L"Unknown Windows error.";
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);

    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }

    return message;
}

void SetWindowsError(std::wstring& errorMessage, const std::wstring& action, DWORD errorCode)
{
    errorMessage = action + L"\n\n" + GetLastErrorMessage(errorCode);
}

} // namespace

bool ReadFileBytes(const std::wstring& path, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
{
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        SetWindowsError(errorMessage, L"Could not open the file.", GetLastError());
        return false;
    }
    ScopedFileHandle scopedFile(file);

    try {
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(scopedFile.Get(), &fileSize)) {
            const DWORD error = GetLastError();
            SetWindowsError(errorMessage, L"Could not read the file size.", error);
            return false;
        }

        if (fileSize.QuadPart < 0) {
            errorMessage = L"Could not read the file size.";
            return false;
        }

        if (static_cast<ULONGLONG>(fileSize.QuadPart) > static_cast<ULONGLONG>(SIZE_MAX)) {
            errorMessage = L"This file is too large for this build.";
            return false;
        }

        if (IsInjectedPreparationAllocationFailure()) throw std::bad_alloc();
        bytes.resize(static_cast<std::size_t>(fileSize.QuadPart));
        std::size_t totalRead = 0;
        while (totalRead < bytes.size()) {
            const DWORD chunkSize = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size() - totalRead, static_cast<std::size_t>(0xFFFFFFFFUL)));

            DWORD bytesRead = 0;
            const BOOL readOk = ReadFile(scopedFile.Get(), bytes.data() + totalRead, chunkSize, &bytesRead, nullptr);
            if (!readOk) {
                const DWORD error = GetLastError();
                SetWindowsError(errorMessage, L"Could not read the file.", error);
                return false;
            }

            if (bytesRead == 0) {
                SetWindowsError(errorMessage, L"Could not read the complete file.", ERROR_READ_FAULT);
                return false;
            }

            totalRead += bytesRead;
        }

        return true;
    } catch (const std::bad_alloc&) {
        bytes.clear();
        SetAllocationFailureMessage(errorMessage);
        return false;
    } catch (const std::length_error&) {
        bytes.clear();
        SetAllocationFailureMessage(errorMessage);
        return false;
    }
}

bool WriteFileBytesAtomically(
    const std::wstring& path,
    const std::vector<std::uint8_t>& bytes,
    std::wstring& errorMessage,
    SaveDiagnostics* diagnostics)
{
    WindowsSaveTransaction transaction;
    bool okay = transaction.Create(path) && transaction.Write(bytes.data(), bytes.size()) && transaction.Flush();
    if (okay) okay = transaction.Commit(path);
    if (!okay) {
        if (!transaction.Diagnostics().outputComplete) transaction.DeleteTemporary();
        errorMessage = transaction.ErrorMessage();
    } else {
        errorMessage.clear();
    }
    if (diagnostics != nullptr) *diagnostics = transaction.Diagnostics();
    return okay;
}

} // namespace classic_notepad
