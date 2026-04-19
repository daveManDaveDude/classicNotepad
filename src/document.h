#pragma once

#include "encoding.h"
#include "line_endings.h"

#include <string>

class Document {
public:
    using TextEncoding = classic_notepad::TextEncoding;
    using LineEndingStyle = classic_notepad::LineEndingStyle;

    bool Load(const std::wstring& path, std::wstring& editorText, std::wstring& errorMessage);
    bool Save(const std::wstring& editorText, std::wstring& errorMessage);
    bool SaveAs(const std::wstring& path, const std::wstring& editorText, std::wstring& errorMessage);

    void ResetUntitled();
    void ResetNewFile(const std::wstring& path);

    bool HasPath() const;
    bool IsModified() const;
    void SetModified(bool modified);

    const std::wstring& Path() const;
    std::wstring DisplayName() const;

private:
    std::wstring path_;
    bool modified_ = false;
    TextEncoding encoding_ = classic_notepad::TextEncoding::Utf8NoBom;
    LineEndingStyle lineEnding_ = classic_notepad::LineEndingStyle::Crlf;
    LineEndingStyle saveLineEnding_ = classic_notepad::LineEndingStyle::Crlf;
};
