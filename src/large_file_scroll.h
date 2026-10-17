#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace classic_notepad {

// Maps a 64-bit document's legal top-row domain to the position domain that
// Win32 actually exposes after applying nPage. For small documents the mapping
// is exact; larger documents use the complete bounded integer thumb range.
class LargeFileScrollMapping {
public:
    static LargeFileScrollMapping Create(std::uint64_t totalRows, std::uint64_t visibleRows)
    {
        LargeFileScrollMapping result;
        result.totalRows_ = std::max<std::uint64_t>(1U, totalRows);
        result.visibleRows_ = std::max<std::uint64_t>(1U, visibleRows);
        result.maximumTop_ = result.totalRows_ > result.visibleRows_
            ? result.totalRows_ - result.visibleRows_ : 0U;

        constexpr std::uint64_t maximumRangeItems =
            static_cast<std::uint64_t>((std::numeric_limits<int>::max)());
        if (result.totalRows_ <= maximumRangeItems) {
            result.rangeItems_ = result.totalRows_;
            result.page_ = static_cast<std::uint32_t>(std::min(
                result.visibleRows_, result.rangeItems_));
        } else {
            result.rangeItems_ = maximumRangeItems;
            if (result.visibleRows_ >= result.totalRows_) {
                result.page_ = static_cast<std::uint32_t>(result.rangeItems_);
            } else {
                std::uint64_t remainder = 0U;
                std::uint64_t scaledPage = MultiplyDivideFloor(
                    result.visibleRows_, result.rangeItems_, result.totalRows_, &remainder);
                if (remainder != 0U) ++scaledPage;
                const std::uint64_t maximumPage = result.maximumTop_ == 0U
                    ? result.rangeItems_ : result.rangeItems_ - 1U;
                result.page_ = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
                    scaledPage, 1U, maximumPage));
            }
        }

        result.rangeMaximum_ = static_cast<int>(result.rangeItems_ - 1U);
        result.maximumPosition_ = result.rangeItems_ - result.page_;
        return result;
    }

    std::uint64_t TotalRows() const { return totalRows_; }
    std::uint64_t VisibleRows() const { return visibleRows_; }
    std::uint64_t MaximumTop() const { return maximumTop_; }
    int RangeMaximum() const { return rangeMaximum_; }
    std::uint32_t Page() const { return page_; }
    std::uint64_t MaximumPosition() const { return maximumPosition_; }

    int TopToPosition(std::uint64_t topRow) const
    {
        if (maximumTop_ == 0U || maximumPosition_ == 0U) return 0;
        const std::uint64_t bounded = std::min(topRow, maximumTop_);
        return static_cast<int>(MultiplyDivideFloor(bounded, maximumPosition_, maximumTop_));
    }

    std::uint64_t PositionToTop(std::uint64_t position) const
    {
        if (maximumTop_ == 0U || maximumPosition_ == 0U) return 0U;
        const std::uint64_t bounded = std::min(position, maximumPosition_);
        return MultiplyDivideFloor(bounded, maximumTop_, maximumPosition_);
    }

private:
    // Exact floor(a*b/divisor) for a <= divisor without forming the product.
    // The same recurrence also returns the remainder for ceil page scaling.
    static std::uint64_t MultiplyDivideFloor(
        std::uint64_t a,
        std::uint64_t b,
        std::uint64_t divisor,
        std::uint64_t* remainder = nullptr)
    {
        if (divisor == 0U || a == 0U || b == 0U) {
            if (remainder != nullptr) *remainder = 0U;
            return 0U;
        }
        a = std::min(a, divisor);
        std::uint64_t quotient = 0U;
        std::uint64_t rest = 0U;
        for (int bit = 63; bit >= 0; --bit) {
            quotient *= 2U;
            if (rest >= divisor - rest) {
                rest -= divisor - rest;
                ++quotient;
            } else {
                rest += rest;
            }
            if (((b >> bit) & 1U) != 0U) {
                if (rest >= divisor - a) {
                    rest -= divisor - a;
                    ++quotient;
                } else {
                    rest += a;
                }
            }
        }
        if (remainder != nullptr) *remainder = rest;
        return quotient;
    }

    std::uint64_t totalRows_ = 1U;
    std::uint64_t visibleRows_ = 1U;
    std::uint64_t maximumTop_ = 0U;
    std::uint64_t rangeItems_ = 1U;
    std::uint64_t maximumPosition_ = 0U;
    int rangeMaximum_ = 0;
    std::uint32_t page_ = 1U;
};

} // namespace classic_notepad
