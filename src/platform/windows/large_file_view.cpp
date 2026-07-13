#include "large_file_view.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>

namespace {

constexpr wchar_t kLargeFileViewClass[] = L"ClassicNotepadLargeFileView";
constexpr UINT_PTR kLargeFileViewTimer = 41;
constexpr UINT kLargeFileViewTimerMs = 100;
constexpr std::size_t kIndexChunkBytes = 8U * 1024U * 1024U;
constexpr std::size_t kRenderedLineByteLimit = 64U * 1024U;
constexpr std::size_t kCopiedVisibleTextLimit = 1024U * 1024U;
constexpr std::uint64_t kClipboardCharacterLimit = 16ULL * 1024ULL * 1024ULL;
constexpr UINT kLargeFileStateChangedMessage = WM_APP + 101;

std::uint64_t ElapsedMilliseconds(std::uint64_t startedTick)
{
    return GetTickCount64() - startedTick;
}

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right)
{
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    return right > maximum - left ? maximum : left + right;
}

std::uint64_t MinFileSize(std::uint64_t fileSize, std::uint64_t offset, std::size_t requested)
{
    if (offset >= fileSize) {
        return 0;
    }
    const std::uint64_t remaining = fileSize - offset;
    return std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(requested));
}

} // namespace

LargeFileView::LargeFileView() = default;

LargeFileView::~LargeFileView()
{
    StopIndexing();
    if (window_ != nullptr && IsWindow(window_)) {
        DestroyWindow(window_);
    }
    CloseMapping();
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
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
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
    SetTimer(window_, kLargeFileViewTimer, kLargeFileViewTimerMs, nullptr);
    RefreshScrollBars();
    return true;
}

HWND LargeFileView::Window() const { return window_; }

void LargeFileView::SetBounds(const RECT& bounds)
{
    if (window_ != nullptr) {
        MoveWindow(window_, bounds.left, bounds.top, std::max(0L, bounds.right - bounds.left),
            std::max(0L, bounds.bottom - bounds.top), TRUE);
        UpdateWindow(window_);
    }
}

void LargeFileView::SetTheme(HFONT font, bool darkMode)
{
    font_ = font;
    darkMode_ = darkMode;
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
        InvalidateRect(window_, nullptr, TRUE);
    }
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
        allSelected_ = true;
    }
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

bool LargeFileView::OpenMapping(const std::wstring& path, std::wstring& errorMessage)
{
    errorMessage.clear();
    path_ = path;
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        errorMessage = L"The large file could not be opened for read-only viewing.";
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
        offset = SaturatingAdd(offset, chunk64);
        indexedBytes_.store(offset);
        if (!batch.empty()) {
            std::lock_guard<std::mutex> lock(lineMutex_);
            lineOffsets_.insert(lineOffsets_.end(), batch.begin(), batch.end());
            lineCharacterOffsets_.insert(
                lineCharacterOffsets_.end(), batchCharacterOffsets.begin(), batchCharacterOffsets.end());
            batch.clear();
            batchCharacterOffsets.clear();
            if (firstUsableMilliseconds_.load() == 0U && lineOffsets_.size() >= 2U) {
                firstUsableMilliseconds_.store(ElapsedMilliseconds(startedTick_));
            }
        }
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
        {
            std::wstring ignored;
            if (!BuildDocumentFromIndex(ignored)) {
                encodingValid_.store(false);
            }
        }
        indexingComplete_.store(true);
        fullIndexMilliseconds_.store(ElapsedMilliseconds(startedTick_));
    } else if (!cancel_.load()) {
        encodingValid_.store(false);
        fullIndexMilliseconds_.store(ElapsedMilliseconds(startedTick_));
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
    constexpr std::uint64_t targetPieceBytes = 8ULL * 1024ULL * 1024ULL;
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
        pieces.push_back({ startByte, endByte - startByte, endCharacter - startCharacter });
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
    selectionStart_ = 0;
    selectionEnd_ = 0;
    return true;
}

bool LargeFileView::EnsureEditable(std::wstring& errorMessage) const
{
    if (!encodingValid_.load()) {
        errorMessage = L"The large file has invalid or unsupported text encoding.";
        return false;
    }
    if (!indexingComplete_.load()) {
        errorMessage = L"Large-file indexing is still in progress. Please try the edit again shortly.";
        return false;
    }
    if (document_ == nullptr) {
        errorMessage = L"The large-document model is not ready.";
        return false;
    }
    return true;
}

void LargeFileView::MarkModified()
{
    modified_.store(true);
    if (parent_ != nullptr && IsWindow(parent_)) {
        PostMessageW(parent_, kLargeFileStateChangedMessage, 0, 0);
    }
    InvalidateRect(window_, nullptr, FALSE);
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
    vertical.nPage = static_cast<UINT>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>((std::numeric_limits<UINT>::max)()),
        std::max<std::uint64_t>(1U, page * static_cast<std::uint64_t>(vertical.nMax) / std::max<std::uint64_t>(1U, total))));
    vertical.nPos = LineToPosition(topLine_);
    SetScrollInfo(window_, SB_VERT, &vertical, TRUE);

    SCROLLINFO horizontal{};
    horizontal.cbSize = sizeof(horizontal);
    horizontal.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    horizontal.nMin = 0;
    horizontal.nMax = 32768;
    horizontal.nPage = 512;
    horizontal.nPos = horizontalOffset_;
    SetScrollInfo(window_, SB_HORZ, &horizontal, TRUE);
}

void LargeFileView::SetTopLine(std::uint64_t line)
{
    const std::uint64_t total = EstimatedTotalLines();
    topLine_ = total == 0U ? 0U : std::min(line, total - 1U);
    RefreshScrollBars();
    InvalidateRect(window_, nullptr, FALSE);
}

void LargeFileView::ScrollVertical(int command, int trackPosition)
{
    const std::uint64_t page = VisibleLineCount();
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
    switch (command) {
    case SB_LINELEFT: horizontalOffset_ = std::max(0, horizontalOffset_ - 16); break;
    case SB_LINERIGHT: horizontalOffset_ = std::min(32768, horizontalOffset_ + 16); break;
    case SB_PAGELEFT: horizontalOffset_ = std::max(0, horizontalOffset_ - 256); break;
    case SB_PAGERIGHT: horizontalOffset_ = std::min(32768, horizontalOffset_ + 256); break;
    case SB_LEFT: horizontalOffset_ = 0; break;
    case SB_RIGHT: horizontalOffset_ = 32768; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: horizontalOffset_ = std::clamp(trackPosition, 0, 32768); break;
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
        return L"Read-only large-file mode";
    }
    const std::uint64_t size = std::max<std::uint64_t>(1U, fileSize_);
    const long double percentValue = static_cast<long double>(indexedBytes_.load()) * 100.0L /
        static_cast<long double>(size);
    const std::uint64_t percent = std::min<std::uint64_t>(100U, static_cast<std::uint64_t>(percentValue));
    return L"Read-only large-file mode — indexing " + std::to_wstring(percent) + L"%";
}

void LargeFileView::Paint(HDC deviceContext)
{
    RECT client{};
    GetClientRect(window_, &client);
    const COLORREF background = darkMode_ ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW);
    const COLORREF textColor = darkMode_ ? RGB(242, 242, 242) : GetSysColor(COLOR_WINDOWTEXT);
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(deviceContext, &client, brush);
    DeleteObject(brush);

    HGDIOBJ previousFont = SelectObject(deviceContext, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(deviceContext, textColor);
    SetBkMode(deviceContext, TRANSPARENT);
    TEXTMETRICW metrics{};
    GetTextMetricsW(deviceContext, &metrics);
    const int lineHeight = std::max(1, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
    const std::uint64_t visible = VisibleLineCount() + 4U;
    int y = 0;
    bool showedAny = false;
    for (std::uint64_t index = 0; index < visible && y < client.bottom; ++index, y += lineHeight) {
        const std::uint64_t line = SaturatingAdd(topLine_, index);
        std::wstring text;
        bool available = false;
        if (!ReadLine(line, text, available)) {
            text = L"Unable to read mapped file data.";
            available = true;
        }
        if (!available) {
            if (!showedAny) {
                text = IndexingMessage();
                TextOutW(deviceContext, 4 - horizontalOffset_, y, text.c_str(), static_cast<int>(text.size()));
            }
            break;
        }
        showedAny = true;
        if (allSelected_) {
            RECT lineRect{ 0, y, client.right, y + lineHeight };
            HBRUSH selectionBrush = CreateSolidBrush(darkMode_ ? RGB(54, 84, 120) : RGB(204, 232, 255));
            FillRect(deviceContext, &lineRect, selectionBrush);
            DeleteObject(selectionBrush);
        }
        TextOutW(deviceContext, 4 - horizontalOffset_, y, text.c_str(), static_cast<int>(text.size()));
    }
    if (!showedAny) {
        const std::wstring message = IndexingMessage();
        TextOutW(deviceContext, 4, 4, message.c_str(), static_cast<int>(message.size()));
    }
    SelectObject(deviceContext, previousFont);
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
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (!EnsureEditable(errorMessage)) return false;
    std::uint64_t afterStart = 0;
    std::uint64_t afterEnd = 0;
    if (!document_->Replace(selectionStart_, selectionEnd_, text, selectionStart_, selectionEnd_, afterStart, afterEnd, errorMessage)) return false;
    selectionStart_ = afterStart;
    selectionEnd_ = afterEnd;
    allSelected_ = false;
    MarkModified();
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
                --selectionStart_;
            } else if (selectionEnd_ < document_->CharacterCount()) {
                ++selectionEnd_;
            } else {
                return true;
            }
        }
    }
    return ReplaceSelection(L"", errorMessage);
}

bool LargeFileView::Undo(std::wstring& errorMessage)
{
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (!EnsureEditable(errorMessage)) return false;
    if (!document_->Undo(selectionStart_, selectionEnd_, errorMessage)) return false;
    allSelected_ = false;
    MarkModified();
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
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (!EnsureEditable(errorMessage)) return 0;
    bool cancelled = false;
    const std::uint64_t count = document_->ReplaceAll(text, classic_notepad::NormalizeLineEndingsForEditor(replacement), matchCase, wholeWord,
        cancelled, [this] { return cancel_.load(); }, errorMessage);
    if (count != 0U) MarkModified();
    return count;
}

bool LargeFileView::WriteStreamToTemporaryFile(
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
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureEditable(errorMessage)) okay = false;
        else okay = document_->StreamText(emit, [this] { return cancel_.load(); }, errorMessage);
    }
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
    if (!WriteStreamToTemporaryFile(temporaryPath, encoding, lineEnding, errorMessage)) {
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
    if (!MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporaryPath.c_str());
        std::wstring restoreError;
        OpenMapping(previousPath, restoreError);
        saveInProgress_.store(false);
        errorMessage = L"The complete temporary large-document save could not replace the destination.";
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
    const std::uint64_t total = EstimatedTotalLines();
    if (indexingComplete_.load() && oneBasedLine > total) {
        errorMessage = L"Line number is beyond the end of the file.";
        return false;
    }
    SetTopLine(oneBasedLine - 1U);
    {
        std::lock_guard<std::mutex> lineLock(lineMutex_);
        if (oneBasedLine - 1U < lineCharacterOffsets_.size()) {
            std::lock_guard<std::mutex> documentLock(documentMutex_);
            selectionStart_ = lineCharacterOffsets_[static_cast<std::size_t>(oneBasedLine - 1U)];
            selectionEnd_ = selectionStart_;
            allSelected_ = false;
        }
    }
    return true;
}

bool LargeFileView::GetLine(std::uint64_t oneBasedLine, std::wstring& text, std::wstring& errorMessage) const
{
    errorMessage.clear();
    if (oneBasedLine == 0U) {
        errorMessage = L"Line number must be at least 1.";
        return false;
    }
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
    info.editable = indexingComplete_.load() && encodingValid_.load();
    info.modified = modified_.load();
    info.indexingComplete = indexingComplete_.load();
    info.encodingValid = encodingValid_.load();
    info.fileSize = fileSize_;
    info.dataOffset = dataOffset_;
    info.indexedBytes = indexedBytes_.load();
    info.indexedLines = IndexedLineCount();
    info.totalLines = info.indexingComplete ? info.indexedLines : EstimatedTotalLines();
    info.firstUsableMilliseconds = firstUsableMilliseconds_.load();
    info.fullIndexMilliseconds = fullIndexMilliseconds_.load();
    info.crlfCount = crlfCount_.load();
    info.lfCount = lfCount_.load();
    info.crCount = crCount_.load();
    info.characterCount = characterCount_.load();
    info.topLine = topLine_;
    info.saveProgressBytes = saveProgressBytes_.load();
    info.saveInProgress = saveInProgress_.load();
    info.encoding = encoding_;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        info.caret = selectionEnd_;
        info.selectionStart = selectionStart_;
        info.selectionEnd = selectionEnd_;
        if (document_ != nullptr) {
            info.characterCount = document_->CharacterCount();
            info.generation = document_->Generation();
            info.pieceCount = document_->PieceCount();
            info.insertedBufferBytes = document_->InsertedBufferBytes();
        }
    }
    return info;
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
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC deviceContext = BeginPaint(window_, &paint);
        Paint(deviceContext);
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        RefreshScrollBars();
        return 0;
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
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wParam == 'A') {
            SelectAll();
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wParam == 'C') {
            std::wstring error;
            CopySelection(error);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wParam == 'X') {
            std::wstring error;
            CutSelection(error);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wParam == 'Z') {
            std::wstring error;
            Undo(error);
            return 0;
        }
        if (wParam == VK_DELETE) { std::wstring error; DeleteSelection(false, error); return 0; }
        if (wParam == VK_BACK) { std::wstring error; DeleteSelection(true, error); return 0; }
        if (wParam == VK_UP) { ScrollVertical(SB_LINEUP, 0); return 0; }
        if (wParam == VK_DOWN) { ScrollVertical(SB_LINEDOWN, 0); return 0; }
        if (wParam == VK_PRIOR) { ScrollVertical(SB_PAGEUP, 0); return 0; }
        if (wParam == VK_NEXT) { ScrollVertical(SB_PAGEDOWN, 0); return 0; }
        if (wParam == VK_HOME) { ScrollVertical(SB_TOP, 0); return 0; }
        if (wParam == VK_END) { ScrollVertical(SB_BOTTOM, 0); return 0; }
        return 0;
    case WM_CHAR:
        if (wParam >= 0x20U || wParam == L'\r' || wParam == L'\n' || wParam == L'\t') {
            std::wstring text(1, static_cast<wchar_t>(wParam));
            std::wstring error;
            InsertText(text, error);
        }
        return 0;
    case WM_PASTE: {
        std::wstring error;
        if (OpenClipboard(window_)) {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            const auto* clipboard = data == nullptr ? nullptr : static_cast<const wchar_t*>(GlobalLock(data));
            if (clipboard != nullptr) {
                PasteText(clipboard, error);
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
        if (wParam == kLargeFileViewTimer) {
            RefreshScrollBars();
            InvalidateRect(window_, nullptr, FALSE);
            if (parent_ != nullptr && IsWindow(parent_)) {
                SendMessageW(parent_, kLargeFileStateChangedMessage, 0, 0);
            }
            return 0;
        }
        break;
    case WM_NCDESTROY:
        KillTimer(window_, kLargeFileViewTimer);
        window_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}
