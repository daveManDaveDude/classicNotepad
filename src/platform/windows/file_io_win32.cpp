#include "file_io.h"

#include "win32_platform.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace classic_notepad {
namespace {

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

bool WriteBytesToHandle(HANDLE file, const std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
{
    std::size_t totalWritten = 0;
    while (totalWritten < bytes.size()) {
        const DWORD chunkSize = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - totalWritten, static_cast<std::size_t>(0xFFFFFFFFUL)));

        DWORD bytesWritten = 0;
        const BOOL writeOk = WriteFile(file, bytes.data() + totalWritten, chunkSize, &bytesWritten, nullptr);
        if (!writeOk) {
            SetWindowsError(errorMessage, L"Could not write the file.", GetLastError());
            return false;
        }

        if (bytesWritten == 0) {
            SetWindowsError(errorMessage, L"Could not write the complete file.", ERROR_WRITE_FAULT);
            return false;
        }

        totalWritten += bytesWritten;
    }

    return true;
}

std::wstring BuildTempSavePath(const std::wstring& path, int attempt)
{
    const std::wstring suffix = L".classic-notepad.tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(attempt);
    return path + suffix;
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

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        SetWindowsError(errorMessage, L"Could not read the file size.", error);
        return false;
    }

    if (fileSize.QuadPart < 0) {
        CloseHandle(file);
        errorMessage = L"Could not read the file size.";
        return false;
    }

    if (static_cast<ULONGLONG>(fileSize.QuadPart) > static_cast<ULONGLONG>(SIZE_MAX)) {
        CloseHandle(file);
        errorMessage = L"This file is too large for this build.";
        return false;
    }

    bytes.resize(static_cast<std::size_t>(fileSize.QuadPart));
    std::size_t totalRead = 0;
    while (totalRead < bytes.size()) {
        const DWORD chunkSize = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - totalRead, static_cast<std::size_t>(0xFFFFFFFFUL)));

        DWORD bytesRead = 0;
        const BOOL readOk = ReadFile(file, bytes.data() + totalRead, chunkSize, &bytesRead, nullptr);
        if (!readOk) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            SetWindowsError(errorMessage, L"Could not read the file.", error);
            return false;
        }

        if (bytesRead == 0) {
            CloseHandle(file);
            SetWindowsError(errorMessage, L"Could not read the complete file.", ERROR_READ_FAULT);
            return false;
        }

        totalRead += bytesRead;
    }

    CloseHandle(file);
    return true;
}

bool WriteFileBytesAtomically(
    const std::wstring& path,
    const std::vector<std::uint8_t>& bytes,
    std::wstring& errorMessage)
{
    constexpr int kTempPathAttempts = 64;
    std::wstring tempPath;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < kTempPathAttempts; ++attempt) {
        tempPath = BuildTempSavePath(path, attempt);
        file = CreateFileW(
            tempPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY,
            nullptr);

        if (file != INVALID_HANDLE_VALUE) {
            break;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            SetWindowsError(errorMessage, L"Could not create a temporary save file.", error);
            return false;
        }
    }

    if (file == INVALID_HANDLE_VALUE) {
        errorMessage = L"Could not allocate a temporary save file.";
        return false;
    }

    const bool wroteAllBytes = WriteBytesToHandle(file, bytes, errorMessage);
    if (!wroteAllBytes) {
        CloseHandle(file);
        DeleteFileW(tempPath.c_str());
        return false;
    }

    if (!FlushFileBuffers(file)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        DeleteFileW(tempPath.c_str());
        SetWindowsError(errorMessage, L"Could not flush the temporary save file.", error);
        return false;
    }

    CloseHandle(file);

    if (!MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(tempPath.c_str());
        SetWindowsError(errorMessage, L"Could not replace the destination file.", error);
        return false;
    }

    return true;
}

} // namespace classic_notepad
