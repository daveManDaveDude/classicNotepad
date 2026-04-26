#pragma once

#include "win32_platform.h"

#include "appearance.h"
#include "document.h"
#include "spell_check.h"

#include <windows.h>
#include <commdlg.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class ClassicNotepadApp {
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
        DWORD start = 0;
        DWORD end = 0;
    };

    explicit ClassicNotepadApp(HINSTANCE instance);
    ~ClassicNotepadApp();

    int Run(int showCommand, const std::wstring& initialFilePath);
    int RunAutomation(int showCommand, const std::wstring& initialFilePath, bool visible);

    bool AutomationNewDocument();
    bool AutomationOpenFile(const std::wstring& path, std::wstring& errorMessage);
    bool AutomationSave(std::wstring& errorMessage);
    bool AutomationSaveAs(const std::wstring& path, std::wstring& errorMessage);
    void AutomationSetText(const std::wstring& text);
    void AutomationInsertText(const std::wstring& text);
    std::wstring AutomationGetText() const;
    std::wstring AutomationGetTitle() const;
    bool AutomationIsModified() const;
    AutomationDocumentMetadata AutomationGetDocumentMetadata() const;
    std::wstring AutomationGetStatusText() const;
    AutomationSelection AutomationGetSelection() const;
    void AutomationSetSelection(DWORD selectionStart, DWORD selectionEnd);
    void AutomationSelectAll();
    void AutomationUndo();
    void AutomationCut();
    void AutomationCopy();
    void AutomationPaste();
    void AutomationDeleteSelection();
    bool AutomationFind(const std::wstring& text, bool matchCase, bool wholeWord, bool searchDown);
    bool AutomationFindNext(bool matchCase, bool wholeWord, bool searchDown);
    bool AutomationReplace(
        const std::wstring& text,
        const std::wstring& replacement,
        bool matchCase,
        bool wholeWord,
        bool searchDown);
    std::size_t AutomationReplaceAll(
        const std::wstring& text,
        const std::wstring& replacement,
        bool matchCase,
        bool wholeWord);
    bool AutomationGoToLine(int lineNumber, std::wstring& errorMessage);
    void AutomationInsertTimeDate();
    void AutomationSetWordWrap(bool enabled);
    bool AutomationGetWordWrap() const;
    void AutomationSetStatusBarVisible(bool visible);
    bool AutomationGetStatusBarVisible() const;
    bool AutomationSetFont(const std::wstring& fontDescription, std::wstring& errorMessage);
    std::wstring AutomationGetFont() const;
    bool AutomationSetPageMarginsThousandths(const RECT& margins, std::wstring& errorMessage);
    RECT AutomationGetPageMarginsThousandths() const;
    bool AutomationPrintToTestSink(const std::wstring& path, std::wstring& errorMessage) const;
    bool AutomationSpellCheckAvailable() const;
    std::vector<SpellingErrorRange> AutomationCheckSpelling(const std::wstring& text) const;
    std::vector<std::wstring> AutomationSuggestSpelling(const std::wstring& word, std::size_t limit) const;
    bool AutomationIgnoreSpelling(const std::wstring& word, std::wstring& errorMessage);
    bool AutomationAddSpelling(const std::wstring& word, bool dryRun, std::wstring& errorMessage);
    bool AutomationDarkModeEnabled() const;
    classic_notepad::AppearanceTheme AutomationAppearanceTheme() const;
    bool AutomationHighContrastThemeActive() const;
    void AutomationSetAppearanceTheme(classic_notepad::AppearanceTheme theme);

private:
    enum class ScrollBarOrientation {
        Vertical,
        Horizontal
    };

    struct ScrollBarMetrics {
        int minimum = 0;
        int maximum = 0;
        int page = 1;
        int position = 0;
    };

    struct OwnerDrawMenuItem {
        std::wstring text;
        bool separator = false;
        bool reserveIconSpace = false;
    };

    bool RegisterMainWindowClass();
    bool RegisterMenuBarClass();
    bool RegisterScrollBarClass();
    bool CreateMainWindow(int showCommand);
    HWND CreateMenuBar();
    HWND CreateCustomScrollBar(ScrollBarOrientation orientation);
    HWND CreateScrollCorner();
    HWND CreateEditor();
    HWND CreateStatusBar();
    void ResizeEditor();
    void UpdateEditorFrameStyle();
    RECT GetEditorHostRect();
    RECT GetEditorRectForScrollBars(const RECT& hostRect, bool verticalVisible, bool horizontalVisible) const;
    void ApplyEditorAndScrollBarLayout(
        const RECT& hostRect,
        bool verticalVisible,
        bool horizontalVisible,
        bool repaint);
    int GetScrollBarThickness(ScrollBarOrientation orientation) const;
    bool UseCustomScrollBars() const;
    int GetMenuBarHeight() const;
    HMENU ActiveMenu() const;
    int GetTopLevelMenuCount() const;
    std::wstring GetTopLevelMenuText(int index) const;
    RECT GetMenuBarItemRect(int index) const;
    int MenuIndexFromPoint(POINT point) const;
    void PaintMenuBar(HDC deviceContext) const;
    void SetHotMenuIndex(int index);
    void ClearMenuMode();
    void ShowMenuPopup(int index, bool fromKeyboard);
    bool ActivateMenuBarFromKeyboard();
    bool ActivateMenuMnemonic(wchar_t mnemonic);
    void UpdateMenuChrome();
    OwnerDrawMenuItem* StoreOwnerDrawMenuItem(
        std::vector<std::unique_ptr<OwnerDrawMenuItem>>& storage,
        const std::wstring& text,
        bool separator,
        bool reserveIconSpace) const;
    bool AppendOwnerDrawMenuItem(
        HMENU menu,
        UINT flags,
        UINT_PTR itemId,
        const std::wstring& text,
        std::vector<std::unique_ptr<OwnerDrawMenuItem>>& storage,
        bool reserveIconSpace = false) const;
    bool AppendOwnerDrawMenuSeparator(
        HMENU menu,
        std::vector<std::unique_ptr<OwnerDrawMenuItem>>& storage,
        bool reserveIconSpace = false) const;
    void ApplyOwnerDrawToPopupMenu(
        HMENU menu,
        std::vector<std::unique_ptr<OwnerDrawMenuItem>>& storage) const;
    void ApplyOwnerDrawToMainMenu();
    void ApplyMenuBackground(HMENU menu) const;
    void UpdateTitle();
    void UpdateMenuState(HMENU menu);
    void UpdateStatusBar();
    void UpdateStatusBarPartLayout();
    void SetStatusCharacterCountFromText(const std::wstring& text);
    std::size_t GetStatusCharacterCount();
    void UpdateStatusBarSizeGripStyle();
    void UpdateScrollBars();
    void InvalidateWidestLineCache();
    int GetEditorLineHeight() const;
    int GetVisibleEditorLineCount(int lineMargin) const;
    int GetLastVisibleEditorLine(int lineMargin) const;
    std::wstring GetEditorLineText(int line, int lengthLimit) const;
    bool GetVisibleEditorText(std::wstring& text, DWORD& rangeStart) const;
    int MeasureWidestEditorLine() const;
    int MeasureFullEditorLineWidth() const;
    int MeasureVisibleEditorLineWidth() const;
    bool NeedsVerticalScrollBar(int availableHeight) const;
    bool NeedsHorizontalScrollBar(int availableWidth) const;
    void SetEditorScrollBarVisibility(bool verticalVisible, bool horizontalVisible);
    void SetNativeEditorScrollBarVisibility(bool verticalVisible, bool horizontalVisible);
    ScrollBarMetrics GetCustomScrollBarMetrics(ScrollBarOrientation orientation) const;
    int GetEditorHorizontalScrollUnit() const;
    RECT GetCustomScrollBarThumbRect(ScrollBarOrientation orientation) const;
    void PaintCustomScrollBar(HWND scrollBar, HDC deviceContext) const;
    void ScrollCustomScrollBarByPage(ScrollBarOrientation orientation, bool forward);
    void ScrollCustomScrollBarToPosition(ScrollBarOrientation orientation, int targetPosition);
    bool HandleMouseWheel(WPARAM wParam);
    void BeginCustomScrollBarInteraction(HWND scrollBar, LPARAM lParam);
    void UpdateCustomScrollBarDrag(HWND scrollBar, LPARAM lParam);
    void EndCustomScrollBarDrag(HWND scrollBar);
    void UpdateThemeFromSystem();
    void ApplyThemeToWindows();
    void RecreateThemeBrushes();
    void DestroyThemeBrushes();
    LRESULT HandleControlColor(HDC deviceContext, HWND controlWindow) const;
    LRESULT HandleDrawItem(WPARAM controlId, LPARAM lParam) const;
    LRESULT HandleNotify(LPARAM lParam) const;
    RECT GetResizeGripRect() const;
    RECT GetStatusBarResizeGripRect() const;
    bool IsResizableFromGrip() const;
    bool IsPointInStatusBarResizeGrip(POINT point) const;
    void DrawDarkResizeGrip(HDC deviceContext) const;
    void DrawDarkStatusBarChrome(HDC deviceContext) const;
    bool StartResizeFromStatusGrip(HWND statusBar, LPARAM lParam) const;
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
    bool FindNextWithFlags(DWORD flags, bool showNotFoundMessage, const std::wstring* textSnapshot = nullptr);
    bool ReplaceCurrentSelectionIfMatch(DWORD flags);
    void ReplaceAllMatches(DWORD flags);
    bool SelectionMatchesFindText(DWORD flags, const std::wstring* textSnapshot = nullptr) const;
    bool PrintEditorText(HDC printerDc, std::wstring& errorMessage) const;
    void CloseFindReplaceDialogs();
    void DestroyOwnedEditorFont();
    void DestroyOwnedMenuFont();
    void DestroyPrintDialogHandles();
    void HandleFindReplaceMessage(LPARAM lParam);
    int ShowGoToDialog(int currentLine, int maxLine);
    void GoToLine(int lineNumber);
    std::wstring BuildTimeDateText() const;
    std::wstring ShowOpenFileDialog();
    std::wstring ShowSaveFileDialog();
    int ShowMessageDialog(const std::wstring& message, UINT flags) const;
    void ShowError(const std::wstring& message);

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMeasureItem(WPARAM controlId, LPARAM lParam) const;

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MenuBarWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ScrollBarWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StatusBarSubclassProc(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData);
    static INT_PTR CALLBACK GoToDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    static INT_PTR CALLBACK AboutDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);

    HINSTANCE instance_;
    HWND mainWindow_ = nullptr;
    HWND menuBar_ = nullptr;
    HWND verticalScrollBar_ = nullptr;
    HWND horizontalScrollBar_ = nullptr;
    HWND scrollCorner_ = nullptr;
    HWND editor_ = nullptr;
    HWND statusBar_ = nullptr;
    HWND findDialog_ = nullptr;
    HWND replaceDialog_ = nullptr;
    HACCEL accelerator_ = nullptr;
    HMENU mainMenu_ = nullptr;
    HFONT menuFont_ = nullptr;
    HFONT editorFont_ = nullptr;
    HBRUSH darkEditorBackgroundBrush_ = nullptr;
    HBRUSH darkStatusBackgroundBrush_ = nullptr;
    HBRUSH darkMenuBackgroundBrush_ = nullptr;
    HGLOBAL pageSetupDevMode_ = nullptr;
    HGLOBAL pageSetupDevNames_ = nullptr;
    RECT pageMarginsThousandths_ {750, 750, 750, 750};
    bool ownsMenuFont_ = false;
    bool ownsEditorFont_ = false;
    bool mainMenuOwnerDrawApplied_ = false;
    WNDPROC originalEditorProc_ = nullptr;
    UINT findReplaceMessage_ = 0;
    Document document_;
    FINDREPLACEW findReplace_ {};
    std::array<std::wstring, 4> statusBarParts_ {};
    std::array<wchar_t, 512> findBuffer_ {};
    std::array<wchar_t, 512> replaceBuffer_ {};
    std::vector<std::unique_ptr<OwnerDrawMenuItem>> mainOwnerDrawMenuItems_;
    DWORD findFlags_ = FR_DOWN;
    bool wordWrap_ = false;
    bool statusBarVisible_ = true;
    bool verticalScrollBarVisible_ = true;
    bool horizontalScrollBarVisible_ = true;
    bool nativeVerticalScrollBarVisible_ = true;
    bool nativeHorizontalScrollBarVisible_ = true;
    bool updatingScrollBars_ = false;
    bool menuTrackingMouse_ = false;
    bool menuKeyboardActive_ = false;
    bool customScrollBarDragging_ = false;
    bool darkModeEnabled_ = false;
    classic_notepad::AppearanceTheme appearanceTheme_ = classic_notepad::AppearanceTheme::System;
    bool automationMode_ = false;
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
    std::size_t statusCharacterCount_ = 0;
    int statusCharacterTextLength_ = 0;
    bool statusCharacterCountDirty_ = false;
    mutable int cachedWidestLineWidth_ = 0;
    mutable bool widestLineCacheDirty_ = true;
    int hotMenuIndex_ = -1;
    int activeMenuIndex_ = -1;
    ScrollBarOrientation activeScrollBar_ = ScrollBarOrientation::Vertical;
    int scrollBarDragStartMouse_ = 0;
    int scrollBarDragStartPosition_ = 0;
    int horizontalScrollPosition_ = 0;
    int mouseWheelRemainder_ = 0;
    RECT lastVerticalScrollBarThumb_ {};
    RECT lastHorizontalScrollBarThumb_ {};
    bool lastVerticalScrollBarPainted_ = false;
    bool lastHorizontalScrollBarPainted_ = false;
};
