#include "windows_text_boundary.h"

#include <algorithm>

namespace classic_notepad::windows {

std::size_t ClosestTextBoundary(
    const std::vector<int>& xBoundary,
    const std::vector<std::uint8_t>& scalarBoundary,
    int wanted)
{
    if (xBoundary.empty() || scalarBoundary.size() != xBoundary.size()) return 0U;
    const auto found = std::lower_bound(xBoundary.begin(), xBoundary.end(), wanted);
    std::size_t right = static_cast<std::size_t>(found - xBoundary.begin());
    while (right < scalarBoundary.size() && scalarBoundary[right] == 0U) ++right;
    if (right >= scalarBoundary.size()) {
        right = scalarBoundary.size() - 1U;
        while (right > 0U && scalarBoundary[right] == 0U) --right;
        return right;
    }
    if (right == 0U) return 0U;
    std::size_t left = right - 1U;
    while (left > 0U && scalarBoundary[left] == 0U) --left;
    const int leftDistance = std::max(0, wanted - xBoundary[left]);
    const int rightDistance = std::max(0, xBoundary[right] - wanted);
    return leftDistance > rightDistance ? right : left;
}

} // namespace classic_notepad::windows
