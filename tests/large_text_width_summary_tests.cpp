#include "large_text_width_summary.h"

#include <cstdint>
#include <iostream>
#include <limits>
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

classic_notepad::LargeFileWidthSummary Summarize(const std::wstring& text)
{
    classic_notepad::LargeFileWidthSummary summary;
    bool prefixOpen = true;
    for (wchar_t character : text) {
        classic_notepad::AppendLargeTextWidthCharacter(summary, prefixOpen, character);
    }
    return summary;
}

bool EqualSummary(
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

void TestEveryTabResidue()
{
    using classic_notepad::AppendLargeTextWidthCharacter;
    using classic_notepad::LargeFileWidthSummary;

    LargeFileWidthSummary summary;
    bool prefixOpen = true;
    AppendLargeTextWidthCharacter(summary, prefixOpen, L'\t');

    for (std::size_t residue = 0U; residue < summary.prefixColumns.size(); ++residue) {
        Expect(summary.prefixColumns[residue] == 8U - residue,
            "Tab prefix width uses each of the eight incoming residues");
        Expect(summary.prefixResidues[residue] == 0U,
            "Tab prefix residue advances to the next eight-column stop");
    }
    Expect(summary.suffixColumns == 8U && summary.suffixResidue == 0U &&
        summary.maximumLineColumns == 8U && !summary.hasLineBreak && prefixOpen,
        "A leading tab updates suffix width without ending the line");
}

void TestPieceSeamsAndLineBreaks()
{
    using classic_notepad::CombineLargeTextWidthSummaries;

    const std::wstring text = L"ab\tX\r\nq\t\nZ\r";
    const auto expected = Summarize(text);
    for (std::size_t split = 0U; split <= text.size(); ++split) {
        const auto left = Summarize(text.substr(0U, split));
        const auto right = Summarize(text.substr(split));
        Expect(EqualSummary(CombineLargeTextWidthSummaries(left, right), expected),
            "Width summaries compose at every seam, including both sides of CRLF");
    }

    auto piecewise = Summarize(std::wstring{});
    for (wchar_t character : text) {
        piecewise = CombineLargeTextWidthSummaries(piecewise, Summarize(std::wstring(1U, character)));
    }
    Expect(EqualSummary(piecewise, expected),
        "One-code-unit pieces preserve tabs and CRLF width resets");
}

void TestSurrogateUtf16UnitsAcrossSeam()
{
    using classic_notepad::CombineLargeTextWidthSummaries;

    const std::wstring pair{
        static_cast<wchar_t>(0xD83DU),
        static_cast<wchar_t>(0xDE00U),
    };
    const auto expected = Summarize(pair);
    Expect(expected.suffixColumns == 2U && expected.maximumLineColumns == 2U,
        "A supplementary scalar retains one visual width unit per UTF-16 code unit");

    const auto high = Summarize(pair.substr(0U, 1U));
    const auto low = Summarize(pair.substr(1U));
    Expect(EqualSummary(CombineLargeTextWidthSummaries(high, low), expected),
        "A high and low surrogate split across pieces compose with exact UTF-16 width");
}

void TestAssociativity()
{
    using classic_notepad::CombineLargeTextWidthSummaries;

    const std::vector<std::wstring> fixtures{
        L"a\tb\r\nc\t\rZ",
        std::wstring{
            L'x', L'\t', static_cast<wchar_t>(0xD83DU), static_cast<wchar_t>(0xDE00U),
            L'\r', L'\n', L'\t', L'y'},
    };

    for (const std::wstring& text : fixtures) {
        for (std::size_t first = 0U; first <= text.size(); ++first) {
            for (std::size_t second = first; second <= text.size(); ++second) {
                const auto left = Summarize(text.substr(0U, first));
                const auto middle = Summarize(text.substr(first, second - first));
                const auto right = Summarize(text.substr(second));
                const auto leftAssociated = CombineLargeTextWidthSummaries(
                    CombineLargeTextWidthSummaries(left, middle), right);
                const auto rightAssociated = CombineLargeTextWidthSummaries(
                    left, CombineLargeTextWidthSummaries(middle, right));
                Expect(EqualSummary(leftAssociated, rightAssociated),
                    "Width-summary composition is associative across tabs, CRLF, and UTF-16 seams");
            }
        }
    }
}

void TestSaturationRetainsResidue()
{
    using classic_notepad::AppendLargeTextWidthCharacter;
    using classic_notepad::CombineLargeTextWidthSummaries;
    using classic_notepad::LargeFileWidthSummary;

    constexpr std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    LargeFileWidthSummary saturated;
    for (std::size_t residue = 0U; residue < saturated.prefixColumns.size(); ++residue) {
        saturated.prefixColumns[residue] = maximum;
    }
    saturated.suffixColumns = maximum;
    saturated.suffixResidue = 7U;
    saturated.maximumLineColumns = maximum;

    bool prefixOpen = true;
    AppendLargeTextWidthCharacter(saturated, prefixOpen, L'\t');
    Expect(saturated.suffixColumns == maximum && saturated.suffixResidue == 0U &&
        saturated.maximumLineColumns == maximum,
        "Saturated tab width remains capped while suffix residue advances exactly");
    for (std::size_t residue = 0U; residue < saturated.prefixColumns.size(); ++residue) {
        Expect(saturated.prefixColumns[residue] == maximum && saturated.prefixResidues[residue] == 0U,
            "Saturated prefix widths retain exact tab residues for every incoming residue");
    }

    LargeFileWidthSummary left;
    left.suffixColumns = maximum;
    left.suffixResidue = 7U;
    left.maximumLineColumns = maximum;
    for (std::size_t residue = 0U; residue < left.prefixColumns.size(); ++residue) {
        left.prefixColumns[residue] = maximum;
    }
    const auto joined = CombineLargeTextWidthSummaries(left, Summarize(L"\t"));
    Expect(joined.suffixColumns == maximum && joined.suffixResidue == 0U &&
        joined.maximumLineColumns == maximum,
        "Composition saturates width without deriving residue from the saturated column count");
}

} // namespace

int main()
{
    TestEveryTabResidue();
    TestPieceSeamsAndLineBreaks();
    TestSurrogateUtf16UnitsAcrossSeam();
    TestAssociativity();
    TestSaturationRetainsResidue();

    if (g_failureCount != 0) {
        std::cerr << g_failureCount << " large text width summary test(s) failed.\n";
        return 1;
    }
    std::cout << "LargeTextWidthSummaryTests passed.\n";
    return 0;
}
