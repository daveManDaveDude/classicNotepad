#include "spell_text_utils.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <utility>

namespace classic_notepad {
namespace {

bool IsHighSurrogate(wchar_t value)
{
    const std::uint32_t unit = static_cast<std::uint32_t>(value);
    return unit >= 0xD800U && unit <= 0xDBFFU;
}

bool IsLowSurrogate(wchar_t value)
{
    const std::uint32_t unit = static_cast<std::uint32_t>(value);
    return unit >= 0xDC00U && unit <= 0xDFFFU;
}

bool IsCoreWordCharacter(wchar_t character)
{
    return std::iswalnum(static_cast<wint_t>(character)) != 0;
}

bool IsWordCharacterAt(const std::wstring& text, std::size_t index)
{
    if (index >= text.size()) {
        return false;
    }

    const wchar_t character = text[index];
    if (IsCoreWordCharacter(character)) {
        return true;
    }

    if (character != L'\'' && character != L'-') {
        return false;
    }

    if (index == 0U || index + 1U >= text.size()) {
        return false;
    }

    return IsCoreWordCharacter(text[index - 1U]) && IsCoreWordCharacter(text[index + 1U]);
}

} // namespace

bool IsUtf16ScalarBoundary(const std::wstring& text, std::size_t offset)
{
    if (offset > text.size()) return false;
    if (offset == 0U || offset == text.size()) return true;
    if constexpr (sizeof(wchar_t) == sizeof(std::uint16_t)) {
        return !(IsHighSurrogate(text[offset - 1U]) && IsLowSurrogate(text[offset]));
    }
    return true;
}

bool IsSpellingWordContinuationUnit(wchar_t character)
{
    return IsCoreWordCharacter(character) || character == L'\'' || character == L'-';
}

void SetSpellingEligibleBounds(
    SpellingTextWindow& window,
    bool leftBoundaryContinuesWord,
    bool rightBoundaryContinuesWord)
{
    window.eligibleStart = 0U;
    window.eligibleEnd = window.text.size();
    if (window.text.empty()) return;

    if (leftBoundaryContinuesWord && IsSpellingWordContinuationUnit(window.text.front())) {
        for (std::size_t index = 0U; index < window.text.size(); ++index) {
            TextRange range{};
            if (ExpandWordRangeAt(window.text, index, range)) {
                bool clippedPrefix = true;
                for (std::size_t prefix = 0U; prefix < range.start; ++prefix) {
                    if (!IsSpellingWordContinuationUnit(window.text[prefix])) {
                        clippedPrefix = false;
                        break;
                    }
                }
                if (clippedPrefix) window.eligibleStart = range.start + range.length;
                break;
            }
        }
    }

    if (rightBoundaryContinuesWord && IsSpellingWordContinuationUnit(window.text.back())) {
        for (std::size_t index = window.text.size(); index > 0U; --index) {
            TextRange range{};
            if (ExpandWordRangeAt(window.text, index - 1U, range)) {
                bool clippedSuffix = true;
                const std::size_t wordEnd = range.start + range.length;
                for (std::size_t suffix = wordEnd; suffix < window.text.size(); ++suffix) {
                    if (!IsSpellingWordContinuationUnit(window.text[suffix])) {
                        clippedSuffix = false;
                        break;
                    }
                }
                if (clippedSuffix) window.eligibleEnd = range.start;
                break;
            }
        }
    }

    if (window.eligibleEnd < window.eligibleStart) window.eligibleEnd = window.eligibleStart;
}

bool MapSpellingBackendRanges(
    const SpellingTextWindow& window,
    const std::vector<TextRange>& backendRanges,
    std::vector<SpellingAnnotation>& annotations)
{
    annotations.clear();
    if (window.revision.session == 0U || window.revision.layout == 0U ||
        window.text.size() > kMaximumSpellingWindowUnits ||
        window.eligibleStart > window.eligibleEnd || window.eligibleEnd > window.text.size() ||
        !IsUtf16ScalarBoundary(window.text, window.eligibleStart) ||
        !IsUtf16ScalarBoundary(window.text, window.eligibleEnd)) {
        return false;
    }

    std::vector<SpellingAnnotation> mapped;
    mapped.reserve(backendRanges.size());
    for (const TextRange& range : backendRanges) {
        if (range.length == 0U || range.start > window.text.size() ||
            range.length > window.text.size() - range.start) {
            return false;
        }
        const std::size_t end = range.start + range.length;
        if (range.start < window.eligibleStart || end > window.eligibleEnd ||
            !IsUtf16ScalarBoundary(window.text, range.start) ||
            !IsUtf16ScalarBoundary(window.text, end) ||
            range.start > (std::numeric_limits<std::uint64_t>::max)() - window.absoluteStart) {
            return false;
        }

        const std::uint64_t absoluteStart = window.absoluteStart + static_cast<std::uint64_t>(range.start);
        if (static_cast<std::uint64_t>(range.length) >
            (std::numeric_limits<std::uint64_t>::max)() - absoluteStart) {
            return false;
        }
        mapped.push_back({absoluteStart, static_cast<std::uint64_t>(range.length)});
    }
    annotations = std::move(mapped);
    return true;
}

bool CheckSpellingWindows(
    const std::vector<SpellingTextWindow>& windows,
    const std::function<std::vector<TextRange>(const std::wstring&)>& backend,
    SpellingAnnotationBatch& candidate)
{
    candidate = {};
    if (windows.empty() || !backend) return false;

    SpellingAnnotationBatch checked;
    checked.revision = windows.front().revision;
    std::vector<SpellingAnnotation> annotations;
    for (const SpellingTextWindow& window : windows) {
        if (!SpellingRevisionMatches(checked.revision, window.revision)) return false;

        std::vector<SpellingAnnotation> validation;
        if (!MapSpellingBackendRanges(window, {}, validation)) return false;
        const std::size_t eligibleLength = window.eligibleEnd - window.eligibleStart;
        if (eligibleLength == 0U) continue;

        const std::wstring eligibleText = window.text.substr(window.eligibleStart, eligibleLength);
        const std::vector<TextRange> backendRanges = backend(eligibleText);
        std::vector<TextRange> windowRanges;
        windowRanges.reserve(backendRanges.size());
        for (const TextRange& range : backendRanges) {
            if (range.length == 0U || range.start > eligibleLength ||
                range.length > eligibleLength - range.start ||
                !IsUtf16ScalarBoundary(eligibleText, range.start) ||
                !IsUtf16ScalarBoundary(eligibleText, range.start + range.length) ||
                range.start > (std::numeric_limits<std::size_t>::max)() - window.eligibleStart) {
                return false;
            }
            windowRanges.push_back({window.eligibleStart + range.start, range.length});
        }

        std::vector<SpellingAnnotation> mapped;
        if (!MapSpellingBackendRanges(window, windowRanges, mapped)) return false;
        annotations.insert(annotations.end(), mapped.begin(), mapped.end());
    }

    std::sort(annotations.begin(), annotations.end(),
        [](const SpellingAnnotation& left, const SpellingAnnotation& right) {
            return left.start < right.start || (left.start == right.start && left.length < right.length);
        });
    annotations.erase(std::unique(annotations.begin(), annotations.end(),
        [](const SpellingAnnotation& left, const SpellingAnnotation& right) {
            return left.start == right.start && left.length == right.length;
        }), annotations.end());
    checked.annotations = std::move(annotations);
    checked.valid = true;
    candidate = std::move(checked);
    return true;
}

bool ResolveSpellingContextTarget(
    const std::vector<SpellingTextWindow>& windows,
    const SpellingRevision& expectedRevision,
    std::uint64_t position,
    bool preferPreviousAtBoundary,
    SpellingContextTarget& target)
{
    target = {};
    std::vector<SpellingContextTarget> directTargets;
    std::vector<SpellingContextTarget> previousTargets;
    for (const SpellingTextWindow& window : windows) {
        if (!SpellingRevisionMatches(window.revision, expectedRevision)) return false;
        std::vector<SpellingAnnotation> validation;
        if (!MapSpellingBackendRanges(window, {}, validation) ||
            window.text.size() > (std::numeric_limits<std::uint64_t>::max)() - window.absoluteStart) {
            return false;
        }

        const std::uint64_t windowEnd = window.absoluteStart + static_cast<std::uint64_t>(window.text.size());
        if (position < window.absoluteStart || position > windowEnd) continue;

        const std::size_t localPosition = static_cast<std::size_t>(position - window.absoluteStart);
        const auto appendTarget = [&window](std::size_t probe,
            std::vector<SpellingContextTarget>& results) {
            TextRange word{};
            if (!ExpandWordRangeAt(window.text, probe, word) || word.length == 0U ||
                word.start < window.eligibleStart || word.start > window.eligibleEnd ||
                word.length > window.eligibleEnd - word.start ||
                !IsUtf16ScalarBoundary(window.text, word.start) ||
                !IsUtf16ScalarBoundary(window.text, word.start + word.length) ||
                word.start > (std::numeric_limits<std::uint64_t>::max)() - window.absoluteStart) {
                return;
            }
            const std::uint64_t absoluteStart = window.absoluteStart + static_cast<std::uint64_t>(word.start);
            if (static_cast<std::uint64_t>(word.length) >
                (std::numeric_limits<std::uint64_t>::max)() - absoluteStart) return;
            SpellingContextTarget candidate;
            candidate.revision = window.revision;
            candidate.start = absoluteStart;
            candidate.length = static_cast<std::uint64_t>(word.length);
            candidate.text = window.text.substr(word.start, word.length);
            results.push_back(std::move(candidate));
        };

        if (localPosition < window.text.size()) appendTarget(localPosition, directTargets);
        if (preferPreviousAtBoundary && localPosition > 0U) {
            appendTarget(localPosition - 1U, previousTargets);
        }
    }

    const std::vector<SpellingContextTarget>& candidates =
        !directTargets.empty() ? directTargets : previousTargets;
    if (candidates.empty()) return false;
    const SpellingContextTarget& resolved = candidates.front();
    for (const SpellingContextTarget& candidate : candidates) {
        if (candidate.start != resolved.start || candidate.length != resolved.length ||
            candidate.text != resolved.text ||
            !SpellingRevisionMatches(candidate.revision, resolved.revision)) return false;
    }
    target = resolved;
    return true;
}

bool SpellingRevisionMatches(const SpellingRevision& left, const SpellingRevision& right)
{
    return left.session == right.session && left.content == right.content && left.layout == right.layout;
}

bool SpellingBatchIsCurrent(const SpellingAnnotationBatch& batch, const SpellingRevision& currentRevision)
{
    return batch.valid && SpellingRevisionMatches(batch.revision, currentRevision);
}

bool TryPublishSpellingBatch(
    SpellingAnnotationBatch& published,
    const SpellingAnnotationBatch& candidate,
    const SpellingRevision& currentRevision)
{
    if (!SpellingBatchIsCurrent(candidate, currentRevision)) return false;
    published = candidate;
    return true;
}

bool ExpandWordRangeAt(const std::wstring& text, std::size_t index, TextRange& range)
{
    range = {};
    if (!IsWordCharacterAt(text, index)) {
        return false;
    }

    std::size_t start = index;
    while (start > 0U && IsWordCharacterAt(text, start - 1U)) {
        --start;
    }

    std::size_t end = index + 1U;
    while (end < text.size() && IsWordCharacterAt(text, end)) {
        ++end;
    }

    while (start < end && !IsCoreWordCharacter(text[start])) {
        ++start;
    }
    while (end > start && !IsCoreWordCharacter(text[end - 1U])) {
        --end;
    }

    if (end <= start) {
        return false;
    }

    range.start = start;
    range.length = end - start;
    return true;
}

std::vector<TextRange> FindSpellCheckWordRanges(const std::wstring& text)
{
    std::vector<TextRange> ranges;
    for (std::size_t index = 0; index < text.size();) {
        TextRange range {};
        if (ExpandWordRangeAt(text, index, range)) {
            ranges.push_back(range);
            index = range.start + range.length;
        } else {
            ++index;
        }
    }

    return ranges;
}

bool RangesOverlap(std::size_t firstStart, std::size_t firstLength, std::size_t secondStart, std::size_t secondLength)
{
    if (firstLength == 0U || secondLength == 0U) {
        return false;
    }

    if (firstStart <= secondStart) {
        return (secondStart - firstStart) < firstLength;
    }

    return (firstStart - secondStart) < secondLength;
}

} // namespace classic_notepad
