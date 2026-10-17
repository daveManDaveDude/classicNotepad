#pragma once

#include "large_file_index.h"
#include "large_document.h"
#include "encoding.h"
#include "line_endings.h"
#include "spell_text_utils.h"
#include "windows_save_transaction.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct LargeFileViewInfo {
    bool active = false;
    bool readOnly = false;
    bool editable = false;
    bool modified = false;
    bool indexingComplete = false;
    bool globalScrollReady = false;
    bool encodingValid = true;
    std::uint64_t fileSize = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t indexedBytes = 0;
    std::uint64_t indexedLines = 0;
    std::uint64_t totalLines = 0;
    std::uint64_t firstUsableMilliseconds = 0;
    std::uint64_t firstViewportPaintMilliseconds = 0;
    std::uint64_t fullIndexMilliseconds = 0;
    std::uint64_t crlfCount = 0;
    std::uint64_t lfCount = 0;
    std::uint64_t crCount = 0;
    std::uint64_t characterCount = 0;
    std::uint64_t displayedCharacterCount = 0;
    bool displayedCharacterCountKnown = false;
    std::uint64_t caret = 0;
    std::uint64_t caretLine = 0;
    std::uint64_t caretColumn = 0;
    std::uint64_t selectionStart = 0;
    std::uint64_t selectionEnd = 0;
    std::uint64_t topLine = 0;
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
    std::uint64_t spellingIssueCount = 0;
    std::uint64_t viewportCacheRebuilds = 0;
    std::uint64_t viewportCacheHits = 0;
    std::uint64_t viewportLastBuildDecodedCharacters = 0;
    std::uint64_t viewportLastBuildMeasuredCharacters = 0;
    std::uint64_t viewportLastLogicalAnchor = 0;
    std::uint64_t viewportLastSegmentStart = 0;
    std::uint64_t viewportLastSegmentEnd = 0;
    std::uint64_t maximumLineColumns = 0;
    std::uint64_t maximumTopLine = 0;
    int verticalScrollRangeMaximum = 0;
    int verticalScrollPosition = 0;
    std::uint32_t verticalScrollPage = 0;
    std::uint64_t visibleLineCount = 0;
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
    classic_notepad::LargeFileEncoding encoding = classic_notepad::LargeFileEncoding::Utf8;
};

struct LargeFileSpellingRange {
    std::uint64_t start = 0;
    std::uint64_t length = 0;
};

using LargeFileVisibleTextWindow = classic_notepad::SpellingTextWindow;

// An editable, mapped, line-addressable text surface. Workers publish
// generation-tagged, coalesced messages; destruction joins them before the
// mapping handles disappear.
class LargeFileView {
public:
    static constexpr std::uint64_t kActivationThresholdBytes = 48ULL * 1024ULL * 1024ULL;

    LargeFileView();
    ~LargeFileView();

    LargeFileView(const LargeFileView&) = delete;
    LargeFileView& operator=(const LargeFileView&) = delete;

    static bool ShouldUseLargeFile(const std::wstring& path, std::uint64_t& fileSize, std::wstring& errorMessage);

    bool Create(
        HINSTANCE instance,
        HWND parent,
        int controlId,
        const std::wstring& path,
        HFONT font,
        bool darkMode,
        std::wstring& errorMessage,
        bool initiallyVisible = true,
        std::uint64_t sessionId = 1U);
    bool WaitForCompleteIndex(std::wstring& errorMessage);
    HWND Window() const;
    void SetBounds(const RECT& bounds);
    bool SetViewportSizeForTesting(int width, int height);
    void SetTheme(HFONT font, bool darkMode);
    void SetWordWrap(bool enabled);
    bool WordWrap() const;
    bool GetVisibleText(std::wstring& text, std::uint64_t& rangeStart, std::wstring& errorMessage);
    bool GetVisibleTextWindows(
        std::vector<LargeFileVisibleTextWindow>& windows,
        std::wstring& errorMessage);
    bool ResolveSpellingContextTargetAtPoint(
        POINT clientPoint,
        bool keyboardInvocation,
        classic_notepad::SpellingContextTarget& target);
    bool GetVisibleTextPositionPoint(std::uint64_t position, POINT& clientPoint);
    void SetSpellingBatch(classic_notepad::SpellingAnnotationBatch batch);
    classic_notepad::SpellingRevision GetSpellingRevision() const;
    void SetContextMenuHandler(
        std::function<bool(POINT, const classic_notepad::SpellingContextTarget&)> handler);
    void Focus(int allocationFailureForTesting = 0);
    void SelectAll();
    bool CopySelection(std::wstring& errorMessage);
    bool CutSelection(std::wstring& errorMessage);
    bool PasteText(const std::wstring& text, std::wstring& errorMessage);
    bool DeleteSelection(bool backspace, std::wstring& errorMessage);
    bool InsertText(const std::wstring& text, std::wstring& errorMessage);
    void InjectNextInsertFailureForTesting(std::wstring errorMessage);
    bool Undo(std::wstring& errorMessage);
    bool SetSelection(std::uint64_t start, std::uint64_t end, std::wstring& errorMessage);
    bool GetSelection(std::uint64_t& start, std::uint64_t& end) const;
    bool GetTextRange(std::uint64_t start, std::uint64_t length, std::wstring& text, std::wstring& errorMessage) const;
    bool Find(
        const std::wstring& text,
        bool matchCase,
        bool wholeWord,
        bool forward,
        std::wstring& errorMessage);
    bool SelectionMatches(
        const std::wstring& text,
        bool matchCase,
        bool wholeWord,
        std::wstring& errorMessage) const;
    std::uint64_t ReplaceAll(
        const std::wstring& text,
        const std::wstring& replacement,
        bool matchCase,
        bool wholeWord,
        std::wstring& errorMessage);
    bool Save(
        const std::wstring& path,
        classic_notepad::TextEncoding encoding,
        classic_notepad::LineEndingStyle lineEnding,
        bool rebase,
        std::wstring& errorMessage);
    bool GoToLine(std::uint64_t oneBasedLine, std::wstring& errorMessage);
    bool GetLine(std::uint64_t oneBasedLine, std::wstring& text, std::wstring& errorMessage) const;
    LargeFileViewInfo GetInfo() const;
    bool SendClickInput(int x, int y, bool shift);
    bool SendDragInput(int startX, int startY, int endX, int endY);
    bool SendKeyInput(UINT virtualKey, bool shift, bool control);
    bool SendVerticalScrollInput(UINT command, int trackPosition = 0);
    bool SendMouseWheelInput(int delta);
    bool SendTextInput(const std::wstring& text);
    bool StartFindAsync(const std::wstring& text, bool matchCase, bool wholeWord, bool forward, std::wstring& errorMessage);
    bool StartReplaceAllAsync(const std::wstring& text, const std::wstring& replacement,
        bool matchCase, bool wholeWord, std::wstring& errorMessage);
    bool StartSaveAsync(const std::wstring& path, classic_notepad::TextEncoding encoding,
        classic_notepad::LineEndingStyle lineEnding, bool rebase, std::wstring& errorMessage);
    void CancelOperation();
    void AcknowledgeOperationCompletion(std::uint64_t token);

private:
    enum class OperationKind : std::uint8_t { Find, ReplaceAll, Save };
    struct OperationCompletion {
        OperationKind kind = OperationKind::Find;
        std::uint64_t token = 0;
        std::uint64_t documentGeneration = 0;
        std::uint64_t selectionRevision = 0;
        bool succeeded = false;
        bool found = false;
        bool cancelled = false;
        std::uint64_t resultCount = 0;
        classic_notepad::LargeDocumentMatch match{};
        std::unique_ptr<classic_notepad::LargeDocument> candidateDocument;
        std::wstring error;
        std::wstring targetPath;
        std::wstring temporaryPath;
        bool targetWasCurrentPath = false;
        bool rebase = false;
        classic_notepad::TextEncoding saveEncoding = classic_notepad::TextEncoding::Utf8NoBom;
        classic_notepad::LineEndingStyle saveLineEnding = classic_notepad::LineEndingStyle::Crlf;
        classic_notepad::SaveDiagnostics saveDiagnostics;
    };
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool OpenMapping(
        const std::wstring& path,
        std::wstring& errorMessage,
        bool injectFailureAfterOpen = false);
    void CloseMapping();
    void StartIndexing();
    void StopIndexing();
    void IndexWorker();
    bool AppendIndexedDocument(
        const std::vector<classic_notepad::LargeFilePieceSummary>& pieces,
        std::wstring& errorMessage);
    bool EnsureEditable(std::wstring& errorMessage) const;
    // documentMutex_ must be held. Whole-document operations use this gate so
    // an editable prefix is never mistaken for a complete source document.
    bool EnsureCompleteDocument(std::wstring& errorMessage) const;
    void PublishOperationCompletion(std::unique_ptr<OperationCompletion> completion, HWND targetWindow);
    void AdoptOperationCompletion(std::uint64_t token);
    bool ReplaceSelection(const std::wstring& text, std::wstring& errorMessage);
    void MarkModified(bool invalidateAll = true);
    void InvalidateDocumentLine(std::uint64_t line);
    bool WriteStreamToTemporaryFile(
        const classic_notepad::LargeDocument& source,
        classic_notepad::WindowsSaveTransaction& transaction,
        classic_notepad::TextEncoding encoding,
        classic_notepad::LineEndingStyle lineEnding,
        std::wstring& errorMessage);
    bool CopyBytes(std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) const;
    bool GetLineBounds(std::uint64_t zeroBasedLine, std::uint64_t& start, std::uint64_t& end, bool& knownEnd) const;
    bool ReadLine(std::uint64_t zeroBasedLine, std::wstring& text, bool& available) const;
    void Paint(HDC deviceContext, const RECT& invalidRect);
    bool ReadEditedLine(
        std::uint64_t zeroBasedLine,
        std::uint64_t& start,
        std::uint64_t& contentEnd,
        std::uint64_t& nextLineStart,
        std::wstring& text) const;
    struct CachedViewportRow {
        std::uint64_t line = 0;
        std::uint64_t start = 0;
        std::uint64_t contentEnd = 0;
        std::uint64_t nextLineStart = 0;
        std::uint64_t segmentStart = 0;
        std::uint64_t segmentEnd = 0;
        std::uint64_t logicalAnchor = 0;
        std::wstring text;
        std::wstring displayText;
        std::vector<std::size_t> displayBoundary;
        std::vector<int> xBoundary;
        std::vector<std::uint8_t> scalarBoundary;
        int drawOriginX = 4;
        bool available = false;
    };
    void RebuildViewportCache(std::uint64_t rowCount);
    void RefreshCachedViewportLine(
        std::uint64_t line,
        std::uint64_t removedCharacters,
        std::uint64_t insertedCharacters);
    void InvalidateViewportCache();
    bool ViewportCacheMatches(std::uint64_t generation, const RECT& client) const;
    void NotifyStateChanged();
    void UpdateCaret();
    void DestroyTextCaret();
    void InjectPresentationAllocationFailureForTesting(bool painting);
    bool HitTest(POINT point, std::uint64_t& position, int* visualX = nullptr,
        bool characterAtPoint = false);
    bool FindCachedRowForPosition(std::uint64_t position, std::size_t& rowIndex) const;
    void MoveCaret(std::uint64_t position, bool extendSelection, bool preservePreferredX = false);
    void MoveHorizontal(bool right, bool byWord, bool extendSelection);
    void MoveVertical(int lines, bool extendSelection);
    void MoveLineEdge(bool end, bool control, bool extendSelection);
    void EnsureCaretVisible();
    void ScrollCaretPage(int direction, bool extendSelection);
    void BeginMouseSelection(POINT point, bool extendSelection);
    void UpdateMouseSelection(POINT point);
    void SelectWordAt(POINT point);
    void ShowContextMenu(POINT screenPoint);
    void ShowInputError(const std::wstring& errorMessage) const;
    void StopOperation();
    void RefreshScrollBars();
    void ScrollVertical(int command, int trackPosition);
    void ScrollHorizontal(int command, int trackPosition);
    void SetTopLine(std::uint64_t line);
    std::uint64_t VisibleLineCount() const;
    std::uint64_t EstimatedTotalLines() const;
    std::uint64_t IndexedLineCount() const;
    std::uint64_t PositionToLine(int position) const;
    int LineToPosition(std::uint64_t line) const;
    std::wstring IndexingMessage() const;
    static bool RegisterClass(HINSTANCE instance);

    HINSTANCE instance_ = nullptr;
    HWND parent_ = nullptr;
    HWND window_ = nullptr;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    std::wstring path_;
    std::uint64_t fileSize_ = 0;
    std::uint64_t dataOffset_ = 0;
    DWORD allocationGranularity_ = 65536;
    HFONT font_ = nullptr;
    bool darkMode_ = false;
    bool allSelected_ = false;
    int horizontalOffset_ = 0;
    int horizontalMaximumOffset_ = 0;
    std::uint64_t topLine_ = 0;
    std::uint64_t topWrapOffset_ = 0;
    bool wordWrap_ = false;
    classic_notepad::SpellingAnnotationBatch spellingBatch_;
    std::function<bool(POINT, const classic_notepad::SpellingContextTarget&)> contextMenuHandler_;
    std::uint64_t startedTick_ = 0;
    std::uint64_t appendedByte_ = 0;
    std::uint64_t appendedCharacter_ = 0;
    std::uint64_t appendedLineBreaks_ = 0;

    classic_notepad::LargeFileEncoding encoding_ = classic_notepad::LargeFileEncoding::Utf8;
    mutable std::mutex lineMutex_;
    std::vector<std::uint64_t> lineOffsets_;
    std::vector<std::uint64_t> lineCharacterOffsets_;
    std::unique_ptr<classic_notepad::LargeDocument> document_;
    classic_notepad::LargeDocumentByteReader sourceReader_;
    mutable std::mutex documentMutex_;
    std::uint64_t selectionStart_ = 0;
    std::uint64_t selectionEnd_ = 0;
    std::uint64_t selectionAnchor_ = 0;
    std::uint64_t caretPosition_ = 0;
    std::uint64_t selectionRevision_ = 0;
    bool focused_ = false;
    int presentationAllocationFailureForTesting_ = 0;
    std::wstring nextInsertFailureForTesting_;
    std::uint64_t callbackAllocationFailures_ = 0;
    bool caretCreated_ = false;
    bool mouseSelecting_ = false;
    bool preferredXValid_ = false;
    bool overwriteMode_ = false;
    wchar_t pendingHighSurrogate_ = 0;
    bool syntheticShift_ = false;
    bool syntheticControl_ = false;
    int preferredX_ = 0;
    POINT lastMousePoint_{};
    std::atomic<bool> stateNotificationPending_{false};
    std::atomic<std::uint64_t> paintGeneration_{0};
    std::uint64_t viewportGeneration_ = 0;
    RECT caretRectangle_{};
    std::vector<CachedViewportRow> viewportRows_;
    std::uint64_t viewportCacheTopLine_ = 0;
    std::uint64_t viewportCacheGeneration_ = 0;
    std::uint64_t viewportCacheLayoutRevision_ = 0;
    std::uint64_t layoutRevision_ = 1;
    std::uint64_t sessionId_ = 1;
    std::uint64_t viewportCacheTopWrapOffset_ = 0;
    int viewportCacheHorizontalOffset_ = 0;
    int viewportCacheHorizontalMaximumOffset_ = 0;
    LONG viewportCacheWidth_ = 0;
    LONG viewportCacheHeight_ = 0;
    HFONT viewportCacheFont_ = nullptr;
    bool viewportCacheWordWrap_ = false;
    std::uint64_t viewportCacheRebuilds_ = 0;
    std::uint64_t viewportCacheHits_ = 0;
    std::uint64_t viewportLastBuildDecodedCharacters_ = 0;
    std::uint64_t viewportLastBuildMeasuredCharacters_ = 0;
    std::uint64_t viewportLastLogicalAnchor_ = 0;
    std::uint64_t viewportLastSegmentStart_ = 0;
    std::uint64_t viewportLastSegmentEnd_ = 0;
    bool viewportCacheValid_ = false;
    std::atomic<bool> modified_{false};
    std::atomic<std::uint64_t> saveProgressBytes_{0};
    std::atomic<bool> saveInProgress_{false};
    std::thread indexThread_;
    std::thread operationThread_;
    mutable std::mutex operationMutex_;
    std::atomic<bool> operationCancel_{false};
    bool operationInProgress_ = false;
    bool operationCompleted_ = false;
    bool operationSucceeded_ = false;
    std::wstring operationName_;
    std::wstring operationError_;
    std::wstring operationTargetPath_;
    classic_notepad::SaveDiagnostics saveDiagnostics_;
    std::uint64_t operationGeneration_ = 0;
    std::uint64_t operationToken_ = 0;
    std::unique_ptr<OperationCompletion> pendingOperationCompletion_;
    std::uint64_t operationProgress_ = 0;
    std::uint64_t operationResultCount_ = 0;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> indexingComplete_{false};
    std::atomic<bool> sourceReady_{false};
    std::atomic<bool> encodingValid_{true};
    std::atomic<std::uint64_t> indexedBytes_{0};
    std::atomic<std::uint64_t> firstUsableMilliseconds_{0};
    std::atomic<std::uint64_t> firstViewportPaintMilliseconds_{0};
    std::atomic<std::uint64_t> fullIndexMilliseconds_{0};
    std::atomic<std::uint64_t> crlfCount_{0};
    std::atomic<std::uint64_t> lfCount_{0};
    std::atomic<std::uint64_t> crCount_{0};
    std::atomic<std::uint64_t> characterCount_{0};
    std::atomic<std::uint64_t> maximumLineColumns_{0};
};
