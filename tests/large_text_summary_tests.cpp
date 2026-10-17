#include "large_text_summary.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failureCount = 0;

void Expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failureCount;
    }
}

std::pair<std::uint64_t, std::uint8_t> MeasureColumns(
    const std::wstring& text,
    std::size_t start,
    std::size_t end,
    std::uint8_t initialResidue)
{
    std::uint64_t columns = 0U;
    std::uint8_t residue = initialResidue;
    for (std::size_t index = start; index < end; ++index) {
        const std::uint64_t advance = text[index] == L'\t' ? 8U - residue : 1U;
        columns += advance;
        residue = static_cast<std::uint8_t>((residue + advance) % 8U);
    }
    return {columns, residue};
}

// Independent reference implementation: it counts logical tokens and measures
// line segments directly, rather than composing the production summaries.
classic_notepad::LargeTextSummary ReferenceSummary(const std::wstring& text)
{
    using classic_notepad::LargeTextSummary;
    LargeTextSummary expected;
    expected.characters = static_cast<std::uint64_t>(text.size());
    expected.empty = text.empty();
    expected.displayedCharactersKnown = true;
    if (text.empty()) return expected;

    expected.first = text.front();
    expected.last = text.back();
    for (std::size_t index = 0U; index < text.size();) {
        if (text[index] == L'\r' || text[index] == L'\n') {
            ++expected.displayedCharacters;
            ++expected.lineBreaks;
            if (text[index] == L'\r' && index + 1U < text.size() && text[index + 1U] == L'\n') {
                ++index;
            }
        } else if (text[index] >= 0xD800 && text[index] <= 0xDBFF &&
            index + 1U < text.size() && text[index + 1U] >= 0xDC00 && text[index + 1U] <= 0xDFFF) {
            ++expected.displayedCharacters;
            ++index;
        } else {
            ++expected.displayedCharacters;
        }
        ++index;
    }

    std::size_t firstBreak = text.find_first_of(L"\r\n");
    if (firstBreak == std::wstring::npos) firstBreak = text.size();
    for (std::size_t residue = 0U; residue < expected.width.prefixColumns.size(); ++residue) {
        const auto measured = MeasureColumns(text, 0U, firstBreak, static_cast<std::uint8_t>(residue));
        expected.width.prefixColumns[residue] = measured.first;
        expected.width.prefixResidues[residue] = measured.second;
    }

    std::size_t suffixStart = 0U;
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (text[index] == L'\r' || text[index] == L'\n') {
            expected.width.hasLineBreak = true;
            suffixStart = index + 1U;
        }
    }
    const auto suffix = MeasureColumns(text, suffixStart, text.size(), 0U);
    expected.width.suffixColumns = suffix.first;
    expected.width.suffixResidue = suffix.second;

    std::size_t lineStart = 0U;
    for (std::size_t index = 0U; index <= text.size(); ++index) {
        if (index == text.size() || text[index] == L'\r' || text[index] == L'\n') {
            expected.width.maximumLineColumns = std::max(
                expected.width.maximumLineColumns,
                MeasureColumns(text, lineStart, index, 0U).first);
            lineStart = index + 1U;
        }
    }
    return expected;
}

bool EqualWidth(
    const classic_notepad::LargeFileWidthSummary& left,
    const classic_notepad::LargeFileWidthSummary& right)
{
    return left.prefixColumns == right.prefixColumns &&
        left.prefixResidues == right.prefixResidues &&
        left.suffixColumns == right.suffixColumns &&
        left.suffixResidue == right.suffixResidue &&
        left.maximumLineColumns == right.maximumLineColumns &&
        left.hasLineBreak == right.hasLineBreak;
}

bool EqualSummary(
    const classic_notepad::LargeTextSummary& left,
    const classic_notepad::LargeTextSummary& right)
{
    return left.characters == right.characters &&
        left.displayedCharacters == right.displayedCharacters &&
        left.displayedCharactersKnown == right.displayedCharactersKnown &&
        left.lineBreaks == right.lineBreaks &&
        left.empty == right.empty &&
        left.first == right.first &&
        left.last == right.last &&
        EqualWidth(left.width, right.width);
}

void TestReferenceCases()
{
    using classic_notepad::SummarizeLargeText;
    const std::vector<std::wstring> cases{
        L"",
        L"plain ASCII",
        L"abc\r\ndef\nghi\r",
        L"\r\n\rX\n",
        L"tabs\tand\tcolumns\r\n漢字",
        std::wstring{L'A', static_cast<wchar_t>(0xD83DU), static_cast<wchar_t>(0xDE00U), L'B'},
        std::wstring{static_cast<wchar_t>(0xD83DU), static_cast<wchar_t>(0xDE00U), L'\r', L'\n', L'\t'},
    };
    for (const std::wstring& text : cases) {
        Expect(EqualSummary(SummarizeLargeText(text), ReferenceSummary(text)),
            "Production text summary equals the independent reference scanner");
    }
}

void TestEverySplitAndGrouping()
{
    using classic_notepad::CombineLargeTextSummaries;
    using classic_notepad::SummarizeLargeText;
    const std::vector<std::wstring> cases{
        L"left\r\nright\nlast\r",
        std::wstring{L'a', static_cast<wchar_t>(0xD83DU), static_cast<wchar_t>(0xDE00U), L'b'},
        std::wstring{L'\r', L'\n', static_cast<wchar_t>(0xD83DU), static_cast<wchar_t>(0xDE00U), L'\t'},
    };

    for (const std::wstring& text : cases) {
        const auto expected = ReferenceSummary(text);
        for (std::size_t split = 0U; split <= text.size(); ++split) {
            const auto left = SummarizeLargeText(text.substr(0U, split));
            const auto right = SummarizeLargeText(text.substr(split));
            Expect(EqualSummary(CombineLargeTextSummaries(left, right), expected),
                "Combining summaries matches reference at every piece seam");
        }
        for (std::size_t first = 0U; first <= text.size(); ++first) {
            for (std::size_t second = first; second <= text.size(); ++second) {
                const auto left = SummarizeLargeText(text.substr(0U, first));
                const auto middle = SummarizeLargeText(text.substr(first, second - first));
                const auto right = SummarizeLargeText(text.substr(second));
                const auto leftGrouped = CombineLargeTextSummaries(
                    CombineLargeTextSummaries(left, middle), right);
                const auto rightGrouped = CombineLargeTextSummaries(
                    left, CombineLargeTextSummaries(middle, right));
                Expect(EqualSummary(leftGrouped, expected) && EqualSummary(rightGrouped, expected),
                    "Three-piece grouping matches the independent reference");
            }
        }
    }
}

void TestEmptyAndKnownUnknownPropagation()
{
    using classic_notepad::CombineLargeTextSummaries;
    using classic_notepad::LargeTextSummary;
    using classic_notepad::SummarizeLargeText;

    const LargeTextSummary unknownEmpty{};
    const auto knownEmpty = SummarizeLargeText(L"");
    const auto knownText = SummarizeLargeText(L"x\r");
    Expect(unknownEmpty.empty && !unknownEmpty.displayedCharactersKnown,
        "Default empty summary has an unknown displayed count");
    Expect(knownEmpty.empty && knownEmpty.displayedCharactersKnown && knownEmpty.displayedCharacters == 0U,
        "Summarized empty text has an exact zero displayed count");
    Expect(EqualSummary(CombineLargeTextSummaries(unknownEmpty, knownText), knownText) &&
        EqualSummary(CombineLargeTextSummaries(knownText, unknownEmpty), knownText),
        "Empty identity preserves all known summary fields");

    LargeTextSummary unknownCr;
    unknownCr.characters = 1U;
    unknownCr.displayedCharacters = 77U;
    unknownCr.displayedCharactersKnown = false;
    unknownCr.lineBreaks = 1U;
    unknownCr.empty = false;
    unknownCr.first = L'\r';
    unknownCr.last = L'\r';
    const auto knownLf = SummarizeLargeText(L"\n");
    const auto combined = CombineLargeTextSummaries(unknownCr, knownLf);
    Expect(!combined.empty && !combined.displayedCharactersKnown && combined.displayedCharacters == 0U,
        "Unknown displayed count propagates without applying the CRLF count correction");
    Expect(combined.characters == 2U && combined.lineBreaks == 1U &&
        combined.first == L'\r' && combined.last == L'\n',
        "CRLF seam composition retains UTF-16 length and correct logical break count");

    LargeTextSummary unknownValue = knownText;
    unknownValue.displayedCharactersKnown = false;
    unknownValue.displayedCharacters = 123U;
    Expect(EqualSummary(CombineLargeTextSummaries(unknownValue, unknownEmpty), unknownValue),
        "Combining an unknown nonempty summary with an empty summary preserves it exactly");
}

} // namespace

int main()
{
    TestReferenceCases();
    TestEverySplitAndGrouping();
    TestEmptyAndKnownUnknownPropagation();

    if (g_failureCount != 0) {
        std::cerr << g_failureCount << " large text summary test(s) failed.\n";
        return 1;
    }
    std::cout << "LargeTextSummaryTests passed.\n";
    return 0;
}
