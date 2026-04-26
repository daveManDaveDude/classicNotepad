#include "document.h"

#include "file_io.h"

#include <cstdint>
#include <string>
#include <vector>

bool Document::Load(const std::wstring& path, std::wstring& editorText, std::wstring& errorMessage)
{
    std::vector<std::uint8_t> bytes;
    if (!classic_notepad::ReadFileBytes(path, bytes, errorMessage)) {
        return false;
    }

    classic_notepad::DecodeTextResult decoded;
    if (!classic_notepad::DecodeTextBytes(bytes, decoded, errorMessage)) {
        return false;
    }

    const classic_notepad::LineEndingAnalysis lineEndings = classic_notepad::AnalyzeLineEndings(decoded.text);

    path_ = path;
    encoding_ = decoded.encoding;
    lineEnding_ = lineEndings.style;
    // The edit control normalizes all line breaks, so mixed files save using the dominant style.
    saveLineEnding_ = lineEndings.dominantStyle;
    editorText = classic_notepad::NormalizeLineEndingsForEditor(decoded.text);
    modified_ = false;
    return true;
}

bool Document::Save(const std::wstring& editorText, std::wstring& errorMessage)
{
    if (path_.empty()) {
        errorMessage = L"No file path has been selected.";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    const LineEndingStyle targetLineEnding =
        lineEnding_ == LineEndingStyle::Mixed ? saveLineEnding_ : lineEnding_;
    const std::wstring diskText = classic_notepad::ConvertLineEndingsFromEditor(editorText, targetLineEnding);
    if (!classic_notepad::EncodeTextBytes(diskText, encoding_, bytes, errorMessage)) {
        return false;
    }

    if (!classic_notepad::WriteFileBytesAtomically(path_, bytes, errorMessage)) {
        return false;
    }

    modified_ = false;
    return true;
}

bool Document::SaveAs(const std::wstring& path, const std::wstring& editorText, std::wstring& errorMessage)
{
    const std::wstring previousPath = path_;
    const bool previousModified = modified_;

    path_ = path;
    if (!Save(editorText, errorMessage)) {
        path_ = previousPath;
        modified_ = previousModified;
        return false;
    }

    return true;
}

void Document::ResetUntitled()
{
    path_.clear();
    modified_ = false;
    encoding_ = TextEncoding::Utf8NoBom;
    lineEnding_ = LineEndingStyle::Crlf;
    saveLineEnding_ = LineEndingStyle::Crlf;
}

void Document::ResetNewFile(const std::wstring& path)
{
    ResetUntitled();
    path_ = path;
}

bool Document::HasPath() const
{
    return !path_.empty();
}

bool Document::IsModified() const
{
    return modified_;
}

void Document::SetModified(bool modified)
{
    modified_ = modified;
}

const std::wstring& Document::Path() const
{
    return path_;
}

std::wstring Document::DisplayName() const
{
    if (path_.empty()) {
        return L"Untitled";
    }

    const std::size_t separator = path_.find_last_of(L"\\/");
    if (separator == std::wstring::npos || separator + 1U >= path_.size()) {
        return path_;
    }

    return path_.substr(separator + 1U);
}

Document::TextEncoding Document::Encoding() const
{
    return encoding_;
}

Document::LineEndingStyle Document::LineEnding() const
{
    return lineEnding_;
}

Document::LineEndingStyle Document::SaveLineEnding() const
{
    return saveLineEnding_;
}
