#pragma once

#include "win32_platform.h"

#include "appearance.h"
#include "document.h"
#include "document_transition_coordinator.h"
#include "large_file_view.h"
#include "spell_check.h"
#include "spell_text_utils.h"
#include "windows_spelling_occurrence.h"

#include <windows.h>
#include <commdlg.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
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

    struct AutomationSpellingActionResult {
        SpellOperationStatus status = SpellOperationStatus::Failure;
        HRESULT hresult = E_FAIL;
        bool changed = false;
        bool partial = false;
        UINT nativeNotification = 0U;
        std::wstring verification = L"not-applicable";
        std::wstring message;
    };

    struct AutomationPasteTiming {
        long long providerSetupMicroseconds = 0;
        long long productionDispatchMicroseconds = 0;
        long long providerCleanupMicroseconds = 0;
    };

    struct AutomationEditorWidthState {
        std::uint64_t generation = 0;
        // EM_GETLINECOUNT reports display rows, which may split a logical row.
        std::uint64_t editDisplayLineCount = 0;
        std::uint64_t pendingLines = 0;
        std::uint64_t measuredDisplayLines = 0;
        std::uint64_t measuredTextUnits = 0;
        std::uint64_t measurementBatches = 0;
        std::uint64_t lastBatchDiscoveredDisplayLines = 0;
        std::uint64_t lastBatchPublishedDisplayLines = 0;
        std::uint64_t lastBatchDiscoveredTextUnits = 0;
        std::uint64_t lastBatchMeasuredTextUnits = 0;
        std::uint64_t maximumBatchDiscoveredDisplayLines = 0;
        std::uint64_t maximumBatchPublishedDisplayLines = 0;
        std::uint64_t maximumBatchTextUnits = 0;
        std::uint64_t localEditMeasuredDisplayLines = 0;
        std::uint64_t localEditMeasuredTextUnits = 0;
        std::uint64_t conservativeLineEstimates = 0;
        std::uint64_t maximumEditDisplayLineLength = 0;
        int documentWidth = 0;
        int viewportWidth = 0;
        int viewportHeight = 0;
        int documentHorizontalRangeMaximumOffset = 0;
        // Legacy app position and document-width maximum use average-character units
        // in custom mode; the native mode position is EDIT's pixel scroll position.
        // Use customHorizontalOffset and horizontalAnchorPixels for cross-mode checks.
        int horizontalOffset = 0;
        int horizontalMaximumOffset = 0;
        // The EDIT control's own range and position from GetScrollInfo, when available.
        int nativeHorizontalOffset = 0;
        int nativeHorizontalMaximumOffset = 0;
        int nativeHorizontalScrollMinimum = 0;
        int nativeHorizontalScrollMaximum = 0;
        int nativeHorizontalScrollPage = 0;
        int fontMaximumAdvance = 0;
        DWORD editTextLimit = 0;
        int windowDpi = 0;
        int caretX = 0;
        int caretY = 0;
        bool caretPositionAvailable = false;
        int caretDisplayLine = -1;
        int charPositionCharacterIndex = -1;
        int charPositionX = 0;
        int charPositionY = 0;
        bool charPositionAvailable = false;
        // EM_CHARFROMPOS returns its character index in LOWORD; keep this raw.
        std::uint16_t hitTestCharacterLowWord = 0;
        std::uint16_t hitTestLineHighWord = 0;
        bool hitTestAvailable = false;
        int customHorizontalOffset = 0;
        int customHorizontalOffsetPixels = 0;
        bool nativeHorizontalScrollInfoAvailable = false;
        int horizontalScrollUnit = 1;
        int horizontalAnchorPixels = 0;
        int verticalFirstVisibleLine = 0;
        int verticalTopContentPosition = 0;
        int verticalPage = 1;
        int verticalMaximumOffset = 0;
        int verticalLineHeight = 1;
        int pastePostCaretFirstVisibleLine = -1;
        int pastePostCaretTopContentPosition = -1;
        int pastePostCaretHorizontalOffset = -1;
        bool measurementPending = false;
        bool horizontalScrollBarVisible = false;
        bool verticalScrollBarVisible = false;
    };
    struct AutomationClipboardDiagnostics {
        int openCalls = 0;
        int closeCalls = 0;
        int unicodeAvailabilityChecks = 0;
        int ansiAvailabilityChecks = 0;
        int unicodeGetDataCalls = 0;
        int ansiGetDataCalls = 0;
        int sizeCalls = 0;
        int lockCalls = 0;
        int unlockCalls = 0;
        int conversionCalls = 0;
        int lastAnsiSourceLength = 0;
    };

    struct AutomationPrintDiagnostics {
        bool succeeded = false;
        bool faultInjected = false;
        std::wstring injectedStage;
        std::wstring failureStage;
        std::wstring errorMessage;
        bool abortDocCalled = false;
        bool abortDocSucceeded = false;
    };

    struct AutomationLargeFileState {
        bool active = false;
        bool readOnly = false;
        bool editable = false;
        bool modified = false;
        bool indexingComplete = false;
        bool globalScrollReady = false;
        bool encodingValid = true;
        std::uint64_t fileSize = 0;
        std::uint64_t indexedBytes = 0;
        std::uint64_t indexedLines = 0;
        std::uint64_t totalLines = 0;
        std::uint64_t firstUsableMilliseconds = 0;
        std::uint64_t firstViewportPaintMilliseconds = 0;
        std::uint64_t fullIndexMilliseconds = 0;
        std::uint64_t characterCount = 0;
        std::uint64_t caret = 0;
        std::uint64_t selectionStart = 0;
        std::uint64_t selectionEnd = 0;
        std::uint64_t topLine = 0;
        std::uint64_t maximumTopLine = 0;
        int verticalScrollRangeMaximum = 0;
        int verticalScrollPosition = 0;
        std::uint32_t verticalScrollPage = 0;
        std::uint64_t visibleLineCount = 0;
        std::uint64_t generation = 0;
        std::uint64_t pieceCount = 0;
        std::uint64_t insertedBufferBytes = 0;
        std::uint64_t saveProgressBytes = 0;
        bool saveInProgress = false;
        std::uint64_t paintGeneration = 0;
        std::uint64_t callbackAllocationFailures = 0;
        std::uint64_t viewportGeneration = 0;
        bool wordWrap = false;
        std::uint64_t viewportVisualRows = 0;
        std::uint64_t wrappedContinuationRows = 0;
        std::uint64_t viewportCacheRebuilds = 0;
        std::uint64_t viewportCacheHits = 0;
        std::uint64_t viewportLastBuildDecodedCharacters = 0;
        std::uint64_t viewportLastBuildMeasuredCharacters = 0;
        std::uint64_t viewportLastLogicalAnchor = 0;
        std::uint64_t viewportLastSegmentStart = 0;
        std::uint64_t viewportLastSegmentEnd = 0;
        std::uint64_t spellingIssueCount = 0;
        std::uint64_t maximumLineColumns = 0;
        int horizontalOffset = 0;
        int horizontalMaximumOffset = 0;
        int viewportWidth = 0;
        int viewportHeight = 0;
        RECT caretRectangle{};
        bool operationInProgress = false;
        bool operationCompleted = false;
        bool operationSucceeded = false;
        std::wstring operationName;
        std::wstring operationError;
        std::wstring operationTargetPath;
        classic_notepad::SaveDiagnostics saveDiagnostics;
        std::uint64_t operationGeneration = 0;
        std::uint64_t operationToken = 0;
        std::uint64_t operationProgress = 0;
        std::uint64_t operationResultCount = 0;
    };

    explicit ClassicNotepadApp(HINSTANCE instance);
    ~ClassicNotepadApp();

    int Run(int showCommand, const std::wstring& initialFilePath);
    int RunAutomation(int showCommand, const std::wstring& initialFilePath, bool visible);

    bool AutomationNewDocument();
    bool AutomationRequestNewTransition(bool saveChanges);
    bool AutomationOpenFile(
        const std::wstring& path,
        std::wstring& errorMessage,
        std::uint32_t ansiCodePageOverride = 0U);
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
    AutomationEditorWidthState AutomationGetEditorWidthState() const;
    bool AutomationSetEditorViewportSize(int width, int height);
    bool AutomationSendEditorHorizontalScroll(UINT command, int trackPosition);
    bool AutomationSendEditorVerticalScroll(UINT command, int trackPosition);
    bool AutomationSendEditorLineScroll(int horizontalUnits, int verticalLines);
    bool AutomationSendEditorKey(UINT virtualKey, bool shift, bool control);
    bool AutomationClickEditorClientPoint(int x, int y);
    bool AutomationSendEditorDrag(int startX, int startY, int endX, int endY);
    void AutomationPasteTextThroughProductionHandler(const std::wstring& text);
    AutomationPasteTiming AutomationGetLastPasteTiming() const;
    std::wstring AutomationGetLastPasteOutcome() const;
    UINT AutomationGetLastPasteNativeNotification() const;
    AutomationClipboardDiagnostics AutomationGetLastClipboardDiagnostics() const;
    bool AutomationFocusEditor();
    bool AutomationSetPasteFaultStage(const std::wstring& stage);
    DWORD AutomationSetEditorTextLimit(DWORD limit);
    struct AutomationNativeReplaceProbe {
        std::wstring textBefore;
        std::wstring textAfter;
        DWORD textLimit = 0;
        UINT notificationMask = 0;
    };
    AutomationNativeReplaceProbe AutomationProbeNativeReplace(
        const std::wstring& text,
        DWORD selectionStart,
        DWORD selectionEnd,
        const std::wstring& replacement,
        DWORD textLimit);
    void AutomationSetSelection(DWORD selectionStart, DWORD selectionEnd);
    void AutomationSelectAll();
    void AutomationUndo();
    void AutomationCut();
    void AutomationCopy();
    void AutomationPaste();
    void AutomationDeleteSelection();
    void AutomationPressInsert();
    void AutomationTypeText(const std::wstring& text);
    void AutomationDelete();
    bool AutomationFind(const std::wstring& text, bool matchCase, bool wholeWord, bool searchDown);
    bool AutomationFindNext(bool matchCase, bool wholeWord, bool searchDown);
    bool AutomationReplace(
        const std::wstring& text,
        const std::wstring& replacement,
        bool matchCase,
        bool wholeWord,
        bool searchDown);
    bool AutomationDispatchFindReplace(
        const std::wstring& action,
        const std::wstring& text,
        const std::wstring& replacement,
        bool matchCase,
        bool wholeWord,
        bool searchDown,
        std::wstring& errorMessage);
    std::size_t AutomationReplaceAll(
        const std::wstring& text,
        const std::wstring& replacement,
        bool matchCase,
        bool wholeWord);
    bool AutomationGoToLine(std::uint64_t lineNumber, std::wstring& errorMessage);
    std::wstring AutomationProbeGoToDialogDefault();
    void AutomationInsertTimeDate();
    void AutomationSetWordWrap(bool enabled);
    bool AutomationGetWordWrap() const;
    void AutomationInvokeWordWrapMenuCommand();
    bool AutomationGetWordWrapMenuState(bool& enabled, bool& checked);
    bool AutomationActiveEditorHasFocus() const;
    bool AutomationNativeEditorHasFocus() const;
    void AutomationSetStatusBarVisible(bool visible);
    bool AutomationGetStatusBarVisible() const;
    bool AutomationSetFont(const std::wstring& fontDescription, std::wstring& errorMessage);
    std::wstring AutomationGetFont() const;
    bool AutomationSetPageMarginsThousandths(const RECT& margins, std::wstring& errorMessage);
    RECT AutomationGetPageMarginsThousandths() const;
    bool AutomationPrintToTestSink(const std::wstring& path, std::wstring& errorMessage) const;
    bool AutomationSetPrintFaultStage(const std::wstring& stage);
    AutomationPrintDiagnostics AutomationGetPrintDiagnostics() const;
    bool AutomationSpellCheckAvailable() const;
    SpellCheckResult AutomationCheckSpelling(const std::wstring& text) const;
    SpellSuggestResult AutomationSuggestSpelling(const std::wstring& word, std::size_t limit) const;
    SpellActionResult AutomationIgnoreSpelling(const std::wstring& word);
    SpellActionResult AutomationAddSpelling(const std::wstring& word, bool dryRun);
    bool AutomationConfigureSpellingTestBackend(const SpellTestBackendConfiguration& configuration);
    bool AutomationClearSpellingTestBackend();
    std::uint64_t AutomationSpellingTestBackendCallCount(const wchar_t* operation) const;
    classic_notepad::SpellingAnnotationBatch AutomationGetSpellingAnnotationBatch() const;
    bool AutomationRefreshSpellCheckNow();
    AutomationSpellingActionResult AutomationApplySpellingAction(
        const std::wstring& action,
        const classic_notepad::SpellingContextTarget& target,
        const std::wstring& suggestion,
        bool injectMappedInsertFailureForTesting = false);
    bool AutomationDarkModeEnabled() const;
    classic_notepad::AppearanceTheme AutomationAppearanceTheme() const;
    bool AutomationHighContrastThemeActive() const;
    void AutomationSetAppearanceTheme(classic_notepad::AppearanceTheme theme);
    AutomationLargeFileState AutomationGetLargeFileState() const;
    bool AutomationGetLargeFileLine(std::uint64_t oneBasedLine, std::wstring& text, std::wstring& errorMessage) const;
    bool AutomationGetLargeFileVisibleText(
        std::uint64_t& rangeStart, std::wstring& text, std::wstring& errorMessage);
    bool AutomationGetLargeFileVisibleWindows(
        std::vector<LargeFileVisibleTextWindow>& windows, std::wstring& errorMessage);
    bool AutomationGetEditorVisibleSpellingWindows(
        std::vector<classic_notepad::SpellingTextWindow>& windows, std::wstring& errorMessage) const;
    bool AutomationResolveSpellingContextTarget(
        POINT clientPoint,
        bool keyboardInvocation,
        classic_notepad::SpellingContextTarget& target);
    bool AutomationGetTextPositionPoint(std::uint64_t position, POINT& clientPoint);
    bool AutomationSendEditorContextMenu(int x, int y, bool keyboardInvocation);
    bool AutomationSetLargeFileViewportSize(int width, int height);
    bool AutomationGetLargeFileTextRange(
        std::uint64_t start,
        std::uint64_t length,
        std::wstring& text,
        std::wstring& errorMessage) const;
    bool AutomationSendLargeFileClick(int x, int y, bool shift);
    bool AutomationSendLargeFileDrag(int startX, int startY, int endX, int endY);
    bool AutomationSendLargeFileKey(UINT virtualKey, bool shift, bool control);
    bool AutomationSendLargeFileVerticalScroll(UINT command, int trackPosition);
    bool AutomationSendLargeFileWheel(int delta);
    bool AutomationSendLargeFileHorizontalScroll(UINT command);
    bool AutomationSendLargeFileText(const std::wstring& text);
    bool AutomationStartLargeFileFind(const std::wstring& text, bool matchCase, bool wholeWord, bool forward,
        std::wstring& errorMessage);
    bool AutomationStartLargeFileReplaceAll(const std::wstring& text, const std::wstring& replacement,
        bool matchCase, bool wholeWord, std::wstring& errorMessage);
    void AutomationCancelLargeFileOperation();
    bool AutomationStartLargeFileSave(const std::wstring& path, std::wstring& errorMessage);

private:
    using DocumentTransition = classic_notepad::DocumentTransitionIntent;

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

    struct NativeStatusLineCheckpoint {
        int textPosition = 0;
        std::uint64_t lineBreaks = 0;
        int lineStartPosition = 0;
        bool previousWasCarriageReturn = false;
    };

    struct EditorViewportAnchor {
        int horizontalPixels = 0;
        int verticalContentPosition = 0;
        bool valid = false;
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
    HWND CreateEditor(bool initiallyVisible, bool wordWrap);
    bool TrySetEditorText(HWND editor, const std::wstring& text, std::wstring& errorMessage) const;
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
    void ResetNativeStatusSummary(int textLength);
    bool AppendNativeStatusCheckpoint(int textPosition);
    void AccumulateNativeStatusCodeUnit(int textPosition, wchar_t character);
    bool TryGetNativeLogicalCoordinate(
        int textPosition, std::uint64_t& oneBasedLine, std::uint64_t& oneBasedColumn) const;
    bool EnsureNativeStatusSummaryComplete();
    bool GetNativeLogicalLineStart(std::uint64_t oneBasedLine, DWORD& textPosition) const;
    std::size_t GetStatusCharacterCount();
    void MeasureStatusCharacterCountBatch();
    std::wstring GetStatusCharacterCountLabel() const;
    void UpdateStatusBarSizeGripStyle();
    void UpdateScrollBars();
    void InvalidateWidestLineCache();
    int GetEditorLineHeight() const;
    int GetVisibleEditorLineCount(int lineMargin) const;
    std::wstring GetEditorLineText(int line, int lengthLimit) const;
    bool GetVisibleEditorTextWindows(std::vector<classic_notepad::SpellingTextWindow>& windows) const;
    bool ResolveEditorSpellingContextTarget(
        POINT clientPoint,
        bool keyboardInvocation,
        classic_notepad::SpellingContextTarget& target) const;
    classic_notepad::SpellingRevision CurrentSpellingRevision() const;
    void ClearSpellingAnnotations();
    void AdvanceSpellingSession();
    void AdvanceSpellingContentRevision();
    void AdvanceSpellingLayoutRevision();
    int MeasureWidestEditorLine() const;
    int MeasureEditorLineWidth(int line, int lineLength, bool& conservative) const;
    int MeasureEditorTextWidth(const std::wstring& text, bool& conservative) const;
    int ConservativeEditorLineWidth(int lineLength) const;
    void BeginEditorWidthIndexRebuild();
    void InvalidateEditorWidthIndex();
    void MeasureEditorWidthBatch();
    void UpdateEditorWidthIndexForMutation();
    void BeginEditorMutationTracking(UINT message, WPARAM wParam);
    bool IsEditorDisplayLineWrapped(int line, int lineCount) const;
    void EndEditorMutationTracking();
    void SetEditorLineWidth(std::size_t line, int width);
    int GetEditorWidthFallback() const;
    int GetEditorFontMaximumAdvance() const;
    int GetEditorTabInterval() const;
    int GetHorizontalAnchorPixels() const;
    void BeginEditorViewportAnchorPreservation();
    void EndEditorViewportAnchorPreservation();
    void RestoreEditorViewportAnchor(const EditorViewportAnchor& anchor);
    int GetNativeEditorHorizontalPosition(int* maximumPosition = nullptr) const;
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
    bool IsSpellingTargetCurrent(
        const classic_notepad::SpellingContextTarget& target,
        std::wstring& currentText,
        std::wstring& errorMessage) const;
    bool ReadCurrentSpellingTextRange(
        std::uint64_t start,
        std::uint64_t length,
        std::wstring& text,
        std::wstring& errorMessage) const;
    AutomationSpellingActionResult ApplySpellingAction(
        const std::wstring& action,
        const classic_notepad::SpellingContextTarget& target,
        const std::wstring& suggestion,
        bool injectMappedInsertFailureForTesting = false);
    bool IsIgnoredSpellingOccurrence(
        const classic_notepad::SpellingAnnotation& annotation,
        const classic_notepad::SpellingRevision& revision,
        const std::vector<classic_notepad::SpellingTextWindow>& windows) const;
    void ClearIgnoredSpellingOccurrences();
    void ReportSpellCheckFailure(SpellOperationStatus status, HRESULT hresult);
    void DrawSpellingUnderlines(HWND editorWindow);
    bool HandleEditorContextMenu(HWND editorWindow, LPARAM lParam);
    bool HandleLargeFileContextMenu(
        POINT screenPoint,
        const classic_notepad::SpellingContextTarget& target);
    void ShowSpellingUnavailableMessage();
    void ShowAboutDialog();

    void HandleInitialFilePath(const std::wstring& path);
    void CreateNewDocumentForPath(const std::wstring& path);
    bool ConfirmCreateMissingFile(const std::wstring& path) const;
    void HandleEditorChanged();
    void HandleNew();
    void HandleOpen();
    void RequestDocumentTransition(DocumentTransition transition);
    void PerformDocumentTransition(DocumentTransition transition);
    bool ResumePendingTransitionAfterLargeSave(const LargeFileViewInfo& info);
    bool CurrentDocumentIsModified() const;
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
    void FocusActiveEditor();
    void HandleChooseFont();
    void HandleToggleStatusBar();
    bool LoadDocument(const std::wstring& path, std::uint32_t ansiCodePageOverride = 0U);
    bool LoadLargeDocument(const std::wstring& path, std::wstring& errorMessage);
    void ExitLargeFileMode();
    bool IsLargeFileMode() const;
    void ShowLargeFileReadOnlyMessage();
    std::wstring GetEditorText() const;
    std::wstring GetSelectedText() const;
    void SetEditorText(const std::wstring& text);
    void ReplaceEditorTextFromCommand(const std::wstring& text);
    void ReplaceSelection(const std::wstring& text);
    void GetSelectionRange(DWORD& selectionStart, DWORD& selectionEnd) const;
    DWORD GetActiveCaretPosition() const;
    void TrackNativeSelectionAfterMessage(HWND editor, UINT message, WPARAM wParam, LPARAM lParam);
    bool HasSelection() const;
    bool CanPasteText() const;
    void SeedFindTextFromSelection();
    bool FindNextWithFlags(DWORD flags, bool showNotFoundMessage, const std::wstring* textSnapshot = nullptr);
    bool ReplaceCurrentSelectionIfMatch(DWORD flags);
    void ReplaceAllMatches(DWORD flags);
    bool SelectionMatchesFindText(DWORD flags, const std::wstring* textSnapshot = nullptr) const;
    bool ReplaceNativeWholeDocumentUndoably(const std::wstring& text, std::wstring& errorMessage);
    bool PrintEditorText(HDC printerDc, std::wstring& errorMessage);
    void CloseFindReplaceDialogs();
    void DestroyOwnedEditorFont();
    void DestroyOwnedMenuFont();
    void DestroyPrintDialogHandles();
    void HandleFindReplaceMessage(LPARAM lParam);
    std::uint64_t ShowGoToDialog(std::uint64_t currentLine, std::uint64_t maxLine);
    void GoToLine(std::uint64_t lineNumber);
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
    std::unique_ptr<LargeFileView> largeFileView_;
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
    HWND nativeSelectionEditor_ = nullptr;
    DWORD nativeSelectionAnchor_ = 0;
    DWORD nativeActiveCaretPosition_ = 0;
    bool nativeSelectionTrackingValid_ = false;
    UINT findReplaceMessage_ = 0;
    Document document_;
    classic_notepad::DocumentTransitionCoordinator documentTransitionCoordinator_;
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
    unsigned int editorViewportAnchorDepth_ = 0;
    EditorViewportAnchor editorViewportAnchor_;
    bool menuTrackingMouse_ = false;
    bool menuKeyboardActive_ = false;
    bool customScrollBarDragging_ = false;
    bool darkModeEnabled_ = false;
    classic_notepad::AppearanceTheme appearanceTheme_ = classic_notepad::AppearanceTheme::System;
    bool automationMode_ = false;
    bool automationGoToDialogProbe_ = false;
    std::wstring automationGoToDialogInitialValue_;
    mutable int automationMessageDialogResultOverride_ = 0;
    bool overwriteMode_ = false;
    bool overtypeAwaitingLowSurrogate_ = false;
    bool suppressEditorChange_ = false;
    bool comInitialized_ = false;
    SpellCheckService spellChecker_;
    classic_notepad::SpellingAnnotationBatch spellingBatch_;
    std::uint64_t spellingSessionId_ = 1;
    std::uint64_t spellingContentRevision_ = 1;
    std::uint64_t spellingLayoutRevision_ = 1;
    bool spellCheckAvailable_ = false;
    bool spellCheckMessageShown_ = false;
    bool spellCheckFailureDialogShown_ = false;
    std::vector<classic_notepad::windows::IgnoredSpellingOccurrence> ignoredSpellingOccurrences_;
    UINT_PTR spellCheckTimerId_ = 0;
    std::wstring automationPasteText_;
    bool automationPasteTextActive_ = false;
    std::wstring automationPasteFaultStage_;
    std::wstring automationPasteLastOutcome_;
    AutomationPasteTiming automationPasteTiming_;
    AutomationClipboardDiagnostics automationPasteClipboardDiagnostics_;
    std::wstring automationPrintFaultStage_;
    AutomationPrintDiagnostics automationPrintDiagnostics_;
    bool editorPasteReplacementActive_ = false;
    bool editorPasteChangeNotification_ = false;
    UINT editorPasteNativeFailureNotification_ = 0;
    HWND automationProbeEditor_ = nullptr;
    UINT automationProbeNotificationMask_ = 0;
    int automationPastePostCaretFirstVisibleLine_ = -1;
    int automationPastePostCaretTopContentPosition_ = -1;
    int automationPastePostCaretHorizontalOffset_ = -1;
    std::size_t statusCharacterCount_ = 0;
    int statusCharacterTextLength_ = 0;
    bool statusCharacterCountDirty_ = false;
    int statusCharacterScanPosition_ = 0;
    wchar_t statusCharacterPreviousUnit_ = 0;
    bool statusCharacterHasPreviousUnit_ = false;
    bool statusCharacterCountComplete_ = true;
    bool statusCharacterCountAvailable_ = true;
    std::uint64_t statusLogicalLineBreakCount_ = 0;
    int statusLogicalLineStartPosition_ = 0;
    bool statusLogicalPreviousWasCarriageReturn_ = false;
    bool statusLogicalLineIndexAvailable_ = true;
    std::vector<NativeStatusLineCheckpoint> nativeStatusLineCheckpoints_;
    mutable int cachedWidestLineWidth_ = 0;
    mutable bool widestLineCacheDirty_ = true;
    std::vector<int> editorLineWidths_;
    std::multiset<int> measuredEditorLineWidths_;
    std::size_t editorWidthScanLine_ = 0;
    std::size_t editorWidthUnknownLines_ = 0;
    std::uint64_t editorWidthGeneration_ = 0;
    std::uint64_t editorWidthMeasuredLines_ = 0;
    std::uint64_t editorWidthMeasuredTextUnits_ = 0;
    std::uint64_t editorWidthMeasurementBatches_ = 0;
    std::uint64_t editorWidthLastBatchDiscoveredLines_ = 0;
    std::uint64_t editorWidthLastBatchPublishedLines_ = 0;
    std::uint64_t editorWidthLastBatchDiscoveredTextUnits_ = 0;
    std::uint64_t editorWidthLastBatchMeasuredTextUnits_ = 0;
    std::uint64_t editorWidthMaximumBatchDiscoveredLines_ = 0;
    std::uint64_t editorWidthMaximumBatchPublishedLines_ = 0;
    std::uint64_t editorWidthMaximumBatchTextUnits_ = 0;
    std::uint64_t editorWidthConservativeLines_ = 0;
    std::uint64_t editorWidthMaximumEditDisplayLineLength_ = 0;
    std::size_t editorWidthLogicalLineStart_ = 0;
    std::uint64_t editorWidthLogicalLineStartOffset_ = 0;
    std::uint64_t editorWidthLogicalLineLength_ = 0;
    std::size_t editorWidthLogicalLineDisplayLines_ = 0;
    std::wstring editorWidthLogicalLineText_;
    bool editorWidthLogicalLineTextExact_ = true;
    bool editorWidthLogicalLineActive_ = false;
    bool editorWidthLogicalLineReady_ = false;
    std::uint64_t editorWidthLocalEditMeasurements_ = 0;
    std::uint64_t editorWidthLocalEditTextUnits_ = 0;
    bool editorWidthIndexNeedsRebuild_ = true;
    bool editorWidthIndexAllocationFailed_ = false;
    bool editorWidthIndexMutationActive_ = false;
    bool editorWidthIndexMutationHandled_ = false;
    bool editorWidthIndexMutationWholeDocument_ = false;
    int editorWidthIndexMutationStartLine_ = 0;
    int editorWidthIndexMutationEndLine_ = 0;
    std::uint64_t editorWidthIndexMutationGeneration_ = 0;
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
