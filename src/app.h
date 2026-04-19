#pragma once

#include "document.h"
#include "spell_check.h"

#include <windows.h>
#include <commdlg.h>

#include <array>
#include <string>
#include <vector>

class ClassicNotepadApp {
public:
    explicit ClassicNotepadApp(HINSTANCE instance);
    ~ClassicNotepadApp();

    int Run(int showCommand, const std::wstring& initialFilePath);

private:
    bool RegisterMainWindowClass();
    bool CreateMainWindow(int showCommand);
    HWND CreateEditor();
    HWND CreateStatusBar();
    void ResizeEditor();
    void UpdateTitle();
    void UpdateMenuState(HMENU menu);
    void UpdateStatusBar();
    void RefreshSpellCheck(bool immediate);
    void ScheduleSpellCheck();
    void RunSpellCheckNow();
    void DrawSpellingUnderlines(HWND editorWindow);
    bool HandleEditorContextMenu(HWND editorWindow, LPARAM lParam);
    void ShowSpellingUnavailableMessage();
    void ShowAboutDialog();

    void HandleInitialFilePath(const std::wstring& path);
    void CreateNewDocumentForPath(const std::wstring& path);
    bool ConfirmCreateMissingFile(const std::wstring& path) const;
    void HandleEditorChanged();
    void HandleNew();
    void HandleOpen();
    bool HandleSave();
    bool HandleSaveAs();
    void HandlePageSetup();
    void HandlePrint();
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
    bool LoadDocument(const std::wstring& path);
    bool ConfirmSaveChanges();
    std::wstring GetEditorText() const;
    std::wstring GetSelectedText() const;
    void SetEditorText(const std::wstring& text);
    void ReplaceEditorTextFromCommand(const std::wstring& text);
    void ReplaceSelection(const std::wstring& text);
    void GetSelectionRange(DWORD& selectionStart, DWORD& selectionEnd) const;
    bool HasSelection() const;
    bool CanPasteText() const;
    void SeedFindTextFromSelection();
    bool FindNextWithFlags(DWORD flags, bool showNotFoundMessage);
    bool ReplaceCurrentSelectionIfMatch(DWORD flags);
    void ReplaceAllMatches(DWORD flags);
    bool SelectionMatchesFindText(DWORD flags) const;
    bool PrintEditorText(HDC printerDc, std::wstring& errorMessage) const;
    void CloseFindReplaceDialogs();
    void DestroyOwnedEditorFont();
    void DestroyPrintDialogHandles();
    void HandleFindReplaceMessage(LPARAM lParam);
    int ShowGoToDialog(int currentLine, int maxLine);
    void GoToLine(int lineNumber);
    std::wstring BuildTimeDateText() const;
    std::wstring ShowOpenFileDialog();
    std::wstring ShowSaveFileDialog();
    void ShowError(const std::wstring& message);

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static INT_PTR CALLBACK GoToDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);

    HINSTANCE instance_;
    HWND mainWindow_ = nullptr;
    HWND editor_ = nullptr;
    HWND statusBar_ = nullptr;
    HWND findDialog_ = nullptr;
    HWND replaceDialog_ = nullptr;
    HACCEL accelerator_ = nullptr;
    HFONT editorFont_ = nullptr;
    HGLOBAL pageSetupDevMode_ = nullptr;
    HGLOBAL pageSetupDevNames_ = nullptr;
    RECT pageMarginsThousandths_ {750, 750, 750, 750};
    bool ownsEditorFont_ = false;
    WNDPROC originalEditorProc_ = nullptr;
    UINT findReplaceMessage_ = 0;
    Document document_;
    FINDREPLACEW findReplace_ {};
    std::array<wchar_t, 512> findBuffer_ {};
    std::array<wchar_t, 512> replaceBuffer_ {};
    DWORD findFlags_ = FR_DOWN;
    bool wordWrap_ = false;
    bool statusBarVisible_ = true;
    bool suppressEditorChange_ = false;
    bool comInitialized_ = false;
    SpellCheckService spellChecker_;
    std::vector<SpellingErrorRange> spellingErrors_;
    bool spellCheckAvailable_ = false;
    bool spellCheckMessageShown_ = false;
    UINT_PTR spellCheckTimerId_ = 0;
    std::wstring contextMenuWord_;
    DWORD contextMenuWordStart_ = 0;
    DWORD contextMenuWordLength_ = 0;
};
