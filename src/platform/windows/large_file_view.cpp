#include "large_file_view.h"
#include "resource.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <string>
#include <windowsx.h>

namespace {

constexpr wchar_t kLargeFileViewClass[] = L"ClassicNotepadLargeFileView";
constexpr UINT_PTR kMouseScrollTimer = 42;
constexpr UINT kMouseScrollTimerMs = 35;
constexpr UINT_PTR kStateNotificationTimer = 43;
constexpr UINT kStateNotificationDelayMs = 16;
constexpr std::size_t kIndexChunkBytes = 8U * 1024U * 1024U;
constexpr std::size_t kRenderedLineByteLimit = 64U * 1024U;
constexpr std::size_t kCopiedVisibleTextLimit = 1024U * 1024U;
constexpr std::uint64_t kClipboardCharacterLimit = 16ULL * 1024ULL * 1024ULL;
constexpr UINT kLargeFileStateChangedMessage = WM_APP + 101;
constexpr UINT kLargeFileWorkerProgressMessage = WM_APP + 102;

std::uint64_t ElapsedMilliseconds(std::uint64_t startedTick)
{
    return GetTickCount64() - startedTick;
}

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right)
{
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    return right > maximum - left ? maximum : left + right;
}

void StoreMaximum(std::atomic<std::uint64_t>& destination, std::uint64_t candidate)
{
    std::uint64_t current = destination.load();
    while (current < candidate && !destination.compare_exchange_weak(current, candidate)) {}
}

std::uint64_t VisualColumns(const std::wstring& text)
{
    std::uint64_t maximum = 0;
    std::uint64_t current = 0;
    for (wchar_t character : text) {
        if (character == L'\r' || character == L'\n') {
            maximum = std::max(maximum, current);
            current = 0;
        } else {
            const std::uint64_t advance = character == L'\t' ? 8U - (current % 8U) : 1U;
            current = SaturatingAdd(current, advance);
        }
    }
    return std::max(maximum, current);
}

std::uint64_t MinFileSize(std::uint64_t fileSize, std::uint64_t offset, std::size_t requested)
{
    if (offset >= fileSize) {
        return 0;
    }
    const std::uint64_t remaining = fileSize - offset;
    return std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(requested));
}

bool IsHighSurrogateUnit(wchar_t value) { return value >= 0xD800 && value <= 0xDBFF; }
bool IsLowSurrogateUnit(wchar_t value) { return value >= 0xDC00 && value <= 0xDFFF; }

struct TextLayout {
    std::wstring displayText;
    std::vector<std::size_t> displayBoundary;
    std::vector<int> xBoundary;
};

TextLayout LayoutText(HDC dc, const std::wstring& text)
{
    constexpr std::size_t tabColumns = 8U;
    TextLayout result;
    result.displayBoundary.resize(text.size() + 1U);
    result.xBoundary.resize(text.size() + 1U);
    result.displayText.reserve(text.size());
    std::size_t column = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        result.displayBoundary[index] = result.displayText.size();
        if (text[index] == L'\t') {
            const std::size_t spaces = tabColumns - (column % tabColumns);
            result.displayText.append(spaces, L' ');
            column += spaces;
        } else {
            result.displayText.push_back(text[index]);
            ++column;
        }
    }
    result.displayBoundary[text.size()] = result.displayText.size();
    if (result.displayText.empty()) return result;
    std::vector<int> displayExtents(result.displayText.size());
    SIZE extent{};
    GetTextExtentExPointW(dc, result.displayText.data(), static_cast<int>(result.displayText.size()),
        (std::numeric_limits<int>::max)(), nullptr, displayExtents.data(), &extent);
    for (std::size_t index = 1; index < result.xBoundary.size(); ++index) {
        const std::size_t display = result.displayBoundary[index];
        result.xBoundary[index] = display == 0U ? 0 : displayExtents[display - 1U];
    }
    return result;
}

std::size_t ClosestTextBoundary(const TextLayout& layout, int wanted)
{
    const auto found = std::lower_bound(layout.xBoundary.begin(), layout.xBoundary.end(), wanted);
    if (found == layout.xBoundary.begin()) return 0U;
    if (found == layout.xBoundary.end()) return layout.xBoundary.size() - 1U;
    const std::size_t right = static_cast<std::size_t>(found - layout.xBoundary.begin());
    const int leftDistance = wanted - layout.xBoundary[right - 1U];
    const int rightDistance = layout.xBoundary[right] - wanted;
    return leftDistance > rightDistance ? right : right - 1U;
}

bool AtomicReplacePath(const std::wstring& temporaryPath, const std::wstring& destinationPath)
{
    constexpr int attempts = 100;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const DWORD attributes = GetFileAttributesW(destinationPath.c_str());
        const bool replaced = attributes != INVALID_FILE_ATTRIBUTES
            ? ReplaceFileW(destinationPath.c_str(), temporaryPath.c_str(), nullptr,
                REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE
            : MoveFileExW(temporaryPath.c_str(), destinationPath.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
        if (replaced) return true;
        const DWORD error = GetLastError();
        const bool transient = error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED ||
            error == ERROR_UNABLE_TO_REMOVE_REPLACED || error == ERROR_UNABLE_TO_MOVE_REPLACEMENT ||
            error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2;
        if (!transient || attempt + 1 == attempts) {
            SetLastError(error);
            return false;
        }
        Sleep(50);
    }
    return false;
}

} // namespace

LargeFileView::LargeFileView() = default;

LargeFileView::~LargeFileView()
{
    StopOperation();
    StopIndexing();
    if (window_ != nullptr && IsWindow(window_)) {
        DestroyWindow(window_);
    }
    CloseMapping();
}

void LargeFileView::StopOperation()
{
    operationCancel_.store(true);
    if (operationThread_.joinable()) operationThread_.join();
    std::lock_guard<std::mutex> lock(operationMutex_);
    operationInProgress_ = false;
}

void LargeFileView::CancelOperation()
{
    operationCancel_.store(true);
}

bool LargeFileView::StartFindAsync(
    const std::wstring& text, bool matchCase, bool wholeWord, bool forward, std::wstring& errorMessage)
{
    if (text.empty()) { errorMessage = L"The search text is empty."; return false; }
    if (operationThread_.joinable()) {
        std::lock_guard<std::mutex> lock(operationMutex_);
        if (operationInProgress_) { errorMessage = L"Another large-document operation is already running."; return false; }
        operationThread_.join();
    }
    std::unique_ptr<classic_notepad::LargeDocument> snapshot;
    std::uint64_t generation = 0;
    std::uint64_t start = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureEditable(errorMessage)) return false;
        snapshot = document_->Clone();
        generation = document_->Generation();
        start = forward ? selectionEnd_ : selectionStart_;
    }
    {
        std::lock_guard<std::mutex> lock(operationMutex_);
        operationInProgress_ = true; operationCompleted_ = false; operationSucceeded_ = false;
        operationName_ = L"Find"; operationError_.clear(); operationGeneration_ = generation;
        operationTargetPath_.clear();
        operationProgress_ = 0; operationResultCount_ = 0;
    }
    operationCancel_.store(false);
    operationThread_ = std::thread([this, snapshot = std::move(snapshot), text, matchCase, wholeWord, forward, start, generation]() mutable {
        classic_notepad::LargeDocumentMatch match;
        std::wstring error;
        const bool found = snapshot->Find(text, start, matchCase, wholeWord, forward, match, error,
            [this] { return operationCancel_.load(); },
            [this](std::uint64_t progress) {
                std::lock_guard<std::mutex> lock(operationMutex_);
                operationProgress_ = progress;
            });
        bool stale = false;
        if (!operationCancel_.load() && error.empty() && found) {
            std::lock_guard<std::mutex> lock(documentMutex_);
            if (document_ == nullptr || document_->Generation() != generation) stale = true;
            else {
                selectionStart_ = match.start; selectionEnd_ = match.start + match.length;
                selectionAnchor_ = selectionStart_; caretPosition_ = selectionEnd_;
            }
        }
        {
            std::lock_guard<std::mutex> lock(operationMutex_);
            operationInProgress_ = false; operationCompleted_ = true;
            operationSucceeded_ = !operationCancel_.load() && error.empty() && !stale;
            operationResultCount_ = found && !stale ? 1U : 0U;
            operationProgress_ = found ? match.start + match.length : snapshot->CharacterCount();
            operationError_ = stale ? L"The document changed before the search result completed." :
                (operationCancel_.load() ? L"The operation was cancelled." : error);
        }
        NotifyStateChanged();
    });
    return true;
}

bool LargeFileView::StartReplaceAllAsync(
    const std::wstring& text, const std::wstring& replacement,
    bool matchCase, bool wholeWord, std::wstring& errorMessage)
{
    if (text.empty()) { errorMessage = L"The replacement text is empty."; return false; }
    if (operationThread_.joinable()) {
        std::lock_guard<std::mutex> lock(operationMutex_);
        if (operationInProgress_) { errorMessage = L"Another large-document operation is already running."; return false; }
        operationThread_.join();
    }
    std::unique_ptr<classic_notepad::LargeDocument> snapshot;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureEditable(errorMessage)) return false;
        snapshot = document_->Clone(); generation = document_->Generation();
    }
    {
        std::lock_guard<std::mutex> lock(operationMutex_);
        operationInProgress_ = true; operationCompleted_ = false; operationSucceeded_ = false;
        operationName_ = L"Replace All"; operationError_.clear(); operationGeneration_ = generation;
        operationTargetPath_.clear();
        operationProgress_ = 0; operationResultCount_ = 0;
    }
    operationCancel_.store(false);
    const std::wstring normalized = classic_notepad::NormalizeLineEndingsForEditor(replacement);
    StoreMaximum(maximumLineColumns_, VisualColumns(normalized));
    operationThread_ = std::thread([this, snapshot = std::move(snapshot), text, normalized, matchCase, wholeWord, generation]() mutable {
        bool cancelled = false; std::wstring error;
        const std::uint64_t count = snapshot->ReplaceAll(text, normalized, matchCase, wholeWord,
            cancelled, [this] { return operationCancel_.load(); }, error,
            [this](std::uint64_t progress) {
                std::lock_guard<std::mutex> lock(operationMutex_);
                operationProgress_ = progress;
            });
        bool stale = false;
        if (!cancelled && error.empty()) {
            std::lock_guard<std::mutex> lock(documentMutex_);
            if (document_ == nullptr || document_->Generation() != generation) stale = true;
            else if (count != 0U) {
                document_ = std::move(snapshot);
                selectionStart_ = selectionEnd_ = selectionAnchor_ = caretPosition_ = 0;
                modified_.store(document_->IsModified());
            }
        }
        {
            std::lock_guard<std::mutex> lock(operationMutex_);
            operationInProgress_ = false; operationCompleted_ = true;
            operationSucceeded_ = !cancelled && error.empty() && !stale;
            operationResultCount_ = count; operationProgress_ = fileSize_;
            operationError_ = stale ? L"The document changed before Replace All completed." :
                (cancelled ? L"The operation was cancelled." : error);
        }
        NotifyStateChanged();
    });
    return true;
}

bool LargeFileView::StartSaveAsync(
    const std::wstring& path, classic_notepad::TextEncoding encoding,
    classic_notepad::LineEndingStyle lineEnding, bool rebase, std::wstring& errorMessage)
{
    if (path.empty()) { errorMessage = L"No file path has been selected."; return false; }
    if (operationThread_.joinable()) {
        std::lock_guard<std::mutex> lock(operationMutex_);
        if (operationInProgress_) { errorMessage = L"Another large-document operation is already running."; return false; }
        operationThread_.join();
    }
    std::unique_ptr<classic_notepad::LargeDocument> snapshot;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureEditable(errorMessage)) return false;
        snapshot = document_->Clone(); generation = document_->Generation();
    }
    {
        std::lock_guard<std::mutex> lock(operationMutex_);
        operationInProgress_ = true; operationCompleted_ = false; operationSucceeded_ = false;
        operationName_ = L"Save"; operationError_.clear(); operationGeneration_ = generation;
        operationTargetPath_ = path;
        operationProgress_ = 0; operationResultCount_ = 0;
    }
    operationCancel_.store(false); saveInProgress_.store(true); saveProgressBytes_.store(0);
    const std::wstring temporaryPath = path + L".classic-notepad-save-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
    operationThread_ = std::thread([this, snapshot = std::move(snapshot), path, temporaryPath,
        encoding, lineEnding, rebase, generation]() mutable {
        std::wstring error;
        bool okay = WriteStreamToTemporaryFile(*snapshot, temporaryPath, encoding, lineEnding, error);
        const bool cancelled = operationCancel_.load();
        bool stale = false;
        if (okay && !cancelled) {
            {
                std::lock_guard<std::mutex> lock(documentMutex_);
                stale = document_ == nullptr || document_->Generation() != generation;
            }
            if (stale || !rebase) {
                okay = AtomicReplacePath(temporaryPath, path);
                if (!okay) error = L"The complete temporary save could not replace the destination.";
            } else {
                const std::wstring previousPath = path_;
                const std::uint64_t savedTopLine = topLine_;
                const std::uint64_t savedSelectionStart = selectionStart_;
                const std::uint64_t savedSelectionEnd = selectionEnd_;
                StopIndexing();
                {
                    std::lock_guard<std::mutex> lock(documentMutex_);
                    CloseMapping();
                    okay = AtomicReplacePath(temporaryPath, path);
                    if (okay) {
                        document_.reset();
                        selectionStart_ = savedSelectionStart;
                        selectionEnd_ = savedSelectionEnd;
                        selectionAnchor_ = savedSelectionStart;
                        caretPosition_ = savedSelectionEnd;
                        modified_.store(false);
                    }
                }
                if (okay) {
                    okay = OpenMapping(path, error);
                    if (okay) {
                        topLine_ = savedTopLine; startedTick_ = GetTickCount64(); StartIndexing(); path_ = path;
                    }
                } else {
                    error = L"The complete temporary save could not replace the destination.";
                    std::wstring ignored; OpenMapping(previousPath, ignored); StartIndexing();
                }
            }
        }
        if (!okay || cancelled) DeleteFileW(temporaryPath.c_str());
        saveInProgress_.store(false);
        {
            std::lock_guard<std::mutex> lock(operationMutex_);
            operationInProgress_ = false; operationCompleted_ = true;
            operationSucceeded_ = okay && !cancelled;
            operationProgress_ = saveProgressBytes_.load();
            operationResultCount_ = okay && !cancelled ? 1U : 0U;
            operationError_ = cancelled ? L"The operation was cancelled." : error;
        }
        NotifyStateChanged();
    });
    return true;
}

bool LargeFileView::ShouldUseLargeFile(const std::wstring& path, std::uint64_t& fileSize, std::wstring& errorMessage)
{
    errorMessage.clear();
    fileSize = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errorMessage = L"The file could not be opened.";
        return false;
    }
    LARGE_INTEGER size{};
    const bool succeeded = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
    CloseHandle(file);
    if (!succeeded) {
        errorMessage = L"The file size could not be determined.";
        return false;
    }
    fileSize = static_cast<std::uint64_t>(size.QuadPart);
    return fileSize >= kActivationThresholdBytes;
}

bool LargeFileView::RegisterClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    windowClass.lpszClassName = kLargeFileViewClass;
    return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool LargeFileView::Create(
    HINSTANCE instance,
    HWND parent,
    int controlId,
    const std::wstring& path,
    HFONT font,
    bool darkMode,
    std::wstring& errorMessage)
{
    instance_ = instance;
    parent_ = parent;
    font_ = font;
    darkMode_ = darkMode;
    if (!RegisterClass(instance_) || !OpenMapping(path, errorMessage)) {
        return false;
    }

    window_ = CreateWindowExW(
        darkMode_ ? 0 : WS_EX_CLIENTEDGE,
        kLargeFileViewClass,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
        0, 0, 0, 0,
        parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
        instance_,
        this);
    if (window_ == nullptr) {
        errorMessage = L"The large-file viewer window could not be created.";
        CloseMapping();
        return false;
    }
    if (font_ != nullptr) {
        SendMessageW(window_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    }
    startedTick_ = GetTickCount64();
    StartIndexing();
    RefreshScrollBars();
    return true;
}

HWND LargeFileView::Window() const { return window_; }

void LargeFileView::SetBounds(const RECT& bounds)
{
    if (window_ != nullptr) {
        InvalidateViewportCache();
        MoveWindow(window_, bounds.left, bounds.top, std::max(0L, bounds.right - bounds.left),
            std::max(0L, bounds.bottom - bounds.top), TRUE);
        UpdateWindow(window_);
    }
}

void LargeFileView::SetTheme(HFONT font, bool darkMode)
{
    font_ = font;
    darkMode_ = darkMode;
    InvalidateViewportCache();
    if (window_ != nullptr) {
        SendMessageW(window_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        const LONG_PTR style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        const LONG_PTR wanted = darkMode_ ? (style & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE))
                                         : (style | static_cast<LONG_PTR>(WS_EX_CLIENTEDGE));
        if (style != wanted) {
            SetWindowLongPtrW(window_, GWL_EXSTYLE, wanted);
            SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        RefreshScrollBars();
        InvalidateRect(window_, nullptr, TRUE);
    }
}

void LargeFileView::SetWordWrap(bool enabled)
{
    if (wordWrap_ == enabled) return;
    wordWrap_ = enabled;
    horizontalOffset_ = 0;
    topWrapOffset_ = 0;
    InvalidateViewportCache();
    if (window_ != nullptr) {
        ShowScrollBar(window_, SB_HORZ, enabled ? FALSE : TRUE);
        RefreshScrollBars();
        EnsureCaretVisible();
        InvalidateRect(window_, nullptr, FALSE);
        UpdateCaret();
        NotifyStateChanged();
    }
}

bool LargeFileView::WordWrap() const { return wordWrap_; }

bool LargeFileView::GetVisibleText(
    std::wstring& text,
    std::uint64_t& rangeStart,
    std::wstring& errorMessage)
{
    text.clear();
    rangeStart = 0;
    errorMessage.clear();
    RebuildViewportCache(VisibleLineCount() + 4U);
    if (viewportRows_.empty()) return false;
    rangeStart = viewportRows_.front().segmentStart;
    const CachedViewportRow& last = viewportRows_.back();
    std::uint64_t rangeEnd = last.segmentEnd;
    if (last.segmentEnd == last.contentEnd) rangeEnd = last.nextLineStart;
    if (rangeEnd < rangeStart) return false;
    constexpr std::uint64_t spellLimit = 96U * 1024U;
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (document_ == nullptr) return false;
    return document_->ReadRange(rangeStart, std::min(rangeEnd - rangeStart, spellLimit), text, errorMessage) &&
        !text.empty();
}

void LargeFileView::SetSpellingRanges(std::vector<LargeFileSpellingRange> ranges)
{
    spellingRanges_ = std::move(ranges);
    if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
}

void LargeFileView::SetContextMenuHandler(std::function<bool(POINT, std::uint64_t)> handler)
{
    contextMenuHandler_ = std::move(handler);
}

void LargeFileView::Focus()
{
    if (window_ != nullptr) {
        SetFocus(window_);
    }
}

void LargeFileView::SelectAll()
{
    std::wstring error;
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (EnsureEditable(error)) {
        selectionStart_ = 0;
        selectionEnd_ = document_->CharacterCount();
        selectionAnchor_ = 0;
        caretPosition_ = selectionEnd_;
        allSelected_ = true;
    }
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

bool LargeFileView::OpenMapping(const std::wstring& path, std::wstring& errorMessage)
{
    InvalidateViewportCache();
    errorMessage.clear();
    path_ = path;
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        errorMessage = L"The large file could not be opened for mapped editing.";
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_, &size) || size.QuadPart < 0) {
        errorMessage = L"The large file size could not be determined.";
        CloseMapping();
        return false;
    }
    fileSize_ = static_cast<std::uint64_t>(size.QuadPart);
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_ == nullptr) {
        errorMessage = L"The large file could not be memory mapped.";
        CloseMapping();
        return false;
    }
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    allocationGranularity_ = std::max<DWORD>(1, systemInfo.dwAllocationGranularity);

    std::vector<std::uint8_t> prefix;
    if (!CopyBytes(0, static_cast<std::size_t>(std::min<std::uint64_t>(fileSize_, 4U)), prefix)) {
        errorMessage = L"The large file header could not be read.";
        CloseMapping();
        return false;
    }
    const classic_notepad::LargeFileEncodingInfo detected =
        classic_notepad::DetectLargeFileEncoding(prefix.data(), prefix.size());
    if (detected.encoding == classic_notepad::LargeFileEncoding::Unsupported) {
        errorMessage = L"Large-file mode supports UTF-8/ASCII and UTF-16 LE with BOM, not UTF-16 BE.";
        CloseMapping();
        return false;
    }
    if (detected.dataOffset > fileSize_) {
        errorMessage = L"The large file has an incomplete byte-order mark.";
        CloseMapping();
        return false;
    }
    encoding_ = detected.encoding;
    dataOffset_ = detected.dataOffset;
    appendedByte_ = dataOffset_;
    appendedCharacter_ = 0;
    appendedLineBreaks_ = 0;
    {
        std::lock_guard<std::mutex> lock(lineMutex_);
        lineOffsets_.clear();
        lineOffsets_.push_back(dataOffset_);
        lineCharacterOffsets_.clear();
        lineCharacterOffsets_.push_back(0);
    }
    return true;
}

void LargeFileView::CloseMapping()
{
    InvalidateViewportCache();
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}

void LargeFileView::StartIndexing()
{
    cancel_.store(false);
    indexingComplete_.store(false);
    encodingValid_.store(true);
    indexedBytes_.store(dataOffset_);
    saveProgressBytes_.store(0);
    saveInProgress_.store(false);
    maximumLineColumns_.store(0);
    horizontalOffset_ = 0;
    horizontalMaximumOffset_ = 0;
    indexThread_ = std::thread(&LargeFileView::IndexWorker, this);
}

void LargeFileView::StopIndexing()
{
    cancel_.store(true);
    if (indexThread_.joinable()) {
        indexThread_.join();
    }
}

bool LargeFileView::CopyBytes(std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) const
{
    bytes.clear();
    if (mapping_ == nullptr || offset > fileSize_ ||
        static_cast<std::uint64_t>(length) > fileSize_ - offset) {
        return false;
    }
    if (length == 0U) {
        return true;
    }
    const std::uint64_t alignedOffset = offset - (offset % allocationGranularity_);
    const std::uint64_t delta64 = offset - alignedOffset;
    if (delta64 > static_cast<std::uint64_t>((std::numeric_limits<SIZE_T>::max)()) ||
        length > static_cast<std::size_t>((std::numeric_limits<SIZE_T>::max)() - static_cast<SIZE_T>(delta64))) {
        return false;
    }
    const SIZE_T viewLength = static_cast<SIZE_T>(delta64) + static_cast<SIZE_T>(length);
    const DWORD high = static_cast<DWORD>(alignedOffset >> 32U);
    const DWORD low = static_cast<DWORD>(alignedOffset & 0xFFFFFFFFULL);
    const auto* view = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, high, low, viewLength));
    if (view == nullptr) {
        return false;
    }
    bytes.assign(view + static_cast<SIZE_T>(delta64), view + static_cast<SIZE_T>(delta64) + length);
    UnmapViewOfFile(view);
    return true;
}

void LargeFileView::IndexWorker()
{
    classic_notepad::LargeFileLineScanner scanner(encoding_, dataOffset_);
    std::vector<std::uint64_t> batch;
    std::vector<std::uint64_t> batchCharacterOffsets;
    std::uint64_t offset = dataOffset_;
    while (!cancel_.load() && offset < fileSize_) {
        const std::uint64_t chunk64 = MinFileSize(fileSize_, offset, kIndexChunkBytes);
        if (chunk64 == 0U || chunk64 > static_cast<std::uint64_t>((std::numeric_limits<SIZE_T>::max)())) {
            encodingValid_.store(false);
            break;
        }
        const std::uint64_t alignedOffset = offset - (offset % allocationGranularity_);
        const std::uint64_t delta64 = offset - alignedOffset;
        if (delta64 > static_cast<std::uint64_t>((std::numeric_limits<SIZE_T>::max)()) ||
            chunk64 > static_cast<std::uint64_t>((std::numeric_limits<SIZE_T>::max)() - static_cast<SIZE_T>(delta64))) {
            encodingValid_.store(false);
            break;
        }
        const DWORD high = static_cast<DWORD>(alignedOffset >> 32U);
        const DWORD low = static_cast<DWORD>(alignedOffset & 0xFFFFFFFFULL);
        const SIZE_T viewLength = static_cast<SIZE_T>(delta64) + static_cast<SIZE_T>(chunk64);
        const auto* view = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, high, low, viewLength));
        if (view == nullptr) {
            encodingValid_.store(false);
            break;
        }
        const bool consumed = scanner.Consume(
            view + static_cast<SIZE_T>(delta64),
            static_cast<std::size_t>(chunk64),
            offset,
            batch,
            &batchCharacterOffsets);
        UnmapViewOfFile(view);
        if (!consumed) {
            encodingValid_.store(false);
            break;
        }
        maximumLineColumns_.store(scanner.MaximumLineColumns());
        offset = SaturatingAdd(offset, chunk64);
        indexedBytes_.store(offset);
        if (!batch.empty()) {
            std::uint64_t safeByte = 0;
            std::uint64_t safeCharacter = 0;
            std::uint64_t safeBreaks = 0;
            {
                std::lock_guard<std::mutex> lock(lineMutex_);
                lineOffsets_.insert(lineOffsets_.end(), batch.begin(), batch.end());
                lineCharacterOffsets_.insert(
                    lineCharacterOffsets_.end(), batchCharacterOffsets.begin(), batchCharacterOffsets.end());
                batch.clear();
                batchCharacterOffsets.clear();
                safeByte = lineOffsets_.back();
                safeCharacter = lineCharacterOffsets_.back();
                safeBreaks = static_cast<std::uint64_t>(lineOffsets_.size() - 1U);
            }
            std::wstring appendError;
            if (!AppendIndexedDocument(safeByte, safeCharacter, safeBreaks, appendError)) encodingValid_.store(false);
        }
        NotifyStateChanged();
    }

    if (!cancel_.load() && encodingValid_.load() && scanner.Finish(batch, &batchCharacterOffsets)) {
        if (!batch.empty()) {
            std::lock_guard<std::mutex> lock(lineMutex_);
            lineOffsets_.insert(lineOffsets_.end(), batch.begin(), batch.end());
            lineCharacterOffsets_.insert(
                lineCharacterOffsets_.end(), batchCharacterOffsets.begin(), batchCharacterOffsets.end());
        }
        if (firstUsableMilliseconds_.load() == 0U) {
            firstUsableMilliseconds_.store(ElapsedMilliseconds(startedTick_));
        }
        indexedBytes_.store(fileSize_);
        crlfCount_.store(scanner.CrlfCount());
        lfCount_.store(scanner.LfCount());
        crCount_.store(scanner.CrCount());
        characterCount_.store(scanner.CharacterCount());
        maximumLineColumns_.store(scanner.MaximumLineColumns());
        {
            std::wstring ignored;
            const std::uint64_t breaks = scanner.CrlfCount() + scanner.LfCount() + scanner.CrCount();
            if (!AppendIndexedDocument(fileSize_, scanner.CharacterCount(), breaks, ignored)) encodingValid_.store(false);
        }
        indexingComplete_.store(true);
        if (firstUsableMilliseconds_.load() == 0U) firstUsableMilliseconds_.store(ElapsedMilliseconds(startedTick_));
        fullIndexMilliseconds_.store(ElapsedMilliseconds(startedTick_));
        NotifyStateChanged();
    } else if (!cancel_.load()) {
        encodingValid_.store(false);
        fullIndexMilliseconds_.store(ElapsedMilliseconds(startedTick_));
        NotifyStateChanged();
    }
}

bool LargeFileView::BuildDocumentFromIndex(std::wstring& errorMessage)
{
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> characters;
    {
        std::lock_guard<std::mutex> lock(lineMutex_);
        offsets = lineOffsets_;
        characters = lineCharacterOffsets_;
    }
    if (offsets.empty() || offsets.size() != characters.size()) {
        errorMessage = L"The large-file line index is incomplete.";
        return false;
    }
    std::vector<classic_notepad::LargeDocumentOriginalPiece> pieces;
    std::uint64_t startByte = dataOffset_;
    std::uint64_t startCharacter = 0;
    constexpr std::uint64_t targetPieceBytes = 64ULL * 1024ULL;
    auto boundaryCharacter = [this](std::uint64_t byteOffset) {
        std::vector<std::uint8_t> bytes;
        if (encoding_ == classic_notepad::LargeFileEncoding::Utf16Le) {
            if (!CopyBytes(byteOffset, 2U, bytes) || bytes.size() != 2U) return wchar_t(0);
            const wchar_t value = static_cast<wchar_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8U));
            return value == L'\r' || value == L'\n' ? value : wchar_t(1);
        }
        if (!CopyBytes(byteOffset, 1U, bytes) || bytes.empty()) return wchar_t(0);
        return bytes[0] == '\r' || bytes[0] == '\n' ? static_cast<wchar_t>(bytes[0]) : wchar_t(1);
    };
    while (startByte < fileSize_) {
        const std::uint64_t target = SaturatingAdd(startByte, targetPieceBytes);
        auto iterator = std::lower_bound(offsets.begin(), offsets.end(), target);
        std::uint64_t endByte = 0;
        std::uint64_t endCharacter = 0;
        if (iterator == offsets.end()) {
            endByte = fileSize_;
            endCharacter = characterCount_.load();
        } else {
            const std::size_t index = static_cast<std::size_t>(iterator - offsets.begin());
            endByte = *iterator;
            endCharacter = characters[index];
        }
        if (endByte <= startByte) {
            endByte = fileSize_;
            endCharacter = characterCount_.load();
        }
        if (endByte > fileSize_) {
            errorMessage = L"The large-file index contains an invalid byte offset.";
            return false;
        }
        const auto startIterator = std::lower_bound(offsets.begin(), offsets.end(), startByte);
        const std::size_t startIndex = static_cast<std::size_t>(startIterator - offsets.begin());
        const auto endIterator = std::lower_bound(offsets.begin(), offsets.end(), endByte);
        const std::size_t endIndex = endByte == fileSize_ ? offsets.size() - 1U :
            static_cast<std::size_t>(endIterator - offsets.begin());
        const std::uint64_t breaks = static_cast<std::uint64_t>(endIndex - startIndex);
        const std::uint64_t unitBytes = encoding_ == classic_notepad::LargeFileEncoding::Utf16Le ? 2U : 1U;
        pieces.push_back({ startByte, endByte - startByte, endCharacter - startCharacter,
            breaks, boundaryCharacter(startByte), boundaryCharacter(endByte - unitBytes), true });
        startByte = endByte;
        startCharacter = endCharacter;
    }
    if (pieces.empty() && fileSize_ == dataOffset_) {
        // An empty mapped file is a valid editable document.
    } else if (startCharacter != characterCount_.load()) {
        errorMessage = L"The large-file character index is incomplete.";
        return false;
    }

    auto model = std::make_unique<classic_notepad::LargeDocument>(
        encoding_,
        [this](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
            return CopyBytes(offset, length, bytes);
        });
    if (!model->Initialize(pieces, errorMessage)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(documentMutex_);
    document_ = std::move(model);
    selectionStart_ = std::min(selectionStart_, document_->CharacterCount());
    selectionEnd_ = std::min(selectionEnd_, document_->CharacterCount());
    selectionAnchor_ = std::min(selectionAnchor_, document_->CharacterCount());
    caretPosition_ = std::min(caretPosition_, document_->CharacterCount());
    return true;
}

bool LargeFileView::AppendIndexedDocument(
    std::uint64_t endByte,
    std::uint64_t endCharacter,
    std::uint64_t totalLineBreaks,
    std::wstring& errorMessage)
{
    if (endByte < appendedByte_ || endCharacter < appendedCharacter_ || totalLineBreaks < appendedLineBreaks_) {
        errorMessage = L"The incremental large-document index moved backwards.";
        return false;
    }
    if (endByte == appendedByte_) return true;
    // Keep source pieces small enough that locating a line within one piece is
    // bounded.  The scanner still maps 8 MiB at a time; these are lightweight
    // tree descriptors cut only at known logical-line boundaries.
    constexpr std::uint64_t targetPieceBytes = 64ULL * 1024ULL;
    struct Boundary { std::uint64_t byte; std::uint64_t character; std::uint64_t breaks; };
    std::vector<Boundary> boundaries;
    {
        std::lock_guard<std::mutex> lineLock(lineMutex_);
        std::uint64_t cursor = appendedByte_;
        while (cursor < endByte) {
            const std::uint64_t target = SaturatingAdd(cursor, targetPieceBytes);
            auto iterator = std::lower_bound(lineOffsets_.begin(), lineOffsets_.end(), target);
            if (iterator == lineOffsets_.end() || *iterator >= endByte) {
                boundaries.push_back({endByte, endCharacter, totalLineBreaks});
                break;
            }
            const std::size_t index = static_cast<std::size_t>(iterator - lineOffsets_.begin());
            boundaries.push_back({*iterator, lineCharacterOffsets_[index], static_cast<std::uint64_t>(index)});
            cursor = *iterator;
        }
    }
    std::vector<std::uint8_t> indexedBytes;
    const std::uint64_t span = endByte - appendedByte_;
    if (span > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
        !CopyBytes(appendedByte_, static_cast<std::size_t>(span), indexedBytes)) {
        errorMessage = L"The indexed source range could not be mapped.";
        return false;
    }
    const auto boundaryCharacter = [&](std::uint64_t byteOffset) {
        if (byteOffset < appendedByte_ || byteOffset >= endByte) return wchar_t(0);
        const std::size_t relative = static_cast<std::size_t>(byteOffset - appendedByte_);
        wchar_t value = 1;
        if (encoding_ == classic_notepad::LargeFileEncoding::Utf16Le) {
            if (relative + 1U >= indexedBytes.size()) return wchar_t(0);
            value = static_cast<wchar_t>(indexedBytes[relative] |
                (static_cast<std::uint16_t>(indexedBytes[relative + 1U]) << 8U));
        } else {
            value = static_cast<wchar_t>(indexedBytes[relative]);
        }
        return value == L'\r' || value == L'\n' ? value : wchar_t(1);
    };
    const std::uint64_t unitBytes = encoding_ == classic_notepad::LargeFileEncoding::Utf16Le ? 2U : 1U;
    std::vector<classic_notepad::LargeDocumentOriginalPiece> descriptors;
    descriptors.reserve(boundaries.size());
    std::uint64_t pieceByte = appendedByte_;
    std::uint64_t pieceCharacter = appendedCharacter_;
    std::uint64_t pieceBreaks = appendedLineBreaks_;
    for (const Boundary& boundary : boundaries) {
        descriptors.push_back({pieceByte, boundary.byte - pieceByte, boundary.character - pieceCharacter,
            boundary.breaks - pieceBreaks, boundaryCharacter(pieceByte),
            boundaryCharacter(boundary.byte - unitBytes), true});
        pieceByte = boundary.byte;
        pieceCharacter = boundary.character;
        pieceBreaks = boundary.breaks;
    }
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (document_ == nullptr) {
        auto model = std::make_unique<classic_notepad::LargeDocument>(
            encoding_, [this](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
                return CopyBytes(offset, length, bytes);
            });
        if (!model->Initialize(descriptors, errorMessage)) return false;
        document_ = std::move(model);
    } else if (!document_->AppendOriginalPieces(descriptors, errorMessage)) {
        return false;
    }
    appendedByte_ = endByte;
    appendedCharacter_ = endCharacter;
    appendedLineBreaks_ = totalLineBreaks;
    if (firstUsableMilliseconds_.load() == 0U) firstUsableMilliseconds_.store(ElapsedMilliseconds(startedTick_));
    return true;
}

void LargeFileView::NotifyStateChanged()
{
    if (window_ != nullptr && !stateNotificationPending_.exchange(true)) {
        PostMessageW(window_, kLargeFileWorkerProgressMessage, 0, 0);
    }
}

bool LargeFileView::EnsureEditable(std::wstring& errorMessage) const
{
    if (!encodingValid_.load()) {
        errorMessage = L"The large file has invalid or unsupported text encoding.";
        return false;
    }
    if (document_ == nullptr) {
        errorMessage = L"The first editable large-document viewport is still being prepared.";
        return false;
    }
    return true;
}

void LargeFileView::MarkModified(bool invalidateAll)
{
    spellingRanges_.clear();
    // Typing can produce many WM_CHAR messages in one input burst.  Defer the
    // title/status update to a single one-shot notification so those messages
    // cannot starve the editor's paint queue.
    if (window_ != nullptr) {
        SetTimer(window_, kStateNotificationTimer, kStateNotificationDelayMs, nullptr);
    }
    if (invalidateAll) InvalidateRect(window_, nullptr, FALSE);
}

void LargeFileView::InvalidateDocumentLine(std::uint64_t line)
{
    if (wordWrap_) {
        InvalidateViewportCache();
        InvalidateRect(window_, nullptr, FALSE);
        UpdateWindow(window_);
        return;
    }
    if (window_ == nullptr || line < topLine_) return;
    HDC dc = GetDC(window_);
    if (dc == nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    HGDIOBJ oldFont = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, oldFont);
    ReleaseDC(window_, dc);
    const int lineHeight = std::max(1, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
    const std::uint64_t row = line - topLine_;
    if (row >= VisibleLineCount() + 1U || row > static_cast<std::uint64_t>((std::numeric_limits<int>::max)() / lineHeight)) return;
    RECT client{};
    GetClientRect(window_, &client);
    const int top = static_cast<int>(row) * lineHeight;
    RECT dirty{client.left, static_cast<LONG>(top), client.right,
        std::min<LONG>(client.bottom, static_cast<LONG>(top + lineHeight))};
    if (dirty.top < dirty.bottom) {
        InvalidateRect(window_, &dirty, FALSE);
        // Complete the tiny local repaint before accepting the next character.
        // This prevents a burst of partial GDI frames from reaching DWM while
        // still avoiding a document-window redraw for each keystroke.
        UpdateWindow(window_);
    }
}

void LargeFileView::InvalidateViewportCache()
{
    viewportRows_.clear();
    viewportCacheValid_ = false;
}

void LargeFileView::RebuildViewportCache(std::uint64_t rowCount)
{
    std::vector<CachedViewportRow> rows;
    rowCount = std::min<std::uint64_t>(rowCount, 4096U);
    rows.reserve(static_cast<std::size_t>(rowCount));
    std::uint64_t generation = 0;
    RECT client{};
    if (window_ != nullptr) GetClientRect(window_, &client);
    const int wrapWidth = std::max(1L, client.right - client.left - 8L);
    HDC dc = window_ == nullptr ? nullptr : GetDC(window_);
    HGDIOBJ oldFont = nullptr;
    if (dc != nullptr) oldFont = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) {
            if (dc != nullptr) { SelectObject(dc, oldFont); ReleaseDC(window_, dc); }
            InvalidateViewportCache();
            return;
        }
        generation = document_->Generation();
        std::uint64_t logicalLine = topLine_;
        while (rows.size() < static_cast<std::size_t>(rowCount) && logicalLine < document_->LineCount()) {
            std::uint64_t start = 0;
            std::uint64_t contentEnd = 0;
            std::uint64_t nextLineStart = 0;
            std::wstring error;
            if (!document_->LineRange(logicalLine, start, contentEnd, nextLineStart, error)) break;
            const std::uint64_t initialOffset = wordWrap_ && logicalLine == topLine_
                ? std::min(topWrapOffset_, contentEnd - start) : 0U;
            std::wstring text;
            const bool available = document_->ReadRange(start + initialOffset,
                std::min<std::uint64_t>(contentEnd - start - initialOffset, kRenderedLineByteLimit), text, error);
            if (!available) break;

            if (!wordWrap_) {
                CachedViewportRow row;
                row.line = logicalLine;
                row.start = start;
                row.contentEnd = contentEnd;
                row.nextLineStart = nextLineStart;
                row.segmentStart = start;
                row.segmentEnd = start + static_cast<std::uint64_t>(text.size());
                row.text = std::move(text);
                row.available = true;
                rows.push_back(std::move(row));
                ++logicalLine;
                continue;
            }

            std::size_t offset = 0;
            bool emitted = false;
            do {
                std::size_t end = text.size();
                if (offset < text.size() && dc != nullptr) {
                    const std::wstring remaining(text.data() + offset, text.size() - offset);
                    const TextLayout layout = LayoutText(dc, remaining);
                    const auto beyond = std::upper_bound(layout.xBoundary.begin(), layout.xBoundary.end(), wrapWidth);
                    const std::size_t fit = std::max<std::size_t>(1U,
                        beyond == layout.xBoundary.begin() ? 0U :
                        static_cast<std::size_t>(beyond - layout.xBoundary.begin() - 1));
                    end = offset + std::min(fit, remaining.size());
                    if (end < text.size()) {
                        std::size_t wordBreak = end;
                        while (wordBreak > offset && !std::iswspace(text[wordBreak - 1U])) --wordBreak;
                        if (wordBreak > offset) end = wordBreak;
                    }
                    if (end < text.size() && end > offset && IsHighSurrogateUnit(text[end - 1U]) &&
                        IsLowSurrogateUnit(text[end])) --end;
                    if (end == offset) end = std::min(text.size(), offset + 1U);
                }
                CachedViewportRow row;
                row.line = logicalLine;
                row.start = start;
                row.contentEnd = contentEnd;
                row.nextLineStart = nextLineStart;
                row.segmentStart = start + initialOffset + static_cast<std::uint64_t>(offset);
                row.segmentEnd = start + initialOffset + static_cast<std::uint64_t>(end);
                row.text.assign(text.data() + offset, end - offset);
                row.available = true;
                rows.push_back(std::move(row));
                emitted = true;
                offset = end;
            } while (offset < text.size() && rows.size() < static_cast<std::size_t>(rowCount));
            if (!emitted && rows.size() < static_cast<std::size_t>(rowCount)) {
                CachedViewportRow row;
                row.line = logicalLine;
                row.start = start;
                row.contentEnd = contentEnd;
                row.nextLineStart = nextLineStart;
                row.segmentStart = row.segmentEnd = start + initialOffset;
                row.available = true;
                rows.push_back(std::move(row));
            }
            ++logicalLine;
        }
    }
    if (dc != nullptr) { SelectObject(dc, oldFont); ReleaseDC(window_, dc); }
    viewportRows_ = std::move(rows);
    viewportCacheTopLine_ = topLine_;
    viewportCacheGeneration_ = generation;
    viewportCacheValid_ = true;
}

void LargeFileView::RefreshCachedViewportLine(
    std::uint64_t line,
    std::uint64_t removedCharacters,
    std::uint64_t insertedCharacters)
{
    if (wordWrap_) {
        InvalidateViewportCache();
        return;
    }
    if (!viewportCacheValid_ || viewportCacheTopLine_ != topLine_ || line < topLine_) return;
    const std::uint64_t index = line - topLine_;
    if (index >= viewportRows_.size()) return;
    CachedViewportRow row;
    row.line = line;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) { InvalidateViewportCache(); return; }
        std::wstring error;
        row.available = document_->LineRange(line, row.start, row.contentEnd, row.nextLineStart, error) &&
            document_->ReadRange(row.start,
                std::min<std::uint64_t>(row.contentEnd - row.start, kRenderedLineByteLimit), row.text, error);
        row.segmentStart = row.start;
        row.segmentEnd = row.start + static_cast<std::uint64_t>(row.text.size());
        generation = document_->Generation();
    }
    viewportRows_[static_cast<std::size_t>(index)] = std::move(row);
    for (std::size_t later = static_cast<std::size_t>(index) + 1U; later < viewportRows_.size(); ++later) {
        CachedViewportRow& shifted = viewportRows_[later];
        const auto shift = [removedCharacters, insertedCharacters](std::uint64_t value) {
            if (insertedCharacters >= removedCharacters) {
                return SaturatingAdd(value, insertedCharacters - removedCharacters);
            }
            const std::uint64_t decrease = removedCharacters - insertedCharacters;
            return value >= decrease ? value - decrease : 0U;
        };
        shifted.start = shift(shifted.start);
        shifted.contentEnd = shift(shifted.contentEnd);
        shifted.nextLineStart = shift(shifted.nextLineStart);
    }
    viewportCacheGeneration_ = generation;
}

std::uint64_t LargeFileView::IndexedLineCount() const
{
    std::lock_guard<std::mutex> lock(lineMutex_);
    return static_cast<std::uint64_t>(lineOffsets_.size());
}

std::uint64_t LargeFileView::VisibleLineCount() const
{
    if (window_ == nullptr) {
        return 1;
    }
    HDC deviceContext = GetDC(window_);
    TEXTMETRICW metrics{};
    HGDIOBJ previousFont = nullptr;
    if (deviceContext != nullptr) {
        previousFont = SelectObject(deviceContext, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        GetTextMetricsW(deviceContext, &metrics);
        SelectObject(deviceContext, previousFont);
        ReleaseDC(window_, deviceContext);
    }
    RECT client{};
    GetClientRect(window_, &client);
    const int height = std::max(1, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
    return static_cast<std::uint64_t>(std::max(1L, (client.bottom - client.top) / height));
}

std::uint64_t LargeFileView::EstimatedTotalLines() const
{
    const std::uint64_t indexedLines = IndexedLineCount();
    if (indexingComplete_.load()) {
        return std::max<std::uint64_t>(1U, indexedLines);
    }
    const std::uint64_t indexedBytes = indexedBytes_.load();
    const std::uint64_t indexedData = indexedBytes > dataOffset_ ? indexedBytes - dataOffset_ : 0U;
    const std::uint64_t fileData = fileSize_ > dataOffset_ ? fileSize_ - dataOffset_ : 0U;
    if (indexedData > 0U && indexedLines > 1U) {
        const long double estimate = (static_cast<long double>(fileData) *
            static_cast<long double>(indexedLines - 1U) / static_cast<long double>(indexedData)) + 1.0L;
        const long double maximum = static_cast<long double>((std::numeric_limits<std::uint64_t>::max)());
        return estimate >= maximum ? (std::numeric_limits<std::uint64_t>::max)()
                                   : std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(estimate));
    }
    const std::uint64_t roughBytesPerLine = encoding_ == classic_notepad::LargeFileEncoding::Utf16Le ? 128U : 64U;
    return std::max<std::uint64_t>(1U, (fileData / roughBytesPerLine) + 1U);
}

int LargeFileView::LineToPosition(std::uint64_t line) const
{
    const std::uint64_t total = EstimatedTotalLines();
    if (total <= 1U) {
        return 0;
    }
    constexpr int range = (std::numeric_limits<int>::max)() - 1;
    const long double value = static_cast<long double>(line) * static_cast<long double>(range) /
        static_cast<long double>(total - 1U);
    return static_cast<int>(std::clamp<long double>(value, 0.0L, static_cast<long double>(range)));
}

std::uint64_t LargeFileView::PositionToLine(int position) const
{
    constexpr int range = (std::numeric_limits<int>::max)() - 1;
    const std::uint64_t total = EstimatedTotalLines();
    if (total <= 1U || position <= 0) {
        return 0;
    }
    const long double value = static_cast<long double>(position) * static_cast<long double>(total - 1U) /
        static_cast<long double>(range);
    return std::min<std::uint64_t>(total - 1U, static_cast<std::uint64_t>(value));
}

void LargeFileView::RefreshScrollBars()
{
    if (window_ == nullptr) {
        return;
    }
    const std::uint64_t total = EstimatedTotalLines();
    const std::uint64_t page = VisibleLineCount();
    if (topLine_ >= total) {
        topLine_ = total - 1U;
    }
    SCROLLINFO vertical{};
    vertical.cbSize = sizeof(vertical);
    vertical.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    vertical.nMin = 0;
    vertical.nMax = (std::numeric_limits<int>::max)() - 1;
    if (indexingComplete_.load()) {
        vertical.nPage = static_cast<UINT>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>((std::numeric_limits<UINT>::max)()),
            std::max<std::uint64_t>(1U, page * static_cast<std::uint64_t>(vertical.nMax) /
                std::max<std::uint64_t>(1U, total))));
        vertical.nPos = LineToPosition(topLine_);
    } else {
        // Do not continuously remap a dragging thumb from a changing global
        // line estimate. Keyboard/caret navigation remains available inside
        // the indexed frontier; global scrollbar interaction is gated until
        // the worker publishes the final stable line count.
        vertical.nPage = static_cast<UINT>(vertical.nMax);
        vertical.nPos = 0;
    }
    SetScrollInfo(window_, SB_VERT, &vertical, TRUE);
    EnableScrollBar(window_, SB_VERT, indexingComplete_.load() ? ESB_ENABLE_BOTH : ESB_DISABLE_BOTH);

    RECT client{};
    GetClientRect(window_, &client);
    const int clientWidth = std::max(1L, client.right - client.left);
    int averageCharacterWidth = 8;
    HDC dc = GetDC(window_);
    if (dc != nullptr) {
        HGDIOBJ oldFont = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics)) averageCharacterWidth = std::max(1L, metrics.tmAveCharWidth);
        SelectObject(dc, oldFont);
        ReleaseDC(window_, dc);
    }
    const std::uint64_t columns = maximumLineColumns_.load();
    const std::uint64_t maximumWidth = static_cast<std::uint64_t>((std::numeric_limits<int>::max)());
    const std::uint64_t contentWidth64 = columns > (maximumWidth - 8U) /
            static_cast<std::uint64_t>(averageCharacterWidth)
        ? maximumWidth
        : columns * static_cast<std::uint64_t>(averageCharacterWidth) + 8U;
    const int contentWidth = static_cast<int>(std::max<std::uint64_t>(1U, contentWidth64));
    horizontalMaximumOffset_ = std::max(0, contentWidth - clientWidth);
    horizontalOffset_ = std::clamp(horizontalOffset_, 0, horizontalMaximumOffset_);
    SCROLLINFO horizontal{};
    horizontal.cbSize = sizeof(horizontal);
    horizontal.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    horizontal.nMin = 0;
    horizontal.nMax = contentWidth - 1;
    horizontal.nPage = static_cast<UINT>(clientWidth);
    horizontal.nPos = horizontalOffset_;
    SetScrollInfo(window_, SB_HORZ, &horizontal, TRUE);
    ShowScrollBar(window_, SB_HORZ, wordWrap_ ? FALSE : TRUE);
}

void LargeFileView::SetTopLine(std::uint64_t line)
{
    const std::uint64_t total = EstimatedTotalLines();
    topLine_ = total == 0U ? 0U : std::min(line, total - 1U);
    topWrapOffset_ = 0;
    InvalidateViewportCache();
    RefreshScrollBars();
    InvalidateRect(window_, nullptr, FALSE);
    NotifyStateChanged();
}

void LargeFileView::ScrollVertical(int command, int trackPosition)
{
    const std::uint64_t page = VisibleLineCount();
    if (wordWrap_ && (command == SB_LINEDOWN || command == SB_PAGEDOWN)) {
        RebuildViewportCache(page + 2U);
        if (viewportRows_.size() > 1U) {
            const std::size_t amount = command == SB_LINEDOWN ? 1U :
                std::min<std::size_t>(viewportRows_.size() - 1U, static_cast<std::size_t>(page - 1U));
            const CachedViewportRow& row = viewportRows_[amount];
            topLine_ = row.line;
            topWrapOffset_ = row.segmentStart - row.start;
            InvalidateViewportCache();
            RefreshScrollBars();
            InvalidateRect(window_, nullptr, FALSE);
            NotifyStateChanged();
        }
        return;
    }
    if (wordWrap_ && command == SB_LINEUP && topWrapOffset_ > 0U) {
        const std::uint64_t wanted = topWrapOffset_;
        topWrapOffset_ = 0;
        InvalidateViewportCache();
        RebuildViewportCache(4096U);
        std::size_t current = 0;
        while (current < viewportRows_.size() && viewportRows_[current].line == topLine_ &&
               viewportRows_[current].segmentStart - viewportRows_[current].start < wanted) ++current;
        if (current > 0U) --current;
        if (current < viewportRows_.size()) {
            topWrapOffset_ = viewportRows_[current].segmentStart - viewportRows_[current].start;
        }
        InvalidateViewportCache();
        RefreshScrollBars();
        InvalidateRect(window_, nullptr, FALSE);
        NotifyStateChanged();
        return;
    }
    switch (command) {
    case SB_LINEUP: SetTopLine(topLine_ == 0U ? 0U : topLine_ - 1U); break;
    case SB_LINEDOWN: SetTopLine(SaturatingAdd(topLine_, 1U)); break;
    case SB_PAGEUP: SetTopLine(topLine_ > page ? topLine_ - page : 0U); break;
    case SB_PAGEDOWN: SetTopLine(SaturatingAdd(topLine_, page)); break;
    case SB_TOP: SetTopLine(0); break;
    case SB_BOTTOM: SetTopLine(EstimatedTotalLines() - 1U); break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: SetTopLine(PositionToLine(trackPosition)); break;
    default: break;
    }
}

void LargeFileView::ScrollHorizontal(int command, int trackPosition)
{
    if (wordWrap_) return;
    RECT client{};
    GetClientRect(window_, &client);
    const int page = std::max(16L, client.right - client.left - 16L);
    switch (command) {
    case SB_LINELEFT: horizontalOffset_ = std::max(0, horizontalOffset_ - 16); break;
    case SB_LINERIGHT: horizontalOffset_ = horizontalOffset_ >= horizontalMaximumOffset_ - std::min(16, horizontalMaximumOffset_)
        ? horizontalMaximumOffset_ : horizontalOffset_ + 16; break;
    case SB_PAGELEFT: horizontalOffset_ = std::max(0, horizontalOffset_ - page); break;
    case SB_PAGERIGHT: horizontalOffset_ = horizontalOffset_ >= horizontalMaximumOffset_ - std::min(page, horizontalMaximumOffset_)
        ? horizontalMaximumOffset_ : horizontalOffset_ + page; break;
    case SB_LEFT: horizontalOffset_ = 0; break;
    case SB_RIGHT: horizontalOffset_ = horizontalMaximumOffset_; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: horizontalOffset_ = std::clamp(trackPosition, 0, horizontalMaximumOffset_); break;
    default: return;
    }
    RefreshScrollBars();
    InvalidateRect(window_, nullptr, FALSE);
}

bool LargeFileView::GetLineBounds(
    std::uint64_t zeroBasedLine,
    std::uint64_t& start,
    std::uint64_t& end,
    bool& knownEnd) const
{
    std::lock_guard<std::mutex> lock(lineMutex_);
    if (zeroBasedLine >= lineOffsets_.size()) {
        return false;
    }
    start = lineOffsets_[static_cast<std::size_t>(zeroBasedLine)];
    if (zeroBasedLine + 1U < lineOffsets_.size()) {
        end = lineOffsets_[static_cast<std::size_t>(zeroBasedLine + 1U)];
        knownEnd = true;
    } else {
        end = fileSize_;
        knownEnd = indexingComplete_.load();
    }
    return true;
}

bool LargeFileView::ReadLine(std::uint64_t zeroBasedLine, std::wstring& text, bool& available) const
{
    text.clear();
    available = false;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    bool knownEnd = false;
    if (!GetLineBounds(zeroBasedLine, start, end, knownEnd) || end < start) {
        return true;
    }
    const std::uint64_t byteCount = end - start;
    const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(byteCount, kRenderedLineByteLimit));
    std::vector<std::uint8_t> bytes;
    if (!CopyBytes(start, requested, bytes)) {
        return false;
    }
    available = true;
    const bool truncated = byteCount > static_cast<std::uint64_t>(requested) || !knownEnd;
    if (knownEnd && !truncated) {
        if (encoding_ == classic_notepad::LargeFileEncoding::Utf8) {
            while (!bytes.empty() && (bytes.back() == '\r' || bytes.back() == '\n')) {
                bytes.pop_back();
            }
        } else {
            while (bytes.size() >= 2U) {
                const std::size_t index = bytes.size() - 2U;
                const std::uint16_t unit = static_cast<std::uint16_t>(bytes[index]) |
                    (static_cast<std::uint16_t>(bytes[index + 1U]) << 8U);
                if (unit != static_cast<std::uint16_t>('\r') && unit != static_cast<std::uint16_t>('\n')) {
                    break;
                }
                bytes.resize(index);
            }
        }
    }

    if (encoding_ == classic_notepad::LargeFileEncoding::Utf8) {
        std::size_t usable = bytes.size();
        int length = 0;
        while (usable > 0U) {
            length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                reinterpret_cast<const char*>(bytes.data()), static_cast<int>(usable), nullptr, 0);
            if (length > 0) {
                break;
            }
            --usable;
        }
        if (usable > 0U && length > 0) {
            text.resize(static_cast<std::size_t>(length));
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(usable), text.data(), length);
        }
    } else {
        const std::size_t usable = bytes.size() - (bytes.size() % 2U);
        text.reserve(usable / 2U);
        for (std::size_t index = 0; index < usable; index += 2U) {
            text.push_back(static_cast<wchar_t>(static_cast<std::uint16_t>(bytes[index]) |
                (static_cast<std::uint16_t>(bytes[index + 1U]) << 8U)));
        }
        if (!text.empty() && text.back() >= 0xD800 && text.back() <= 0xDBFF) {
            text.pop_back();
        }
    }
    if (truncated) {
        text += L" …";
    }
    return true;
}

std::wstring LargeFileView::IndexingMessage() const
{
    if (!encodingValid_.load()) {
        return L"Unsupported or invalid non-UTF-8 content in large-file mode.";
    }
    if (indexingComplete_.load()) {
        return L"Large document is ready for editing.";
    }
    const std::uint64_t size = std::max<std::uint64_t>(1U, fileSize_);
    const long double percentValue = static_cast<long double>(indexedBytes_.load()) * 100.0L /
        static_cast<long double>(size);
    const std::uint64_t percent = std::min<std::uint64_t>(100U, static_cast<std::uint64_t>(percentValue));
    return L"Preparing editable large document - indexing " + std::to_wstring(percent) + L"%";
}

bool LargeFileView::ReadEditedLine(
    std::uint64_t zeroBasedLine,
    std::uint64_t& start,
    std::uint64_t& contentEnd,
    std::uint64_t& nextLineStart,
    std::wstring& text) const
{
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (document_ == nullptr) return false;
    std::wstring error;
    if (!document_->LineRange(zeroBasedLine, start, contentEnd, nextLineStart, error)) return false;
    return document_->ReadRange(start, std::min<std::uint64_t>(contentEnd - start, kRenderedLineByteLimit), text, error);
}

void LargeFileView::Paint(HDC deviceContext, const RECT& invalidRect)
{
    if (firstViewportPaintMilliseconds_.load() == 0U) {
        firstViewportPaintMilliseconds_.store(std::max<std::uint64_t>(1U, ElapsedMilliseconds(startedTick_)));
    }
    RECT client{};
    GetClientRect(window_, &client);
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);
    HDC buffer = CreateCompatibleDC(deviceContext);
    HBITMAP bitmap = CreateCompatibleBitmap(deviceContext, width, height);
    HGDIOBJ previousBitmap = SelectObject(buffer, bitmap);
    const COLORREF background = darkMode_ ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW);
    const COLORREF textColor = darkMode_ ? RGB(242, 242, 242) : GetSysColor(COLOR_WINDOWTEXT);
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(buffer, &invalidRect, brush);
    DeleteObject(brush);

    HGDIOBJ previousFont = SelectObject(buffer, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(buffer, textColor);
    SetBkMode(buffer, TRANSPARENT);
    TEXTMETRICW metrics{};
    GetTextMetricsW(buffer, &metrics);
    const int lineHeight = std::max(1, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
    const std::uint64_t firstRow = static_cast<std::uint64_t>(std::max<LONG>(0, invalidRect.top) / lineHeight);
    const std::uint64_t visible = static_cast<std::uint64_t>((std::max<LONG>(0, invalidRect.bottom) / lineHeight) + 2);
    int y = static_cast<int>(firstRow) * lineHeight;
    bool showedAny = false;
    bool editedReady = false;
    std::uint64_t editedGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        editedReady = document_ != nullptr;
        if (editedReady) editedGeneration = document_->Generation();
    }
    if (editedReady && (!viewportCacheValid_ || viewportCacheTopLine_ != topLine_ ||
            viewportCacheGeneration_ != editedGeneration)) {
        RebuildViewportCache(VisibleLineCount() + 2U);
    }
    for (std::uint64_t index = firstRow; index < visible && y < client.bottom; ++index, y += lineHeight) {
        std::uint64_t line = SaturatingAdd(topLine_, index);
        std::wstring text;
        bool available = false;
        std::uint64_t lineStart = 0;
        std::uint64_t lineEnd = 0;
        std::uint64_t nextLineStart = 0;
        if (editedReady) {
            const std::uint64_t cacheIndex = index;
            if (viewportCacheValid_ && cacheIndex < viewportRows_.size()) {
                const CachedViewportRow& row = viewportRows_[static_cast<std::size_t>(cacheIndex)];
                line = row.line;
                available = row.available;
                lineStart = row.segmentStart;
                lineEnd = row.segmentEnd;
                nextLineStart = row.nextLineStart;
                text = row.text;
            }
        } else if (!ReadLine(line, text, available)) {
            text = L"Unable to read mapped file data."; available = true;
        }
        if (!available) {
            if (!showedAny) {
                text = IndexingMessage();
                TextOutW(buffer, 4 - horizontalOffset_, y, text.c_str(), static_cast<int>(text.size()));
            }
            break;
        }
        showedAny = true;
        const std::uint64_t selectedStart = std::min(selectionStart_, selectionEnd_);
        const std::uint64_t selectedEnd = std::max(selectionStart_, selectionEnd_);
        int x = 4 - (wordWrap_ ? 0 : horizontalOffset_);
        const std::uint64_t approximateVisibleCharacters =
            static_cast<std::uint64_t>((wordWrap_ ? 0 : horizontalOffset_) + width) /
                static_cast<std::uint64_t>(std::max<LONG>(1, metrics.tmAveCharWidth)) + 64U;
        const std::size_t paintedCharacters = static_cast<std::size_t>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(text.size()),
            std::min<std::uint64_t>(approximateVisibleCharacters,
                static_cast<std::uint64_t>((std::numeric_limits<int>::max)()))));
        const std::wstring paintedText(text.data(), paintedCharacters);
        const TextLayout layout = LayoutText(buffer, paintedText);
        TextOutW(buffer, x, y, layout.displayText.c_str(), static_cast<int>(layout.displayText.size()));
        if (editedReady && selectedStart < lineEnd && selectedEnd > lineStart) {
            const std::size_t first = static_cast<std::size_t>(std::min<std::uint64_t>(paintedCharacters, selectedStart > lineStart ? selectedStart - lineStart : 0U));
            const std::size_t last = static_cast<std::size_t>(std::min<std::uint64_t>(paintedCharacters, selectedEnd - lineStart));
            const int selectionLeft = layout.xBoundary[first];
            const int selectionWidth = std::max(1, layout.xBoundary[last] - selectionLeft);
            RECT selectionRect{x + selectionLeft, y, x + selectionLeft + selectionWidth, y + lineHeight};
            HBRUSH selectionBrush = CreateSolidBrush(darkMode_ ? RGB(54, 84, 120) : RGB(0, 120, 215));
            FillRect(buffer, &selectionRect, selectionBrush);
            DeleteObject(selectionBrush);
            SetTextColor(buffer, darkMode_ ? RGB(255, 255, 255) : RGB(255, 255, 255));
            const int saved = SaveDC(buffer);
            IntersectClipRect(buffer, selectionRect.left, selectionRect.top, selectionRect.right, selectionRect.bottom);
            TextOutW(buffer, x, y, layout.displayText.c_str(), static_cast<int>(layout.displayText.size()));
            RestoreDC(buffer, saved);
            SetTextColor(buffer, textColor);
        }
        if (editedReady && !spellingRanges_.empty()) {
            for (const LargeFileSpellingRange& issue : spellingRanges_) {
                const std::uint64_t issueEnd = SaturatingAdd(issue.start, issue.length);
                if (issue.start >= lineEnd || issueEnd <= lineStart) continue;
                const std::size_t first = static_cast<std::size_t>(std::min<std::uint64_t>(
                    paintedCharacters, std::max(issue.start, lineStart) - lineStart));
                const std::size_t last = static_cast<std::size_t>(std::min<std::uint64_t>(
                    paintedCharacters, std::min(issueEnd, lineEnd) - lineStart));
                if (last <= first) continue;
                const int underlineY = std::min<int>(y + lineHeight - 2, y + static_cast<int>(metrics.tmAscent) + 2);
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(220, 45, 45));
                HGDIOBJ oldPen = SelectObject(buffer, pen);
                const int left = x + layout.xBoundary[first];
                const int right = x + std::max(layout.xBoundary[first] + 2, layout.xBoundary[last]);
                MoveToEx(buffer, left, underlineY, nullptr);
                for (int pixel = left + 2, phase = 1; pixel <= right; pixel += 2, phase = -phase) {
                    LineTo(buffer, pixel, underlineY + phase);
                }
                SelectObject(buffer, oldPen);
                DeleteObject(pen);
            }
        }
    }
    if (!showedAny) {
        const std::wstring message = IndexingMessage();
        TextOutW(buffer, 4, 4, message.c_str(), static_cast<int>(message.size()));
    }
    SelectObject(buffer, previousFont);
    BitBlt(deviceContext, invalidRect.left, invalidRect.top,
        invalidRect.right - invalidRect.left, invalidRect.bottom - invalidRect.top,
        buffer, invalidRect.left, invalidRect.top, SRCCOPY);
    SelectObject(buffer, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    paintGeneration_.fetch_add(1U);
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        viewportGeneration_ = document_ == nullptr ? 0U : document_->Generation();
    }
}

void LargeFileView::DestroyTextCaret()
{
    if (caretCreated_) {
        HideCaret(window_);
        DestroyCaret();
        caretCreated_ = false;
    }
}

bool LargeFileView::HitTest(POINT point, std::uint64_t& position, int* visualX)
{
    HDC dc = GetDC(window_);
    if (dc == nullptr) return false;
    HGDIOBJ oldFont = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int lineHeight = std::max(1, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
    const std::size_t visualRow = static_cast<std::size_t>(std::max<LONG>(0, point.y) / lineHeight);
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ != nullptr) generation = document_->Generation();
    }
    if (!viewportCacheValid_ || viewportCacheTopLine_ != topLine_ || viewportCacheGeneration_ != generation ||
        visualRow >= viewportRows_.size()) {
        RebuildViewportCache(VisibleLineCount() + 2U);
    }
    if (visualRow >= viewportRows_.size() || !viewportRows_[visualRow].available) {
        SelectObject(dc, oldFont); ReleaseDC(window_, dc); return false;
    }
    const CachedViewportRow& row = viewportRows_[visualRow];
    const std::wstring& text = row.text;
    const std::uint64_t start = row.segmentStart;
    const std::uint64_t end = row.segmentEnd;
    const int wanted = static_cast<int>(std::max<LONG>(0, point.x + (wordWrap_ ? 0 : horizontalOffset_) - 4));
    const TextLayout layout = LayoutText(dc, text);
    const std::size_t fit = ClosestTextBoundary(layout, wanted);
    position = start + static_cast<std::uint64_t>(fit);
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ != nullptr && !document_->IsCharacterBoundary(position, error) && position < end) ++position;
    }
    if (visualX != nullptr) *visualX = layout.xBoundary[fit];
    SelectObject(dc, oldFont);
    ReleaseDC(window_, dc);
    return true;
}

bool LargeFileView::FindCachedRowForPosition(std::uint64_t position, std::size_t& rowIndex) const
{
    for (std::size_t index = 0; index < viewportRows_.size(); ++index) {
        const CachedViewportRow& row = viewportRows_[index];
        if (!row.available) continue;
        if (position >= row.segmentStart && position < row.segmentEnd) {
            rowIndex = index;
            return true;
        }
        if (position == row.segmentEnd &&
            (row.segmentEnd == row.contentEnd || index + 1U == viewportRows_.size() ||
             viewportRows_[index + 1U].segmentStart != position)) {
            rowIndex = index;
            return true;
        }
    }
    return false;
}

void LargeFileView::EnsureCaretVisible()
{
    classic_notepad::LargeDocumentLineColumn coordinate;
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || !document_->PositionToLineColumn(caretPosition_, coordinate, error)) return;
    }
    if (wordWrap_) {
        if (coordinate.line < topLine_) {
            topLine_ = coordinate.line;
            topWrapOffset_ = 0;
            InvalidateViewportCache();
        }
        RebuildViewportCache(VisibleLineCount() + 2U);
        std::size_t row = 0;
        if (!FindCachedRowForPosition(caretPosition_, row) || row >= VisibleLineCount()) {
            topLine_ = coordinate.line;
            topWrapOffset_ = coordinate.column;
            InvalidateViewportCache();
            RebuildViewportCache(VisibleLineCount() + 2U);
        }
        RefreshScrollBars();
        return;
    }
    const std::uint64_t visible = VisibleLineCount();
    if (coordinate.line < topLine_) topLine_ = coordinate.line;
    else if (coordinate.line >= topLine_ + visible) topLine_ = coordinate.line - visible + 1U;
    RefreshScrollBars();
}

void LargeFileView::UpdateCaret()
{
    if (!focused_ || window_ == nullptr) return;
    classic_notepad::LargeDocumentLineColumn coordinate;
    std::uint64_t lineStart = 0, lineEnd = 0, next = 0;
    std::wstring line;
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || !document_->PositionToLineColumn(caretPosition_, coordinate, error)) return;
    }
    std::size_t visualRow = 0;
    if (wordWrap_) {
        RebuildViewportCache(VisibleLineCount() + 2U);
        if (!FindCachedRowForPosition(caretPosition_, visualRow) || visualRow >= VisibleLineCount()) {
            DestroyTextCaret(); return;
        }
        const CachedViewportRow& cached = viewportRows_[visualRow];
        lineStart = cached.segmentStart;
        lineEnd = cached.segmentEnd;
        next = cached.nextLineStart;
        line = cached.text;
    } else {
        if (coordinate.line < topLine_ || coordinate.line >= topLine_ + VisibleLineCount()) {
            DestroyTextCaret(); return;
        }
        if (!ReadEditedLine(coordinate.line, lineStart, lineEnd, next, line)) return;
        visualRow = static_cast<std::size_t>(coordinate.line - topLine_);
    }
    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(line.size(), caretPosition_ - lineStart));
    HDC dc = GetDC(window_);
    if (dc == nullptr) return;
    HGDIOBJ oldFont = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const TextLayout layout = LayoutText(dc, line);
    SelectObject(dc, oldFont); ReleaseDC(window_, dc);
    const int caretHeight = std::max(1, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
    if (!caretCreated_) {
        if (!CreateCaret(window_, nullptr, 2, caretHeight)) return;
        caretCreated_ = true;
        ShowCaret(window_);
    }
    const int caretX = 4 + layout.xBoundary[count] - (wordWrap_ ? 0 : horizontalOffset_);
    SetCaretPos(caretX, static_cast<int>(visualRow) * caretHeight);
    caretRectangle_ = {caretX,
        static_cast<LONG>(visualRow) * caretHeight,
        caretX + 2,
        (static_cast<LONG>(visualRow) + 1) * caretHeight};
}

void LargeFileView::MoveCaret(std::uint64_t position, bool extendSelection, bool preservePreferredX)
{
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) return;
        position = std::min(position, document_->CharacterCount());
        if (!document_->IsCharacterBoundary(position, error)) return;
        if (!extendSelection) selectionAnchor_ = position;
        caretPosition_ = position;
        selectionStart_ = std::min(selectionAnchor_, caretPosition_);
        selectionEnd_ = std::max(selectionAnchor_, caretPosition_);
        allSelected_ = selectionStart_ == 0U && selectionEnd_ == document_->CharacterCount();
    }
    if (!preservePreferredX) preferredXValid_ = false;
    EnsureCaretVisible();
    InvalidateRect(window_, nullptr, FALSE);
    UpdateCaret();
    NotifyStateChanged();
}

void LargeFileView::MoveHorizontal(bool right, bool byWord, bool extendSelection)
{
    std::uint64_t position = caretPosition_;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) return;
        const std::uint64_t total = document_->CharacterCount();
        std::wstring error;
        auto word = [](wchar_t ch) { return std::iswalnum(static_cast<wint_t>(ch)) != 0 || ch == L'_'; };
        if (right) {
            if (position < total) {
                wchar_t ch = 0; document_->CharacterAt(position, ch, error);
                position += IsHighSurrogateUnit(ch) && position + 1U < total ? 2U : 1U;
            }
            if (byWord) {
                wchar_t ch = 0;
                while (position < total && document_->CharacterAt(position, ch, error) && !word(ch)) ++position;
                while (position < total && document_->CharacterAt(position, ch, error) && word(ch)) ++position;
            }
        } else {
            if (position > 0U) {
                --position; wchar_t ch = 0;
                if (position > 0U && document_->CharacterAt(position, ch, error) && IsLowSurrogateUnit(ch)) --position;
            }
            if (byWord) {
                wchar_t ch = 0;
                while (position > 0U && document_->CharacterAt(position - 1U, ch, error) && !word(ch)) --position;
                while (position > 0U && document_->CharacterAt(position - 1U, ch, error) && word(ch)) --position;
            }
        }
    }
    MoveCaret(position, extendSelection);
}

void LargeFileView::MoveVertical(int lines, bool extendSelection)
{
    classic_notepad::LargeDocumentLineColumn coordinate;
    std::uint64_t totalLines = 0;
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || !document_->PositionToLineColumn(caretPosition_, coordinate, error)) return;
        totalLines = document_->LineCount();
    }
    if (!preferredXValid_) {
        POINT caret{};
        if (GetCaretPos(&caret)) preferredX_ = caret.x;
        else preferredX_ = static_cast<int>(coordinate.column) * 8;
        preferredXValid_ = true;
    }
    if (wordWrap_) {
        const std::uint64_t visibleRows = VisibleLineCount();
        RebuildViewportCache(visibleRows + 2U);
        std::size_t currentRow = 0;
        if (!FindCachedRowForPosition(caretPosition_, currentRow)) return;
        std::int64_t target = static_cast<std::int64_t>(currentRow) + lines;
        if (target < 0) {
            for (std::int64_t count = 0; count < -target; ++count) ScrollVertical(SB_LINEUP, 0);
            RebuildViewportCache(visibleRows + 2U);
            target = 0;
        } else if (target >= static_cast<std::int64_t>(viewportRows_.size())) {
            target = static_cast<std::int64_t>(viewportRows_.size()) - 1;
        }
        if (target < 0 || static_cast<std::size_t>(target) >= viewportRows_.size()) return;
        HDC dc = GetDC(window_); TEXTMETRICW metrics{};
        if (dc != nullptr) {
            HGDIOBJ old = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
            GetTextMetricsW(dc, &metrics); SelectObject(dc, old); ReleaseDC(window_, dc);
        }
        POINT point{preferredX_, static_cast<LONG>(target) *
            std::max(1, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading)) + 1};
        std::uint64_t position = caretPosition_;
        if (HitTest(point, position)) MoveCaret(position, extendSelection, true);
        return;
    }
    const std::int64_t candidate = static_cast<std::int64_t>(coordinate.line) + lines;
    const std::uint64_t targetLine = static_cast<std::uint64_t>(std::clamp<std::int64_t>(
        candidate, 0, static_cast<std::int64_t>(totalLines - 1U)));
    POINT point{preferredX_, static_cast<LONG>((targetLine >= topLine_ ? targetLine - topLine_ : 0U) *
        std::max<std::uint64_t>(1U, 16U))};
    if (targetLine < topLine_) topLine_ = targetLine;
    else if (targetLine >= topLine_ + VisibleLineCount()) topLine_ = targetLine - VisibleLineCount() + 1U;
    HDC dc = GetDC(window_); TEXTMETRICW metrics{};
    if (dc != nullptr) {
        HGDIOBJ old = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        GetTextMetricsW(dc, &metrics); SelectObject(dc, old); ReleaseDC(window_, dc);
    }
    point.y = static_cast<LONG>(targetLine - topLine_) *
        std::max(1, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading)) + 1;
    std::uint64_t position = caretPosition_;
    if (HitTest(point, position)) MoveCaret(position, extendSelection, true);
}

void LargeFileView::MoveLineEdge(bool end, bool control, bool extendSelection)
{
    std::uint64_t position = 0;
    std::wstring error;
    if (wordWrap_ && !control) {
        RebuildViewportCache(VisibleLineCount() + 2U);
        std::size_t rowIndex = 0;
        if (FindCachedRowForPosition(caretPosition_, rowIndex)) {
            position = end ? viewportRows_[rowIndex].segmentEnd : viewportRows_[rowIndex].segmentStart;
            MoveCaret(position, extendSelection);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) return;
        if (control) {
            position = end ? document_->CharacterCount() : 0U;
        } else {
            classic_notepad::LargeDocumentLineColumn coordinate;
            std::uint64_t start = 0, contentEnd = 0, next = 0;
            if (!document_->PositionToLineColumn(caretPosition_, coordinate, error) ||
                !document_->LineRange(coordinate.line, start, contentEnd, next, error)) return;
            position = end ? contentEnd : start;
        }
    }
    MoveCaret(position, extendSelection);
}

void LargeFileView::ScrollCaretPage(int direction, bool extendSelection)
{
    const int amount = static_cast<int>(std::max<std::uint64_t>(1U, VisibleLineCount() - 1U));
    MoveVertical(direction * amount, extendSelection);
}

void LargeFileView::BeginMouseSelection(POINT point, bool extendSelection)
{
    SetFocus(window_);
    std::uint64_t position = 0;
    if (!HitTest(point, position)) return;
    if (!extendSelection) selectionAnchor_ = position;
    mouseSelecting_ = true;
    lastMousePoint_ = point;
    SetCapture(window_);
    SetTimer(window_, kMouseScrollTimer, kMouseScrollTimerMs, nullptr);
    MoveCaret(position, true);
}

void LargeFileView::UpdateMouseSelection(POINT point)
{
    if (!mouseSelecting_) return;
    lastMousePoint_ = point;
    RECT client{}; GetClientRect(window_, &client);
    POINT clamped{std::clamp(point.x, client.left, client.right), std::clamp(point.y, client.top, client.bottom - 1)};
    std::uint64_t position = 0;
    if (HitTest(clamped, position)) MoveCaret(position, true);
}

void LargeFileView::SelectWordAt(POINT point)
{
    std::uint64_t position = 0;
    if (!HitTest(point, position)) return;
    std::uint64_t start = position, end = position;
    std::wstring error;
    auto isWord = [](wchar_t ch) { return std::iswalnum(static_cast<wint_t>(ch)) != 0 || ch == L'_'; };
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) return;
        wchar_t ch = 0;
        if (position == document_->CharacterCount() && position != 0U) --start, --end;
        while (start > 0U && document_->CharacterAt(start - 1U, ch, error) && isWord(ch)) --start;
        while (end < document_->CharacterCount() && document_->CharacterAt(end, ch, error) && isWord(ch)) ++end;
        selectionAnchor_ = start;
    }
    MoveCaret(end, true);
}

void LargeFileView::ShowInputError(const std::wstring& errorMessage) const
{
    if (!errorMessage.empty()) MessageBoxW(parent_, errorMessage.c_str(), L"Classic Notepad", MB_OK | MB_ICONERROR);
}

void LargeFileView::ShowContextMenu(POINT screenPoint)
{
    POINT clientPoint = screenPoint;
    if (screenPoint.x == -1 && screenPoint.y == -1) {
        clientPoint = {caretRectangle_.left, caretRectangle_.bottom};
        screenPoint = clientPoint;
        ClientToScreen(window_, &screenPoint);
    } else {
        ScreenToClient(window_, &clientPoint);
    }
    std::uint64_t contextPosition = caretPosition_;
    HitTest(clientPoint, contextPosition);
    if (contextMenuHandler_ && contextMenuHandler_(screenPoint, contextPosition)) return;
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING, ID_EDIT_UNDO, L"Undo");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EDIT_CUT, L"Cut");
    AppendMenuW(menu, MF_STRING, ID_EDIT_COPY, L"Copy");
    AppendMenuW(menu, MF_STRING, ID_EDIT_PASTE, L"Paste");
    AppendMenuW(menu, MF_STRING, ID_EDIT_DELETE, L"Delete");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EDIT_SELECT_ALL, L"Select All");
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (command != 0U) SendMessageW(parent_, WM_COMMAND, command, 0);
}

bool LargeFileView::GetSelection(std::uint64_t& start, std::uint64_t& end) const
{
    std::lock_guard<std::mutex> lock(documentMutex_);
    start = selectionStart_;
    end = selectionEnd_;
    return true;
}

bool LargeFileView::SetSelection(std::uint64_t start, std::uint64_t end, std::wstring& errorMessage)
{
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (!EnsureEditable(errorMessage) || start > end || end > document_->CharacterCount()) {
        if (errorMessage.empty()) errorMessage = L"The selection is outside the large document.";
        return false;
    }
    selectionStart_ = start;
    selectionEnd_ = end;
    selectionAnchor_ = start;
    caretPosition_ = end;
    allSelected_ = start == 0U && end == document_->CharacterCount();
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

bool LargeFileView::GetTextRange(
    std::uint64_t start,
    std::uint64_t length,
    std::wstring& text,
    std::wstring& errorMessage) const
{
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (!EnsureEditable(errorMessage)) return false;
    return document_->ReadRange(start, length, text, errorMessage);
}

bool LargeFileView::CopySelection(std::wstring& errorMessage)
{
    std::wstring text;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureEditable(errorMessage)) return false;
        if (selectionEnd_ < selectionStart_ || selectionEnd_ - selectionStart_ > kClipboardCharacterLimit) {
            errorMessage = L"The selected text is too large to place on the clipboard.";
            return false;
        }
        if (!document_->ReadRange(selectionStart_, selectionEnd_ - selectionStart_, text, errorMessage)) return false;
    }
    if (!OpenClipboard(window_)) { errorMessage = L"The clipboard is currently unavailable."; return false; }
    EmptyClipboard();
    if (text.size() > ((std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t)) - 1U) {
        CloseClipboard(); errorMessage = L"The selected text is too large to place on the clipboard."; return false;
    }
    const std::size_t bytes = (text.size() + 1U) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) { CloseClipboard(); errorMessage = L"There is not enough memory for the clipboard selection."; return false; }
    void* destination = GlobalLock(memory);
    if (destination == nullptr) { GlobalFree(memory); CloseClipboard(); errorMessage = L"The clipboard selection could not be prepared."; return false; }
    std::memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) { GlobalFree(memory); CloseClipboard(); errorMessage = L"The clipboard selection could not be copied."; return false; }
    CloseClipboard();
    return true;
}

bool LargeFileView::ReplaceSelection(const std::wstring& text, std::wstring& errorMessage)
{
    classic_notepad::LargeDocumentLineColumn beforeStart{};
    classic_notepad::LargeDocumentLineColumn beforeEnd{};
    classic_notepad::LargeDocumentLineColumn afterCoordinate{};
    bool oneLineEdit = false;
    bool modifiedState = false;
    std::uint64_t removedCharacters = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureEditable(errorMessage)) return false;
        const bool hasLineEnding = text.find_first_of(L"\r\n") != std::wstring::npos;
        if (!document_->PositionToLineColumn(selectionStart_, beforeStart, errorMessage) ||
            !document_->PositionToLineColumn(selectionEnd_, beforeEnd, errorMessage)) return false;
        removedCharacters = selectionEnd_ - selectionStart_;
        std::uint64_t afterStart = 0;
        std::uint64_t afterEnd = 0;
        if (!document_->Replace(selectionStart_, selectionEnd_, text, selectionStart_, selectionEnd_,
                afterStart, afterEnd, errorMessage)) return false;
        selectionStart_ = afterStart;
        selectionEnd_ = afterEnd;
        selectionAnchor_ = afterStart;
        caretPosition_ = afterEnd;
        allSelected_ = false;
        const bool haveAfter = document_->PositionToLineColumn(caretPosition_, afterCoordinate, errorMessage);
        StoreMaximum(maximumLineColumns_, VisualColumns(text));
        const auto observeEditedLine = [this](std::uint64_t line) {
            std::uint64_t lineStart = 0, contentEnd = 0, nextLineStart = 0;
            std::wstring ignored;
            if (!document_->LineRange(line, lineStart, contentEnd, nextLineStart, ignored)) return;
            const std::uint64_t length = contentEnd - lineStart;
            StoreMaximum(maximumLineColumns_, length);
            if (length <= kRenderedLineByteLimit) {
                std::wstring lineText;
                if (document_->ReadRange(lineStart, length, lineText, ignored)) {
                    StoreMaximum(maximumLineColumns_, VisualColumns(lineText));
                }
            }
        };
        observeEditedLine(beforeStart.line);
        if (haveAfter && afterCoordinate.line != beforeStart.line) observeEditedLine(afterCoordinate.line);
        oneLineEdit = haveAfter && !hasLineEnding && beforeStart.line == beforeEnd.line &&
            beforeStart.line == afterCoordinate.line;
        modifiedState = document_->IsModified();
    }
    modified_.store(modifiedState);
    if (oneLineEdit) RefreshCachedViewportLine(
        beforeStart.line, removedCharacters, static_cast<std::uint64_t>(text.size()));
    else InvalidateViewportCache();
    RefreshScrollBars();
    MarkModified(!oneLineEdit);
    if (oneLineEdit) InvalidateDocumentLine(beforeStart.line);
    return true;
}

bool LargeFileView::InsertText(const std::wstring& text, std::wstring& errorMessage)
{
    return ReplaceSelection(classic_notepad::NormalizeLineEndingsForEditor(text), errorMessage);
}

bool LargeFileView::PasteText(const std::wstring& text, std::wstring& errorMessage)
{
    return InsertText(text, errorMessage);
}

bool LargeFileView::CutSelection(std::wstring& errorMessage)
{
    if (!CopySelection(errorMessage)) return false;
    return ReplaceSelection(L"", errorMessage);
}

bool LargeFileView::DeleteSelection(bool backspace, std::wstring& errorMessage)
{
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureEditable(errorMessage)) return false;
        if (selectionStart_ == selectionEnd_) {
            if (backspace) {
                if (selectionStart_ == 0U) return true;
                wchar_t character = 0;
                --selectionStart_;
                if (selectionStart_ > 0U && document_->CharacterAt(selectionStart_, character, errorMessage) &&
                    IsLowSurrogateUnit(character)) --selectionStart_;
            } else if (selectionEnd_ < document_->CharacterCount()) {
                wchar_t character = 0;
                document_->CharacterAt(selectionEnd_, character, errorMessage);
                selectionEnd_ += IsHighSurrogateUnit(character) ? 2U : 1U;
            } else {
                return true;
            }
        }
    }
    return ReplaceSelection(L"", errorMessage);
}

bool LargeFileView::Undo(std::wstring& errorMessage)
{
    classic_notepad::LargeDocumentLineColumn beforeStart{};
    classic_notepad::LargeDocumentLineColumn beforeEnd{};
    classic_notepad::LargeDocumentLineColumn afterStart{};
    classic_notepad::LargeDocumentLineColumn afterEnd{};
    bool oneLineEdit = false;
    bool modifiedState = false;
    std::uint64_t beforeCharacterCount = 0;
    std::uint64_t afterCharacterCount = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureEditable(errorMessage)) return false;
        if (!document_->PositionToLineColumn(selectionStart_, beforeStart, errorMessage) ||
            !document_->PositionToLineColumn(selectionEnd_, beforeEnd, errorMessage)) return false;
        beforeCharacterCount = document_->CharacterCount();
        if (!document_->Undo(selectionStart_, selectionEnd_, errorMessage)) return false;
        afterCharacterCount = document_->CharacterCount();
        selectionAnchor_ = selectionStart_;
        caretPosition_ = selectionEnd_;
        allSelected_ = false;
        const bool haveAfter = document_->PositionToLineColumn(selectionStart_, afterStart, errorMessage) &&
            document_->PositionToLineColumn(selectionEnd_, afterEnd, errorMessage);
        oneLineEdit = haveAfter && beforeStart.line == beforeEnd.line &&
            afterStart.line == afterEnd.line && beforeStart.line == afterStart.line;
        modifiedState = document_->IsModified();
    }
    modified_.store(modifiedState);
    if (oneLineEdit) RefreshCachedViewportLine(beforeStart.line,
        beforeCharacterCount > afterCharacterCount ? beforeCharacterCount - afterCharacterCount : 0U,
        afterCharacterCount > beforeCharacterCount ? afterCharacterCount - beforeCharacterCount : 0U);
    else InvalidateViewportCache();
    RefreshScrollBars();
    MarkModified(!oneLineEdit);
    if (oneLineEdit) InvalidateDocumentLine(beforeStart.line);
    return true;
}

bool LargeFileView::Find(
    const std::wstring& text,
    bool matchCase,
    bool wholeWord,
    bool forward,
    std::wstring& errorMessage)
{
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (!EnsureEditable(errorMessage)) return false;
    classic_notepad::LargeDocumentMatch match;
    const std::uint64_t start = forward ? selectionEnd_ : selectionStart_;
    if (!document_->Find(text, start, matchCase, wholeWord, forward, match, errorMessage)) return false;
    selectionStart_ = match.start;
    selectionEnd_ = match.start + match.length;
    selectionAnchor_ = selectionStart_;
    caretPosition_ = selectionEnd_;
    allSelected_ = false;
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

std::uint64_t LargeFileView::ReplaceAll(
    const std::wstring& text,
    const std::wstring& replacement,
    bool matchCase,
    bool wholeWord,
    std::wstring& errorMessage)
{
    bool cancelled = false;
    std::uint64_t count = 0;
    bool modifiedState = modified_.load();
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureEditable(errorMessage)) return 0;
        count = document_->ReplaceAll(text, classic_notepad::NormalizeLineEndingsForEditor(replacement), matchCase, wholeWord,
            cancelled, [this] { return cancel_.load(); }, errorMessage);
        modifiedState = document_->IsModified();
    }
    if (count != 0U) {
        StoreMaximum(maximumLineColumns_, VisualColumns(replacement));
        modified_.store(modifiedState);
        RefreshScrollBars();
        MarkModified();
    }
    return count;
}

bool LargeFileView::WriteStreamToTemporaryFile(
    const classic_notepad::LargeDocument& source,
    const std::wstring& temporaryPath,
    classic_notepad::TextEncoding encoding,
    classic_notepad::LineEndingStyle lineEnding,
    std::wstring& errorMessage)
{
    HANDLE output = CreateFileW(
        temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        errorMessage = L"The temporary large-document save file could not be created.";
        return false;
    }
    bool okay = true;
    bool includeBom = true;
    bool pendingCr = false;
    auto writeBytes = [&output, &errorMessage](const std::vector<std::uint8_t>& bytes) {
        std::size_t written = 0;
        while (written < bytes.size()) {
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, (std::numeric_limits<DWORD>::max)()));
            DWORD actual = 0;
            if (!WriteFile(output, bytes.data() + written, request, &actual, nullptr) || actual == 0U) {
                errorMessage = L"The temporary large-document save file could not be written.";
                return false;
            }
            written += actual;
        }
        return true;
    };
    auto emit = [&](const std::wstring& source) {
        std::wstring normalized;
        normalized.reserve(source.size() + 2U);
        const std::wstring ending = lineEnding == classic_notepad::LineEndingStyle::Lf
            ? L"\n" : (lineEnding == classic_notepad::LineEndingStyle::Cr ? L"\r" : L"\r\n");
        for (wchar_t character : source) {
            if (pendingCr) {
                normalized += ending;
                pendingCr = false;
                if (character == L'\n') continue;
            }
            if (character == L'\r') pendingCr = true;
            else if (character == L'\n') normalized += ending;
            else normalized.push_back(character);
        }
        if (normalized.empty()) return true;
        std::vector<std::uint8_t> bytes;
        if (!classic_notepad::EncodeTextChunk(normalized, encoding, includeBom, bytes, errorMessage)) return false;
        includeBom = false;
        saveProgressBytes_.fetch_add(static_cast<std::uint64_t>(bytes.size()));
        return writeBytes(bytes);
    };
    okay = source.StreamText(emit, [this] { return cancel_.load() || operationCancel_.load(); }, errorMessage);
    if (okay && pendingCr) {
        const std::wstring ending = lineEnding == classic_notepad::LineEndingStyle::Lf
            ? L"\n" : (lineEnding == classic_notepad::LineEndingStyle::Cr ? L"\r" : L"\r\n");
        std::vector<std::uint8_t> bytes;
        okay = classic_notepad::EncodeTextChunk(ending, encoding, includeBom, bytes, errorMessage) && writeBytes(bytes);
        if (okay) {
            includeBom = false;
            saveProgressBytes_.fetch_add(static_cast<std::uint64_t>(bytes.size()));
        }
    }
    if (okay && includeBom) {
        std::vector<std::uint8_t> bytes;
        okay = classic_notepad::EncodeTextChunk(L"", encoding, true, bytes, errorMessage) && writeBytes(bytes);
        if (okay) saveProgressBytes_.fetch_add(static_cast<std::uint64_t>(bytes.size()));
    }
    if (okay && !FlushFileBuffers(output)) {
        errorMessage = L"The temporary large-document save file could not be flushed.";
        okay = false;
    }
    CloseHandle(output);
    if (!okay) {
        DeleteFileW(temporaryPath.c_str());
    }
    return okay;
}

bool LargeFileView::Save(
    const std::wstring& path,
    classic_notepad::TextEncoding encoding,
    classic_notepad::LineEndingStyle lineEnding,
    bool rebase,
    std::wstring& errorMessage)
{
    errorMessage.clear();
    if (!EnsureEditable(errorMessage)) return false;
    if (path.empty()) { errorMessage = L"No file path has been selected."; return false; }
    if (!modified_.load() && path == path_) return true;
    saveInProgress_.store(true);
    saveProgressBytes_.store(0);
    const std::wstring temporaryPath = path + L".classic-notepad-save-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
    bool wrote = false;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ != nullptr) wrote = WriteStreamToTemporaryFile(*document_, temporaryPath, encoding, lineEnding, errorMessage);
    }
    if (!wrote) {
        saveInProgress_.store(false);
        return false;
    }
    // Windows can reject MOVEFILE_REPLACE_EXISTING while a mapping still pins
    // the destination. The sibling temporary file is complete and flushed at
    // this point, so this is the first safe point to release the backing file.
    const std::wstring previousPath = path_;
    const std::uint64_t savedTopLine = topLine_;
    StopIndexing();
    CloseMapping();
    if (!AtomicReplacePath(temporaryPath, path)) {
        const DWORD replaceError = GetLastError();
        DeleteFileW(temporaryPath.c_str());
        std::wstring restoreError;
        OpenMapping(previousPath, restoreError);
        saveInProgress_.store(false);
        errorMessage = L"The complete temporary large-document save could not replace the destination (Windows error " +
            std::to_wstring(replaceError) + L").";
        return false;
    }
    modified_.store(false);
    saveInProgress_.store(false);
    if (rebase) {
        {
            std::lock_guard<std::mutex> lock(documentMutex_);
            document_.reset();
            selectionStart_ = 0;
            selectionEnd_ = 0;
        }
        if (!OpenMapping(path, errorMessage)) return false;
        topLine_ = savedTopLine;
        startedTick_ = GetTickCount64();
        StartIndexing();
        path_ = path;
    }
    PostMessageW(parent_, kLargeFileStateChangedMessage, 0, 0);
    return true;
}

bool LargeFileView::GoToLine(std::uint64_t oneBasedLine, std::wstring& errorMessage)
{
    errorMessage.clear();
    if (oneBasedLine == 0U) {
        errorMessage = L"Line number must be at least 1.";
        return false;
    }
    {
        std::lock_guard<std::mutex> documentLock(documentMutex_);
        if (!EnsureEditable(errorMessage)) return false;
        if (oneBasedLine > document_->LineCount()) {
            errorMessage = indexingComplete_.load() ? L"Line number is beyond the end of the file."
                : L"That line is beyond the current loading frontier.";
            return false;
        }
        if (!document_->LineStart(oneBasedLine - 1U, caretPosition_, errorMessage)) return false;
        selectionStart_ = selectionEnd_ = selectionAnchor_ = caretPosition_;
        allSelected_ = false;
    }
    SetTopLine(oneBasedLine - 1U);
    UpdateCaret();
    return true;
}

bool LargeFileView::GetLine(std::uint64_t oneBasedLine, std::wstring& text, std::wstring& errorMessage) const
{
    errorMessage.clear();
    if (oneBasedLine == 0U) {
        errorMessage = L"Line number must be at least 1.";
        return false;
    }
    std::uint64_t start = 0, end = 0, next = 0;
    if (ReadEditedLine(oneBasedLine - 1U, start, end, next, text)) return true;
    bool available = false;
    if (!ReadLine(oneBasedLine - 1U, text, available)) {
        errorMessage = L"The mapped file data could not be read.";
        return false;
    }
    if (!available) {
        errorMessage = L"That line has not been indexed yet.";
        return false;
    }
    return true;
}

LargeFileViewInfo LargeFileView::GetInfo() const
{
    LargeFileViewInfo info;
    info.active = true;
    info.readOnly = false;
    info.editable = false;
    info.modified = modified_.load();
    info.indexingComplete = indexingComplete_.load();
    info.globalScrollReady = info.indexingComplete;
    info.encodingValid = encodingValid_.load();
    info.fileSize = fileSize_;
    info.dataOffset = dataOffset_;
    info.indexedBytes = indexedBytes_.load();
    info.indexedLines = IndexedLineCount();
    info.totalLines = info.indexingComplete ? info.indexedLines : EstimatedTotalLines();
    info.firstUsableMilliseconds = firstUsableMilliseconds_.load();
    info.firstViewportPaintMilliseconds = firstViewportPaintMilliseconds_.load();
    info.fullIndexMilliseconds = fullIndexMilliseconds_.load();
    info.crlfCount = crlfCount_.load();
    info.lfCount = lfCount_.load();
    info.crCount = crCount_.load();
    info.characterCount = characterCount_.load();
    info.topLine = topLine_;
    info.saveProgressBytes = saveProgressBytes_.load();
    info.saveInProgress = saveInProgress_.load();
    info.paintGeneration = paintGeneration_.load();
    info.viewportGeneration = viewportGeneration_;
    info.wordWrap = wordWrap_;
    info.viewportVisualRows = static_cast<std::uint64_t>(viewportRows_.size());
    for (std::size_t index = 1; index < viewportRows_.size(); ++index) {
        if (viewportRows_[index].line == viewportRows_[index - 1U].line) ++info.wrappedContinuationRows;
    }
    info.spellingIssueCount = static_cast<std::uint64_t>(spellingRanges_.size());
    info.maximumLineColumns = maximumLineColumns_.load();
    info.horizontalOffset = horizontalOffset_;
    info.horizontalMaximumOffset = horizontalMaximumOffset_;
    info.caretRectangle = caretRectangle_;
    info.encoding = encoding_;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        info.caret = selectionEnd_;
        info.selectionStart = selectionStart_;
        info.selectionEnd = selectionEnd_;
        if (document_ != nullptr) {
            info.editable = encodingValid_.load();
            info.characterCount = document_->CharacterCount();
            info.totalLines = document_->LineCount();
            info.generation = document_->Generation();
            info.pieceCount = document_->PieceCount();
            info.insertedBufferBytes = document_->InsertedBufferBytes();
            classic_notepad::LargeDocumentLineColumn coordinate{};
            std::wstring coordinateError;
            if (document_->PositionToLineColumn(info.caret, coordinate, coordinateError)) {
                info.caretLine = coordinate.line;
                info.caretColumn = coordinate.column;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(operationMutex_);
        info.operationInProgress = operationInProgress_;
        info.operationCompleted = operationCompleted_;
        info.operationSucceeded = operationSucceeded_;
        info.operationName = operationName_;
        info.operationError = operationError_;
        info.operationTargetPath = operationTargetPath_;
        info.operationGeneration = operationGeneration_;
        info.operationProgress = operationProgress_;
        info.operationResultCount = operationResultCount_;
    }
    return info;
}

bool LargeFileView::SendClickInput(int x, int y, bool shift)
{
    if (window_ == nullptr || !IsWindow(window_)) return false;
    syntheticShift_ = shift;
    const LPARAM point = MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
    SendMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(window_, WM_LBUTTONUP, 0, point);
    syntheticShift_ = false;
    return true;
}

bool LargeFileView::SendDragInput(int startX, int startY, int endX, int endY)
{
    if (window_ == nullptr || !IsWindow(window_)) return false;
    SendMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON,
        MAKELPARAM(static_cast<short>(startX), static_cast<short>(startY)));
    constexpr int steps = 8;
    for (int step = 1; step <= steps; ++step) {
        const int x = startX + (endX - startX) * step / steps;
        const int y = startY + (endY - startY) * step / steps;
        SendMessageW(window_, WM_MOUSEMOVE, MK_LBUTTON,
            MAKELPARAM(static_cast<short>(x), static_cast<short>(y)));
    }
    SendMessageW(window_, WM_LBUTTONUP, 0,
        MAKELPARAM(static_cast<short>(endX), static_cast<short>(endY)));
    return true;
}

bool LargeFileView::SendKeyInput(UINT virtualKey, bool shift, bool control)
{
    if (window_ == nullptr || !IsWindow(window_)) return false;
    syntheticShift_ = shift;
    syntheticControl_ = control;
    SendMessageW(window_, WM_KEYDOWN, virtualKey, 1);
    SendMessageW(window_, WM_KEYUP, virtualKey, 0xC0000001);
    syntheticShift_ = false;
    syntheticControl_ = false;
    return true;
}

bool LargeFileView::SendTextInput(const std::wstring& text)
{
    if (window_ == nullptr || !IsWindow(window_)) return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        wchar_t character = text[index];
        if (character == L'\r' && index + 1U < text.size() && text[index + 1U] == L'\n') ++index;
        if (character == L'\n') character = L'\r';
        SendMessageW(window_, WM_CHAR, character, 1);
    }
    return true;
}

LRESULT CALLBACK LargeFileView::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    LargeFileView* view = reinterpret_cast<LargeFileView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        view = static_cast<LargeFileView*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
        if (view != nullptr) {
            view->window_ = window;
        }
    }
    if (view != nullptr) {
        return view->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT LargeFileView::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB | DLGC_WANTALLKEYS;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC deviceContext = BeginPaint(window_, &paint);
        Paint(deviceContext, paint.rcPaint);
        EndPaint(window_, &paint);
        UpdateCaret();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateViewportCache();
        RefreshScrollBars();
        UpdateCaret();
        return 0;
    case WM_SETFOCUS:
        focused_ = true;
        UpdateCaret();
        NotifyStateChanged();
        return 0;
    case WM_KILLFOCUS:
        focused_ = false;
        DestroyTextCaret();
        return 0;
    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        BeginMouseSelection(point, syntheticShift_ || (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SelectWordAt(point);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (mouseSelecting_) UpdateMouseSelection({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_LBUTTONUP:
        if (mouseSelecting_) {
            UpdateMouseSelection({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            mouseSelecting_ = false;
            KillTimer(window_, kMouseScrollTimer);
            if (GetCapture() == window_) ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        mouseSelecting_ = false;
        KillTimer(window_, kMouseScrollTimer);
        return 0;
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1 && point.y == -1) {
            point = {4, 4}; ClientToScreen(window_, &point);
        }
        ShowContextMenu(point);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_TRACKPOS;
        GetScrollInfo(window_, SB_VERT, &scroll);
        ScrollVertical(LOWORD(wParam), scroll.nTrackPos);
        return 0;
    }
    case WM_HSCROLL: {
        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_TRACKPOS;
        GetScrollInfo(window_, SB_HORZ, &scroll);
        ScrollHorizontal(LOWORD(wParam), scroll.nTrackPos);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const int lines = std::max(1, static_cast<int>(std::abs(delta) / WHEEL_DELTA) * 3);
        for (int index = 0; index < lines; ++index) {
            ScrollVertical(delta > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        const bool control = syntheticControl_ || (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = syntheticShift_ || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (control && wParam == 'A') {
            SelectAll();
            return 0;
        }
        if (control && wParam == 'C') {
            std::wstring error;
            if (!CopySelection(error)) ShowInputError(error);
            return 0;
        }
        if (control && wParam == 'X') {
            std::wstring error;
            if (!CutSelection(error)) ShowInputError(error);
            return 0;
        }
        if (control && wParam == 'V') { SendMessageW(window_, WM_PASTE, 0, 0); return 0; }
        if (control && wParam == 'Z') {
            std::wstring error;
            if (!Undo(error)) ShowInputError(error);
            return 0;
        }
        if (wParam == VK_DELETE) { std::wstring error; if (!DeleteSelection(false, error)) ShowInputError(error); return 0; }
        if (wParam == VK_BACK) { std::wstring error; if (!DeleteSelection(true, error)) ShowInputError(error); return 0; }
        if (wParam == VK_LEFT) { MoveHorizontal(false, control, shift); return 0; }
        if (wParam == VK_RIGHT) { MoveHorizontal(true, control, shift); return 0; }
        if (wParam == VK_UP) { MoveVertical(-1, shift); return 0; }
        if (wParam == VK_DOWN) { MoveVertical(1, shift); return 0; }
        if (wParam == VK_PRIOR) { ScrollCaretPage(-1, shift); return 0; }
        if (wParam == VK_NEXT) { ScrollCaretPage(1, shift); return 0; }
        if (wParam == VK_HOME) { MoveLineEdge(false, control, shift); return 0; }
        if (wParam == VK_END) { MoveLineEdge(true, control, shift); return 0; }
        if (wParam == VK_INSERT && !control && !shift) { overwriteMode_ = !overwriteMode_; return 0; }
        return 0;
    }
    case WM_CHAR:
        if (wParam >= 0x20U || wParam == L'\r' || wParam == L'\n' || wParam == L'\t') {
            const wchar_t unit = static_cast<wchar_t>(wParam);
            if (IsHighSurrogateUnit(unit)) { pendingHighSurrogate_ = unit; return 0; }
            std::wstring text;
            if (pendingHighSurrogate_ != 0) {
                if (!IsLowSurrogateUnit(unit)) { ShowInputError(L"Invalid Unicode keyboard input."); pendingHighSurrogate_ = 0; return 0; }
                text.push_back(pendingHighSurrogate_); pendingHighSurrogate_ = 0;
            }
            if (unit == L'\r' || unit == L'\n') text = L"\r\n";
            else text.push_back(unit);
            std::wstring error;
            if (overwriteMode_ && selectionStart_ == selectionEnd_) {
                std::lock_guard<std::mutex> lock(documentMutex_);
                if (document_ != nullptr && selectionEnd_ < document_->CharacterCount()) {
                    wchar_t character = 0;
                    if (!document_->CharacterAt(selectionEnd_, character, error)) {
                        ShowInputError(error);
                        return 0;
                    }
                    selectionEnd_ += IsHighSurrogateUnit(character) ? 2U : 1U;
                }
            }
            if (!InsertText(text, error)) ShowInputError(error);
            UpdateCaret();
        }
        return 0;
    case WM_PASTE: {
        std::wstring error;
        if (OpenClipboard(window_)) {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            const auto* clipboard = data == nullptr ? nullptr : static_cast<const wchar_t*>(GlobalLock(data));
            if (clipboard != nullptr) {
                if (!PasteText(clipboard, error)) ShowInputError(error);
                GlobalUnlock(data);
            }
            CloseClipboard();
        }
        return 0;
    }
    case WM_CUT: { std::wstring error; CutSelection(error); return 0; }
    case WM_CLEAR: { std::wstring error; DeleteSelection(false, error); return 0; }
    case WM_UNDO: { std::wstring error; Undo(error); return 0; }
    case WM_TIMER:
        if (wParam == kStateNotificationTimer) {
            KillTimer(window_, kStateNotificationTimer);
            if (parent_ != nullptr && IsWindow(parent_)) {
                PostMessageW(parent_, kLargeFileStateChangedMessage, 0, 0);
            }
            return 0;
        }
        if (wParam == kMouseScrollTimer && mouseSelecting_) {
            RECT client{}; GetClientRect(window_, &client);
            if (lastMousePoint_.y < client.top) ScrollVertical(SB_LINEUP, 0);
            else if (lastMousePoint_.y >= client.bottom) ScrollVertical(SB_LINEDOWN, 0);
            UpdateMouseSelection(lastMousePoint_);
            return 0;
        }
        break;
    case kLargeFileWorkerProgressMessage:
        stateNotificationPending_.store(false);
        {
            std::lock_guard<std::mutex> lock(operationMutex_);
            if (operationCompleted_ && operationSucceeded_ && operationName_ == L"Find" && operationResultCount_ != 0U) {
                EnsureCaretVisible();
            }
        }
        RefreshScrollBars();
        InvalidateRect(window_, nullptr, FALSE);
        UpdateCaret();
        if (parent_ != nullptr && IsWindow(parent_)) PostMessageW(parent_, kLargeFileStateChangedMessage, 0, 0);
        return 0;
    case WM_NCDESTROY:
        KillTimer(window_, kMouseScrollTimer);
        KillTimer(window_, kStateNotificationTimer);
        DestroyTextCaret();
        window_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}
