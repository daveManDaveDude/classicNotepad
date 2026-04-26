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
    void HandleExit();

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
    void DeleteSelection();
    void SetWordWrap(bool enabled);
    bool GetWordWrap() const;
    void SetStatusBarVisible(bool visible);
    bool GetStatusBarVisible() const;

    void OnBufferChanged();
    void OnCursorMoved();
    void OnWindowDestroyed();

private:
    void BuildWindow(GtkApplication* application);
    void SetBufferText(const std::wstring& text, bool markModified);
    void SetInitialTitleAndStatus();
    bool MissingFilePath(const std::wstring& path) const;
    std::wstring SuggestedSaveName() const;

    Document document_;
    std::wstring initialPath_;
    std::wstring title_;
    std::wstring statusText_;

    GtkApplication* application_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* textView_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    GtkWidget* statusBar_ = nullptr;
    GtkWidget* statusLabel_ = nullptr;

    bool suppressChange_ = false;
    bool automationMode_ = false;
    bool automationVisible_ = false;
    bool wordWrap_ = false;
    bool statusBarVisible_ = true;
};

} // namespace classic_notepad::linux_ui
