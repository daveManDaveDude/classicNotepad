#pragma once

#include "appearance.h"
#include "spelling.h"

#include <cstddef>
#include <string>
#include <vector>

namespace classic_notepad::macos {

struct MacAutomationSelection {
    std::size_t start = 0;
    std::size_t end = 0;
};

struct MacAutomationDocumentMetadata {
    std::wstring path;
    std::wstring displayName;
    bool hasPath = false;
    std::wstring encoding;
    std::wstring lineEnding;
    std::wstring saveLineEnding;
};

struct MacAutomationPageMargins {
    int left = 750;
    int top = 1000;
    int right = 750;
    int bottom = 1000;
};

struct MacAutomationSpellingIssue {
    std::size_t start = 0;
    std::size_t length = 0;
    std::wstring replacement;
    std::wstring action;
};

struct MacAutomationAppearance {
    classic_notepad::AppearanceTheme theme = classic_notepad::AppearanceTheme::System;
    bool darkMode = false;
    bool highContrast = false;
    std::wstring effectiveAppearance;
};

class MacAutomationHost {
public:
    virtual ~MacAutomationHost() = default;

    virtual void PumpEvents() = 0;
    virtual void NewDocument() = 0;
    virtual bool OpenFile(const std::wstring& path, std::wstring& errorMessage) = 0;
    virtual bool Save(std::wstring& errorMessage) = 0;
    virtual bool SaveAs(const std::wstring& path, std::wstring& errorMessage) = 0;
    virtual void SetText(const std::wstring& text) = 0;
    virtual void InsertText(const std::wstring& text) = 0;
    virtual std::wstring GetText() const = 0;
    virtual std::wstring GetTitle() const = 0;
    virtual bool IsModified() const = 0;
    virtual MacAutomationDocumentMetadata GetDocumentMetadata() const = 0;
    virtual std::wstring GetStatusText() const = 0;
    virtual MacAutomationSelection GetSelection() const = 0;
    virtual void SetSelection(std::size_t start, std::size_t end) = 0;
    virtual void SelectAll() = 0;
    virtual void Undo() = 0;
    virtual void DeleteSelection() = 0;
    virtual void DeleteForward() = 0;
    virtual bool Find(const std::wstring& text, bool matchCase, bool wholeWord, bool searchDown) = 0;
    virtual bool FindNext(bool matchCase, bool wholeWord, bool searchDown) = 0;
    virtual bool Replace(
        const std::wstring& text,
        const std::wstring& replacement,
        bool matchCase,
        bool wholeWord,
        bool searchDown) = 0;
    virtual std::size_t ReplaceAll(const std::wstring& text, const std::wstring& replacement, bool matchCase, bool wholeWord) = 0;
    virtual bool GoToLine(int lineNumber, std::wstring& errorMessage) = 0;
    virtual void InsertTimeDate() = 0;
    virtual void SetWordWrap(bool enabled) = 0;
    virtual bool GetWordWrap() const = 0;
    virtual void SetStatusBarVisible(bool visible) = 0;
    virtual bool GetStatusBarVisible() const = 0;
    virtual bool SetFont(const std::wstring& font, std::wstring& errorMessage) = 0;
    virtual std::wstring GetFont() const = 0;
    virtual MacAutomationPageMargins GetPageMargins() const = 0;
    virtual bool SetPageMargins(const MacAutomationPageMargins& margins, std::wstring& errorMessage) = 0;
    virtual bool PrintToTestSink(const std::wstring& path, std::wstring& errorMessage) const = 0;
    virtual MacAutomationAppearance GetAppearance() const = 0;
    virtual void SetAppearanceTheme(classic_notepad::AppearanceTheme theme) = 0;
    virtual classic_notepad::SpellCapability SpellCheckCapability() const = 0;
    virtual std::wstring SpellCheckLanguage() const = 0;
    virtual std::vector<MacAutomationSpellingIssue> CheckSpelling(const std::wstring& text) const = 0;
    virtual std::vector<std::wstring> SuggestSpelling(const std::wstring& word, std::size_t limit) const = 0;
    virtual bool IgnoreSpelling(const std::wstring& word, std::wstring& errorMessage) = 0;
    virtual bool AddSpelling(const std::wstring& word, bool dryRun, std::wstring& errorMessage) = 0;
};

class MacAutomationController {
public:
    explicit MacAutomationController(MacAutomationHost& host);
    int Run();

private:
    MacAutomationHost& host_;
    std::wstring testClipboard_;
};

} // namespace classic_notepad::macos
