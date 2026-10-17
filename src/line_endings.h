#pragma once

#include <cstddef>
#include <cstdint>
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
    std::uint64_t crlfCount = 0;
    std::uint64_t lfCount = 0;
    std::uint64_t crCount = 0;
};

LineEndingAnalysis AnalyzeLineEndings(const std::wstring& text);
LineEndingAnalysis AnalyzeLineEndingCounts(
    std::uint64_t crlfCount,
    std::uint64_t lfCount,
    std::uint64_t crCount);
std::wstring NormalizeLineEndingsForEditor(const std::wstring& text);
void NormalizeLineEndingsForEditorInPlace(std::wstring& text);
std::wstring ConvertLineEndingsFromEditor(const std::wstring& text, LineEndingStyle target);

} // namespace classic_notepad
