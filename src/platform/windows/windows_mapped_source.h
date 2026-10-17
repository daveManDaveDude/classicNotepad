#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace classic_notepad::windows {

// Shared owner for duplicated read-only file and mapping handles used by
// LargeDocument readers and their snapshots.
class SharedMappedSource {
public:
    SharedMappedSource(
        HANDLE file,
        HANDLE mapping,
        std::uint64_t fileSize,
        DWORD allocationGranularity) noexcept;
    ~SharedMappedSource();

    SharedMappedSource(const SharedMappedSource&) = delete;
    SharedMappedSource& operator=(const SharedMappedSource&) = delete;

    bool Read(
        std::uint64_t offset,
        std::size_t length,
        std::vector<std::uint8_t>& bytes) const;

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    std::uint64_t fileSize_ = 0;
    DWORD allocationGranularity_ = 65536;
};

} // namespace classic_notepad::windows
