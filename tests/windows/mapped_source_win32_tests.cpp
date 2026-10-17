#include "../../src/platform/windows/windows_mapped_source.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~ScopedHandle()
    {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept
    {
        HANDLE released = handle_;
        handle_ = nullptr;
        return released;
    }
    void reset(HANDLE handle = nullptr) noexcept
    {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

struct DeleteTempFile {
    const wchar_t* path = nullptr;
    ~DeleteTempFile()
    {
        if (path != nullptr) DeleteFileW(path);
    }
};

bool RunMappedSourceChecks()
{
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const DWORD granularity = std::max<DWORD>(1U, systemInfo.dwAllocationGranularity);
    auto invalidMapping = std::make_shared<classic_notepad::windows::SharedMappedSource>(
        INVALID_HANDLE_VALUE, nullptr, 1024U, granularity);
    std::vector<std::uint8_t> invalidRead{0xA5U, 0x5AU};
    Expect(!invalidMapping->Read(0U, 1U, invalidRead),
        "A nonempty read rejects an invalid mapping");
    Expect(invalidRead.empty(), "A nonempty read against an invalid mapping clears output");
    invalidRead.assign({0xA5U, 0x5AU});
    Expect(!invalidMapping->Read(0U, 0U, invalidRead),
        "A zero-length read rejects an invalid mapping before zero-length success");
    Expect(invalidRead.empty(), "A zero-length read against an invalid mapping clears output");

    wchar_t tempDirectory[MAX_PATH + 1U]{};
    const DWORD tempDirectoryLength = GetTempPathW(
        static_cast<DWORD>(std::size(tempDirectory)), tempDirectory);
    if (tempDirectoryLength == 0U || tempDirectoryLength >= std::size(tempDirectory)) {
        Expect(false, "Windows returns a temporary directory for the mapped-source fixture");
        return false;
    }

    wchar_t tempPath[MAX_PATH + 1U]{};
    if (GetTempFileNameW(tempDirectory, L"map", 0U, tempPath) == 0U) {
        Expect(false, "Windows creates a temporary mapped-source fixture path");
        return false;
    }
    const DeleteTempFile deleteTemp{tempPath};

    ScopedHandle writer(CreateFileW(tempPath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (writer.get() == INVALID_HANDLE_VALUE) {
        Expect(false, "The temporary mapped-source fixture opens for writing");
        return false;
    }

    const std::size_t fileSize = static_cast<std::size_t>(granularity) * 2U + 257U;
    std::vector<std::uint8_t> expected(fileSize);
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        expected[index] = static_cast<std::uint8_t>((index * 37U + index / 11U + 19U) % 251U);
    }

    DWORD written = 0U;
    if (!WriteFile(writer.get(), expected.data(), static_cast<DWORD>(expected.size()), &written, nullptr) ||
        written != expected.size() || !FlushFileBuffers(writer.get())) {
        Expect(false, "The deterministic mapped-source bytes are written completely");
        return false;
    }

    ScopedHandle originalMapping(CreateFileMappingW(writer.get(), nullptr, PAGE_READONLY, 0U, 0U, nullptr));
    if (originalMapping.get() == nullptr) {
        Expect(false, "The temporary file has a read-only file mapping");
        return false;
    }

    HANDLE duplicatedFile = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), writer.get(), GetCurrentProcess(), &duplicatedFile,
            0U, FALSE, DUPLICATE_SAME_ACCESS)) {
        Expect(false, "The fixture duplicates its source file handle");
        return false;
    }
    ScopedHandle duplicatedFileGuard(duplicatedFile);

    HANDLE duplicatedMapping = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), originalMapping.get(), GetCurrentProcess(), &duplicatedMapping,
            0U, FALSE, DUPLICATE_SAME_ACCESS)) {
        Expect(false, "The fixture duplicates its mapping handle");
        return false;
    }
    ScopedHandle duplicatedMappingGuard(duplicatedMapping);
    const HANDLE closedFileValue = duplicatedFile;
    const HANDLE closedMappingValue = duplicatedMapping;

    auto source = std::make_shared<classic_notepad::windows::SharedMappedSource>(
        duplicatedFileGuard.get(), duplicatedMappingGuard.get(),
        static_cast<std::uint64_t>(fileSize), granularity);
    duplicatedFileGuard.release();
    duplicatedMappingGuard.release();

    // Close the handles used to construct the duplicates. Reads must now depend
    // only on the shared source's owned duplicates.
    originalMapping.reset();
    writer.reset();

    const auto verifyRead = [&](std::uint64_t offset, std::size_t length, const char* label) {
        std::vector<std::uint8_t> actual{0xA5U, 0x5AU};
        const bool read = source->Read(offset, length, actual);
        const bool inRange = offset <= fileSize &&
            static_cast<std::uint64_t>(length) <= static_cast<std::uint64_t>(fileSize) - offset;
        Expect(read == inRange, label);
        if (!inRange) {
            Expect(actual.empty(), "A rejected read clears the destination vector");
            return;
        }
        const auto first = expected.begin() + static_cast<std::ptrdiff_t>(offset);
        const auto last = first + static_cast<std::ptrdiff_t>(length);
        Expect(actual == std::vector<std::uint8_t>(first, last), label);
    };

    verifyRead(0U, 32U, "A read from the beginning returns exact mapped bytes");
    verifyRead(static_cast<std::uint64_t>(granularity) - 3U, 11U,
        "An unaligned read crossing an allocation-granularity boundary returns exact bytes");
    verifyRead(static_cast<std::uint64_t>(granularity) * 2U - 5U, 19U,
        "A second unaligned read uses full file offsets");
    verifyRead(static_cast<std::uint64_t>(fileSize) - 17U, 17U,
        "A read ending at the file boundary succeeds exactly");
    verifyRead(static_cast<std::uint64_t>(fileSize), 0U,
        "A zero-length read at EOF succeeds and clears prior output");
    verifyRead(static_cast<std::uint64_t>(fileSize) - 2U, 3U,
        "A request beyond EOF is rejected");
    verifyRead(static_cast<std::uint64_t>(fileSize) + 1U, 0U,
        "An out-of-range zero-length request is rejected");

    for (std::size_t repeat = 0U; repeat < 64U; ++repeat) {
        const std::uint64_t offset = static_cast<std::uint64_t>(repeat) * 997U;
        verifyRead(offset, 53U, "Repeated unaligned reads remain exact");
    }

    auto retainedSnapshot = source;
    source.reset();
    Expect(retainedSnapshot.use_count() == 1U,
        "A copied shared owner retains the source after the first owner is released");
    std::vector<std::uint8_t> retainedRead;
    Expect(retainedSnapshot->Read(1U, 5U, retainedRead) &&
        std::equal(retainedRead.begin(), retainedRead.end(), expected.begin() + 1U),
        "The retained shared owner continues reading after the original handles and first owner close");
    retainedSnapshot.reset();

    DWORD handleFlags = 0U;
    SetLastError(ERROR_SUCCESS);
    const BOOL fileStillOpen = GetHandleInformation(closedFileValue, &handleFlags);
    Expect(!fileStillOpen && GetLastError() == ERROR_INVALID_HANDLE,
        "Final shared destruction closes its duplicated file handle");
    SetLastError(ERROR_SUCCESS);
    const BOOL mappingStillOpen = GetHandleInformation(closedMappingValue, &handleFlags);
    Expect(!mappingStillOpen && GetLastError() == ERROR_INVALID_HANDLE,
        "Final shared destruction closes its duplicated mapping handle");
    return failures == 0;
}

} // namespace

int main()
{
    if (!RunMappedSourceChecks() || failures != 0) {
        std::cerr << failures << " Windows mapped-source test(s) failed.\n";
        return 1;
    }
    std::cout << "WindowsMappedSourceTests passed.\n";
    return 0;
}
