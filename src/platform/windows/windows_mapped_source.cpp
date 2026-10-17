#include "windows_mapped_source.h"

#include <limits>

namespace classic_notepad::windows {
namespace {

class MappedViewCleanup {
public:
    explicit MappedViewCleanup(const void* view) noexcept : view_(view) {}
    ~MappedViewCleanup() noexcept
    {
        if (view_ != nullptr) UnmapViewOfFile(view_);
    }

    MappedViewCleanup(const MappedViewCleanup&) = delete;
    MappedViewCleanup& operator=(const MappedViewCleanup&) = delete;

private:
    const void* view_;
};

} // namespace

SharedMappedSource::SharedMappedSource(
    HANDLE file,
    HANDLE mapping,
    std::uint64_t fileSize,
    DWORD allocationGranularity) noexcept
    : file_(file)
    , mapping_(mapping)
    , fileSize_(fileSize)
    , allocationGranularity_(allocationGranularity)
{
}

SharedMappedSource::~SharedMappedSource()
{
    if (mapping_ != nullptr) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

bool SharedMappedSource::Read(
    std::uint64_t offset,
    std::size_t length,
    std::vector<std::uint8_t>& bytes) const
{
    bytes.clear();
    if (mapping_ == nullptr || offset > fileSize_ ||
        static_cast<std::uint64_t>(length) > fileSize_ - offset) return false;
    if (length == 0U) return true;
    const std::uint64_t alignedOffset = offset - (offset % allocationGranularity_);
    const std::uint64_t delta64 = offset - alignedOffset;
    if (delta64 > static_cast<std::uint64_t>((std::numeric_limits<SIZE_T>::max)()) ||
        length > static_cast<std::size_t>((std::numeric_limits<SIZE_T>::max)() - static_cast<SIZE_T>(delta64))) {
        return false;
    }
    const SIZE_T viewLength = static_cast<SIZE_T>(delta64) + static_cast<SIZE_T>(length);
    const DWORD high = static_cast<DWORD>(alignedOffset >> 32U);
    const DWORD low = static_cast<DWORD>(alignedOffset & 0xFFFFFFFFULL);
    const auto* view = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, high, low, viewLength));
    if (view == nullptr) return false;
    const MappedViewCleanup mappingCleanup(view);
    bytes.assign(view + static_cast<SIZE_T>(delta64), view + static_cast<SIZE_T>(delta64) + length);
    return true;
}

} // namespace classic_notepad::windows
