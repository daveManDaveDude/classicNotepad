#include "line_endings.h"

namespace classic_notepad {
namespace {

LineEndingStyle DominantLineEnding(std::uint64_t crlfCount, std::uint64_t lfCount, std::uint64_t crCount)
{
    if (lfCount > crlfCount && lfCount >= crCount) {
        return LineEndingStyle::Lf;
    }

    if (crCount > crlfCount && crCount > lfCount) {
        return LineEndingStyle::Cr;
    }

    return LineEndingStyle::Crlf;
}

} // namespace

LineEndingAnalysis AnalyzeLineEndings(const std::wstring& text)
{
    std::uint64_t crlfCount = 0;
    std::uint64_t lfCount = 0;
    std::uint64_t crCount = 0;

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            if (index + 1U < text.size() && text[index + 1U] == L'\n') {
                ++crlfCount;
                ++index;
            } else {
                ++crCount;
            }
        } else if (text[index] == L'\n') {
            ++lfCount;
        }
    }

    return AnalyzeLineEndingCounts(crlfCount, lfCount, crCount);
}

LineEndingAnalysis AnalyzeLineEndingCounts(
    std::uint64_t crlfCount,
    std::uint64_t lfCount,
    std::uint64_t crCount)
{
    LineEndingAnalysis analysis{};
    analysis.crlfCount = crlfCount;
    analysis.lfCount = lfCount;
    analysis.crCount = crCount;

    const bool hasCrlf = crlfCount > 0U;
    const bool hasLf = lfCount > 0U;
    const bool hasCr = crCount > 0U;
    const int styleCount = (hasCrlf ? 1 : 0) + (hasLf ? 1 : 0) + (hasCr ? 1 : 0);

    analysis.dominantStyle = DominantLineEnding(crlfCount, lfCount, crCount);

    if (styleCount == 0) {
        analysis.style = LineEndingStyle::Crlf;
    } else if (styleCount > 1) {
        analysis.style = LineEndingStyle::Mixed;
    } else {
        analysis.style = analysis.dominantStyle;
    }

    return analysis;
}

std::wstring NormalizeLineEndingsForEditor(const std::wstring& text)
{
    std::wstring normalized;
    normalized.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            if (index + 1U < text.size() && text[index + 1U] == L'\n') {
                ++index;
            }
            normalized += L"\r\n";
        } else if (text[index] == L'\n') {
            normalized += L"\r\n";
        } else {
            normalized.push_back(text[index]);
        }
    }

    return normalized;
}

void NormalizeLineEndingsForEditorInPlace(std::wstring& text)
{
    std::size_t extraCharacters = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\n' && (index == 0U || text[index - 1U] != L'\r')) {
            ++extraCharacters;
        } else if (text[index] == L'\r' && (index + 1U >= text.size() || text[index + 1U] != L'\n')) {
            ++extraCharacters;
        }
    }

    if (extraCharacters == 0U) {
        return;
    }

    const std::size_t originalSize = text.size();
    text.resize(originalSize + extraCharacters);
    std::size_t read = originalSize;
    std::size_t write = text.size();
    while (read > 0U) {
        const wchar_t character = text[--read];
        if (character == L'\n') {
            if (read > 0U && text[read - 1U] == L'\r') {
                text[--write] = L'\n';
                text[--write] = L'\r';
                --read;
            } else {
                text[--write] = L'\n';
                text[--write] = L'\r';
            }
        } else if (character == L'\r') {
            text[--write] = L'\n';
            text[--write] = L'\r';
        } else {
            text[--write] = character;
        }
    }
}

std::wstring ConvertLineEndingsFromEditor(const std::wstring& text, LineEndingStyle target)
{
    if (target == LineEndingStyle::Crlf || target == LineEndingStyle::Mixed) {
        return NormalizeLineEndingsForEditor(text);
    }

    const wchar_t replacement = target == LineEndingStyle::Lf ? L'\n' : L'\r';
    std::wstring converted;
    converted.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            if (index + 1U < text.size() && text[index + 1U] == L'\n') {
                ++index;
            }
            converted.push_back(replacement);
        } else if (text[index] == L'\n') {
            converted.push_back(replacement);
        } else {
            converted.push_back(text[index]);
        }
    }

    return converted;
}

} // namespace classic_notepad
