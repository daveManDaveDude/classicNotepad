#include "document.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

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

bool ReadAllBytes(const std::wstring& path, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
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

bool WriteAllBytes(const std::wstring& path, const std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
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

} // namespace

bool Document::Load(const std::wstring& path, std::wstring& editorText, std::wstring& errorMessage)
{
    std::vector<std::uint8_t> bytes;
    if (!ReadAllBytes(path, bytes, errorMessage)) {
        return false;
    }

    classic_notepad::DecodeTextResult decoded;
    if (!classic_notepad::DecodeTextBytes(bytes, decoded, errorMessage)) {
        return false;
    }

    const classic_notepad::LineEndingAnalysis lineEndings = classic_notepad::AnalyzeLineEndings(decoded.text);

    path_ = path;
    encoding_ = decoded.encoding;
    lineEnding_ = lineEndings.style;
    // The edit control normalizes all line breaks, so mixed files save using the dominant style.
    saveLineEnding_ = lineEndings.dominantStyle;
    editorText = classic_notepad::NormalizeLineEndingsForEditor(decoded.text);
    modified_ = false;
    return true;
}

bool Document::Save(const std::wstring& editorText, std::wstring& errorMessage)
{
    if (path_.empty()) {
        errorMessage = L"No file path has been selected.";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    const LineEndingStyle targetLineEnding =
        lineEnding_ == LineEndingStyle::Mixed ? saveLineEnding_ : lineEnding_;
    const std::wstring diskText = classic_notepad::ConvertLineEndingsFromEditor(editorText, targetLineEnding);
    if (!classic_notepad::EncodeTextBytes(diskText, encoding_, bytes, errorMessage)) {
        return false;
    }

    if (!WriteAllBytes(path_, bytes, errorMessage)) {
        return false;
    }

    modified_ = false;
    return true;
}

bool Document::SaveAs(const std::wstring& path, const std::wstring& editorText, std::wstring& errorMessage)
{
    const std::wstring previousPath = path_;
    const bool previousModified = modified_;

    path_ = path;
    if (!Save(editorText, errorMessage)) {
        path_ = previousPath;
        modified_ = previousModified;
        return false;
    }

    return true;
}

void Document::ResetUntitled()
{
    path_.clear();
    modified_ = false;
    encoding_ = TextEncoding::Utf8NoBom;
    lineEnding_ = LineEndingStyle::Crlf;
    saveLineEnding_ = LineEndingStyle::Crlf;
}

void Document::ResetNewFile(const std::wstring& path)
{
    ResetUntitled();
    path_ = path;
}

bool Document::HasPath() const
{
    return !path_.empty();
}

bool Document::IsModified() const
{
    return modified_;
}

void Document::SetModified(bool modified)
{
    modified_ = modified;
}

const std::wstring& Document::Path() const
{
    return path_;
}

std::wstring Document::DisplayName() const
{
    if (path_.empty()) {
        return L"Untitled";
    }

    const std::size_t separator = path_.find_last_of(L"\\/");
    if (separator == std::wstring::npos || separator + 1U >= path_.size()) {
        return path_;
    }

    return path_.substr(separator + 1U);
}

Document::TextEncoding Document::Encoding() const
{
    return encoding_;
}

Document::LineEndingStyle Document::LineEnding() const
{
    return lineEnding_;
}

Document::LineEndingStyle Document::SaveLineEnding() const
{
    return saveLineEnding_;
}
