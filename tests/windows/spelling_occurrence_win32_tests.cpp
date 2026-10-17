#include "../../src/platform/windows/windows_spelling_occurrence.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using classic_notepad::SpellingAnnotation;
using classic_notepad::SpellingRevision;
using classic_notepad::SpellingTextWindow;
using classic_notepad::windows::IgnoredSpellingOccurrence;
using classic_notepad::windows::IsIgnoredSpellingOccurrence;

int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

SpellingTextWindow MakeWindow(std::uint64_t absoluteStart, std::wstring text)
{
    SpellingTextWindow window;
    window.absoluteStart = absoluteStart;
    window.text = std::move(text);
    window.eligibleEnd = window.text.size();
    return window;
}

IgnoredSpellingOccurrence MakeOccurrence(
    SpellingRevision revision,
    std::uint64_t start,
    std::wstring text,
    std::uint64_t length = 0U)
{
    IgnoredSpellingOccurrence occurrence;
    occurrence.revision = revision;
    occurrence.start = start;
    occurrence.length = length == 0U ? static_cast<std::uint64_t>(text.size()) : length;
    occurrence.text = std::move(text);
    return occurrence;
}

void ExpectMatch(
    const std::vector<IgnoredSpellingOccurrence>& ignored,
    SpellingAnnotation annotation,
    SpellingRevision revision,
    const std::vector<SpellingTextWindow>& windows,
    bool expected,
    const char* message)
{
    Expect(IsIgnoredSpellingOccurrence(ignored, annotation, revision, windows) == expected, message);
}

void RunRevisionAndExactIdentityChecks()
{
    const SpellingRevision storedRevision{7U, 11U, 2U};
    const SpellingRevision currentRevision{7U, 11U, 99U};
    const std::vector<IgnoredSpellingOccurrence> ignored{
        MakeOccurrence(storedRevision, 101U, L"foo")};
    const std::vector<SpellingTextWindow> windows{MakeWindow(100U, L"xfoo!")};

    ExpectMatch(ignored, {101U, 3U}, currentRevision, windows, true,
        "Matching session/content ignores layout revision changes");
    ExpectMatch(ignored, {101U, 3U}, {8U, 11U, 2U}, windows, false,
        "A different document session does not match");
    ExpectMatch(ignored, {101U, 3U}, {7U, 12U, 2U}, windows, false,
        "A different content revision does not match");
    ExpectMatch(ignored, {102U, 3U}, currentRevision, windows, false,
        "An absolute start mismatch does not match");
    ExpectMatch(ignored, {101U, 2U}, currentRevision, windows, false,
        "A UTF-16 length mismatch does not match");
    ExpectMatch(ignored, {101U, 3U}, currentRevision,
        {MakeWindow(100U, L"xbar!")}, false,
        "Changed text at the same absolute occurrence does not match");

    std::vector<SpellingTextWindow> differentlyTaggedWindows{MakeWindow(100U, L"xfoo!")};
    differentlyTaggedWindows.front().revision = {99U, 88U, 77U};
    ExpectMatch(ignored, {101U, 3U}, currentRevision, differentlyTaggedWindows, true,
        "The pure matcher retains its existing text-window revision agnosticism");
}

void RunWindowBoundaryAndIdentityChecks()
{
    const SpellingRevision revision{1U, 4U, 5U};
    const std::vector<IgnoredSpellingOccurrence> occurrence{
        MakeOccurrence(revision, 104U, L"word")};

    ExpectMatch({}, {104U, 4U}, revision,
        {MakeWindow(100U, L"pre word post")}, false,
        "An empty occurrence list cannot suppress a matching annotation");
    ExpectMatch(occurrence, {104U, 4U}, revision, {}, false,
        "An empty window list cannot match an ignored occurrence");
    ExpectMatch(
        {MakeOccurrence(revision, 104U, L"other"), occurrence.front()},
        {104U, 4U}, revision,
        {MakeWindow(100U, L"pre word post")}, true,
        "A later matching occurrence is found after an earlier nonmatch");
    ExpectMatch(occurrence, {104U, 4U}, revision,
        {MakeWindow(100U, L"pre xord post"), MakeWindow(100U, L"pre word post")}, true,
        "A later matching window is found after an earlier nonmatch");
    ExpectMatch(occurrence, {104U, 4U}, revision,
        {MakeWindow(100U, L"pre word post")}, true,
        "A word is matched at its absolute offset inside a bounded window");
    ExpectMatch(occurrence, {104U, 4U}, revision,
        {MakeWindow(108U, L"word")}, false,
        "A window beginning after the occurrence cannot match it");
    ExpectMatch(occurrence, {99U, 4U}, revision,
        {MakeWindow(100U, L"pre word post")}, false,
        "A target preceding the window is rejected without unsigned underflow");
    ExpectMatch(occurrence, {104U, 4U}, revision,
        {MakeWindow(100U, L"pre")}, false,
        "An annotation starting after the window end is rejected");
    ExpectMatch(occurrence, {104U, 4U}, revision,
        {MakeWindow(100U, L"pre wo")}, false,
        "An annotation extending past the remaining window text is rejected");

    const std::vector<SpellingTextWindow> splitWordWindows{
        MakeWindow(100U, L"pre wo"), MakeWindow(106U, L"rd post")};
    ExpectMatch(occurrence, {104U, 4U}, revision, splitWordWindows, false,
        "Separated windows are checked independently without concatenating their text");

    const std::wstring embeddedNullText(L"a\0b", 3U);
    const std::vector<IgnoredSpellingOccurrence> embeddedNullOccurrence{
        MakeOccurrence(revision, 300U, embeddedNullText)};
    ExpectMatch(embeddedNullOccurrence, {300U, 3U}, revision,
        {MakeWindow(300U, embeddedNullText)}, true,
        "Exact comparison preserves embedded NUL code units");
    ExpectMatch(embeddedNullOccurrence, {300U, 3U}, revision,
        {MakeWindow(300U, std::wstring(L"a\0c", 3U))}, false,
        "An embedded-NUL text difference is detected");
}

void RunLongOffsetAndUnicodeChecks()
{
    const SpellingRevision revision{12U, 34U, 56U};
    const std::wstring supplementaryAndCjk = L"\U0001F600漢字";
    const std::uint64_t highStart = 52428800U + 65540U;
    const std::vector<IgnoredSpellingOccurrence> occurrence{
        MakeOccurrence(revision, highStart + 2U, supplementaryAndCjk)};
    const std::vector<SpellingTextWindow> windows{
        MakeWindow(highStart, L"xx" + supplementaryAndCjk + L"!")};
    ExpectMatch(occurrence,
        {highStart + 2U, static_cast<std::uint64_t>(supplementaryAndCjk.size())},
        revision, windows, true,
        "Supplementary and CJK text matches at offsets above 52 million and 65,535");
    ExpectMatch(occurrence,
        {highStart + 2U, static_cast<std::uint64_t>(supplementaryAndCjk.size() - 1U)},
        revision, windows, false,
        "UTF-16 code-unit length mismatch rejects a supplementary/CJK occurrence");
}

void RunMaximumOffsetGuards()
{
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    const SpellingRevision revision{2U, 3U, 4U};
    const std::vector<IgnoredSpellingOccurrence> atMaximum{
        MakeOccurrence(revision, maximum, L"x")};

    ExpectMatch(atMaximum, {maximum, 1U}, revision,
        {MakeWindow(maximum, L"x")}, true,
        "Equal maximum absolute offsets use zero relative displacement without addition");
    ExpectMatch(atMaximum, {maximum, 1U}, revision,
        {MakeWindow(0U, L"x")}, false,
        "A maximum annotation offset safely exceeds a small window");

    const std::vector<IgnoredSpellingOccurrence> precedingMaximum{
        MakeOccurrence(revision, maximum, L"x")};
    ExpectMatch(precedingMaximum, {maximum, 1U}, revision,
        {MakeWindow(maximum - 1U, L"x")}, false,
        "A one-unit relative offset at UINT64_MAX fails the subtraction-based length bound");
}

} // namespace

int main()
{
    RunRevisionAndExactIdentityChecks();
    RunWindowBoundaryAndIdentityChecks();
    RunLongOffsetAndUnicodeChecks();
    RunMaximumOffsetGuards();
    if (failures != 0) {
        std::cerr << failures << " Windows spelling occurrence check(s) failed.\n";
        return 1;
    }
    std::cout << "WindowsSpellingOccurrenceTests passed.\n";
    return 0;
}
