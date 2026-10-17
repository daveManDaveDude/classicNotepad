#include "document.h"

#include "file_io.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

bool Document::Load(const std::wstring& path, std::wstring& editorText, std::wstring& errorMessage)
{
    StagedLoad staged;
    if (!PrepareLoad(path, staged, errorMessage)) {
        return false;
    }
    editorText = std::move(staged.editorText);
    CommitLoad(std::move(staged));
    return true;
}

bool Document::PrepareLoad(
    const std::wstring& path,
    StagedLoad& staged,
    std::wstring& errorMessage) const
{
    return PrepareLoadImpl(path, staged, errorMessage, 0U);
}

#if defined(_WIN32) && !defined(CLASSIC_NOTEPAD_PORTABLE_CORE_SMOKE)
bool Document::PrepareLoadWithAnsiCodePage(
    const std::wstring& path,
    StagedLoad& staged,
    std::wstring& errorMessage,
    std::uint32_t ansiCodePage) const
{
    return PrepareLoadImpl(path, staged, errorMessage, ansiCodePage);
}
#endif

bool Document::PrepareLoadImpl(
    const std::wstring& path,
    StagedLoad& staged,
    std::wstring& errorMessage,
    std::uint32_t ansiCodePage) const
{
    staged = {};
    std::vector<std::uint8_t> bytes;
    if (!classic_notepad::ReadFileBytes(path, bytes, errorMessage)) {
        return false;
    }

    classic_notepad::DecodeTextResult decoded;
    bool decodedSuccessfully = false;
    if (ansiCodePage == 0U) {
        decodedSuccessfully = classic_notepad::DecodeTextBytes(bytes, decoded, errorMessage);
    } else {
#if defined(_WIN32) && !defined(CLASSIC_NOTEPAD_PORTABLE_CORE_SMOKE)
        decodedSuccessfully = classic_notepad::DecodeTextBytesWithAnsiCodePage(
            bytes, decoded, errorMessage, ansiCodePage);
#endif
    }
    if (!decodedSuccessfully) {
        return false;
    }

    if (decoded.text.find(L'\0') != std::wstring::npos) {
        errorMessage = L"The file contains an embedded NUL character that the editor control cannot represent.";
        return false;
    }

    const classic_notepad::LineEndingAnalysis lineEndings = classic_notepad::AnalyzeLineEndings(decoded.text);
    staged.path = path;
    staged.encoding = decoded.encoding;
    staged.lineEnding = lineEndings.style;
    // The edit control normalizes all line breaks, so mixed files save using the dominant style.
    staged.saveLineEnding = lineEndings.dominantStyle;
    // Reuse the decoded allocation. Large files previously required a second
    // full UTF-16 copy here, causing severe paging before the editor was shown.
    classic_notepad::NormalizeLineEndingsForEditorInPlace(decoded.text);
    staged.editorText = std::move(decoded.text);
    errorMessage.clear();
    return true;
}

void Document::CommitLoad(StagedLoad&& staged)
{
    path_ = std::move(staged.path);
    encoding_ = staged.encoding;
    lineEnding_ = staged.lineEnding;
    saveLineEnding_ = staged.saveLineEnding;
    modified_ = false;
}

bool Document::Save(const std::wstring& editorText, std::wstring& errorMessage)
{
    lastSaveDiagnostics_ = {};
    if (path_.empty()) {
        errorMessage = L"No file path has been selected.";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    const std::wstring diskText = classic_notepad::ConvertLineEndingsFromEditor(editorText, saveLineEnding_);
    if (!classic_notepad::EncodeTextBytes(diskText, encoding_, bytes, errorMessage)) {
        return false;
    }

    if (!classic_notepad::WriteFileBytesAtomically(path_, bytes, errorMessage, &lastSaveDiagnostics_)) {
        return false;
    }

    modified_ = false;
    return true;
}

const classic_notepad::SaveDiagnostics& Document::LastSaveDiagnostics() const
{
    return lastSaveDiagnostics_;
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

void Document::OpenReadOnlyLargeFile(const std::wstring& path, TextEncoding encoding)
{
    StagedLargeLoad staged;
    PrepareReadOnlyLargeFile(path, encoding, staged);
    CommitReadOnlyLargeFile(std::move(staged));
}

void Document::PrepareReadOnlyLargeFile(
    const std::wstring& path,
    TextEncoding encoding,
    StagedLargeLoad& staged) const
{
    PrepareReadOnlyLargeFile(path, encoding, LineEndingStyle::Crlf, LineEndingStyle::Crlf, staged);
}

void Document::PrepareReadOnlyLargeFile(
    const std::wstring& path,
    TextEncoding encoding,
    LineEndingStyle lineEnding,
    LineEndingStyle saveLineEnding,
    StagedLargeLoad& staged) const
{
    staged.path = path;
    staged.encoding = encoding;
    staged.lineEnding = lineEnding;
    staged.saveLineEnding = saveLineEnding;
}

void Document::CommitReadOnlyLargeFile(StagedLargeLoad&& staged) noexcept
{
    path_ = std::move(staged.path);
    encoding_ = staged.encoding;
    lineEnding_ = staged.lineEnding;
    saveLineEnding_ = staged.saveLineEnding;
    modified_ = false;
}

void Document::AdoptLargeFileSaveAs(std::wstring path) noexcept
{
    path_ = std::move(path);
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
