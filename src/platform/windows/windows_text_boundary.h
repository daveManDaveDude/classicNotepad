#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace classic_notepad::windows {

std::size_t ClosestTextBoundary(
    const std::vector<int>& xBoundary,
    const std::vector<std::uint8_t>& scalarBoundary,
    int wanted);

} // namespace classic_notepad::windows
