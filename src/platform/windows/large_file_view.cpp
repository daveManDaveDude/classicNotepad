#include "large_file_view.h"
#include "resource.h"
#include "windows_mapped_source.h"
#include "windows_text_boundary.h"
#include "../../editor_text_semantics.h"
#include "../../large_file_scroll.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <new>
#include <stdexcept>
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
constexpr std::uint64_t kViewportContextCharacters = 128U;
constexpr std::size_t kVisibleSpellWindowLimit = 96U * 1024U;
constexpr std::size_t kCopiedVisibleTextLimit = 1024U * 1024U;
constexpr std::uint64_t kClipboardCharacterLimit = 16ULL * 1024ULL * 1024ULL;
constexpr UINT kLargeFileStateChangedMessage = WM_APP + 101;
constexpr UINT kLargeFileWorkerProgressMessage = WM_APP + 102;
constexpr UINT kLargeFileOperationCompletedMessage = WM_APP + 103;
std::atomic<std::uint64_t> gNextLargeFileOperationToken{1};

template<class Cleanup>
class ScopeExit {
public:
    explicit ScopeExit(Cleanup cleanup) : cleanup_(std::move(cleanup)) {}
    ~ScopeExit() noexcept { cleanup_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
private:
    Cleanup cleanup_;
};

template<class Cleanup>
ScopeExit<Cleanup> OnScopeExit(Cleanup cleanup)
{
    return ScopeExit<Cleanup>(std::move(cleanup));
}

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

bool IsHighSurrogateUnit(wchar_t value) { return value >= 0xD800 && value <= 0xDBFF; }
bool IsLowSurrogateUnit(wchar_t value) { return value >= 0xDC00 && value <= 0xDFFF; }

bool ReadScalarSafeViewportRange(
    const classic_notepad::LargeDocument& document,
    std::uint64_t& start,
    std::uint64_t maxCharacters,
    std::wstring& text,
    std::wstring& errorMessage)
{
    text.clear();
    errorMessage.clear();
    if (maxCharacters == 0U) return true;
    const std::uint64_t characterCount = document.CharacterCount();
    if (start > characterCount) {
        errorMessage = L"The viewport range is outside the large document.";
        return false;
    }

    if (start < characterCount) {
        std::wstring first;
        if (!document.ReadRange(start, 1U, first, errorMessage)) return false;
        if (!first.empty() && IsLowSurrogateUnit(first.front())) {
            if (start == 0U) {
                errorMessage = L"The viewport starts at an unmatched Unicode surrogate.";
                return false;
            }
            std::wstring previous;
            if (!document.ReadRange(start - 1U, 1U, previous, errorMessage)) return false;
            if (previous.size() != 1U || !IsHighSurrogateUnit(previous.front())) {
                errorMessage = L"The viewport starts at an unmatched Unicode surrogate.";
                return false;
            }
            --start;
            if (maxCharacters != (std::numeric_limits<std::uint64_t>::max)()) ++maxCharacters;
        }
    }

    const std::uint64_t available = characterCount - start;
    const std::uint64_t requested = std::min(maxCharacters, available);
    if (!document.ReadRange(start, requested, text, errorMessage)) return false;
    if (!text.empty() && IsHighSurrogateUnit(text.back())) {
        const std::uint64_t nextPosition = start + static_cast<std::uint64_t>(text.size());
        if (nextPosition >= characterCount) {
            errorMessage = L"The viewport ends at an unmatched Unicode surrogate.";
            return false;
        }
        std::wstring next;
        if (!document.ReadRange(nextPosition, 1U, next, errorMessage)) return false;
        if (next.size() != 1U || !IsLowSurrogateUnit(next.front())) {
            errorMessage = L"The viewport ends at an unmatched Unicode surrogate.";
            return false;
        }
        text.push_back(next.front());
    }
    return true;
}

struct TextLayout {
    std::wstring displayText;
    std::vector<std::size_t> displayBoundary;
    std::vector<int> xBoundary;
    std::vector<std::uint8_t> scalarBoundary;
};

TextLayout LayoutText(HDC dc, const std::wstring& text, std::uint64_t startingColumn = 0U)
{
    constexpr std::size_t tabColumns = 8U;
    TextLayout result;
    result.displayBoundary.resize(text.size() + 1U);
    result.xBoundary.resize(text.size() + 1U);
    result.scalarBoundary.assign(text.size() + 1U, 1U);
    result.displayText.reserve(text.size());
    std::size_t column = static_cast<std::size_t>(startingColumn % tabColumns);
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
    for (std::size_t index = 1; index < text.size(); ++index) {
        if (IsHighSurrogateUnit(text[index - 1U]) && IsLowSurrogateUnit(text[index])) {
            result.scalarBoundary[index] = 0U;
            result.xBoundary[index] = result.xBoundary[index - 1U];
        }
    }
    return result;
}

std::size_t ClosestTextBoundary(const TextLayout& layout, int wanted)
{
    return classic_notepad::windows::ClosestTextBoundary(
        layout.xBoundary, layout.scalarBoundary, wanted);
}

std::uint64_t ScaleRatio(std::uint64_t value, std::uint64_t numerator, std::uint64_t denominator)
{
    if (value == 0U || numerator == 0U || denominator == 0U) return 0U;
    const std::uint64_t quotient = value / denominator;
    const std::uint64_t remainder = value % denominator;
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    if (quotient != 0U && numerator > maximum / quotient) return maximum;
    const std::uint64_t whole = quotient * numerator;
    if (remainder != 0U && numerator > maximum / remainder) {
        const long double fraction = static_cast<long double>(remainder) *
            static_cast<long double>(numerator) / static_cast<long double>(denominator);
        const std::uint64_t partial = fraction >= static_cast<long double>(maximum)
            ? maximum : static_cast<std::uint64_t>(fraction);
        return partial > maximum - whole ? maximum : whole + partial;
    }
    const std::uint64_t partial = (remainder * numerator) / denominator;
    return partial > maximum - whole ? maximum : whole + partial;
}

int ClampToInt(std::int64_t value);

std::uint64_t LogicalColumnAtHorizontalOffset(
    std::uint64_t lineLength, int horizontalOffset, int horizontalMaximumOffset)
{
    if (lineLength == 0U || horizontalMaximumOffset <= 0) return 0U;
    const std::uint64_t column = ScaleRatio(lineLength,
        static_cast<std::uint64_t>(std::clamp(horizontalOffset, 0, horizontalMaximumOffset)),
        static_cast<std::uint64_t>(horizontalMaximumOffset));
    return std::min(lineLength, column);
}

int HorizontalOffsetAtLogicalColumn(
    std::uint64_t lineLength, std::uint64_t column, int horizontalMaximumOffset)
{
    if (lineLength == 0U || horizontalMaximumOffset <= 0) return 0;
    const std::uint64_t offset = ScaleRatio(
        static_cast<std::uint64_t>(horizontalMaximumOffset), std::min(column, lineLength), lineLength);
    return static_cast<int>(std::min<std::uint64_t>(
        offset, static_cast<std::uint64_t>(horizontalMaximumOffset)));
}

int HorizontalAnchorViewportX(int clientWidth, int horizontalOffset, int horizontalMaximumOffset)
{
    const std::uint64_t available = static_cast<std::uint64_t>(std::max(0, clientWidth - 8));
    const std::uint64_t offset = static_cast<std::uint64_t>(
        std::clamp(horizontalOffset, 0, std::max(0, horizontalMaximumOffset)));
    const std::uint64_t maximum = static_cast<std::uint64_t>(std::max(0, horizontalMaximumOffset));
    return ClampToInt(4LL + static_cast<std::int64_t>(ScaleRatio(available, offset, maximum)));
}

int ClampToInt(std::int64_t value)
{
    return static_cast<int>(std::clamp<std::int64_t>(value,
        (std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)()));
}

void WaitAtSaveSnapshotTestGate()
{
    wchar_t enabled[8]{};
    if (GetEnvironmentVariableW(L"CLASSIC_NOTEPAD_TEST_SAVE_GATE", enabled,
            static_cast<DWORD>(std::size(enabled))) == 0U || enabled[0] == L'0') {
        return;
    }
    const std::wstring base = L"Local\\ClassicNotepadA2SaveGate-" + std::to_wstring(GetCurrentProcessId());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, (base + L"-ready").c_str());
    HANDLE proceed = CreateEventW(nullptr, TRUE, FALSE, (base + L"-proceed").c_str());
    if (ready == nullptr || proceed == nullptr) {
        if (ready != nullptr) CloseHandle(ready);
        if (proceed != nullptr) CloseHandle(proceed);
        return;
    }
    SetEvent(ready);
    // This opt-in synchronization point exists only for deterministic Windows
    // regression coverage. It is inert unless the test environment enables it.
    WaitForSingleObject(proceed, 30000U);
    CloseHandle(proceed);
    CloseHandle(ready);
}

void WaitAtOperationTestGate(const wchar_t* operation)
{
    wchar_t configured[32]{};
    if (GetEnvironmentVariableW(L"CLASSIC_NOTEPAD_TEST_OPERATION_GATE", configured,
            static_cast<DWORD>(std::size(configured))) == 0U || std::wstring(configured) != operation) {
        return;
    }
    const std::wstring base = L"Local\\ClassicNotepadA2OperationGate-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + operation;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, (base + L"-ready").c_str());
    HANDLE proceed = CreateEventW(nullptr, TRUE, FALSE, (base + L"-proceed").c_str());
    if (ready == nullptr || proceed == nullptr) {
        if (ready != nullptr) CloseHandle(ready);
        if (proceed != nullptr) CloseHandle(proceed);
        return;
    }
    SetEvent(ready);
    WaitForSingleObject(proceed, 30000U);
    CloseHandle(proceed);
    CloseHandle(ready);
}

void WaitAtOperationAdoptionTestGate(const wchar_t* operation)
{
    wchar_t configured[32]{};
    if (GetEnvironmentVariableW(L"CLASSIC_NOTEPAD_TEST_ADOPTION_GATE", configured,
            static_cast<DWORD>(std::size(configured))) == 0U ||
        (std::wstring(configured) != operation && std::wstring(configured) != L"all")) {
        return;
    }
    const std::wstring base = L"Local\\ClassicNotepadA4AdoptionGate-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + operation;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, (base + L"-ready").c_str());
    HANDLE proceed = CreateEventW(nullptr, TRUE, FALSE, (base + L"-proceed").c_str());
    if (ready == nullptr || proceed == nullptr) {
        if (ready != nullptr) CloseHandle(ready);
        if (proceed != nullptr) CloseHandle(proceed);
        return;
    }
    SetEvent(ready);
    WaitForSingleObject(proceed, 30000U);
    CloseHandle(proceed);
    CloseHandle(ready);
}

void ReleaseOperationTestGatesForTeardown()
{
    const std::wstring process = std::to_wstring(GetCurrentProcessId());
    const auto release = [](const std::wstring& name) {
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
        if (event != nullptr) {
            SetEvent(event);
            CloseHandle(event);
        }
    };
    release(L"Local\\ClassicNotepadA2SaveGate-" + process + L"-proceed");
    for (const wchar_t* operation : {L"find", L"replace", L"save"}) {
        release(L"Local\\ClassicNotepadA2OperationGate-" + process + L"-" + operation + L"-proceed");
        release(L"Local\\ClassicNotepadA4AdoptionGate-" + process + L"-" + operation + L"-proceed");
    }
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
    ReleaseOperationTestGatesForTeardown();
    if (operationThread_.joinable()) operationThread_.join();
    std::lock_guard<std::mutex> lock(operationMutex_);
    operationInProgress_ = false;
    pendingOperationCompletion_.reset();
}

void LargeFileView::CancelOperation()
{
    operationCancel_.store(true);
}

void LargeFileView::AcknowledgeOperationCompletion(std::uint64_t token)
{
    std::lock_guard<std::mutex> lock(operationMutex_);
    if (operationToken_ == token && operationCompleted_) {
        operationInProgress_ = false;
    }
}

bool LargeFileView::StartFindAsync(
    const std::wstring& text, bool matchCase, bool wholeWord, bool forward, std::wstring& errorMessage)
{
    if (text.empty()) { errorMessage = L"The search text is empty."; return false; }
    if (operationThread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(operationMutex_);
            if (operationInProgress_) { errorMessage = L"Another large-document operation is already running."; return false; }
        }
        operationThread_.join();
    }
    std::unique_ptr<classic_notepad::LargeDocument> snapshot;
    std::uint64_t generation = 0;
    std::uint64_t start = 0;
    std::uint64_t selectionRevision = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureCompleteDocument(errorMessage)) return false;
        snapshot = document_->Clone();
        generation = document_->Generation();
        start = forward ? selectionEnd_ : selectionStart_;
        selectionRevision = selectionRevision_;
    }
    std::uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(operationMutex_);
        operationInProgress_ = true; operationCompleted_ = false; operationSucceeded_ = false;
        operationName_ = L"Find"; operationError_.clear(); operationGeneration_ = generation;
        token = operationToken_ = gNextLargeFileOperationToken.fetch_add(1U);
        pendingOperationCompletion_.reset();
        operationTargetPath_.clear();
        operationProgress_ = 0; operationResultCount_ = 0;
    }
    operationCancel_.store(false);
    const HWND completionWindow = window_;
    operationThread_ = std::thread([this, snapshot = std::move(snapshot), text, matchCase, wholeWord,
        forward, start, generation, selectionRevision, token, completionWindow]() mutable {
        WaitAtOperationTestGate(L"find");
        classic_notepad::LargeDocumentMatch match;
        std::wstring error;
        const bool found = snapshot->Find(text, start, matchCase, wholeWord, forward, match, error,
            [this] { return operationCancel_.load(); },
            [this](std::uint64_t progress) {
                std::lock_guard<std::mutex> lock(operationMutex_);
                operationProgress_ = progress;
            });
        auto completion = std::make_unique<OperationCompletion>();
        completion->kind = OperationKind::Find;
        completion->token = token;
        completion->documentGeneration = generation;
        completion->selectionRevision = selectionRevision;
        completion->succeeded = !operationCancel_.load() && error.empty();
        completion->found = found;
        completion->cancelled = operationCancel_.load();
        completion->resultCount = found ? 1U : 0U;
        completion->match = match;
        completion->error = completion->cancelled ? L"The operation was cancelled." : error;
        WaitAtOperationAdoptionTestGate(L"find");
        PublishOperationCompletion(std::move(completion), completionWindow);
    });
    return true;
}

bool LargeFileView::StartReplaceAllAsync(
    const std::wstring& text, const std::wstring& replacement,
    bool matchCase, bool wholeWord, std::wstring& errorMessage)
{
    if (text.empty()) { errorMessage = L"The replacement text is empty."; return false; }
    if (operationThread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(operationMutex_);
            if (operationInProgress_) { errorMessage = L"Another large-document operation is already running."; return false; }
        }
        operationThread_.join();
    }
    std::unique_ptr<classic_notepad::LargeDocument> snapshot;
    std::uint64_t generation = 0;
    std::uint64_t selectionRevision = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureCompleteDocument(errorMessage)) return false;
        snapshot = document_->Clone(); generation = document_->Generation();
        selectionRevision = selectionRevision_;
    }
    std::uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(operationMutex_);
        operationInProgress_ = true; operationCompleted_ = false; operationSucceeded_ = false;
        operationName_ = L"Replace All"; operationError_.clear(); operationGeneration_ = generation;
        token = operationToken_ = gNextLargeFileOperationToken.fetch_add(1U);
        pendingOperationCompletion_.reset();
        operationTargetPath_.clear();
        operationProgress_ = 0; operationResultCount_ = 0;
    }
    operationCancel_.store(false);
    const HWND completionWindow = window_;
    const std::wstring normalized = classic_notepad::NormalizeLineEndingsForEditor(replacement);
    operationThread_ = std::thread([this, snapshot = std::move(snapshot), text, normalized, matchCase,
        wholeWord, generation, selectionRevision, token, completionWindow]() mutable {
        WaitAtOperationTestGate(L"replace");
        bool cancelled = false; std::wstring error;
        const std::uint64_t count = snapshot->ReplaceAll(text, normalized, matchCase, wholeWord,
            cancelled, [this] { return operationCancel_.load(); }, error,
            [this](std::uint64_t progress) {
                std::lock_guard<std::mutex> lock(operationMutex_);
                operationProgress_ = progress;
            });
        cancelled = cancelled || operationCancel_.load();
        auto completion = std::make_unique<OperationCompletion>();
        completion->kind = OperationKind::ReplaceAll;
        completion->token = token;
        completion->documentGeneration = generation;
        completion->selectionRevision = selectionRevision;
        completion->succeeded = !cancelled && error.empty();
        completion->cancelled = cancelled;
        completion->resultCount = count;
        completion->candidateDocument = std::move(snapshot);
        completion->error = cancelled ? L"The operation was cancelled." : error;
        WaitAtOperationAdoptionTestGate(L"replace");
        PublishOperationCompletion(std::move(completion), completionWindow);
    });
    return true;
}

bool LargeFileView::StartSaveAsync(
    const std::wstring& path, classic_notepad::TextEncoding encoding,
    classic_notepad::LineEndingStyle lineEnding, bool rebase, std::wstring& errorMessage)
{
    if (path.empty()) { errorMessage = L"No file path has been selected."; return false; }
    if (operationThread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(operationMutex_);
            if (operationInProgress_) { errorMessage = L"Another large-document operation is already running."; return false; }
        }
        operationThread_.join();
    }
    std::unique_ptr<classic_notepad::LargeDocument> snapshot;
    std::uint64_t generation = 0;
    std::uint64_t selectionRevision = 0;
    bool targetWasCurrentPath = false;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureCompleteDocument(errorMessage)) return false;
        snapshot = document_->Clone(); generation = document_->Generation();
        selectionRevision = selectionRevision_;
        targetWasCurrentPath = path == path_;
    }
    std::uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(operationMutex_);
        operationInProgress_ = true; operationCompleted_ = false; operationSucceeded_ = false;
        operationName_ = L"Save"; operationError_.clear(); operationGeneration_ = generation;
        token = operationToken_ = gNextLargeFileOperationToken.fetch_add(1U);
        pendingOperationCompletion_.reset();
        operationTargetPath_ = path;
        saveDiagnostics_ = {};
        operationProgress_ = 0; operationResultCount_ = 0;
    }
    operationCancel_.store(false); saveInProgress_.store(true); saveProgressBytes_.store(0);
    const HWND completionWindow = window_;
    operationThread_ = std::thread([this, snapshot = std::move(snapshot), path,
        encoding, lineEnding, rebase, generation, selectionRevision, targetWasCurrentPath, token,
        completionWindow]() mutable {
        std::wstring error;
        classic_notepad::WindowsSaveTransaction transaction;
        bool okay = transaction.Create(path);
        if (okay) okay = WriteStreamToTemporaryFile(*snapshot, transaction, encoding, lineEnding, error);
        if (!okay && error.empty()) error = transaction.ErrorMessage();
        bool cancelled = operationCancel_.load();
        if (okay && !cancelled) {
            WaitAtSaveSnapshotTestGate();
            cancelled = operationCancel_.load();
        }
        cancelled = cancelled || operationCancel_.load();
        auto completion = std::make_unique<OperationCompletion>();
        completion->kind = OperationKind::Save;
        completion->token = token;
        completion->documentGeneration = generation;
        completion->selectionRevision = selectionRevision;
        completion->succeeded = okay && !cancelled;
        completion->cancelled = cancelled;
        completion->resultCount = okay && !cancelled ? 1U : 0U;
        completion->error = cancelled ? L"The operation was cancelled." : error;
        completion->targetPath = path;
        completion->temporaryPath = transaction.TemporaryPath();
        completion->targetWasCurrentPath = targetWasCurrentPath;
        completion->rebase = rebase;
        completion->saveEncoding = encoding;
        completion->saveLineEnding = lineEnding;
        if (!okay || cancelled) {
            transaction.DeleteTemporary();
        }
        completion->saveDiagnostics = transaction.Diagnostics();
        WaitAtOperationAdoptionTestGate(L"save");
        PublishOperationCompletion(std::move(completion), completionWindow);
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
    std::wstring& errorMessage,
    bool initiallyVisible,
    std::uint64_t sessionId)
{
    instance_ = instance;
    parent_ = parent;
    font_ = font;
    darkMode_ = darkMode;
    sessionId_ = sessionId == 0U ? 1U : sessionId;
    if (!RegisterClass(instance_) || !OpenMapping(path, errorMessage)) {
        return false;
    }

    window_ = CreateWindowExW(
        darkMode_ ? 0 : WS_EX_CLIENTEDGE,
        kLargeFileViewClass,
        nullptr,
        WS_CHILD | (initiallyVisible ? WS_VISIBLE : 0) | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
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

bool LargeFileView::WaitForCompleteIndex(std::wstring& errorMessage)
{
    errorMessage.clear();
    if (indexThread_.joinable()) {
        HANDLE threadHandle = indexThread_.native_handle();
        for (;;) {
            const DWORD waitResult = MsgWaitForMultipleObjects(
                1U, &threadHandle, FALSE, 50U, QS_PAINT | QS_TIMER);
            if (waitResult == WAIT_OBJECT_0) break;
            if (waitResult == WAIT_FAILED) {
                errorMessage = L"The large file could not be fully validated (Windows error " +
                    std::to_wstring(GetLastError()) + L").";
                StopIndexing();
                return false;
            }
            if (waitResult == WAIT_OBJECT_0 + 1U) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, WM_PAINT, WM_PAINT, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                while (PeekMessageW(&message, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE)) {
                    DispatchMessageW(&message);
                }
            }
        }
        indexThread_.join();
    }

    if (!encodingValid_.load()) {
        errorMessage = L"The large file contains malformed Unicode, an embedded NUL, or unsupported text encoding.";
        return false;
    }
    if (!sourceReady_.load() || !indexingComplete_.load() || indexedBytes_.load() != fileSize_) {
        errorMessage = L"The large file could not be fully validated and indexed.";
        return false;
    }
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

bool LargeFileView::SetViewportSizeForTesting(int width, int height)
{
    if (window_ == nullptr || width <= 0 || height <= 0) return false;
    RECT adjusted{0, 0, width, height};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
    const DWORD extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE));
    if (!AdjustWindowRectEx(&adjusted, style, FALSE, extendedStyle)) return false;
    return SetWindowPos(window_, nullptr, 0, 0, adjusted.right - adjusted.left, adjusted.bottom - adjusted.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
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
        NotifyStateChanged();
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
    RebuildViewportCache(VisibleLineCount() + 2U);
    if (viewportRows_.empty() || !viewportRows_.front().available) return false;
    rangeStart = viewportRows_.front().segmentStart;
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (document_ == nullptr || rangeStart > document_->CharacterCount()) return false;
    return ReadScalarSafeViewportRange(*document_, rangeStart,
        std::min<std::uint64_t>(document_->CharacterCount() - rangeStart, kRenderedLineByteLimit),
        text, errorMessage) && !text.empty();
}

bool LargeFileView::GetVisibleTextWindows(
    std::vector<LargeFileVisibleTextWindow>& windows,
    std::wstring& errorMessage)
{
    windows.clear();
    errorMessage.clear();
    RebuildViewportCache(VisibleLineCount() + 2U);
    const std::size_t visibleRows = static_cast<std::size_t>(std::min<std::uint64_t>(
        VisibleLineCount(), static_cast<std::uint64_t>(viewportRows_.size())));
    std::lock_guard<std::mutex> documentLock(documentMutex_);
    if (document_ == nullptr || document_->Generation() != viewportCacheGeneration_ ||
        viewportCacheLayoutRevision_ != layoutRevision_) {
        errorMessage = L"The visible spelling window changed while it was being read.";
        return false;
    }
    windows.reserve(visibleRows);
    std::size_t remainingUnits = kVisibleSpellWindowLimit;
    for (std::size_t index = 0; index < visibleRows; ++index) {
        const CachedViewportRow& row = viewportRows_[index];
        if (!row.available || row.text.empty()) continue;

        const bool hasLeftNeighbor = row.segmentStart > row.start;
        const bool hasRightNeighbor = row.segmentEnd < row.contentEnd;
        const std::size_t neighborUnits = static_cast<std::size_t>(hasLeftNeighbor) +
            static_cast<std::size_t>(hasRightNeighbor);
        if (row.text.size() > remainingUnits || neighborUnits > remainingUnits - row.text.size()) {
            break;
        }

        std::wstring neighbor;
        bool leftBoundaryContinuesWord = false;
        bool rightBoundaryContinuesWord = false;
        if (hasLeftNeighbor) {
            if (!document_->ReadRange(row.segmentStart - 1U, 1U, neighbor, errorMessage) || neighbor.empty()) {
                windows.clear();
                return false;
            }
            leftBoundaryContinuesWord = classic_notepad::IsSpellingWordContinuationUnit(neighbor.front());
        }
        if (hasRightNeighbor) {
            if (!document_->ReadRange(row.segmentEnd, 1U, neighbor, errorMessage) || neighbor.empty()) {
                windows.clear();
                return false;
            }
            rightBoundaryContinuesWord = classic_notepad::IsSpellingWordContinuationUnit(neighbor.front());
        }

        LargeFileVisibleTextWindow window;
        window.revision = {sessionId_, viewportCacheGeneration_, viewportCacheLayoutRevision_};
        window.absoluteStart = row.segmentStart;
        window.text = row.text;
        classic_notepad::SetSpellingEligibleBounds(
            window, leftBoundaryContinuesWord, rightBoundaryContinuesWord);
        windows.push_back(std::move(window));
        remainingUnits -= row.text.size() + neighborUnits;
    }
    return !windows.empty();
}

bool LargeFileView::ResolveSpellingContextTargetAtPoint(
    POINT clientPoint,
    bool keyboardInvocation,
    classic_notepad::SpellingContextTarget& target)
{
    target = {};
    const classic_notepad::SpellingRevision revision = GetSpellingRevision();
    if (!classic_notepad::SpellingBatchIsCurrent(spellingBatch_, revision)) return false;

    std::uint64_t position = 0U;
    if (keyboardInvocation) {
        std::lock_guard<std::mutex> lock(documentMutex_);
        position = caretPosition_;
    } else {
        RECT client{};
        if (window_ == nullptr || !GetClientRect(window_, &client) || !PtInRect(&client, clientPoint) ||
            !HitTest(clientPoint, position, nullptr, true)) return false;
    }

    std::vector<LargeFileVisibleTextWindow> windows;
    std::wstring error;
    if (!GetVisibleTextWindows(windows, error) ||
        !classic_notepad::SpellingBatchIsCurrent(spellingBatch_, GetSpellingRevision())) return false;
    return classic_notepad::ResolveSpellingContextTarget(
        windows, revision, position, keyboardInvocation, target);
}

bool LargeFileView::GetVisibleTextPositionPoint(std::uint64_t position, POINT& clientPoint)
{
    if (window_ == nullptr) return false;
    RebuildViewportCache(VisibleLineCount() + 2U);
    std::size_t rowIndex = 0U;
    if (!FindCachedRowForPosition(position, rowIndex) || rowIndex >= VisibleLineCount() ||
        rowIndex >= viewportRows_.size()) return false;
    const CachedViewportRow& row = viewportRows_[rowIndex];
    if (position < row.segmentStart || position >= row.segmentEnd) return false;
    const std::size_t local = static_cast<std::size_t>(position - row.segmentStart);
    if (local >= row.xBoundary.size() || local >= row.scalarBoundary.size() ||
        row.scalarBoundary[local] == 0U) return false;

    HDC dc = GetDC(window_);
    if (dc == nullptr) return false;
    const HGDIOBJ oldFont = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW metrics{};
    const bool hasMetrics = GetTextMetricsW(dc, &metrics) != 0;
    SelectObject(dc, oldFont);
    ReleaseDC(window_, dc);
    if (!hasMetrics) return false;
    const int lineHeight = std::max(1, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
    if (rowIndex > static_cast<std::size_t>((std::numeric_limits<int>::max)() / lineHeight)) return false;
    clientPoint.x = row.drawOriginX + row.xBoundary[local];
    clientPoint.y = static_cast<int>(rowIndex) * lineHeight + lineHeight / 2;
    RECT client{};
    return GetClientRect(window_, &client) && PtInRect(&client, clientPoint) != 0;
}

void LargeFileView::SetSpellingBatch(classic_notepad::SpellingAnnotationBatch batch)
{
    if (!classic_notepad::SpellingBatchIsCurrent(batch, GetSpellingRevision())) {
        batch = {};
    }
    spellingBatch_ = std::move(batch);
    if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
}

classic_notepad::SpellingRevision LargeFileView::GetSpellingRevision() const
{
    std::lock_guard<std::mutex> lock(documentMutex_);
    const std::uint64_t contentRevision = document_ == nullptr ? 0U : document_->Generation();
    return {sessionId_, contentRevision, layoutRevision_};
}

void LargeFileView::SetContextMenuHandler(
    std::function<bool(POINT, const classic_notepad::SpellingContextTarget&)> handler)
{
    contextMenuHandler_ = std::move(handler);
}

void LargeFileView::Focus(int allocationFailureForTesting)
{
    presentationAllocationFailureForTesting_ = allocationFailureForTesting;
    if (window_ != nullptr) {
        SetFocus(window_);
        // Hidden automation windows may not receive focus from SetFocus. Deliver
        // the real callback deterministically when its test fault is still armed.
        if (presentationAllocationFailureForTesting_ == 1 || presentationAllocationFailureForTesting_ == 2) {
            SendMessageW(window_, WM_SETFOCUS, 0, 0);
        } else if (presentationAllocationFailureForTesting_ >= 3) {
            SendMessageW(window_, WM_PAINT, 0, 0);
        }
    }
}

void LargeFileView::InjectPresentationAllocationFailureForTesting(bool painting)
{
    const int failure = presentationAllocationFailureForTesting_;
    if (failure == 0 || painting != (failure >= 3)) return;
    presentationAllocationFailureForTesting_ = 0;
    if (failure == 1 || failure == 3) throw std::bad_alloc();
    throw std::length_error("injected mapped callback layout allocation failure");
}

void LargeFileView::SelectAll()
{
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (EnsureCompleteDocument(error)) {
            selectionStart_ = 0;
            selectionEnd_ = document_->CharacterCount();
            selectionAnchor_ = 0;
            caretPosition_ = selectionEnd_;
            allSelected_ = true;
            ++selectionRevision_;
        }
    }
    if (window_ != nullptr) {
        EnsureCaretVisible();
        InvalidateRect(window_, nullptr, FALSE);
        UpdateCaret();
    }
}

bool LargeFileView::OpenMapping(
    const std::wstring& path,
    std::wstring& errorMessage,
    bool injectFailureAfterOpen)
{
    InvalidateViewportCache();
    errorMessage.clear();
    SetLastError(ERROR_SUCCESS);
    path_ = path;
    // Deny write access for as long as any document or snapshot retains this
    // source. A mapped read view alone does not make the underlying bytes
    // immutable when another process can still open the file for writing.
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION) {
            errorMessage = L"The large file could not be opened because another process has it open for writing. "
                L"Close that writer and try again (Windows error " + std::to_wstring(error) + L").";
        } else {
            errorMessage = L"The large file could not be opened for mapped editing (Windows error " +
                std::to_wstring(error) + L").";
        }
        return false;
    }
    if (injectFailureAfterOpen) {
        errorMessage = L"The test injected a mapped reopen failure after opening the committed output.";
        CloseMapping();
        SetLastError(ERROR_OPEN_FAILED);
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_, &size) || size.QuadPart < 0) {
        const DWORD error = GetLastError();
        errorMessage = L"The large file size could not be determined.";
        if (error != ERROR_SUCCESS) errorMessage += L" (Windows error " + std::to_wstring(error) + L").";
        CloseMapping();
        return false;
    }
    fileSize_ = static_cast<std::uint64_t>(size.QuadPart);
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_ == nullptr) {
        errorMessage = L"The large file could not be memory mapped (Windows error " +
            std::to_wstring(GetLastError()) + L").";
        CloseMapping();
        return false;
    }
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    allocationGranularity_ = std::max<DWORD>(1, systemInfo.dwAllocationGranularity);

    HANDLE readerFile = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), file_, GetCurrentProcess(), &readerFile,
            0, FALSE, DUPLICATE_SAME_ACCESS)) {
        const DWORD error = GetLastError();
        errorMessage = L"The large-file source handle could not be retained for document snapshots (Windows error " +
            std::to_wstring(error) + L").";
        CloseMapping();
        return false;
    }
    const auto readerFileCleanup = OnScopeExit([&] {
        if (readerFile != INVALID_HANDLE_VALUE) CloseHandle(readerFile);
    });
    HANDLE readerMapping = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), mapping_, GetCurrentProcess(), &readerMapping,
            0, FALSE, DUPLICATE_SAME_ACCESS)) {
        const DWORD error = GetLastError();
        errorMessage = L"The large-file source could not be retained for document snapshots (Windows error " +
            std::to_wstring(error) + L").";
        CloseMapping();
        return false;
    }
    const auto readerMappingCleanup = OnScopeExit([&] {
        if (readerMapping != nullptr) CloseHandle(readerMapping);
    });
    auto sharedSource = std::make_shared<classic_notepad::windows::SharedMappedSource>(
        readerFile, readerMapping, fileSize_, allocationGranularity_);
    readerFile = INVALID_HANDLE_VALUE;
    readerMapping = nullptr;
    sourceReader_ = [sharedSource](std::uint64_t offset, std::size_t length,
        std::vector<std::uint8_t>& bytes) {
        return sharedSource->Read(offset, length, bytes);
    };

    std::vector<std::uint8_t> prefix;
    if (!CopyBytes(0, static_cast<std::size_t>(std::min<std::uint64_t>(fileSize_, 4U)), prefix)) {
        errorMessage = L"The large file header could not be read (Windows error " +
            std::to_wstring(GetLastError()) + L").";
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
    sourceReader_ = {};
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
    sourceReady_.store(false);
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
    classic_notepad::LargeFilePieceBuilder pieceBuilder(encoding_, dataOffset_);
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
        std::vector<classic_notepad::LargeFilePieceSummary> completedPieces;
        const bool consumed = scanner.Consume(
            view + static_cast<SIZE_T>(delta64),
            static_cast<std::size_t>(chunk64),
            offset,
            batch,
            &batchCharacterOffsets);
        const bool piecesConsumed = pieceBuilder.Consume(
            view + static_cast<SIZE_T>(delta64),
            static_cast<std::size_t>(chunk64),
            offset,
            completedPieces);
        UnmapViewOfFile(view);
        if (!consumed || !piecesConsumed) {
            encodingValid_.store(false);
            break;
        }
        maximumLineColumns_.store(scanner.MaximumLineColumns());
        offset = SaturatingAdd(offset, chunk64);
        indexedBytes_.store(offset);
        if (!batch.empty()) {
            std::lock_guard<std::mutex> lock(lineMutex_);
            lineOffsets_.insert(lineOffsets_.end(), batch.begin(), batch.end());
            lineCharacterOffsets_.insert(
                lineCharacterOffsets_.end(), batchCharacterOffsets.begin(), batchCharacterOffsets.end());
            batch.clear();
            batchCharacterOffsets.clear();
        }
        if (!completedPieces.empty()) {
            std::wstring appendError;
            if (!AppendIndexedDocument(completedPieces, appendError)) {
                encodingValid_.store(false);
                break;
            }
        }
        NotifyStateChanged();
    }

    const bool scannerFinished = !cancel_.load() && encodingValid_.load() &&
        offset == fileSize_ && scanner.NextOffset() == fileSize_ &&
        scanner.Finish(batch, &batchCharacterOffsets);
    if (scannerFinished && !cancel_.load()) {
        std::vector<classic_notepad::LargeFilePieceSummary> finalPieces;
        std::wstring appendError;
        if (!pieceBuilder.Finish(finalPieces) || !AppendIndexedDocument(finalPieces, appendError)) {
            sourceReady_.store(false);
            indexingComplete_.store(false);
            encodingValid_.store(false);
            fullIndexMilliseconds_.store(ElapsedMilliseconds(startedTick_));
            NotifyStateChanged();
            return;
        }
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
        const std::uint64_t breaks = scanner.CrlfCount() + scanner.LfCount() + scanner.CrCount();
        bool modelCoversSource = false;
        if (!cancel_.load()) {
            std::lock_guard<std::mutex> lock(documentMutex_);
            modelCoversSource = document_ != nullptr && appendedByte_ == fileSize_ &&
                appendedCharacter_ == scanner.CharacterCount() && appendedLineBreaks_ == breaks &&
                indexedBytes_.load() == fileSize_;
            if (modelCoversSource && encodingValid_.load() && !cancel_.load()) {
                maximumLineColumns_.store(document_->MaximumLineColumns());
                sourceReady_.store(true);
                indexingComplete_.store(true);
            }
        }
        if (modelCoversSource && encodingValid_.load() && !cancel_.load()) {
            if (firstUsableMilliseconds_.load() == 0U) firstUsableMilliseconds_.store(ElapsedMilliseconds(startedTick_));
            fullIndexMilliseconds_.store(ElapsedMilliseconds(startedTick_));
            NotifyStateChanged();
        } else {
            sourceReady_.store(false);
            indexingComplete_.store(false);
            if (!cancel_.load() || !modelCoversSource) encodingValid_.store(false);
            fullIndexMilliseconds_.store(ElapsedMilliseconds(startedTick_));
            NotifyStateChanged();
        }
    } else if (!cancel_.load()) {
        sourceReady_.store(false);
        indexingComplete_.store(false);
        encodingValid_.store(false);
        fullIndexMilliseconds_.store(ElapsedMilliseconds(startedTick_));
        NotifyStateChanged();
    }
}

bool LargeFileView::AppendIndexedDocument(
    const std::vector<classic_notepad::LargeFilePieceSummary>& pieces,
    std::wstring& errorMessage)
{
    std::vector<classic_notepad::LargeDocumentOriginalPiece> descriptors;
    descriptors.reserve(pieces.size());
    std::uint64_t nextByte = appendedByte_;
    std::uint64_t nextCharacter = appendedCharacter_;
    for (const classic_notepad::LargeFilePieceSummary& piece : pieces) {
        if (piece.byteOffset != nextByte || piece.characterOffset != nextCharacter ||
            piece.byteLength == 0U || piece.characterLength == 0U ||
            piece.byteLength > classic_notepad::kLargeFileOriginalPieceMaximumBytes ||
            piece.characterLength > classic_notepad::kLargeFileOriginalPieceMaximumCharacters ||
            piece.byteOffset > fileSize_ || piece.byteLength > fileSize_ - piece.byteOffset ||
            piece.characterLength > (std::numeric_limits<std::uint64_t>::max)() - nextCharacter) {
            errorMessage = L"The bounded large-file piece index is not contiguous or has invalid totals.";
            return false;
        }
        descriptors.push_back({piece.byteOffset, piece.byteLength, piece.characterLength,
            piece.logicalLineBreaks, piece.firstCharacter, piece.lastCharacter, true,
            piece.widthSummary, piece.widthSummaryKnown, piece.displayedCharacterCount, true});
        nextByte += piece.byteLength;
        nextCharacter += piece.characterLength;
    }
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (document_ == nullptr) {
        auto model = std::make_unique<classic_notepad::LargeDocument>(
            encoding_, sourceReader_);
        if (!model->Initialize(descriptors, errorMessage)) return false;
        document_ = std::move(model);
    } else if (!descriptors.empty() && !document_->AppendOriginalPieces(descriptors, errorMessage)) {
        return false;
    }
    if (nextByte > fileSize_) {
        errorMessage = L"The bounded large-file piece index extends beyond the source file.";
        return false;
    }
    appendedByte_ = nextByte;
    appendedCharacter_ = nextCharacter;
    appendedLineBreaks_ = document_->LineCount() - 1U;
    if (firstUsableMilliseconds_.load() == 0U) firstUsableMilliseconds_.store(ElapsedMilliseconds(startedTick_));
    return true;
}

void LargeFileView::NotifyStateChanged()
{
    if (window_ != nullptr && !stateNotificationPending_.exchange(true)) {
        PostMessageW(window_, kLargeFileWorkerProgressMessage, 0, 0);
    }
}

void LargeFileView::PublishOperationCompletion(
    std::unique_ptr<OperationCompletion> completion, HWND targetWindow)
{
    if (completion == nullptr) return;
    const std::uint64_t token = completion->token;
    {
        std::lock_guard<std::mutex> lock(operationMutex_);
        if (!operationInProgress_ || operationToken_ != token || pendingOperationCompletion_ != nullptr) {
            return;
        }
        pendingOperationCompletion_ = std::move(completion);
        operationProgress_ = operationName_ == L"Save" ? saveProgressBytes_.load() : operationProgress_;
    }
    if (targetWindow != nullptr && IsWindow(targetWindow)) {
        PostMessageW(targetWindow, kLargeFileOperationCompletedMessage, static_cast<WPARAM>(token), 0);
    }
}

void LargeFileView::AdoptOperationCompletion(std::uint64_t token)
{
    std::unique_ptr<OperationCompletion> completion;
    {
        std::lock_guard<std::mutex> lock(operationMutex_);
        if (!operationInProgress_ || operationToken_ != token || pendingOperationCompletion_ == nullptr ||
            pendingOperationCompletion_->token != token) return;
        completion = std::move(pendingOperationCompletion_);
    }

    bool succeeded = completion->succeeded;
    bool preserveTemporary = false;
    bool deleteTemporary = false;
    std::wstring error = completion->error;
    std::uint64_t resultCount = completion->resultCount;
    if (completion->cancelled || operationCancel_.load()) {
        succeeded = false;
        error = L"The operation was cancelled.";
        deleteTemporary = completion->kind == OperationKind::Save;
    } else if (completion->kind == OperationKind::Find) {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || document_->Generation() != completion->documentGeneration ||
            selectionRevision_ != completion->selectionRevision) {
            succeeded = false;
            resultCount = 0;
            error = L"The document or selection changed before the search result completed.";
        } else if (completion->found) {
            selectionStart_ = completion->match.start;
            selectionEnd_ = completion->match.start + completion->match.length;
            selectionAnchor_ = selectionStart_;
            caretPosition_ = selectionEnd_;
            ++selectionRevision_;
        }
    } else if (completion->kind == OperationKind::ReplaceAll) {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || document_->Generation() != completion->documentGeneration ||
            selectionRevision_ != completion->selectionRevision) {
            succeeded = false;
            resultCount = 0;
            error = L"The document or selection changed before Replace All completed.";
        } else if (resultCount != 0U && completion->candidateDocument != nullptr) {
            document_ = std::move(completion->candidateDocument);
            maximumLineColumns_.store(document_->MaximumLineColumns());
            selectionStart_ = selectionEnd_ = selectionAnchor_ = caretPosition_ = 0;
            ++selectionRevision_;
            modified_.store(document_->IsModified());
            InvalidateViewportCache();
        }
    } else {
        std::lock_guard<std::mutex> lock(documentMutex_);
        const bool currentRevision = document_ != nullptr &&
            document_->Generation() == completion->documentGeneration;
        const bool rejectStaleInPlaceSave = !currentRevision && completion->targetWasCurrentPath;
        if (rejectStaleInPlaceSave) {
            succeeded = false;
            error = L"The document changed while its Save was in progress. The newer edits remain open and unsaved.";
            deleteTemporary = true;
        } else if (!succeeded) {
            deleteTemporary = true;
        } else {
            std::wstring replaceError;
            const bool replaced = classic_notepad::ReplaceWindowsSaveFile(
                completion->temporaryPath, completion->targetPath, completion->saveDiagnostics, replaceError);
            if (replaced) {
                if (completion->rebase) path_ = completion->targetPath;
                if (currentRevision && completion->rebase) {
                    document_->MarkSaved();
                    modified_.store(false);
                } else if (document_ != nullptr) {
                    modified_.store(document_->IsModified());
                    if (!currentRevision) {
                        error = L"The captured snapshot was saved to " + completion->targetPath +
                            L". Newer edits remain open and unsaved.";
                    }
                }
            } else {
                succeeded = false;
                preserveTemporary = completion->saveDiagnostics.outputComplete &&
                    !completion->saveDiagnostics.recoveryPath.empty();
                deleteTemporary = !preserveTemporary;
                error = replaceError;
            }
        }
        if (succeeded || preserveTemporary) deleteTemporary = false;
    }

    if (deleteTemporary && !completion->temporaryPath.empty()) {
        DeleteFileW(completion->temporaryPath.c_str());
        completion->saveDiagnostics.outputComplete = false;
        completion->saveDiagnostics.recoveryPath.clear();
    }
    saveInProgress_.store(false);
    {
        std::lock_guard<std::mutex> lock(operationMutex_);
        if (operationToken_ != token) return;
        operationCompleted_ = true;
        operationSucceeded_ = succeeded;
        operationResultCount_ = succeeded ? resultCount : 0U;
        operationProgress_ = completion->kind == OperationKind::Save ? saveProgressBytes_.load() : operationProgress_;
        operationError_ = std::move(error);
        if (completion->kind == OperationKind::Save) saveDiagnostics_ = completion->saveDiagnostics;
    }

    if (completion->kind == OperationKind::Find && succeeded && resultCount != 0U) EnsureCaretVisible();
    RefreshScrollBars();
    InvalidateRect(window_, nullptr, FALSE);
    UpdateCaret();
    if (parent_ != nullptr && IsWindow(parent_)) {
        PostMessageW(parent_, kLargeFileStateChangedMessage, static_cast<WPARAM>(token), 0);
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

bool LargeFileView::EnsureCompleteDocument(std::wstring& errorMessage) const
{
    if (!EnsureEditable(errorMessage)) return false;
    if (!sourceReady_.load() || !indexingComplete_.load() || indexedBytes_.load() != fileSize_ ||
        appendedByte_ != fileSize_ || appendedCharacter_ != characterCount_.load() ||
        appendedLineBreaks_ != crlfCount_.load() + lfCount_.load() + crCount_.load()) {
        errorMessage = L"The large document is still being indexed. Save and whole-document operations are unavailable until indexing completes.";
        return false;
    }
    return true;
}

void LargeFileView::MarkModified(bool invalidateAll)
{
    spellingBatch_ = {};
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
    ++layoutRevision_;
    if (layoutRevision_ == 0U) layoutRevision_ = 1U;
    spellingBatch_ = {};
    if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
}

bool LargeFileView::ViewportCacheMatches(std::uint64_t generation, const RECT& client) const
{
    return viewportCacheValid_ && viewportCacheTopLine_ == topLine_ &&
        viewportCacheGeneration_ == generation && viewportCacheTopWrapOffset_ == topWrapOffset_ &&
        viewportCacheHorizontalOffset_ == horizontalOffset_ &&
        viewportCacheHorizontalMaximumOffset_ == horizontalMaximumOffset_ &&
        viewportCacheWidth_ == client.right - client.left &&
        viewportCacheHeight_ == client.bottom - client.top &&
        viewportCacheFont_ == font_ && viewportCacheWordWrap_ == wordWrap_;
}

void LargeFileView::RebuildViewportCache(std::uint64_t rowCount)
{
    RECT client{};
    if (window_ != nullptr) GetClientRect(window_, &client);
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) {
            InvalidateViewportCache();
            return;
        }
        generation = document_->Generation();
    }
    if (ViewportCacheMatches(generation, client)) {
        ++viewportCacheHits_;
        return;
    }

    std::vector<CachedViewportRow> rows;
    rowCount = std::min<std::uint64_t>(rowCount, 4096U);
    rows.reserve(static_cast<std::size_t>(rowCount));
    const int wrapWidth = std::max(1L, client.right - client.left - 8L);
    const int clientWidth = std::max(1L, client.right - client.left);
    int averageCharacterWidth = 8;
    HDC dc = window_ == nullptr ? nullptr : GetDC(window_);
    HGDIOBJ oldFont = nullptr;
    if (dc != nullptr) {
        oldFont = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics)) averageCharacterWidth = std::max(1L, metrics.tmAveCharWidth);
    }
    const auto dcCleanup = OnScopeExit([&] {
        if (dc != nullptr) { SelectObject(dc, oldFont); ReleaseDC(window_, dc); }
    });
    ++viewportCacheRebuilds_;
    viewportLastBuildDecodedCharacters_ = 0;
    viewportLastBuildMeasuredCharacters_ = 0;
    viewportLastLogicalAnchor_ = 0;
    viewportLastSegmentStart_ = 0;
    viewportLastSegmentEnd_ = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) {
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
            if (!wordWrap_) {
                const std::uint64_t lineLength = contentEnd - start;
                const std::uint64_t anchorColumn = LogicalColumnAtHorizontalOffset(
                    lineLength, horizontalOffset_, horizontalMaximumOffset_);
                const std::uint64_t segmentColumn = anchorColumn > kViewportContextCharacters
                    ? anchorColumn - kViewportContextCharacters : 0U;
                std::uint64_t viewportStart = start + segmentColumn;
                const std::uint64_t requested = std::min<std::uint64_t>(
                    lineLength - segmentColumn,
                    std::min<std::uint64_t>(kRenderedLineByteLimit,
                        static_cast<std::uint64_t>(clientWidth) +
                            2U * kViewportContextCharacters + 32U));
                std::wstring text;
                if (!ReadScalarSafeViewportRange(*document_, viewportStart, requested, text, error)) break;
                viewportLastBuildDecodedCharacters_ = SaturatingAdd(
                    viewportLastBuildDecodedCharacters_, static_cast<std::uint64_t>(text.size()));
                CachedViewportRow row;
                row.line = logicalLine;
                row.start = start;
                row.contentEnd = contentEnd;
                row.nextLineStart = nextLineStart;
                row.segmentStart = viewportStart;
                row.segmentEnd = viewportStart + static_cast<std::uint64_t>(text.size());
                row.logicalAnchor = start + anchorColumn;
                const std::uint64_t segmentColumnActual = viewportStart - start;
                row.text = std::move(text);
                row.available = true;
                if (dc != nullptr) {
                    InjectPresentationAllocationFailureForTesting(true);
                    TextLayout layout = LayoutText(dc, row.text, segmentColumnActual);
                    viewportLastBuildMeasuredCharacters_ = SaturatingAdd(
                        viewportLastBuildMeasuredCharacters_, static_cast<std::uint64_t>(row.text.size()));
                    row.displayText = std::move(layout.displayText);
                    row.displayBoundary = std::move(layout.displayBoundary);
                    row.xBoundary = std::move(layout.xBoundary);
                    row.scalarBoundary = std::move(layout.scalarBoundary);
                }
                std::size_t anchorBoundary = static_cast<std::size_t>(std::min<std::uint64_t>(
                    row.logicalAnchor - row.segmentStart, static_cast<std::uint64_t>(row.text.size())));
                if (row.scalarBoundary.size() == row.text.size() + 1U) {
                    while (anchorBoundary > 0U && row.scalarBoundary[anchorBoundary] == 0U) --anchorBoundary;
                }
                row.logicalAnchor = row.segmentStart + static_cast<std::uint64_t>(anchorBoundary);
                const int anchorViewportX = HorizontalAnchorViewportX(
                    clientWidth, horizontalOffset_, horizontalMaximumOffset_);
                const int localAnchorX = row.xBoundary.size() == row.text.size() + 1U
                    ? row.xBoundary[anchorBoundary]
                    : ClampToInt(static_cast<std::int64_t>(std::min<std::uint64_t>(
                        ScaleRatio(static_cast<std::uint64_t>(anchorBoundary),
                            static_cast<std::uint64_t>(averageCharacterWidth), 1U),
                        static_cast<std::uint64_t>((std::numeric_limits<int>::max)()))));
                row.drawOriginX = ClampToInt(static_cast<std::int64_t>(anchorViewportX) - localAnchorX);
                if (rows.empty()) {
                    viewportLastLogicalAnchor_ = row.logicalAnchor;
                    viewportLastSegmentStart_ = row.segmentStart;
                    viewportLastSegmentEnd_ = row.segmentEnd;
                }
                rows.push_back(std::move(row));
                ++logicalLine;
                continue;
            }

            const std::uint64_t initialOffset = logicalLine == topLine_
                ? std::min(topWrapOffset_, contentEnd - start) : 0U;
            std::uint64_t viewportStart = start + initialOffset;
            std::wstring text;
            const bool available = ReadScalarSafeViewportRange(*document_, viewportStart,
                std::min<std::uint64_t>(contentEnd - start - initialOffset, kRenderedLineByteLimit), text, error);
            if (!available) break;
            viewportLastBuildDecodedCharacters_ = SaturatingAdd(
                viewportLastBuildDecodedCharacters_, static_cast<std::uint64_t>(text.size()));

            std::size_t offset = 0;
            bool emitted = false;
            do {
                std::size_t end = text.size();
                if (offset < text.size() && dc != nullptr) {
                    const std::wstring remaining(text.data() + offset, text.size() - offset);
                    const TextLayout layout = LayoutText(
                        dc, remaining, initialOffset + static_cast<std::uint64_t>(offset));
                    viewportLastBuildMeasuredCharacters_ = SaturatingAdd(
                        viewportLastBuildMeasuredCharacters_, static_cast<std::uint64_t>(remaining.size()));
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
                    if (end == offset) {
                        end = std::min(text.size(), offset + 1U);
                        if (end < text.size() && IsHighSurrogateUnit(text[offset]) &&
                            IsLowSurrogateUnit(text[end])) ++end;
                    }
                }
                CachedViewportRow row;
                row.line = logicalLine;
                row.start = start;
                row.contentEnd = contentEnd;
                row.nextLineStart = nextLineStart;
                row.segmentStart = viewportStart + static_cast<std::uint64_t>(offset);
                row.segmentEnd = viewportStart + static_cast<std::uint64_t>(end);
                row.logicalAnchor = row.segmentStart;
                row.text.assign(text.data() + offset, end - offset);
                row.available = true;
                row.drawOriginX = 4;
                if (dc != nullptr) {
                    InjectPresentationAllocationFailureForTesting(true);
                    TextLayout layout = LayoutText(dc, row.text, initialOffset + static_cast<std::uint64_t>(offset));
                    viewportLastBuildMeasuredCharacters_ = SaturatingAdd(
                        viewportLastBuildMeasuredCharacters_, static_cast<std::uint64_t>(row.text.size()));
                    row.displayText = std::move(layout.displayText);
                    row.displayBoundary = std::move(layout.displayBoundary);
                    row.xBoundary = std::move(layout.xBoundary);
                    row.scalarBoundary = std::move(layout.scalarBoundary);
                }
                if (rows.empty()) {
                    viewportLastLogicalAnchor_ = row.logicalAnchor;
                    viewportLastSegmentStart_ = row.segmentStart;
                    viewportLastSegmentEnd_ = row.segmentEnd;
                }
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
                row.segmentStart = row.segmentEnd = viewportStart;
                row.logicalAnchor = viewportStart;
                row.available = true;
                rows.push_back(std::move(row));
            }
            ++logicalLine;
        }
    }
    viewportRows_ = std::move(rows);
    viewportCacheTopLine_ = topLine_;
    viewportCacheGeneration_ = generation;
    viewportCacheLayoutRevision_ = layoutRevision_;
    viewportCacheTopWrapOffset_ = topWrapOffset_;
    viewportCacheHorizontalOffset_ = horizontalOffset_;
    viewportCacheHorizontalMaximumOffset_ = horizontalMaximumOffset_;
    viewportCacheWidth_ = client.right - client.left;
    viewportCacheHeight_ = client.bottom - client.top;
    viewportCacheFont_ = font_;
    viewportCacheWordWrap_ = wordWrap_;
    viewportCacheValid_ = true;
}

void LargeFileView::RefreshCachedViewportLine(
    std::uint64_t line,
    std::uint64_t removedCharacters,
    std::uint64_t insertedCharacters)
{
    static_cast<void>(line);
    static_cast<void>(removedCharacters);
    static_cast<void>(insertedCharacters);
    // A local edit changes the document generation and can shift every later
    // absolute segment coordinate. Rebuild the bounded visible mapping on the
    // next presentation pass rather than retaining stale absolute geometry.
    InvalidateViewportCache();
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
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ != nullptr) return std::max<std::uint64_t>(1U, document_->LineCount());
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
    return classic_notepad::LargeFileScrollMapping::Create(total, VisibleLineCount()).TopToPosition(line);
}

std::uint64_t LargeFileView::PositionToLine(int position) const
{
    const std::uint64_t total = EstimatedTotalLines();
    return classic_notepad::LargeFileScrollMapping::Create(total, VisibleLineCount()).PositionToTop(
        position <= 0 ? 0U : static_cast<std::uint64_t>(position));
}

void LargeFileView::RefreshScrollBars()
{
    if (window_ == nullptr) {
        return;
    }
    const std::uint64_t total = EstimatedTotalLines();
    const std::uint64_t page = VisibleLineCount();
    const classic_notepad::LargeFileScrollMapping verticalMapping =
        classic_notepad::LargeFileScrollMapping::Create(total, page);
    if (wordWrap_) {
        // Wrapped scrolling is anchored to a visual continuation within a
        // logical line. The unwrapped page mapping cannot clamp that anchor:
        // a short document may have more wrapped rows than logical lines.
        if (indexingComplete_.load()) {
            std::lock_guard<std::mutex> lock(documentMutex_);
            if (document_ != nullptr) {
                const std::uint64_t logicalLineCount = document_->LineCount();
                const std::uint64_t lastLogicalLine = logicalLineCount == 0U
                    ? 0U : logicalLineCount - 1U;
                if (topLine_ > lastLogicalLine) {
                    topLine_ = lastLogicalLine;
                    topWrapOffset_ = 0U;
                    InvalidateViewportCache();
                } else {
                    std::uint64_t lineStart = 0U;
                    std::uint64_t contentEnd = 0U;
                    std::uint64_t nextLineStart = 0U;
                    std::wstring error;
                    if (document_->LineRange(topLine_, lineStart, contentEnd, nextLineStart, error)) {
                        std::uint64_t boundedOffset = std::min(topWrapOffset_, contentEnd - lineStart);
                        while (boundedOffset > 0U &&
                            !document_->IsCharacterBoundary(lineStart + boundedOffset, error)) {
                            --boundedOffset;
                        }
                        if (boundedOffset != topWrapOffset_) {
                            topWrapOffset_ = boundedOffset;
                            InvalidateViewportCache();
                        }
                    }
                }
            }
        } else if (topLine_ >= total) {
            topLine_ = total == 0U ? 0U : total - 1U;
            topWrapOffset_ = 0U;
            InvalidateViewportCache();
        }
    } else if (topLine_ > verticalMapping.MaximumTop()) {
        topLine_ = verticalMapping.MaximumTop();
        topWrapOffset_ = 0U;
        InvalidateViewportCache();
    }
    SCROLLINFO vertical{};
    vertical.cbSize = sizeof(vertical);
    vertical.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    vertical.nMin = 0;
    vertical.nMax = verticalMapping.RangeMaximum();
    if (indexingComplete_.load()) {
        vertical.nPage = static_cast<UINT>(verticalMapping.Page());
        vertical.nPos = verticalMapping.TopToPosition(topLine_);
    } else {
        // Do not continuously remap a dragging thumb from a changing global
        // line estimate. Keyboard/caret navigation remains available inside
        // the indexed frontier; global scrollbar interaction is gated until
        // the worker publishes the final stable line count.
        vertical.nPage = static_cast<UINT>(vertical.nMax + 1);
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
    const int priorHorizontalOffset = horizontalOffset_;
    horizontalOffset_ = std::clamp(horizontalOffset_, 0, horizontalMaximumOffset_);
    if (horizontalOffset_ != priorHorizontalOffset) InvalidateViewportCache();
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
    const std::uint64_t maximumTop = wordWrap_
        ? (total == 0U ? 0U : total - 1U)
        : classic_notepad::LargeFileScrollMapping::Create(total, VisibleLineCount()).MaximumTop();
    topLine_ = maximumTop;
    topLine_ = std::min(line, topLine_);
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
    case SB_BOTTOM:
        SetTopLine(classic_notepad::LargeFileScrollMapping::Create(
            EstimatedTotalLines(), page).MaximumTop());
        break;
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
    InvalidateViewportCache();
    InvalidateRect(window_, nullptr, FALSE);
    UpdateCaret();
    NotifyStateChanged();
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
    return ReadScalarSafeViewportRange(
        *document_, start, std::min<std::uint64_t>(contentEnd - start, kRenderedLineByteLimit), text, error);
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
    const auto bufferCleanup = OnScopeExit([&] {
        SelectObject(buffer, previousFont);
        SelectObject(buffer, previousBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
    });
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
    if (editedReady) {
        if (ViewportCacheMatches(editedGeneration, client)) ++viewportCacheHits_;
        else RebuildViewportCache(VisibleLineCount() + 2U);
    }
    for (std::uint64_t index = firstRow; index < visible && y < client.bottom; ++index, y += lineHeight) {
        std::uint64_t line = SaturatingAdd(topLine_, index);
        std::wstring text;
        bool available = false;
        std::uint64_t lineStart = 0;
        std::uint64_t lineEnd = 0;
        std::uint64_t nextLineStart = 0;
        const CachedViewportRow* cachedRow = nullptr;
        if (editedReady) {
            const std::uint64_t cacheIndex = index;
            if (viewportCacheValid_ && cacheIndex < viewportRows_.size()) {
                const CachedViewportRow& row = viewportRows_[static_cast<std::size_t>(cacheIndex)];
                cachedRow = &row;
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
        int x = cachedRow != nullptr ? cachedRow->drawOriginX
            : 4 - (wordWrap_ ? 0 : horizontalOffset_);
        TextLayout fallbackLayout;
        const std::wstring* displayText = nullptr;
        const std::vector<int>* xBoundary = nullptr;
        std::size_t paintedCharacters = text.size();
        if (cachedRow != nullptr && cachedRow->xBoundary.size() == text.size() + 1U) {
            displayText = &cachedRow->displayText;
            xBoundary = &cachedRow->xBoundary;
        } else {
            paintedCharacters = static_cast<std::size_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(text.size()), std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(std::max<LONG>(1L, width)) + 2U * kViewportContextCharacters,
                    static_cast<std::uint64_t>((std::numeric_limits<int>::max)()))));
            const std::wstring paintedText(text.data(), paintedCharacters);
            InjectPresentationAllocationFailureForTesting(true);
            fallbackLayout = LayoutText(buffer, paintedText);
            displayText = &fallbackLayout.displayText;
            xBoundary = &fallbackLayout.xBoundary;
        }
        if (displayText == nullptr || xBoundary == nullptr) continue;
        TextOutW(buffer, x, y, displayText->c_str(), static_cast<int>(displayText->size()));
        if (editedReady && selectedStart < lineEnd && selectedEnd > lineStart) {
            const std::size_t first = static_cast<std::size_t>(std::min<std::uint64_t>(paintedCharacters, selectedStart > lineStart ? selectedStart - lineStart : 0U));
            const std::size_t last = static_cast<std::size_t>(std::min<std::uint64_t>(paintedCharacters, selectedEnd - lineStart));
            const int selectionLeft = (*xBoundary)[first];
            const int selectionWidth = std::max(1, (*xBoundary)[last] - selectionLeft);
            RECT selectionRect{x + selectionLeft, y, x + selectionLeft + selectionWidth, y + lineHeight};
            HBRUSH selectionBrush = CreateSolidBrush(darkMode_ ? RGB(54, 84, 120) : RGB(0, 120, 215));
            FillRect(buffer, &selectionRect, selectionBrush);
            DeleteObject(selectionBrush);
            SetTextColor(buffer, darkMode_ ? RGB(255, 255, 255) : RGB(255, 255, 255));
            const int saved = SaveDC(buffer);
            IntersectClipRect(buffer, selectionRect.left, selectionRect.top, selectionRect.right, selectionRect.bottom);
            TextOutW(buffer, x, y, displayText->c_str(), static_cast<int>(displayText->size()));
            RestoreDC(buffer, saved);
            SetTextColor(buffer, textColor);
        }
        const bool spellingBatchCurrent = editedReady && classic_notepad::SpellingBatchIsCurrent(
            spellingBatch_, {sessionId_, editedGeneration, viewportCacheLayoutRevision_});
        if (spellingBatchCurrent && !spellingBatch_.annotations.empty()) {
            for (const classic_notepad::SpellingAnnotation& issue : spellingBatch_.annotations) {
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
                const int left = x + (*xBoundary)[first];
                const int right = x + std::max((*xBoundary)[first] + 2, (*xBoundary)[last]);
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

bool LargeFileView::HitTest(POINT point, std::uint64_t& position, int* visualX, bool characterAtPoint)
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
    RECT client{};
    GetClientRect(window_, &client);
    if (!ViewportCacheMatches(generation, client) || visualRow >= viewportRows_.size()) {
        RebuildViewportCache(VisibleLineCount() + 2U);
    } else {
        ++viewportCacheHits_;
    }
    if (visualRow >= viewportRows_.size() || !viewportRows_[visualRow].available) {
        SelectObject(dc, oldFont); ReleaseDC(window_, dc); return false;
    }
    const CachedViewportRow& row = viewportRows_[visualRow];
    const std::wstring& text = row.text;
    const std::uint64_t start = row.segmentStart;
    const int wanted = ClampToInt(static_cast<std::int64_t>(point.x) - row.drawOriginX);
    TextLayout fallbackLayout;
    const std::vector<int>* xBoundary = &row.xBoundary;
    const std::vector<std::uint8_t>* scalarBoundary = &row.scalarBoundary;
    if (row.xBoundary.size() != text.size() + 1U || row.scalarBoundary.size() != text.size() + 1U) {
        fallbackLayout = LayoutText(dc, text, row.segmentStart - row.start);
        xBoundary = &fallbackLayout.xBoundary;
        scalarBoundary = &fallbackLayout.scalarBoundary;
    }
    if (characterAtPoint) {
        for (std::size_t index = 0U; index < text.size();) {
            if ((*scalarBoundary)[index] == 0U) {
                ++index;
                continue;
            }
            std::size_t next = index + 1U;
            while (next < scalarBoundary->size() && (*scalarBoundary)[next] == 0U) ++next;
            if (next >= scalarBoundary->size()) break;
            if (wanted >= (*xBoundary)[index] && wanted < (*xBoundary)[next]) {
                position = start + static_cast<std::uint64_t>(index);
                if (visualX != nullptr) *visualX = (*xBoundary)[index];
                SelectObject(dc, oldFont);
                ReleaseDC(window_, dc);
                return true;
            }
            index = next;
        }
        SelectObject(dc, oldFont);
        ReleaseDC(window_, dc);
        return false;
    }
    const std::size_t fit = classic_notepad::windows::ClosestTextBoundary(
        *xBoundary, *scalarBoundary, wanted);
    position = start + static_cast<std::uint64_t>(fit);
    if (visualX != nullptr) *visualX = (*xBoundary)[fit];
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
    std::uint64_t lineStart = 0;
    std::uint64_t lineEnd = 0;
    std::uint64_t nextLineStart = 0;
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || !document_->PositionToLineColumn(caretPosition_, coordinate, error) ||
            !document_->LineRange(coordinate.line, lineStart, lineEnd, nextLineStart, error)) return;
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
            const bool caretAtLineEnd = caretPosition_ == lineEnd && coordinate.column > 0U;
            std::uint64_t backtrack = 0U;
            if (caretAtLineEnd) {
                RECT client{};
                GetClientRect(window_, &client);
                const std::uint64_t wrapWidth = static_cast<std::uint64_t>(std::max(
                    1L, client.right - client.left - 8L));
                std::uint64_t averageCharacterWidth = 8U;
                HDC dc = GetDC(window_);
                if (dc != nullptr) {
                    HGDIOBJ oldFont = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
                    TEXTMETRICW metrics{};
                    if (GetTextMetricsW(dc, &metrics)) {
                        averageCharacterWidth = static_cast<std::uint64_t>(std::max(1L, metrics.tmAveCharWidth));
                    }
                    SelectObject(dc, oldFont);
                    ReleaseDC(window_, dc);
                }
                const std::uint64_t charactersPerRow = std::max<std::uint64_t>(
                    1U, wrapWidth / averageCharacterWidth);
                const std::uint64_t visibleRows = std::max<std::uint64_t>(
                    1U, VisibleLineCount() > 1U ? VisibleLineCount() - 1U : 1U);
                const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
                const std::uint64_t pageCharacters = charactersPerRow > maximum / visibleRows
                    ? maximum : charactersPerRow * visibleRows;
                backtrack = std::min(coordinate.column, std::max<std::uint64_t>(1U, pageCharacters));
            }

            // A caret at EOF belongs to the end of the final non-empty
            // wrapped segment. Starting the viewport exactly at EOF produces
            // an empty row, so back up by an estimated page and narrow that
            // bounded suffix until the measured wrapped rows contain the caret.
            // Mid-line navigation retains its existing immediate-caret anchor.
            for (unsigned int attempt = 0U; attempt < 16U; ++attempt) {
                topWrapOffset_ = caretAtLineEnd
                    ? coordinate.column - backtrack
                    : coordinate.column;
                InvalidateViewportCache();
                RebuildViewportCache(VisibleLineCount() + 2U);
                if (FindCachedRowForPosition(caretPosition_, row) && row < VisibleLineCount()) {
                    break;
                }
                if (!caretAtLineEnd || backtrack <= 1U) {
                    break;
                }
                backtrack = std::max<std::uint64_t>(1U, backtrack / 2U);
            }
        }
        RefreshScrollBars();
        return;
    }
    const std::uint64_t visible = VisibleLineCount();
    if (coordinate.line < topLine_) {
        topLine_ = coordinate.line;
        InvalidateViewportCache();
    } else if (coordinate.line >= topLine_ && coordinate.line - topLine_ >= visible) {
        topLine_ = coordinate.line - visible + 1U;
        InvalidateViewportCache();
    }
    RefreshScrollBars();
    RECT client{};
    GetClientRect(window_, &client);
    RebuildViewportCache(visible + 2U);
    std::size_t rowIndex = 0;
    bool inside = FindCachedRowForPosition(caretPosition_, rowIndex);
    if (inside && rowIndex < viewportRows_.size()) {
        const CachedViewportRow& row = viewportRows_[rowIndex];
        const std::uint64_t local = caretPosition_ - row.segmentStart;
        if (local < row.xBoundary.size()) {
            const int x = row.drawOriginX + row.xBoundary[static_cast<std::size_t>(local)];
            if (x >= 4 && x <= client.right - 2L) return;
        }
    }

    const std::uint64_t lineLength = lineEnd - lineStart;
    const std::uint64_t caretColumn = caretPosition_ >= lineStart ? caretPosition_ - lineStart : 0U;
    // Use the same row-length fraction that RebuildViewportCache converts back
    // into the visible logical anchor. Skipped-prefix pixel widths are only an
    // approximation; local caret x is measured from the cached segment.
    const int newOffset = HorizontalOffsetAtLogicalColumn(
        lineLength, caretColumn, horizontalMaximumOffset_);
    if (newOffset != horizontalOffset_) {
        horizontalOffset_ = newOffset;
        InvalidateViewportCache();
        RefreshScrollBars();
        RebuildViewportCache(visible + 2U);
    }
}

void LargeFileView::UpdateCaret()
{
    if (!focused_ || window_ == nullptr) return;
    classic_notepad::LargeDocumentLineColumn coordinate;
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || !document_->PositionToLineColumn(caretPosition_, coordinate, error)) return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ != nullptr) generation = document_->Generation();
    }
    if (!ViewportCacheMatches(generation, client)) RebuildViewportCache(VisibleLineCount() + 2U);
    else ++viewportCacheHits_;
    std::size_t visualRow = 0;
    const std::uint64_t visible = VisibleLineCount();
    if ((!wordWrap_ && (coordinate.line < topLine_ ||
            (coordinate.line >= topLine_ && coordinate.line - topLine_ >= visible))) ||
        !FindCachedRowForPosition(caretPosition_, visualRow) || visualRow >= VisibleLineCount()) {
        // Caret updates also run after repaint and scroll messages. They must
        // not undo an explicit user scroll by moving the viewport back to an
        // off-screen caret. Caret-moving paths call EnsureCaretVisible before
        // reaching this point; when the caret is outside the shared mapping it
        // is simply hidden until navigation brings it back into the viewport.
        DestroyTextCaret();
        return;
    }
    const CachedViewportRow& row = viewportRows_[visualRow];
    const std::uint64_t count64 = caretPosition_ - row.segmentStart;
    if (count64 >= row.xBoundary.size() ||
        row.scalarBoundary.size() != row.xBoundary.size() ||
        row.scalarBoundary[static_cast<std::size_t>(count64)] == 0U) {
        DestroyTextCaret();
        return;
    }
    const std::size_t count = static_cast<std::size_t>(count64);
    HDC dc = GetDC(window_);
    if (dc == nullptr) return;
    HGDIOBJ oldFont = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const auto dcCleanup = OnScopeExit([&] { SelectObject(dc, oldFont); ReleaseDC(window_, dc); });
    const int caretHeight = std::max(1, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
    if (!caretCreated_) {
        if (!CreateCaret(window_, nullptr, 2, caretHeight)) return;
        caretCreated_ = true;
        ShowCaret(window_);
    }
    const int caretX = row.drawOriginX + row.xBoundary[count];
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
        ++selectionRevision_;
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
        auto read = [&](std::uint64_t at, wchar_t& character) {
            return at < total && document_->CharacterAt(at, character, error);
        };
        auto nextScalar = [&](std::uint64_t at) {
            wchar_t first = 0;
            if (!read(at, first)) return at;
            if (classic_notepad::IsHighSurrogateUnit(first) && at + 1U < total) {
                wchar_t second = 0;
                if (read(at + 1U, second) && classic_notepad::IsLowSurrogateUnit(second)) return at + 2U;
            }
            return at + 1U;
        };
        auto previousScalar = [&](std::uint64_t at) {
            if (at == 0U) return at;
            wchar_t last = 0;
            if (!read(at - 1U, last)) return at;
            if (classic_notepad::IsLowSurrogateUnit(last) && at > 1U) {
                wchar_t first = 0;
                if (read(at - 2U, first) && classic_notepad::IsHighSurrogateUnit(first)) return at - 2U;
            }
            return at - 1U;
        };
        auto isWordAt = [&](std::uint64_t at) {
            wchar_t character = 0;
            return read(at, character) && classic_notepad::IsWordCharacterUnit(character);
        };
        auto isCjkWordAt = [&](std::uint64_t at) {
            wchar_t first = 0;
            if (!read(at, first)) return false;
            std::uint32_t scalar = static_cast<std::uint32_t>(first);
            if (classic_notepad::IsHighSurrogateUnit(first) && at + 1U < total) {
                wchar_t second = 0;
                if (read(at + 1U, second) && classic_notepad::IsLowSurrogateUnit(second)) {
                    scalar = 0x10000U + ((static_cast<std::uint32_t>(first) - 0xD800U) << 10U) +
                        (static_cast<std::uint32_t>(second) - 0xDC00U);
                }
            }
            return (scalar >= 0x3400U && scalar <= 0x4DBFU) ||
                (scalar >= 0x4E00U && scalar <= 0x9FFFU) ||
                (scalar >= 0xF900U && scalar <= 0xFAFFU) ||
                (scalar >= 0x20000U && scalar <= 0x323AFU) ||
                (scalar >= 0x3040U && scalar <= 0x30FFU) ||
                (scalar >= 0x31F0U && scalar <= 0x31FFU) ||
                (scalar >= 0xAC00U && scalar <= 0xD7AFU);
        };
        auto advanceWordSegment = [&](std::uint64_t at) {
            if (isCjkWordAt(at)) return nextScalar(at);
            while (at < total && isWordAt(at) && !isCjkWordAt(at)) {
                const std::uint64_t next = nextScalar(at);
                if (next == at) break;
                at = next;
            }
            return at;
        };
        auto retreatWordSegment = [&](std::uint64_t at) {
            const std::uint64_t previous = previousScalar(at);
            if (previous >= at || !isWordAt(previous)) return at;
            if (isCjkWordAt(previous)) return previous;
            at = previous;
            while (at > 0U) {
                const std::uint64_t nextPrevious = previousScalar(at);
                if (nextPrevious >= at || !isWordAt(nextPrevious) || isCjkWordAt(nextPrevious)) break;
                at = nextPrevious;
            }
            return at;
        };
        auto isHorizontalSpaceAt = [&](std::uint64_t at) {
            wchar_t character = 0;
            return read(at, character) && (character == L' ' || character == L'\t');
        };
        auto isSupplementaryScalarAt = [&](std::uint64_t at) {
            wchar_t first = 0;
            wchar_t second = 0;
            return at + 1U < total && read(at, first) && read(at + 1U, second) &&
                classic_notepad::IsHighSurrogateUnit(first) &&
                classic_notepad::IsLowSurrogateUnit(second);
        };
        auto skipCjkSeparators = [&](std::uint64_t& at) {
            while (at < total && !isWordAt(at) && !isHorizontalSpaceAt(at) &&
                !isSupplementaryScalarAt(at)) {
                wchar_t character = 0;
                if (!read(at, character) || character == L'\r' || character == L'\n') break;
                const std::uint64_t next = nextScalar(at);
                if (next == at) break;
                at = next;
            }
            while (at < total && isHorizontalSpaceAt(at)) {
                const std::uint64_t next = nextScalar(at);
                if (next == at) break;
                at = next;
            }
        };

        if (byWord) {
            if (right) {
                wchar_t current = 0;
                if (read(position, current) && (current == L'\r' || current == L'\n')) {
                    if (current == L'\r' && position + 1U < total) {
                        wchar_t next = 0;
                        position += read(position + 1U, next) && next == L'\n' ? 2U : 1U;
                    } else {
                        ++position;
                    }
                } else if (isSupplementaryScalarAt(position)) {
                    position = nextScalar(position);
                } else if (isWordAt(position)) {
                    const bool startsWithCjkWord = isCjkWordAt(position);
                    position = advanceWordSegment(position);
                    if (startsWithCjkWord) skipCjkSeparators(position);
                    // EDIT includes one horizontal separator run when moving
                    // forward from within a word. Other punctuation, including
                    // supplementary scalars, remains a separate stop.
                    while (position < total && isHorizontalSpaceAt(position)) {
                        const std::uint64_t next = nextScalar(position);
                        if (next == position) break;
                        position = next;
                    }
                } else if (isHorizontalSpaceAt(position)) {
                    while (position < total && isHorizontalSpaceAt(position)) {
                        const std::uint64_t next = nextScalar(position);
                        if (next == position) break;
                        position = next;
                    }
                    if (position < total && isWordAt(position)) {
                        const bool startsWithCjkWord = isCjkWordAt(position);
                        position = advanceWordSegment(position);
                        if (startsWithCjkWord) skipCjkSeparators(position);
                    }
                } else {
                    while (position < total && !isWordAt(position) &&
                        !isHorizontalSpaceAt(position)) {
                        wchar_t character = 0;
                        if (!read(position, character) || character == L'\r' || character == L'\n') break;
                        const std::uint64_t next = nextScalar(position);
                        if (next == position) break;
                        position = next;
                    }
                    while (position < total && isHorizontalSpaceAt(position)) {
                        const std::uint64_t next = nextScalar(position);
                        if (next == position) break;
                        position = next;
                    }
                }
            } else {
                wchar_t previousUnit = 0;
                const bool havePrevious = position > 0U && read(position - 1U, previousUnit);
                if (havePrevious && previousUnit == L'\n' && position > 1U) {
                    wchar_t beforePrevious = 0;
                    position -= read(position - 2U, beforePrevious) && beforePrevious == L'\r' ? 2U : 1U;
                } else if (havePrevious && previousUnit == L'\r') {
                    --position;
                } else if (previousScalar(position) < position &&
                    isSupplementaryScalarAt(previousScalar(position))) {
                    position = previousScalar(position);
                } else {
                    bool skippedSpace = false;
                    while (position > 0U) {
                        const std::uint64_t previous = previousScalar(position);
                        if (previous >= position || !isHorizontalSpaceAt(previous)) break;
                        position = previous;
                        skippedSpace = true;
                    }
                    std::uint64_t previous = previousScalar(position);
                    if (previous < position && isWordAt(previous)) {
                        position = retreatWordSegment(position);
                    } else if (skippedSpace && previous < position) {
                        wchar_t character = 0;
                        while (previous < position && read(previous, character) &&
                            character != L'\r' && character != L'\n' &&
                            !isWordAt(previous) && !isHorizontalSpaceAt(previous)) {
                            position = previous;
                            previous = previousScalar(position);
                        }
                    } else if (previous < position && !isWordAt(previous)) {
                        while (previous < position && !isWordAt(previous) &&
                            !isHorizontalSpaceAt(previous)) {
                            position = previous;
                            previous = previousScalar(position);
                        }
                        if (previous < position && isCjkWordAt(previous)) position = previous;
                    }
                }
            }
        } else if (right && position < total) {
            wchar_t current = 0;
            if (!read(position, current)) return;
            if (current == L'\r' && position + 1U < total) {
                wchar_t next = 0;
                if (read(position + 1U, next) && next == L'\n') position += 2U;
                else position = nextScalar(position);
            } else if (current == L'\n' && position > 0U) {
                wchar_t previous = 0;
                if (read(position - 1U, previous) && previous == L'\r') ++position;
                else position = nextScalar(position);
            } else {
                position = nextScalar(position);
            }
        } else if (!right && position > 0U) {
            wchar_t previous = 0;
            if (!read(position - 1U, previous)) return;
            if (previous == L'\n' && position > 1U) {
                wchar_t before = 0;
                if (read(position - 2U, before) && before == L'\r') position -= 2U;
                else position = previousScalar(position);
            } else if (previous == L'\r' && position < total) {
                wchar_t current = 0;
                if (read(position, current) && current == L'\n') --position;
                else position = previousScalar(position);
            } else {
                position = previousScalar(position);
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
    else if (targetLine >= topLine_ && targetLine - topLine_ >= VisibleLineCount())
        topLine_ = targetLine - VisibleLineCount() + 1U;
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
    const bool keyboardInvocation = screenPoint.x == -1 && screenPoint.y == -1;
    POINT clientPoint{};
    if (keyboardInvocation) {
        clientPoint = {caretRectangle_.left, caretRectangle_.bottom};
        screenPoint = clientPoint;
        ClientToScreen(window_, &screenPoint);
    } else {
        clientPoint = screenPoint;
        ScreenToClient(window_, &clientPoint);
    }
    classic_notepad::SpellingContextTarget target;
    if (contextMenuHandler_ &&
        ResolveSpellingContextTargetAtPoint(clientPoint, keyboardInvocation, target) &&
        contextMenuHandler_(screenPoint, target)) return;
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
    const std::uint64_t previousLayoutRevision = layoutRevision_;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureEditable(errorMessage) || start > end || end > document_->CharacterCount()) {
            if (errorMessage.empty()) errorMessage = L"The selection is outside the large document.";
            return false;
        }
        if (!document_->IsCharacterBoundary(start, errorMessage) ||
            !document_->IsCharacterBoundary(end, errorMessage)) return false;
        selectionStart_ = start;
        selectionEnd_ = end;
        selectionAnchor_ = start;
        caretPosition_ = end;
        allSelected_ = start == 0U && end == document_->CharacterCount();
        ++selectionRevision_;
    }
    EnsureCaretVisible();
    InvalidateRect(window_, nullptr, FALSE);
    UpdateCaret();
    if (layoutRevision_ != previousLayoutRevision) NotifyStateChanged();
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
        ++selectionRevision_;
        const bool haveAfter = document_->PositionToLineColumn(caretPosition_, afterCoordinate, errorMessage);
        if (indexingComplete_.load()) maximumLineColumns_.store(document_->MaximumLineColumns());
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
    EnsureCaretVisible();
    InvalidateRect(window_, nullptr, FALSE);
    UpdateCaret();
    return true;
}

bool LargeFileView::InsertText(const std::wstring& text, std::wstring& errorMessage)
{
    if (!nextInsertFailureForTesting_.empty()) {
        errorMessage = std::move(nextInsertFailureForTesting_);
        nextInsertFailureForTesting_.clear();
        return false;
    }
    return ReplaceSelection(classic_notepad::NormalizeLineEndingsForEditor(text), errorMessage);
}

void LargeFileView::InjectNextInsertFailureForTesting(std::wstring errorMessage)
{
    nextInsertFailureForTesting_ = std::move(errorMessage);
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
            const std::uint64_t total = document_->CharacterCount();
            const std::uint64_t contextStart = selectionStart_ > 2U ? selectionStart_ - 2U : 0U;
            const std::uint64_t contextLength = std::min<std::uint64_t>(4U, total - contextStart);
            std::wstring context;
            if (!document_->ReadRange(contextStart, contextLength, context, errorMessage)) return false;
            classic_notepad::TextUnitRange localRange{};
            if (!classic_notepad::CollapsedDeleteRange(
                    context, static_cast<std::size_t>(selectionStart_ - contextStart), backspace, localRange)) {
                errorMessage = L"The deletion position could not be read safely.";
                return false;
            }
            if (localRange.start == localRange.end) return true;
            selectionStart_ = contextStart + localRange.start;
            selectionEnd_ = contextStart + localRange.end;
            ++selectionRevision_;
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
        ++selectionRevision_;
        const bool haveAfter = document_->PositionToLineColumn(selectionStart_, afterStart, errorMessage) &&
            document_->PositionToLineColumn(selectionEnd_, afterEnd, errorMessage);
        if (indexingComplete_.load()) maximumLineColumns_.store(document_->MaximumLineColumns());
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
    EnsureCaretVisible();
    InvalidateRect(window_, nullptr, FALSE);
    UpdateCaret();
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
    if (!EnsureCompleteDocument(errorMessage)) return false;
    classic_notepad::LargeDocumentMatch match;
    const std::uint64_t start = forward ? selectionEnd_ : selectionStart_;
    if (!document_->Find(text, start, matchCase, wholeWord, forward, match, errorMessage)) return false;
    selectionStart_ = match.start;
    selectionEnd_ = match.start + match.length;
    selectionAnchor_ = selectionStart_;
    caretPosition_ = selectionEnd_;
    allSelected_ = false;
    ++selectionRevision_;
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

bool LargeFileView::SelectionMatches(
    const std::wstring& text,
    bool matchCase,
    bool wholeWord,
    std::wstring& errorMessage) const
{
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (!EnsureCompleteDocument(errorMessage)) return false;
    if (selectionEnd_ < selectionStart_ || selectionEnd_ - selectionStart_ != text.size()) return false;
    return document_->MatchesAt(selectionStart_, text, matchCase, wholeWord, errorMessage);
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
        if (!EnsureCompleteDocument(errorMessage)) return 0;
        count = document_->ReplaceAll(text, classic_notepad::NormalizeLineEndingsForEditor(replacement), matchCase, wholeWord,
            cancelled, [this] { return cancel_.load(); }, errorMessage);
        if (count != 0U && indexingComplete_.load()) {
            maximumLineColumns_.store(document_->MaximumLineColumns());
        }
        modifiedState = document_->IsModified();
    }
    if (count != 0U) {
        modified_.store(modifiedState);
        RefreshScrollBars();
        MarkModified();
    }
    return count;
}

bool LargeFileView::WriteStreamToTemporaryFile(
    const classic_notepad::LargeDocument& source,
    classic_notepad::WindowsSaveTransaction& transaction,
    classic_notepad::TextEncoding encoding,
    classic_notepad::LineEndingStyle lineEnding,
    std::wstring& errorMessage)
{
    bool okay = true;
    bool includeBom = true;
    bool pendingCr = false;
    auto writeBytes = [&transaction](const std::vector<std::uint8_t>& bytes) {
        return transaction.Write(bytes.data(), bytes.size());
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
    okay = source.StreamText(emit, [this] { return operationCancel_.load(); }, errorMessage);
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
    if (okay) {
        okay = transaction.Flush();
        if (!okay) errorMessage = transaction.ErrorMessage();
    }
    if (!okay && errorMessage.empty()) errorMessage = transaction.ErrorMessage();
    return okay;
}

bool LargeFileView::Save(
    const std::wstring& path,
    classic_notepad::TextEncoding encoding,
    classic_notepad::LineEndingStyle lineEnding,
    bool /*rebase*/,
    std::wstring& errorMessage)
{
    errorMessage.clear();
    {
        std::lock_guard<std::mutex> operationLock(operationMutex_);
        saveDiagnostics_ = {};
    }
    if (path.empty()) { errorMessage = L"No file path has been selected."; return false; }
    classic_notepad::WindowsSaveTransaction transaction;
    bool wrote = false;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (!EnsureCompleteDocument(errorMessage)) return false;
        if (!modified_.load() && path == path_) return true;
        generation = document_->Generation();
        saveInProgress_.store(true);
        saveProgressBytes_.store(0);
        wrote = transaction.Create(path) &&
            WriteStreamToTemporaryFile(*document_, transaction, encoding, lineEnding, errorMessage);
    }
    if (!wrote) {
        if (errorMessage.empty()) errorMessage = transaction.ErrorMessage();
        if (!transaction.Diagnostics().outputComplete) transaction.DeleteTemporary();
        {
            std::lock_guard<std::mutex> operationLock(operationMutex_);
            saveDiagnostics_ = transaction.Diagnostics();
        }
        saveInProgress_.store(false);
        return false;
    }
    const std::wstring temporaryPath = transaction.TemporaryPath();
    const std::wstring previousPath = path_;
    const std::uint64_t savedTopLine = topLine_;
    const std::uint64_t savedFileSize = fileSize_;
    const std::uint64_t savedDataOffset = dataOffset_;
    const std::uint64_t savedAppendedByte = appendedByte_;
    const std::uint64_t savedAppendedCharacter = appendedCharacter_;
    const std::uint64_t savedAppendedLineBreaks = appendedLineBreaks_;
    const classic_notepad::LargeFileEncoding savedEncoding = encoding_;
    std::vector<std::uint64_t> savedLineOffsets;
    std::vector<std::uint64_t> savedLineCharacterOffsets;
    {
        std::lock_guard<std::mutex> lock(lineMutex_);
        savedLineOffsets = lineOffsets_;
        savedLineCharacterOffsets = lineCharacterOffsets_;
    }

    HANDLE recoveryFile = INVALID_HANDLE_VALUE;
    HANDLE recoveryMapping = nullptr;
    DWORD duplicateError = ERROR_SUCCESS;
    BOOL duplicatedFile = FALSE;
    BOOL duplicatedMapping = FALSE;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        duplicatedFile = DuplicateHandle(GetCurrentProcess(), file_, GetCurrentProcess(),
            &recoveryFile, 0, FALSE, DUPLICATE_SAME_ACCESS);
        duplicateError = duplicatedFile ? ERROR_SUCCESS : GetLastError();
        duplicatedMapping = duplicatedFile && DuplicateHandle(GetCurrentProcess(), mapping_,
            GetCurrentProcess(), &recoveryMapping, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    if (!duplicatedFile || !duplicatedMapping) {
        const DWORD errorCode = duplicatedFile ? GetLastError() : duplicateError;
        if (recoveryFile != INVALID_HANDLE_VALUE) CloseHandle(recoveryFile);
        if (recoveryMapping != nullptr) CloseHandle(recoveryMapping);
        auto& diagnostics = transaction.Diagnostics();
        diagnostics.failureStage = classic_notepad::SaveFailureStage::Prepare;
        diagnostics.win32Error = errorCode;
        diagnostics.outputComplete = true;
        diagnostics.committed = false;
        diagnostics.recoveryPath = temporaryPath;
        saveInProgress_.store(false);
        errorMessage = classic_notepad::FormatWindowsSaveError(diagnostics,
            L"The source mapping could not be protected before replacement.");
        {
            std::lock_guard<std::mutex> operationLock(operationMutex_);
            saveDiagnostics_ = diagnostics;
        }
        return false;
    }

    StopIndexing();
    cancel_.store(false);
    bool okay = false;
    bool preserveTemporary = false;
    bool startIndexingAfterSave = false;
    DWORD reopenErrorCode = ERROR_SUCCESS;
    std::wstring reopenError;
    {
        // Validate the current model and make replacement/reopen/adoption one
        // critical section. A failed replacement or reopen keeps the edited
        // model and its original mapped source alive.
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || document_->Generation() != generation) {
            preserveTemporary = true;
            transaction.Diagnostics().failureStage = classic_notepad::SaveFailureStage::Replace;
            transaction.Diagnostics().win32Error = ERROR_RETRY;
            transaction.Diagnostics().outputComplete = true;
            transaction.Diagnostics().recoveryPath = temporaryPath;
            errorMessage = L"The document changed while its Save was in progress. The complete earlier snapshot remains available at: " +
                temporaryPath;
        } else {
            CloseMapping();
            okay = transaction.Commit(path);
            if (!okay) {
                file_ = recoveryFile;
                mapping_ = recoveryMapping;
                recoveryFile = INVALID_HANDLE_VALUE;
                recoveryMapping = nullptr;
                path_ = previousPath;
                cancel_.store(false);
                preserveTemporary = true;
                errorMessage = transaction.ErrorMessage();
            } else {
                const bool injectReopenFailure = classic_notepad::WindowsSaveFaultRequested(L"reopen");
                const bool reopened = OpenMapping(path, reopenError, injectReopenFailure);
                if (reopened) {
                    okay = true;
                    document_.reset();
                    selectionStart_ = 0;
                    selectionEnd_ = 0;
                    selectionAnchor_ = 0;
                    caretPosition_ = 0;
                    modified_.store(false);
                    path_ = path;
                    startIndexingAfterSave = true;
                } else {
                    okay = false;
                    reopenErrorCode = GetLastError();
                    if (reopenErrorCode == ERROR_SUCCESS) reopenErrorCode = ERROR_OPEN_FAILED;
                    file_ = recoveryFile;
                    mapping_ = recoveryMapping;
                    recoveryFile = INVALID_HANDLE_VALUE;
                    recoveryMapping = nullptr;
                    fileSize_ = savedFileSize;
                    dataOffset_ = savedDataOffset;
                    encoding_ = savedEncoding;
                    appendedByte_ = savedAppendedByte;
                    appendedCharacter_ = savedAppendedCharacter;
                    appendedLineBreaks_ = savedAppendedLineBreaks;
                    path_ = previousPath;
                    cancel_.store(false);
                    {
                        std::lock_guard<std::mutex> lineLock(lineMutex_);
                        lineOffsets_ = std::move(savedLineOffsets);
                        lineCharacterOffsets_ = std::move(savedLineCharacterOffsets);
                    }
                    transaction.SetReopenFailure(reopenErrorCode, path);
                    errorMessage = transaction.ErrorMessage() + L" The original document remains open and dirty. " + reopenError;
                }
            }
        }
    }
    if (recoveryMapping != nullptr) CloseHandle(recoveryMapping);
    if (recoveryFile != INVALID_HANDLE_VALUE) CloseHandle(recoveryFile);
    if (startIndexingAfterSave) {
        topLine_ = savedTopLine;
        startedTick_ = GetTickCount64();
        StartIndexing();
    }
    if (!okay && !preserveTemporary) DeleteFileW(temporaryPath.c_str());
    {
        std::lock_guard<std::mutex> operationLock(operationMutex_);
        saveDiagnostics_ = transaction.Diagnostics();
    }
    saveInProgress_.store(false);
    if (!okay) return false;
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
        ++selectionRevision_;
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
    info.maximumTopLine = classic_notepad::LargeFileScrollMapping::Create(
        EstimatedTotalLines(), VisibleLineCount()).MaximumTop();
    info.visibleLineCount = VisibleLineCount();
    if (window_ != nullptr) {
        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        if (GetScrollInfo(window_, SB_VERT, &scroll)) {
            info.verticalScrollRangeMaximum = scroll.nMax;
            info.verticalScrollPage = scroll.nPage;
            info.verticalScrollPosition = scroll.nPos;
        }
    }
    info.saveProgressBytes = saveProgressBytes_.load();
    info.saveInProgress = saveInProgress_.load();
    info.paintGeneration = paintGeneration_.load();
    info.callbackAllocationFailures = callbackAllocationFailures_;
    info.viewportGeneration = viewportGeneration_;
    info.wordWrap = wordWrap_;
    info.viewportVisualRows = static_cast<std::uint64_t>(viewportRows_.size());
    info.viewportCacheRebuilds = viewportCacheRebuilds_;
    info.viewportCacheHits = viewportCacheHits_;
    info.viewportLastBuildDecodedCharacters = viewportLastBuildDecodedCharacters_;
    info.viewportLastBuildMeasuredCharacters = viewportLastBuildMeasuredCharacters_;
    info.viewportLastLogicalAnchor = viewportLastLogicalAnchor_;
    info.viewportLastSegmentStart = viewportLastSegmentStart_;
    info.viewportLastSegmentEnd = viewportLastSegmentEnd_;
    for (std::size_t index = 1; index < viewportRows_.size(); ++index) {
        if (viewportRows_[index].line == viewportRows_[index - 1U].line) ++info.wrappedContinuationRows;
    }
    info.maximumLineColumns = maximumLineColumns_.load();
    info.horizontalOffset = horizontalOffset_;
    info.horizontalMaximumOffset = horizontalMaximumOffset_;
    RECT viewport{};
    if (window_ != nullptr) GetClientRect(window_, &viewport);
    info.viewportWidth = static_cast<int>(viewport.right - viewport.left);
    info.viewportHeight = static_cast<int>(viewport.bottom - viewport.top);
    info.caretRectangle = caretRectangle_;
    info.encoding = encoding_;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        info.caret = caretPosition_;
        info.selectionStart = selectionStart_;
        info.selectionEnd = selectionEnd_;
        if (document_ != nullptr) {
            info.editable = encodingValid_.load();
            info.characterCount = document_->CharacterCount();
            info.displayedCharacterCount = document_->DisplayedCharacterCount();
            info.displayedCharacterCountKnown = document_->HasExactDisplayedCharacterCount();
            info.totalLines = document_->LineCount();
            info.generation = document_->Generation();
            info.spellingIssueCount = classic_notepad::SpellingBatchIsCurrent(
                spellingBatch_, {sessionId_, info.generation, layoutRevision_})
                ? static_cast<std::uint64_t>(spellingBatch_.annotations.size()) : 0U;
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
        info.saveDiagnostics = saveDiagnostics_;
        info.operationGeneration = operationGeneration_;
        info.operationToken = operationToken_;
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

bool LargeFileView::SendVerticalScrollInput(UINT command, int trackPosition)
{
    if (window_ == nullptr || !IsWindow(window_) || command > SB_ENDSCROLL) return false;
    if (command == SB_THUMBPOSITION || command == SB_THUMBTRACK) {
        ScrollVertical(static_cast<int>(command), trackPosition);
    } else {
        SendMessageW(window_, WM_VSCROLL, MAKEWPARAM(command, 0), 0);
    }
    return true;
}

bool LargeFileView::SendMouseWheelInput(int delta)
{
    if (window_ == nullptr || !IsWindow(window_)) return false;
    const int bounded = std::clamp(delta, -32768, 32767);
    SendMessageW(window_, WM_MOUSEWHEEL,
        MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(bounded))), 0);
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
        try {
            return view->HandleMessage(message, wParam, lParam);
        } catch (const std::bad_alloc&) {
            // C++ exceptions must never unwind through the Win32 callback ABI.
        } catch (const std::length_error&) {
        }
        ++view->callbackAllocationFailures_;
        view->InvalidateViewportCache();
        view->DestroyTextCaret();
        OutputDebugStringW(L"Classic Notepad: mapped view presentation ran out of memory; the active document is retained.\n");
        // A later paint/input event can rebuild the presentation. Avoid an
        // immediate retry loop while memory remains unavailable.
        if (message == WM_NCCREATE) return FALSE;
        if (message == WM_CREATE) return -1;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT LargeFileView::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_SYSKEYDOWN:
    case WM_SYSCHAR:
    case WM_SYSKEYUP:
        if (parent_ != nullptr && IsWindow(parent_)) {
            return SendMessageW(parent_, message, wParam, lParam);
        }
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB | DLGC_WANTALLKEYS;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC deviceContext = BeginPaint(window_, &paint);
        const auto paintCleanup = OnScopeExit([&] { EndPaint(window_, &paint); });
        Paint(deviceContext, paint.rcPaint);
        UpdateCaret();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE: {
        // A resize can shrink a wrapped viewport enough to clip a caret that
        // was visible in the previous layout. Preserve an explicit scroll
        // away from the caret, though: only re-anchor when the caret occupied
        // the old cached viewport before this size change.
        bool caretWasVisibleInWrappedViewport = false;
        if (wordWrap_ && viewportCacheValid_ && viewportCacheWordWrap_ &&
            viewportCacheTopLine_ == topLine_ && viewportCacheTopWrapOffset_ == topWrapOffset_ &&
            viewportCacheLayoutRevision_ == layoutRevision_) {
            std::size_t caretRow = 0;
            if (FindCachedRowForPosition(caretPosition_, caretRow) && caretRow < viewportRows_.size()) {
                const CachedViewportRow& row = viewportRows_[caretRow];
                const std::uint64_t local = caretPosition_ >= row.segmentStart
                    ? caretPosition_ - row.segmentStart : (std::numeric_limits<std::uint64_t>::max)();
                const LONG caretHeight = caretRectangle_.bottom - caretRectangle_.top;
                if (local < row.xBoundary.size() && caretHeight > 0 &&
                    viewportCacheWidth_ > 0 && viewportCacheHeight_ > 0) {
                    const std::uint64_t oldVisibleRows = std::max<std::uint64_t>(1U,
                        static_cast<std::uint64_t>(viewportCacheHeight_ / caretHeight));
                    const int caretX = row.drawOriginX + row.xBoundary[static_cast<std::size_t>(local)];
                    caretWasVisibleInWrappedViewport = caretRow < oldVisibleRows &&
                        caretX >= 0 && caretX + 2 <= viewportCacheWidth_;
                }
            }
        }
        InvalidateViewportCache();
        RefreshScrollBars();
        if (caretWasVisibleInWrappedViewport) EnsureCaretVisible();
        UpdateCaret();
        NotifyStateChanged();
        return 0;
    }
    case WM_SETFOCUS:
        InjectPresentationAllocationFailureForTesting(false);
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
            if (overwriteMode_ && unit != L'\r' && unit != L'\n' &&
                selectionStart_ == selectionEnd_) {
                std::lock_guard<std::mutex> lock(documentMutex_);
                if (document_ != nullptr && selectionEnd_ < document_->CharacterCount()) {
                    wchar_t character = 0;
                    if (!document_->CharacterAt(selectionEnd_, character, error)) {
                        ShowInputError(error);
                        return 0;
                    }
                    if (character != L'\r' && character != L'\n') {
                        std::uint64_t replacementEnd = selectionEnd_ + 1U;
                        if (classic_notepad::IsHighSurrogateUnit(character) &&
                            replacementEnd < document_->CharacterCount()) {
                            wchar_t lowSurrogate = 0;
                            if (!document_->CharacterAt(replacementEnd, lowSurrogate, error)) {
                                ShowInputError(error);
                                return 0;
                            }
                            if (classic_notepad::IsLowSurrogateUnit(lowSurrogate)) ++replacementEnd;
                        }
                        selectionEnd_ = replacementEnd;
                    }
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
    case kLargeFileOperationCompletedMessage:
        AdoptOperationCompletion(static_cast<std::uint64_t>(wParam));
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
