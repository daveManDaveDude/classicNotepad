#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace classic_notepad {

struct TextRange {
    std::size_t start = 0;
    std::size_t length = 0;
};

struct SpellingRevision {
    std::uint64_t session = 0;
    std::uint64_t content = 0;
    std::uint64_t layout = 0;
};

struct SpellingTextWindow {
    SpellingRevision revision;
    std::uint64_t absoluteStart = 0;
    std::wstring text;
    std::size_t eligibleStart = 0;
    std::size_t eligibleEnd = 0;
};

struct SpellingAnnotation {
    std::uint64_t start = 0;
    std::uint64_t length = 0;
};

struct SpellingAnnotationBatch {
    SpellingRevision revision;
    std::vector<SpellingAnnotation> annotations;
    bool valid = false;
};

struct SpellingContextTarget {
    SpellingRevision revision;
    std::uint64_t start = 0;
    std::uint64_t length = 0;
    std::wstring text;
};

constexpr std::size_t kMaximumSpellingWindowUnits = 96U * 1024U;

bool IsUtf16ScalarBoundary(const std::wstring& text, std::size_t offset);
bool IsSpellingWordContinuationUnit(wchar_t character);
void SetSpellingEligibleBounds(
    SpellingTextWindow& window,
    bool leftBoundaryContinuesWord,
    bool rightBoundaryContinuesWord);
bool MapSpellingBackendRanges(
    const SpellingTextWindow& window,
    const std::vector<TextRange>& backendRanges,
    std::vector<SpellingAnnotation>& annotations);
bool CheckSpellingWindows(
    const std::vector<SpellingTextWindow>& windows,
    const std::function<std::vector<TextRange>(const std::wstring&)>& backend,
    SpellingAnnotationBatch& candidate);
bool ResolveSpellingContextTarget(
    const std::vector<SpellingTextWindow>& windows,
    const SpellingRevision& expectedRevision,
    std::uint64_t position,
    bool preferPreviousAtBoundary,
    SpellingContextTarget& target);
bool SpellingRevisionMatches(const SpellingRevision& left, const SpellingRevision& right);
bool SpellingBatchIsCurrent(const SpellingAnnotationBatch& batch, const SpellingRevision& currentRevision);
bool TryPublishSpellingBatch(
    SpellingAnnotationBatch& published,
    const SpellingAnnotationBatch& candidate,
    const SpellingRevision& currentRevision);

bool ExpandWordRangeAt(const std::wstring& text, std::size_t index, TextRange& range);
std::vector<TextRange> FindSpellCheckWordRanges(const std::wstring& text);
bool RangesOverlap(std::size_t firstStart, std::size_t firstLength, std::size_t secondStart, std::size_t secondLength);

} // namespace classic_notepad
