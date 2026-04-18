#pragma once

#include <cstddef>
#include <string>

namespace classic_notepad {

enum class LineEndingStyle {
    Crlf,
    Lf,
    Cr,
    Mixed
};

struct LineEndingAnalysis {
    LineEndingStyle style = LineEndingStyle::Crlf;
    LineEndingStyle dominantStyle = LineEndingStyle::Crlf;
    std::size_t crlfCount = 0;
    std::size_t lfCount = 0;
    std::size_t crCount = 0;
};

LineEndingAnalysis AnalyzeLineEndings(const std::wstring& text);
std::wstring NormalizeLineEndingsForEditor(const std::wstring& text);
std::wstring ConvertLineEndingsFromEditor(const std::wstring& text, LineEndingStyle target);

} // namespace classic_notepad
