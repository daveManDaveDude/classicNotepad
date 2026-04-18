#include "line_endings.h"

namespace classic_notepad {
namespace {

LineEndingStyle DominantLineEnding(const LineEndingAnalysis& analysis)
{
    if (analysis.lfCount > analysis.crlfCount && analysis.lfCount >= analysis.crCount) {
        return LineEndingStyle::Lf;
    }

    if (analysis.crCount > analysis.crlfCount && analysis.crCount > analysis.lfCount) {
        return LineEndingStyle::Cr;
    }

    return LineEndingStyle::Crlf;
}

} // namespace

LineEndingAnalysis AnalyzeLineEndings(const std::wstring& text)
{
    LineEndingAnalysis analysis{};

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            if (index + 1U < text.size() && text[index + 1U] == L'\n') {
                ++analysis.crlfCount;
                ++index;
            } else {
                ++analysis.crCount;
            }
        } else if (text[index] == L'\n') {
            ++analysis.lfCount;
        }
    }

    const bool hasCrlf = analysis.crlfCount > 0U;
    const bool hasLf = analysis.lfCount > 0U;
    const bool hasCr = analysis.crCount > 0U;
    const int styleCount = (hasCrlf ? 1 : 0) + (hasLf ? 1 : 0) + (hasCr ? 1 : 0);

    analysis.dominantStyle = DominantLineEnding(analysis);

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
