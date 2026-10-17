#include "../../src/platform/windows/windows_text_boundary.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void ExpectIndex(
    const std::vector<int>& xBoundary,
    const std::vector<std::uint8_t>& scalarBoundary,
    int wanted,
    std::size_t expected,
    const char* message)
{
    const std::size_t actual = classic_notepad::windows::ClosestTextBoundary(
        xBoundary, scalarBoundary, wanted);
    if (actual == expected) return;
    ++failures;
    std::cerr << "FAIL: " << message << " (expected " << expected
        << ", got " << actual << ")\n";
}

void RunBoundaryChecks()
{
    ExpectIndex({}, {}, 0, 0U, "Empty boundaries return the zero fallback");
    ExpectIndex({0, 10, 20}, {1, 1}, 10, 0U,
        "Mismatched scalar bitmap returns the zero fallback");
    ExpectIndex({0, 10}, {}, 10, 0U,
        "A nonempty coordinate list with an empty scalar bitmap returns the zero fallback");
    ExpectIndex({0}, {1U}, -1, 0U, "A singleton boundary clamps a point before it");
    ExpectIndex({0}, {1U}, 0, 0U, "A singleton boundary retains an exact point");
    ExpectIndex({0}, {1U}, 1, 0U, "A singleton boundary clamps a point after it");

    const std::vector<int> simpleX{0, 10, 20};
    const std::vector<std::uint8_t> allScalar{1U, 1U, 1U};
    ExpectIndex(simpleX, allScalar, -100, 0U, "Before the first boundary clamps to the first");
    ExpectIndex(simpleX, allScalar, 0, 0U, "The first exact boundary is retained");
    ExpectIndex(simpleX, allScalar, 5, 0U, "An equal-distance tie selects the left boundary");
    ExpectIndex(simpleX, allScalar, 6, 1U, "The nearer right boundary is selected");
    ExpectIndex(simpleX, allScalar, 20, 2U, "The final exact boundary is retained");
    ExpectIndex(simpleX, allScalar, 100, 2U, "After the last boundary clamps to the last");

    const std::vector<int> repeatedX{0, 10, 10, 20};
    const std::vector<std::uint8_t> repeatedScalar{1U, 1U, 1U, 1U};
    ExpectIndex(repeatedX, repeatedScalar, 10, 1U,
        "Repeated exact coordinates resolve to the first lower-bound match");
    ExpectIndex(repeatedX, repeatedScalar, 11, 2U,
        "A point just beyond a repeated coordinate resolves to its nearer repeated boundary");

    const std::vector<int> ineligibleX{0, 10, 20};
    const std::vector<std::uint8_t> ineligibleScalar{1U, 0U, 1U};
    ExpectIndex(ineligibleX, ineligibleScalar, 8, 0U,
        "An ineligible right boundary is skipped in favor of the nearest scalar boundary");
    ExpectIndex(ineligibleX, ineligibleScalar, 12, 2U,
        "An ineligible left boundary is skipped in favor of the nearest scalar boundary");
    ExpectIndex(ineligibleX, {0U, 0U, 0U}, 10, 0U,
        "An all-ineligible bitmap uses the existing zero fallback");
    const std::vector<int> consecutiveIneligibleX{0, 10, 20, 30, 40};
    const std::vector<std::uint8_t> consecutiveIneligibleScalar{1U, 0U, 0U, 1U, 0U};
    ExpectIndex(consecutiveIneligibleX, consecutiveIneligibleScalar, 15, 0U,
        "Consecutive ineligible boundaries preserve the left tie behavior");
    ExpectIndex(consecutiveIneligibleX, consecutiveIneligibleScalar, 16, 3U,
        "Consecutive ineligible boundaries select the nearer eligible boundary");
    ExpectIndex(consecutiveIneligibleX, consecutiveIneligibleScalar, 50, 3U,
        "An ineligible tail falls back to the preceding eligible boundary");
    ExpectIndex({0, 10, 20}, {2U, 255U, 2U}, 10, 1U,
        "Any nonzero scalar bitmap value marks a valid boundary");

    // The middle entry models the interior UTF-16 boundary of a surrogate pair.
    const std::vector<int> pairX{0, 8, 8};
    const std::vector<std::uint8_t> pairScalar{1U, 0U, 1U};
    ExpectIndex(pairX, pairScalar, 7, 2U,
        "A hit adjacent to a pair interior resolves to a scalar-safe boundary");
    ExpectIndex(pairX, pairScalar, 8, 2U,
        "An exact pair-interior coordinate advances to the scalar-safe pair end");
    ExpectIndex(pairX, pairScalar, 9, 2U,
        "A point after repeated pair coordinates falls back to the scalar-safe end");

    const std::vector<int> wideExtentX{0, 1000000, 2000000};
    const std::vector<std::uint8_t> wideExtentScalar{1U, 1U, 1U};
    ExpectIndex(wideExtentX, wideExtentScalar, 1499999, 1U,
        "Large pixel extents preserve nearest-boundary selection");
    ExpectIndex(wideExtentX, wideExtentScalar, 1500000, 1U,
        "A large pixel extent tie selects the left boundary");
    ExpectIndex(wideExtentX, wideExtentScalar, 1500001, 2U,
        "A point after a large pixel extent tie selects the nearer right boundary");

    std::vector<int> longRowX(131073U);
    std::vector<std::uint8_t> longRowScalar(longRowX.size(), 1U);
    for (std::size_t index = 0; index < longRowX.size(); ++index) {
        longRowX[index] = static_cast<int>(index);
    }
    ExpectIndex(longRowX, longRowScalar, 65535, 65535U,
        "The full-width offset before the 16-bit boundary is preserved");
    ExpectIndex(longRowX, longRowScalar, 65536, 65536U,
        "The full-width offset at the 16-bit boundary is preserved");
    ExpectIndex(longRowX, longRowScalar, 131072, 131072U,
        "The final index of a long row is not truncated");

    longRowScalar[100000U] = 0U;
    ExpectIndex(longRowX, longRowScalar, 100000, 99999U,
        "Long-row scalar bitmap skips an ineligible surrogate interior with the existing left tie rule");
}

} // namespace

int main()
{
    RunBoundaryChecks();
    if (failures != 0) {
        std::cerr << failures << " Windows text-boundary check(s) failed.\n";
        return 1;
    }
    std::cout << "WindowsTextBoundaryTests passed.\n";
    return 0;
}
