#pragma once

#include "large_file_index.h"
#include "large_document.h"
#include "encoding.h"
#include "line_endings.h"

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
    std::uint64_t viewportGeneration = 0;
    bool wordWrap = false;
    std::uint64_t viewportVisualRows = 0;
    std::uint64_t wrappedContinuationRows = 0;
    std::uint64_t spellingIssueCount = 0;
    std::uint64_t maximumLineColumns = 0;
    int horizontalOffset = 0;
    int horizontalMaximumOffset = 0;
    RECT caretRectangle{};
    bool operationInProgress = false;
    bool operationCompleted = false;
    bool operationSucceeded = false;
    std::wstring operationName;
    std::wstring operationError;
    std::wstring operationTargetPath;
    std::uint64_t operationGeneration = 0;
    std::uint64_t operationProgress = 0;
    std::uint64_t operationResultCount = 0;
    classic_notepad::LargeFileEncoding encoding = classic_notepad::LargeFileEncoding::Utf8;
};

struct LargeFileSpellingRange {
    std::uint64_t start = 0;
    std::uint64_t length = 0;
};

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
        std::wstring& errorMessage);
    HWND Window() const;
    void SetBounds(const RECT& bounds);
    void SetTheme(HFONT font, bool darkMode);
    void SetWordWrap(bool enabled);
    bool WordWrap() const;
    bool GetVisibleText(std::wstring& text, std::uint64_t& rangeStart, std::wstring& errorMessage);
    void SetSpellingRanges(std::vector<LargeFileSpellingRange> ranges);
    void SetContextMenuHandler(std::function<bool(POINT, std::uint64_t)> handler);
    void Focus();
    void SelectAll();
    bool CopySelection(std::wstring& errorMessage);
    bool CutSelection(std::wstring& errorMessage);
    bool PasteText(const std::wstring& text, std::wstring& errorMessage);
    bool DeleteSelection(bool backspace, std::wstring& errorMessage);
    bool InsertText(const std::wstring& text, std::wstring& errorMessage);
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
    bool SendTextInput(const std::wstring& text);
    bool StartFindAsync(const std::wstring& text, bool matchCase, bool wholeWord, bool forward, std::wstring& errorMessage);
    bool StartReplaceAllAsync(const std::wstring& text, const std::wstring& replacement,
        bool matchCase, bool wholeWord, std::wstring& errorMessage);
    bool StartSaveAsync(const std::wstring& path, classic_notepad::TextEncoding encoding,
        classic_notepad::LineEndingStyle lineEnding, bool rebase, std::wstring& errorMessage);
    void CancelOperation();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool OpenMapping(const std::wstring& path, std::wstring& errorMessage);
    void CloseMapping();
    void StartIndexing();
    void StopIndexing();
    void IndexWorker();
    bool BuildDocumentFromIndex(std::wstring& errorMessage);
    bool AppendIndexedDocument(
        std::uint64_t endByte,
        std::uint64_t endCharacter,
        std::uint64_t totalLineBreaks,
        std::wstring& errorMessage);
    bool EnsureEditable(std::wstring& errorMessage) const;
    bool ReplaceSelection(const std::wstring& text, std::wstring& errorMessage);
    void MarkModified(bool invalidateAll = true);
    void InvalidateDocumentLine(std::uint64_t line);
    bool WriteStreamToTemporaryFile(
        const classic_notepad::LargeDocument& source,
        const std::wstring& temporaryPath,
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
        std::wstring text;
        bool available = false;
    };
    void RebuildViewportCache(std::uint64_t rowCount);
    void RefreshCachedViewportLine(
        std::uint64_t line,
        std::uint64_t removedCharacters,
        std::uint64_t insertedCharacters);
    void InvalidateViewportCache();
    void NotifyStateChanged();
    void UpdateCaret();
    void DestroyTextCaret();
    bool HitTest(POINT point, std::uint64_t& position, int* visualX = nullptr);
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
    std::vector<LargeFileSpellingRange> spellingRanges_;
    std::function<bool(POINT, std::uint64_t)> contextMenuHandler_;
    std::uint64_t startedTick_ = 0;
    std::uint64_t appendedByte_ = 0;
    std::uint64_t appendedCharacter_ = 0;
    std::uint64_t appendedLineBreaks_ = 0;

    classic_notepad::LargeFileEncoding encoding_ = classic_notepad::LargeFileEncoding::Utf8;
    mutable std::mutex lineMutex_;
    std::vector<std::uint64_t> lineOffsets_;
    std::vector<std::uint64_t> lineCharacterOffsets_;
    std::unique_ptr<classic_notepad::LargeDocument> document_;
    mutable std::mutex documentMutex_;
    std::uint64_t selectionStart_ = 0;
    std::uint64_t selectionEnd_ = 0;
    std::uint64_t selectionAnchor_ = 0;
    std::uint64_t caretPosition_ = 0;
    bool focused_ = false;
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
    std::uint64_t operationGeneration_ = 0;
    std::uint64_t operationProgress_ = 0;
    std::uint64_t operationResultCount_ = 0;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> indexingComplete_{false};
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
