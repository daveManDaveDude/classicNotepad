#include "file_io.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace classic_notepad {
namespace {

std::filesystem::path MakePath(const std::wstring& path)
{
    return std::filesystem::path(path);
}

std::wstring ErrorMessage(const wchar_t* action, const std::error_code& error)
{
    const std::string message = error.message();
    return std::wstring(action) + L"\n\n" + std::wstring(message.begin(), message.end());
}

std::filesystem::path BuildTempSavePath(const std::filesystem::path& path, int attempt)
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path tempPath = path;
    tempPath += L".classic-notepad.tmp.";
    tempPath += std::to_wstring(ticks);
    tempPath += L".";
    tempPath += std::to_wstring(attempt);
    return tempPath;
}

} // namespace

bool ReadFileBytes(const std::wstring& path, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
{
    const std::filesystem::path filePath = MakePath(path);
    std::error_code error;
    const std::uintmax_t fileSize = std::filesystem::file_size(filePath, error);
    if (error) {
        errorMessage = ErrorMessage(L"Could not read the file size.", error);
        return false;
    }

    if (fileSize > static_cast<std::uintmax_t>(SIZE_MAX)) {
        errorMessage = L"This file is too large for this build.";
        return false;
    }

    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        errorMessage = L"Could not open the file.";
        return false;
    }

    bytes.resize(static_cast<std::size_t>(fileSize));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            errorMessage = L"Could not read the complete file.";
            return false;
        }
    }

    return true;
}

bool WriteFileBytesAtomically(
    const std::wstring& path,
    const std::vector<std::uint8_t>& bytes,
    std::wstring& errorMessage)
{
    const std::filesystem::path targetPath = MakePath(path);
    constexpr int kTempPathAttempts = 64;

    std::filesystem::path tempPath;
    bool allocatedTempPath = false;
    for (int attempt = 0; attempt < kTempPathAttempts; ++attempt) {
        tempPath = BuildTempSavePath(targetPath, attempt);
        if (!std::filesystem::exists(tempPath)) {
            allocatedTempPath = true;
            break;
        }
    }

    if (!allocatedTempPath) {
        errorMessage = L"Could not allocate a temporary save file.";
        return false;
    }

    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            errorMessage = L"Could not create a temporary save file.";
            return false;
        }

        if (!bytes.empty()) {
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        output.flush();

        if (!output) {
            std::error_code removeError;
            std::filesystem::remove(tempPath, removeError);
            errorMessage = L"Could not write the complete file.";
            return false;
        }
    }

    std::error_code error;
    std::filesystem::rename(tempPath, targetPath, error);
    if (error) {
        std::error_code removeTargetError;
        std::filesystem::remove(targetPath, removeTargetError);
        error.clear();
        std::filesystem::rename(tempPath, targetPath, error);
    }

    if (error) {
        std::error_code removeTempError;
        std::filesystem::remove(tempPath, removeTempError);
        errorMessage = ErrorMessage(L"Could not replace the destination file.", error);
        return false;
    }

    return true;
}

} // namespace classic_notepad
