#pragma once

#include "encoding.h"
#include "file_io.h"
#include "line_endings.h"

#include <string>

class Document {
public:
    using TextEncoding = classic_notepad::TextEncoding;
    using LineEndingStyle = classic_notepad::LineEndingStyle;

    struct StagedLoad {
        std::wstring path;
        std::wstring editorText;
        TextEncoding encoding = TextEncoding::Utf8NoBom;
        LineEndingStyle lineEnding = LineEndingStyle::Crlf;
        LineEndingStyle saveLineEnding = LineEndingStyle::Crlf;
    };

    struct StagedLargeLoad {
        std::wstring path;
        TextEncoding encoding = TextEncoding::Utf8NoBom;
        LineEndingStyle lineEnding = LineEndingStyle::Crlf;
        LineEndingStyle saveLineEnding = LineEndingStyle::Crlf;
    };

    bool Load(const std::wstring& path, std::wstring& editorText, std::wstring& errorMessage);
    bool PrepareLoad(const std::wstring& path, StagedLoad& staged, std::wstring& errorMessage) const;
#if defined(_WIN32) && !defined(CLASSIC_NOTEPAD_PORTABLE_CORE_SMOKE)
    bool PrepareLoadWithAnsiCodePage(
        const std::wstring& path,
        StagedLoad& staged,
        std::wstring& errorMessage,
        std::uint32_t ansiCodePage) const;
#endif
    void CommitLoad(StagedLoad&& staged);
    bool Save(const std::wstring& editorText, std::wstring& errorMessage);
    bool SaveAs(const std::wstring& path, const std::wstring& editorText, std::wstring& errorMessage);
    const classic_notepad::SaveDiagnostics& LastSaveDiagnostics() const;

    // Large-file mode deliberately records metadata without decoding the file
    // into an editable document buffer.
    void OpenReadOnlyLargeFile(const std::wstring& path, TextEncoding encoding);
    void PrepareReadOnlyLargeFile(const std::wstring& path, TextEncoding encoding, StagedLargeLoad& staged) const;
    void PrepareReadOnlyLargeFile(
        const std::wstring& path,
        TextEncoding encoding,
        LineEndingStyle lineEnding,
        LineEndingStyle saveLineEnding,
        StagedLargeLoad& staged) const;
    void CommitReadOnlyLargeFile(StagedLargeLoad&& staged) noexcept;
    void AdoptLargeFileSaveAs(std::wstring path) noexcept;

    void ResetUntitled();
    void ResetNewFile(const std::wstring& path);

    bool HasPath() const;
    bool IsModified() const;
    void SetModified(bool modified);

    const std::wstring& Path() const;
    std::wstring DisplayName() const;
    TextEncoding Encoding() const;
    LineEndingStyle LineEnding() const;
    LineEndingStyle SaveLineEnding() const;

private:
    bool PrepareLoadImpl(
        const std::wstring& path,
        StagedLoad& staged,
        std::wstring& errorMessage,
        std::uint32_t ansiCodePage) const;

    std::wstring path_;
    bool modified_ = false;
    TextEncoding encoding_ = classic_notepad::TextEncoding::Utf8NoBom;
    LineEndingStyle lineEnding_ = classic_notepad::LineEndingStyle::Crlf;
    LineEndingStyle saveLineEnding_ = classic_notepad::LineEndingStyle::Crlf;
    classic_notepad::SaveDiagnostics lastSaveDiagnostics_;
};
