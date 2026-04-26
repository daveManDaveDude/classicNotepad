#pragma once

#include "document.h"

#include <gtk/gtk.h>

#include <cstddef>
#include <string>

namespace classic_notepad::linux_ui {

std::wstring WideFromUtf8(const char* text);
std::wstring WideFromUtf8(const std::string& text);
std::string Utf8FromWide(const std::wstring& text);

class GtkNotepadApp {
public:
    struct AutomationDocumentMetadata {
        std::wstring path;
        std::wstring displayName;
        std::wstring encoding;
        std::wstring lineEnding;
        std::wstring saveLineEnding;
        bool hasPath = false;
    };

    struct AutomationSelection {
        std::size_t start = 0;
        std::size_t end = 0;
    };

    struct AutomationPageMargins {
        int left = 750;
        int top = 750;
        int right = 750;
        int bottom = 750;
    };

    explicit GtkNotepadApp(std::wstring initialPath);
    ~GtkNotepadApp();

    int Run(int argc, char** argv);
    int RunAutomation(bool visible);

    void Activate(GtkApplication* application);
    void PumpEvents();

    GtkApplication* Application() const;
    GtkWindow* Window() const;
    GtkTextBuffer* Buffer() const;
    GtkWidget* TextView() const;
    bool IsAutomationMode() const;

    void HandleNew();
    void HandleOpen();
    bool HandleSave();
    bool HandleSaveAs();
    void HandlePageSetup();
    void HandlePrint();
    void HandleExit();
    void HandleUndo();
    void HandleCut();
    void HandleCopy();
    void HandlePaste();
    void HandleDelete();
    void HandleFind();
    void HandleFindNext();
    void HandleReplace();
    void HandleGoTo();
    void HandleSelectAll();
    void HandleTimeDate();
    void HandleToggleWordWrap();
    void HandleChooseFont();
    void HandleToggleStatusBar();
    void HandleAbout();

    bool NewDocument();
    bool OpenFile(const std::wstring& path, std::wstring& errorMessage);
    bool Save(std::wstring& errorMessage);
    bool SaveAs(const std::wstring& path, std::wstring& errorMessage);
    bool ConfirmDiscard();
    void CreateNewDocumentForPath(const std::wstring& path);
    void HandleInitialFilePath(bool createMissingWithoutPrompt);
    void UpdateTitle();
    void UpdateStatus();
    void ShowError(const std::wstring& message);

    void SetText(const std::wstring& text);
    void InsertText(const std::wstring& text);
    std::wstring GetText() const;
    std::wstring GetTitle() const;
    bool IsModified() const;
    AutomationDocumentMetadata GetDocumentMetadata() const;
    std::wstring GetStatusText() const;
    AutomationSelection GetSelection() const;
    void SetSelection(std::size_t selectionStart, std::size_t selectionEnd);
    void SelectAll();
    void Undo();
    void Cut();
    void Copy();
    void Paste();
    void DeleteSelection();
    bool Find(const std::wstring& text, bool matchCase, bool wholeWord, bool searchDown);
    bool FindNext(bool matchCase, bool wholeWord, bool searchDown);
    bool Replace(
        const std::wstring& text,
        const std::wstring& replacement,
        bool matchCase,
        bool wholeWord,
        bool searchDown);
    std::size_t ReplaceAll(const std::wstring& text, const std::wstring& replacement, bool matchCase, bool wholeWord);
    bool GoToLine(int lineNumber, std::wstring& errorMessage);
    void InsertTimeDate();
    void SetWordWrap(bool enabled);
    bool GetWordWrap() const;
    void SetStatusBarVisible(bool visible);
    bool GetStatusBarVisible() const;
    bool SetFont(const std::wstring& fontDescription, std::wstring& errorMessage);
    std::wstring GetFont() const;
    bool SetPageMargins(const AutomationPageMargins& margins, std::wstring& errorMessage);
    AutomationPageMargins GetPageMargins() const;
    bool PrintToTestSink(const std::wstring& path, std::wstring& errorMessage) const;
    bool SpellCheckAvailable() const;

    void OnBufferChanged();
    void OnCursorMoved();
    void OnWindowDestroyed();

private:
    void BuildWindow(GtkApplication* application);
    void SetBufferText(const std::wstring& text, bool markModified);
    void SetInitialTitleAndStatus();
    bool MissingFilePath(const std::wstring& path) const;
    std::wstring SuggestedSaveName() const;
    std::wstring GetRawText() const;
    std::wstring GetSelectedText() const;
    void ReplaceSelection(const std::wstring& text);
    std::wstring BuildTimeDateText() const;
    int CurrentLine() const;
    int MaxLine() const;
    void ApplyFont();
    void InstallContextMenu();
    GtkPageSetup* EnsurePageSetup();
    GtkPrintSettings* EnsurePrintSettings();
    void StorePageSetup(GtkPageSetup* pageSetup);

    Document document_;
    std::wstring initialPath_;
    std::wstring title_;
    std::wstring statusText_;
    std::wstring findText_;
    std::wstring replaceText_;
    bool findMatchCase_ = false;
    bool findWholeWord_ = false;
    bool findSearchDown_ = true;
    std::wstring fontDescription_ = L"Monospace 11";

    GtkApplication* application_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* menuBar_ = nullptr;
    GtkWidget* textView_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    GtkWidget* statusBar_ = nullptr;
    GtkWidget* statusLabel_ = nullptr;
    GtkCssProvider* fontProvider_ = nullptr;
    GtkPageSetup* pageSetup_ = nullptr;
    GtkPrintSettings* printSettings_ = nullptr;

    bool suppressChange_ = false;
    bool automationMode_ = false;
    bool automationVisible_ = false;
    bool wordWrap_ = false;
    bool statusBarVisible_ = true;
    AutomationPageMargins pageMargins_;
};

} // namespace classic_notepad::linux_ui
