#include "app.h"

#include "app_version.h"
#include "automation.h"
#include "../../editor_text_semantics.h"
#include "encoding.h"
#include "line_endings.h"
#include "resource.h"
#include "spell_text_utils.h"
#include "windows_menu_text.h"
#include "../../text_search_semantics.h"
#include "text_metadata.h"

#include <cderr.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kMainWindowClass[] = L"ClassicNotepadMainWindow";
constexpr wchar_t kMenuBarClass[] = L"ClassicNotepadMenuBar";
constexpr wchar_t kScrollBarClass[] = L"ClassicNotepadScrollBar";
constexpr wchar_t kMessageDialogClass[] = L"ClassicNotepadMessageDialog";
constexpr wchar_t kAppTitle[] = L"Untitled - Classic Notepad";
constexpr wchar_t kFileDialogFilter[] = L"Text Documents (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
constexpr wchar_t kFindReplaceMessageName[] = L"commdlg_FindReplace";
constexpr DWORD kFindOptionFlags = FR_DOWN | FR_MATCHCASE | FR_WHOLEWORD;
constexpr int kTabWidthSpaces = 8;
constexpr UINT_PTR kSpellCheckTimerId = 1;
constexpr UINT_PTR kEditorWidthIndexTimerId = 2;
constexpr UINT_PTR kAutomationContextMenuDismissTimerId = 3;
constexpr UINT_PTR kStatusCharacterCountTimerId = 4;
constexpr UINT kSpellCheckDelayMs = 300;
constexpr UINT kEditorWidthIndexDelayMs = 10;
constexpr std::size_t kEditorWidthIndexBatchLineLimit = 96U;
constexpr std::size_t kEditorWidthIndexBatchTextUnitLimit = 64U * 1024U;
constexpr std::size_t kEditorWidthIndexLocalLineLimit = 96U;
constexpr std::size_t kEditorWidthIndexLocalTextUnitLimit = 64U * 1024U;
constexpr UINT kSpellMenuSuggestionBase = 50000;
constexpr UINT kSpellMenuNoSuggestions = 50005;
constexpr UINT kSpellMenuIgnoreOnce = 50006;
constexpr UINT kSpellMenuAddToDictionary = 50007;
constexpr std::size_t kMaxVisibleSpellCheckChars = 96U * 1024U;
constexpr int kExactLineWidthMeasureCharLimit = 4096;
constexpr int kMaxEditorLineFetchChars = 0xFFFE;
constexpr int kStatusCharacterCountBatchUnitLimit = 64 * 1024;
constexpr int kDefaultEditorFontPointSize = 11;
constexpr wchar_t kDefaultEditorFontFace[] = L"Consolas";
constexpr COLORREF kDarkEditorBackground = RGB(32, 32, 32);
constexpr COLORREF kDarkEditorText = RGB(242, 242, 242);
constexpr COLORREF kDarkStatusBackground = RGB(43, 43, 43);
constexpr COLORREF kDarkStatusText = RGB(216, 216, 216);
constexpr COLORREF kDarkStatusSeparator = RGB(68, 68, 68);
constexpr COLORREF kLightStatusSeparator = RGB(210, 210, 210);
constexpr COLORREF kDarkKeyLine = RGB(0, 0, 0);
constexpr COLORREF kDarkResizeGrip = kDarkKeyLine;
constexpr COLORREF kDarkMenuBackground = RGB(38, 38, 38);
constexpr COLORREF kDarkMenuHotBackground = RGB(58, 58, 58);
constexpr COLORREF kDarkMenuActiveBackground = RGB(70, 70, 70);
constexpr COLORREF kDarkMenuText = RGB(245, 245, 245);
constexpr COLORREF kDarkMenuDisabledText = RGB(150, 150, 150);
constexpr COLORREF kDarkMenuSeparator = RGB(82, 82, 82);
constexpr COLORREF kDarkScrollTrack = RGB(32, 32, 32);
constexpr COLORREF kDarkScrollThumb = RGB(78, 78, 78);
constexpr COLORREF kDarkScrollThumbActive = RGB(96, 96, 96);
constexpr int kWindows11MenuFontPixelSize = 14;
constexpr wchar_t kWindows11MenuFontFace[] = L"Segoe UI Variable";
constexpr wchar_t kFallbackMenuFontFace[] = L"Segoe UI";
constexpr int kMenuHorizontalPadding = 12;
constexpr int kPopupMenuHorizontalPadding = 12;
constexpr int kPopupMenuCheckWidth = 28;
constexpr int kPopupMenuAcceleratorGap = 24;
constexpr int kPopupMenuItemHeight = 32;
constexpr int kPopupMenuSeparatorHeight = 9;
constexpr int kMinimumScrollThumbLength = 28;
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmUseImmersiveDarkModeBefore20H1 = 19;
constexpr DWORD kDwmBorderColor = 34;
constexpr COLORREF kDwmDefaultColor = 0xFFFFFFFF;
constexpr int kAboutIconSizePixels = 72;
constexpr int kStatusBarPartCount = 4;
constexpr UINT kStatusBarCustomDrawCode = static_cast<UINT>(-12);
constexpr std::size_t kFileDialogPathBufferChars = 32768U;
constexpr int kStatusBarHorizontalPadding = 12;
constexpr int kStatusBarExtraPartRoom = 20;
constexpr int kStatusBarSeparatorInset = 7;
constexpr int kStatusBarMinHeight = 32;
constexpr std::array<int, kStatusBarPartCount - 1> kStatusBarPartRightPercents{ 18, 40, 62 };

std::wstring FormatSpellHRESULT(HRESULT hresult)
{
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"0x%08lX", static_cast<unsigned long>(hresult));
    return buffer;
}

classic_notepad::TransitionSaveObservation MakeTransitionSaveObservation(const LargeFileViewInfo& info)
{
    return {
        info.operationInProgress,
        info.operationCompleted,
        info.operationName == L"Save",
        info.operationToken,
        info.operationSucceeded,
        info.modified,
    };
}

bool IsValidSpellReplacement(const std::wstring& replacement)
{
    if (replacement.empty()) return false;
    for (std::size_t index = 0U; index < replacement.size(); ++index) {
        const std::uint32_t unit = static_cast<std::uint32_t>(replacement[index]);
        if (unit <= 0x001FU || (unit >= 0x007FU && unit <= 0x009FU)) return false;
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            if (index + 1U >= replacement.size()) return false;
            const std::uint32_t next = static_cast<std::uint32_t>(replacement[index + 1U]);
            if (next < 0xDC00U || next > 0xDFFFU) return false;
            ++index;
        } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
            return false;
        }
    }
    return true;
}

class ScopedLocalBufferLock {
public:
    explicit ScopedLocalBufferLock(HLOCAL memory) noexcept
        : memory_(memory), value_(memory == nullptr ? nullptr : static_cast<wchar_t*>(LocalLock(memory)))
    {
    }
    ~ScopedLocalBufferLock()
    {
        if (memory_ != nullptr && value_ != nullptr) LocalUnlock(memory_);
    }
    ScopedLocalBufferLock(const ScopedLocalBufferLock&) = delete;
    ScopedLocalBufferLock& operator=(const ScopedLocalBufferLock&) = delete;
    wchar_t* Get() const noexcept { return value_; }

private:
    HLOCAL memory_ = nullptr;
    wchar_t* value_ = nullptr;
};

bool TryGetNativeCollapsedDeleteRange(
    HWND editor,
    DWORD caret,
    bool backspace,
    classic_notepad::TextUnitRange& range)
{
    if (editor == nullptr || !IsWindow(editor)) return false;
    const int textLength = std::max(0, GetWindowTextLengthW(editor));
    if (caret > static_cast<DWORD>(textLength)) return false;
    const HLOCAL buffer = reinterpret_cast<HLOCAL>(SendMessageW(editor, EM_GETHANDLE, 0, 0));
    ScopedLocalBufferLock lockedBuffer(buffer);
    if (lockedBuffer.Get() == nullptr) return false;
    return classic_notepad::CollapsedDeleteRange(
        std::wstring_view(lockedBuffer.Get(), static_cast<std::size_t>(textLength)),
        caret, backspace, range);
}

bool TryGetNativeOvertypeRange(
    HWND editor,
    DWORD caret,
    classic_notepad::TextUnitRange& range)
{
    if (editor == nullptr || !IsWindow(editor)) return false;
    const int textLength = std::max(0, GetWindowTextLengthW(editor));
    if (caret >= static_cast<DWORD>(textLength)) return false;
    const HLOCAL buffer = reinterpret_cast<HLOCAL>(SendMessageW(editor, EM_GETHANDLE, 0, 0));
    ScopedLocalBufferLock lockedBuffer(buffer);
    if (lockedBuffer.Get() == nullptr) return false;
    const std::wstring_view text(lockedBuffer.Get(), static_cast<std::size_t>(textLength));
    range = {caret, caret};
    if (text[caret] == L'\r' || text[caret] == L'\n') return true;
    if (classic_notepad::IsLowSurrogateUnit(text[caret]) && caret > 0U &&
        classic_notepad::IsHighSurrogateUnit(text[caret - 1U])) {
        range = {caret - 1U, caret + 1U};
        return true;
    }
    range.end = classic_notepad::NextScalarBoundary(text, caret);
    if (range.end <= caret) return false;
    return true;
}

class ScopedWindow {
public:
    explicit ScopedWindow(HWND window = nullptr) noexcept : window_(window) {}
    ~ScopedWindow()
    {
        if (window_ != nullptr && IsWindow(window_)) DestroyWindow(window_);
    }
    ScopedWindow(const ScopedWindow&) = delete;
    ScopedWindow& operator=(const ScopedWindow&) = delete;
    HWND Get() const noexcept { return window_; }
    HWND Release() noexcept
    {
        HWND window = window_;
        window_ = nullptr;
        return window;
    }

private:
    HWND window_;
};

class ScopedBoolRestore {
public:
    ScopedBoolRestore(bool& value, bool replacement) noexcept : value_(value), previous_(value)
    {
        value_ = replacement;
    }
    ~ScopedBoolRestore() { value_ = previous_; }
    ScopedBoolRestore(const ScopedBoolRestore&) = delete;
    ScopedBoolRestore& operator=(const ScopedBoolRestore&) = delete;

private:
    bool& value_;
    bool previous_;
};

template <typename TValue>
class ScopedValueRestore {
public:
    ScopedValueRestore(TValue& value, TValue replacement) noexcept
        : value_(value), previous_(value)
    {
        value_ = replacement;
    }
    ~ScopedValueRestore() { value_ = previous_; }
    ScopedValueRestore(const ScopedValueRestore&) = delete;
    ScopedValueRestore& operator=(const ScopedValueRestore&) = delete;

private:
    TValue& value_;
    TValue previous_;
};

bool IsInjectedPostAdoptionAllocationFailure(bool automationMode) noexcept
{
    if (!automationMode) return false;
    wchar_t stage[64]{};
    const DWORD length = GetEnvironmentVariableW(
        L"CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE", stage, static_cast<DWORD>(std::size(stage)));
    return length > 0U && length < std::size(stage) && wcscmp(stage, L"after-adoption") == 0;
}

bool IsInjectedMappedMetadataAllocationFailure(bool automationMode) noexcept
{
    if (!automationMode) return false;
    wchar_t stage[64]{};
    const DWORD length = GetEnvironmentVariableW(
        L"CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE", stage, static_cast<DWORD>(std::size(stage)));
    if (length == 0U || length >= std::size(stage)) return false;
    if (wcscmp(stage, L"mapped-adoption") == 0) return true;
    if (wcscmp(stage, L"mapped-adoption-after-first") == 0) {
        static unsigned int candidateCount = 0;
        return ++candidateCount > 1U;
    }
    return false;
}

void SetAllocationFailureMessage(std::wstring& errorMessage) noexcept
{
    try {
        errorMessage.assign(L"The operation could not be completed because memory allocation failed.");
    } catch (...) {
        errorMessage.clear();
    }
}

void InjectMappedPresentationAllocationFailure(bool automationMode)
{
    if (!automationMode) return;
    wchar_t stage[64]{};
    const DWORD length = GetEnvironmentVariableW(
        L"CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE", stage, static_cast<DWORD>(std::size(stage)));
    if (length == 0U || length >= std::size(stage)) return;
    const bool badAlloc = wcscmp(stage, L"mapped-presentation-bad-alloc") == 0;
    const bool lengthError = wcscmp(stage, L"mapped-presentation-length-error") == 0;
    if (!badAlloc && !lengthError) return;
    static unsigned int publicationCount = 0;
    if (++publicationCount == 1U) return;
    if (badAlloc) throw std::bad_alloc();
    throw std::length_error("injected mapped presentation allocation failure");
}

void ReportAdoptedDocumentPresentationFailure(HWND mainWindow, bool automationMode) noexcept
{
    // Use a literal and native reporting: constructing the themed dialog can
    // itself allocate while recovering from an allocation failure.
    constexpr wchar_t message[] =
        L"The document was opened successfully, but its title or status display could not be refreshed because memory is unavailable.";
    OutputDebugStringW(message);
    if (!automationMode) MessageBoxW(mainWindow, message, L"Classic Notepad", MB_OK | MB_ICONWARNING);
}

int MappedCallbackAllocationFailureForTesting(bool automationMode) noexcept
{
    if (!automationMode) return 0;
    wchar_t stage[64]{};
    const DWORD length = GetEnvironmentVariableW(
        L"CLASSIC_NOTEPAD_TEST_ALLOCATION_FAILURE", stage, static_cast<DWORD>(std::size(stage)));
    if (length == 0U || length >= std::size(stage)) return 0;
    int failure = 0;
    if (wcscmp(stage, L"mapped-focus-bad-alloc") == 0) failure = 1;
    else if (wcscmp(stage, L"mapped-focus-length-error") == 0) failure = 2;
    else if (wcscmp(stage, L"mapped-paint-bad-alloc") == 0) failure = 3;
    else if (wcscmp(stage, L"mapped-paint-length-error") == 0) failure = 4;
    if (failure == 0) return 0;
    static unsigned int publicationCount = 0;
    return ++publicationCount == 1U ? 0 : failure;
}

bool IsPlainInsertKeyPress()
{
    return (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
        (GetKeyState(VK_SHIFT) & 0x8000) == 0 &&
        (GetKeyState(VK_MENU) & 0x8000) == 0;
}

bool IsOvertypeCharacter(WPARAM character)
{
    return character >= 0x20 && character != 0x7F;
}

bool IsHighSurrogateUnit(wchar_t value)
{
    const std::uint32_t unit = static_cast<std::uint32_t>(value);
    return unit >= 0xD800U && unit <= 0xDBFFU;
}

bool IsLowSurrogateUnit(wchar_t value)
{
    const std::uint32_t unit = static_cast<std::uint32_t>(value);
    return unit >= 0xDC00U && unit <= 0xDFFFU;
}

struct GoToDialogState {
    std::uint64_t currentLine = 1;
    std::uint64_t maxLine = 1;
    std::uint64_t selectedLine = 1;
    std::wstring* automationInitialValue = nullptr;
};

struct AboutDialogState {
    HINSTANCE instance = nullptr;
    HICON largeIcon = nullptr;
    bool ownsLargeIcon = false;
    bool darkMode = false;
    HBRUSH backgroundBrush = nullptr;
};

struct FontFamilySearch {
    const wchar_t* faceName = nullptr;
    bool found = false;
};

struct MessageDialogButton {
    int id = IDOK;
    std::wstring text;
    RECT rect {};
    bool defaultButton = false;
};

struct MessageDialogState {
    HINSTANCE instance = nullptr;
    HWND owner = nullptr;
    std::wstring title;
    std::wstring message;
    UINT flags = MB_OK;
    bool darkMode = false;
    int result = IDCANCEL;
    bool done = false;
    HFONT font = nullptr;
    HICON icon = nullptr;
    SIZE clientSize {};
    RECT iconRect {};
    RECT messageRect {};
    std::vector<MessageDialogButton> buttons;
};

LRESULT CALLBACK MessageDialogWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

void AddMessageDialogButton(std::vector<MessageDialogButton>& buttons, int id, const wchar_t* text)
{
    MessageDialogButton button;
    button.id = id;
    button.text = text;
    buttons.push_back(button);
}

int CALLBACK EnumFontFamilyMatchProc(const LOGFONTW* logFont, const TEXTMETRICW*, DWORD, LPARAM lParam)
{
    auto* search = reinterpret_cast<FontFamilySearch*>(lParam);
    if (search == nullptr || search->faceName == nullptr || logFont == nullptr) {
        return 0;
    }

    if (CompareStringOrdinal(logFont->lfFaceName, -1, search->faceName, -1, TRUE) == CSTR_EQUAL) {
        search->found = true;
        return 0;
    }

    return 1;
}

int SaturatingTextWidthEstimate(int characterCount, int averageCharacterWidth)
{
    if (characterCount <= 0 || averageCharacterWidth <= 0) {
        return 0;
    }

    if (characterCount > std::numeric_limits<int>::max() / averageCharacterWidth) {
        return std::numeric_limits<int>::max();
    }

    return characterCount * averageCharacterWidth;
}

bool GetChildWindowRect(HWND window, RECT& rect)
{
    if (window == nullptr) {
        return false;
    }

    GetWindowRect(window, &rect);
    HWND parent = GetParent(window);
    if (parent != nullptr) {
        MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    }

    return true;
}

void MoveChildWindowIfNeeded(HWND window, const RECT& rect, bool repaint)
{
    RECT currentRect{};
    if (GetChildWindowRect(window, currentRect) && EqualRect(&currentRect, &rect)) {
        return;
    }

    MoveWindow(
        window,
        rect.left,
        rect.top,
        std::max(0, static_cast<int>(rect.right - rect.left)),
        std::max(0, static_cast<int>(rect.bottom - rect.top)),
        repaint ? TRUE : FALSE);
}

POINT PointFromEditorCharPosition(HWND editor, DWORD charIndex)
{
    const LRESULT packedPosition = SendMessageW(editor, EM_POSFROMCHAR, static_cast<WPARAM>(charIndex), 0);
    POINT point{};
    point.x = static_cast<short>(LOWORD(packedPosition));
    point.y = static_cast<short>(HIWORD(packedPosition));
    return point;
}

bool TextMatchesAt(
    const std::wstring& text,
    std::size_t position,
    const std::wstring& needle,
    bool matchCase,
    bool wholeWord)
{
    return classic_notepad::SearchTextMatchesAt(text, position, needle, matchCase, wholeWord);
}

void CopyToFixedBuffer(std::array<wchar_t, 512>& buffer, const std::wstring& text)
{
    buffer.fill(L'\0');
    wcsncpy_s(buffer.data(), buffer.size(), text.c_str(), _TRUNCATE);
}

struct ClipboardApi {
    void* context = nullptr;
    BOOL (*open)(void*, HWND) = nullptr;
    void (*close)(void*) = nullptr;
    BOOL (*isFormatAvailable)(void*, UINT) = nullptr;
    HGLOBAL (*getData)(void*, UINT) = nullptr;
    bool (*getSize)(void*, HGLOBAL, SIZE_T*) = nullptr;
    const void* (*lock)(void*, HGLOBAL) = nullptr;
    void (*unlock)(void*, HGLOBAL) = nullptr;
    int (*convertAnsi)(void*, UINT, DWORD, LPCCH, int, LPWSTR, int) = nullptr;
    void (*beforeTextAllocation)(void*) = nullptr;
};

BOOL NativeClipboardOpen(void*, HWND owner) { return OpenClipboard(owner); }
void NativeClipboardClose(void*) { CloseClipboard(); }
BOOL NativeClipboardIsFormatAvailable(void*, UINT format) { return IsClipboardFormatAvailable(format); }
HGLOBAL NativeClipboardGetData(void*, UINT format) { return static_cast<HGLOBAL>(GetClipboardData(format)); }
bool NativeClipboardGetSize(void*, HGLOBAL memory, SIZE_T* size)
{
    *size = GlobalSize(memory);
    return true;
}
const void* NativeClipboardLock(void*, HGLOBAL memory) { return GlobalLock(memory); }
void NativeClipboardUnlock(void*, HGLOBAL memory) { GlobalUnlock(memory); }
int NativeClipboardConvertAnsi(void*, UINT codePage, DWORD flags, LPCCH source, int sourceLength, LPWSTR target, int targetLength)
{
    return MultiByteToWideChar(codePage, flags, source, sourceLength, target, targetLength);
}
void NativeClipboardBeforeTextAllocation(void*) {}

const ClipboardApi& GetNativeClipboardApi()
{
    static const ClipboardApi api{
        nullptr,
        NativeClipboardOpen,
        NativeClipboardClose,
        NativeClipboardIsFormatAvailable,
        NativeClipboardGetData,
        NativeClipboardGetSize,
        NativeClipboardLock,
        NativeClipboardUnlock,
        NativeClipboardConvertAnsi,
        NativeClipboardBeforeTextAllocation,
    };
    return api;
}

class ClipboardOpenScope {
public:
    ClipboardOpenScope(const ClipboardApi& api, HWND owner)
        : api_(api), opened_(api_.open(api_.context, owner) != FALSE)
    {
    }

    ~ClipboardOpenScope()
    {
        if (opened_) {
            api_.close(api_.context);
        }
    }

    ClipboardOpenScope(const ClipboardOpenScope&) = delete;
    ClipboardOpenScope& operator=(const ClipboardOpenScope&) = delete;

    bool IsOpen() const
    {
        return opened_;
    }

private:
    const ClipboardApi& api_;
    bool opened_ = false;
};

enum class PasteOutcome {
    NoText,
    Empty,
    AcquisitionFailure,
    InvalidData,
    CapacityRejected,
    AllocationFailure,
    NativeFailure,
    NativePartial,
    Succeeded,
};

struct ClipboardTextReadResult {
    PasteOutcome outcome = PasteOutcome::NoText;
    std::wstring text;
};

const wchar_t* PasteOutcomeName(PasteOutcome outcome) noexcept
{
    switch (outcome) {
    case PasteOutcome::NoText: return L"no-text";
    case PasteOutcome::Empty: return L"empty";
    case PasteOutcome::AcquisitionFailure: return L"acquisition-failure";
    case PasteOutcome::InvalidData: return L"invalid-data";
    case PasteOutcome::CapacityRejected: return L"capacity-rejected";
    case PasteOutcome::AllocationFailure: return L"allocation-failure";
    case PasteOutcome::NativeFailure: return L"native-failure";
    case PasteOutcome::NativePartial: return L"native-partial";
    case PasteOutcome::Succeeded: return L"success";
    }
    return L"native-failure";
}

class GlobalMemoryLockScope {
public:
    GlobalMemoryLockScope(const ClipboardApi& api, HGLOBAL memory) noexcept
        : api_(api), memory_(memory), pointer_(memory == nullptr ? nullptr : api_.lock(api_.context, memory))
    {
    }

    ~GlobalMemoryLockScope()
    {
        if (pointer_ != nullptr) api_.unlock(api_.context, memory_);
    }

    GlobalMemoryLockScope(const GlobalMemoryLockScope&) = delete;
    GlobalMemoryLockScope& operator=(const GlobalMemoryLockScope&) = delete;

    const void* Get() const noexcept { return pointer_; }

private:
    const ClipboardApi& api_;
    HGLOBAL memory_ = nullptr;
    const void* pointer_ = nullptr;
};

PasteOutcome ReadUnicodeClipboardText(const ClipboardApi& api, std::wstring& text)
{
    HGLOBAL memory = api.getData(api.context, CF_UNICODETEXT);
    if (memory == nullptr) {
        return PasteOutcome::AcquisitionFailure;
    }

    SIZE_T byteCount = 0U;
    if (!api.getSize(api.context, memory, &byteCount)) {
        return PasteOutcome::AcquisitionFailure;
    }
    if (byteCount < sizeof(wchar_t) || byteCount % sizeof(wchar_t) != 0U) {
        return PasteOutcome::InvalidData;
    }

    GlobalMemoryLockScope lock(api, memory);
    if (lock.Get() == nullptr) {
        return PasteOutcome::AcquisitionFailure;
    }

    const auto* clipboardText = static_cast<const wchar_t*>(lock.Get());
    const std::size_t unitCount = static_cast<std::size_t>(byteCount / sizeof(wchar_t));
    std::size_t textLength = 0U;
    while (textLength < unitCount && clipboardText[textLength] != L'\0') ++textLength;
    if (textLength == unitCount) {
        return PasteOutcome::InvalidData;
    }
    if (textLength == 0U) {
        return PasteOutcome::Empty;
    }

    for (std::size_t index = 0U; index < textLength; ++index) {
        const wchar_t unit = clipboardText[index];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (index + 1U >= textLength || clipboardText[index + 1U] < 0xDC00 || clipboardText[index + 1U] > 0xDFFF) {
                return PasteOutcome::InvalidData;
            }
            ++index;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return PasteOutcome::InvalidData;
        }
    }

    if (api.beforeTextAllocation != nullptr) api.beforeTextAllocation(api.context);
    text.assign(clipboardText, textLength);
    return PasteOutcome::Succeeded;
}

PasteOutcome ReadAnsiClipboardText(const ClipboardApi& api, std::wstring& text)
{
    HGLOBAL memory = api.getData(api.context, CF_TEXT);
    if (memory == nullptr) {
        return PasteOutcome::AcquisitionFailure;
    }

    SIZE_T byteCount = 0U;
    if (!api.getSize(api.context, memory, &byteCount)) {
        return PasteOutcome::AcquisitionFailure;
    }
    if (byteCount == 0U) {
        return PasteOutcome::InvalidData;
    }

    GlobalMemoryLockScope lock(api, memory);
    if (lock.Get() == nullptr) {
        return PasteOutcome::AcquisitionFailure;
    }

    const auto* clipboardText = static_cast<const char*>(lock.Get());
    const auto* terminator = static_cast<const char*>(std::memchr(clipboardText, '\0', static_cast<std::size_t>(byteCount)));
    if (terminator == nullptr) {
        return PasteOutcome::InvalidData;
    }

    const std::size_t byteLength = static_cast<std::size_t>(terminator - clipboardText);
    if (byteLength == 0U) {
        return PasteOutcome::Empty;
    }
    if (byteLength > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return PasteOutcome::CapacityRejected;
    }

    const int sourceLength = static_cast<int>(byteLength);
    const int wideLength = api.convertAnsi(api.context, CP_ACP, MB_ERR_INVALID_CHARS, clipboardText, sourceLength, nullptr, 0);
    if (wideLength <= 0) {
        return PasteOutcome::InvalidData;
    }

    if (api.beforeTextAllocation != nullptr) api.beforeTextAllocation(api.context);
    std::wstring converted(static_cast<std::size_t>(wideLength), L'\0');
    if (api.convertAnsi(
            api.context, CP_ACP, MB_ERR_INVALID_CHARS, clipboardText, sourceLength, converted.data(), wideLength) != wideLength) {
        return PasteOutcome::InvalidData;
    }

    text.swap(converted);
    return text.empty() ? PasteOutcome::Empty : PasteOutcome::Succeeded;
}

ClipboardTextReadResult TryReadClipboardText(HWND owner, const ClipboardApi& api = GetNativeClipboardApi())
{
    ClipboardOpenScope clipboard(api, owner);
    if (!clipboard.IsOpen()) {
        return { PasteOutcome::AcquisitionFailure, {} };
    }

    const bool hasUnicodeText = api.isFormatAvailable(api.context, CF_UNICODETEXT) != FALSE;
    const bool hasAnsiText = api.isFormatAvailable(api.context, CF_TEXT) != FALSE;
    if (!hasUnicodeText && !hasAnsiText) {
        return { PasteOutcome::NoText, {} };
    }

    ClipboardTextReadResult result;
    try {
        result.outcome = hasUnicodeText
            ? ReadUnicodeClipboardText(api, result.text)
            : ReadAnsiClipboardText(api, result.text);
    } catch (const std::bad_alloc&) {
        result.text.clear();
        result.outcome = PasteOutcome::AllocationFailure;
    } catch (const std::length_error&) {
        result.text.clear();
        result.outcome = PasteOutcome::AllocationFailure;
    }
    return result;
}

class ScopedGlobalMemory {
public:
    ScopedGlobalMemory() = default;
    explicit ScopedGlobalMemory(HGLOBAL memory) : memory_(memory) {}
    ~ScopedGlobalMemory() { if (memory_ != nullptr) GlobalFree(memory_); }
    ScopedGlobalMemory(const ScopedGlobalMemory&) = delete;
    ScopedGlobalMemory& operator=(const ScopedGlobalMemory&) = delete;
    ScopedGlobalMemory(ScopedGlobalMemory&& other) noexcept : memory_(other.memory_) { other.memory_ = nullptr; }
    ScopedGlobalMemory& operator=(ScopedGlobalMemory&& other) noexcept
    {
        if (this != &other) {
            if (memory_ != nullptr) GlobalFree(memory_);
            memory_ = other.memory_;
            other.memory_ = nullptr;
        }
        return *this;
    }
    HGLOBAL Get() const noexcept { return memory_; }

private:
    HGLOBAL memory_ = nullptr;
};

bool CreateSyntheticClipboardBlock(const void* bytes, SIZE_T byteCount, ScopedGlobalMemory& result)
{
    const SIZE_T allocationSize = std::max<SIZE_T>(byteCount, 1U);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, allocationSize);
    if (memory == nullptr) return false;
    ScopedGlobalMemory owned(memory);
    void* destination = GlobalLock(memory);
    if (destination == nullptr) return false;
    if (byteCount != 0U) std::memcpy(destination, bytes, static_cast<std::size_t>(byteCount));
    GlobalUnlock(memory);
    result = std::move(owned);
    return true;
}

struct SyntheticClipboardContext {
    std::wstring faultStage;
    HGLOBAL unicodeMemory = nullptr;
    HGLOBAL ansiMemory = nullptr;
    SIZE_T unicodeLogicalSize = 0U;
    SIZE_T ansiLogicalSize = 0U;
    bool hasUnicodeText = true;
    bool hasAnsiText = true;
    ClassicNotepadApp::AutomationClipboardDiagnostics diagnostics;
};

class SyntheticClipboardFixture {
public:
    SyntheticClipboardFixture(const std::wstring& text, const std::wstring& faultStage)
    {
        context_.faultStage = faultStage;
        if (faultStage == L"no-text") {
            context_.hasUnicodeText = false;
            context_.hasAnsiText = false;
            return;
        }

        std::wstring unicodeText = text;
        std::vector<unsigned char> unicodeBytes;
        if (faultStage == L"unicode-unterminated") {
            unicodeBytes.resize(unicodeText.size() * sizeof(wchar_t));
            if (!unicodeBytes.empty()) std::memcpy(unicodeBytes.data(), unicodeText.data(), unicodeBytes.size());
        } else if (faultStage == L"unicode-padding") {
            unicodeBytes.resize((unicodeText.size() + 3U) * sizeof(wchar_t));
            if (!unicodeText.empty()) std::memcpy(unicodeBytes.data(), unicodeText.data(), unicodeText.size() * sizeof(wchar_t));
            const wchar_t padding[] = { L'\0', L'X', L'Y' };
            std::memcpy(unicodeBytes.data() + unicodeText.size() * sizeof(wchar_t), padding, sizeof(padding));
        } else {
            if (faultStage == L"invalid") unicodeText.assign(1U, static_cast<wchar_t>(0xD800));
            unicodeBytes.resize((unicodeText.size() + 1U) * sizeof(wchar_t));
            if (!unicodeText.empty()) std::memcpy(unicodeBytes.data(), unicodeText.data(), unicodeText.size() * sizeof(wchar_t));
            std::memset(unicodeBytes.data() + unicodeText.size() * sizeof(wchar_t), 0, sizeof(wchar_t));
        }

        if (faultStage == L"ansi" || faultStage == L"conversion") {
            context_.hasUnicodeText = false;
        }
        if (context_.hasUnicodeText) {
            const SIZE_T physicalSize = static_cast<SIZE_T>(std::max<std::size_t>(unicodeBytes.size(), sizeof(wchar_t)));
            unicodeBytes.resize(static_cast<std::size_t>(physicalSize), 0U);
            if (!CreateSyntheticClipboardBlock(unicodeBytes.data(), physicalSize, unicodeMemory_)) {
                throw std::bad_alloc();
            }
            context_.unicodeMemory = unicodeMemory_.Get();
            context_.unicodeLogicalSize = faultStage == L"unicode-odd-size"
                ? static_cast<SIZE_T>(3U)
                : static_cast<SIZE_T>(unicodeBytes.size());
        }

        if (context_.hasAnsiText) {
            std::vector<char> ansiBytes;
            const int sourceLength = text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())
                ? -1
                : static_cast<int>(text.size());
            if (sourceLength < 0) throw std::length_error("synthetic ANSI clipboard payload is too large");
            int byteLength = 0;
            if (sourceLength != 0) {
                byteLength = WideCharToMultiByte(CP_ACP, 0, text.data(), sourceLength, nullptr, 0, nullptr, nullptr);
                if (byteLength <= 0) throw std::bad_alloc();
            }
            ansiBytes.resize(static_cast<std::size_t>(byteLength) + 1U, '\0');
            if (byteLength != 0 && WideCharToMultiByte(
                    CP_ACP, 0, text.data(), sourceLength, ansiBytes.data(), byteLength, nullptr, nullptr) != byteLength) {
                throw std::bad_alloc();
            }
            if (!CreateSyntheticClipboardBlock(ansiBytes.data(), static_cast<SIZE_T>(ansiBytes.size()), ansiMemory_)) {
                throw std::bad_alloc();
            }
            context_.ansiMemory = ansiMemory_.Get();
            context_.ansiLogicalSize = static_cast<SIZE_T>(ansiBytes.size());
        }
    }

    ClipboardApi Api()
    {
        return ClipboardApi{
            &context_, Open, Close, IsFormatAvailable, GetData, GetSize, Lock, Unlock, ConvertAnsi, BeforeTextAllocation };
    }

    ClassicNotepadApp::AutomationClipboardDiagnostics Diagnostics() const { return context_.diagnostics; }

private:
    static SyntheticClipboardContext& Context(void* value) { return *static_cast<SyntheticClipboardContext*>(value); }
    static BOOL Open(void* value, HWND)
    {
        SyntheticClipboardContext& context = Context(value);
        ++context.diagnostics.openCalls;
        return context.faultStage == L"open" ? FALSE : TRUE;
    }
    static void Close(void* value) { ++Context(value).diagnostics.closeCalls; }
    static BOOL IsFormatAvailable(void* value, UINT format)
    {
        SyntheticClipboardContext& context = Context(value);
        if (format == CF_UNICODETEXT) {
            ++context.diagnostics.unicodeAvailabilityChecks;
            return context.hasUnicodeText ? TRUE : FALSE;
        }
        if (format == CF_TEXT) {
            ++context.diagnostics.ansiAvailabilityChecks;
            return context.hasAnsiText ? TRUE : FALSE;
        }
        return FALSE;
    }
    static HGLOBAL GetData(void* value, UINT format)
    {
        SyntheticClipboardContext& context = Context(value);
        if (format == CF_UNICODETEXT) {
            ++context.diagnostics.unicodeGetDataCalls;
            return context.faultStage == L"get" ? nullptr : context.unicodeMemory;
        }
        if (format == CF_TEXT) {
            ++context.diagnostics.ansiGetDataCalls;
            return context.ansiMemory;
        }
        return nullptr;
    }
    static bool GetSize(void* value, HGLOBAL memory, SIZE_T* size)
    {
        SyntheticClipboardContext& context = Context(value);
        ++context.diagnostics.sizeCalls;
        if (context.faultStage == L"size") return false;
        if (memory == context.unicodeMemory) *size = context.unicodeLogicalSize;
        else if (memory == context.ansiMemory) *size = context.ansiLogicalSize;
        else return false;
        return true;
    }
    static const void* Lock(void* value, HGLOBAL memory)
    {
        SyntheticClipboardContext& context = Context(value);
        ++context.diagnostics.lockCalls;
        if (context.faultStage == L"lock") return nullptr;
        return GlobalLock(memory);
    }
    static void Unlock(void* value, HGLOBAL memory)
    {
        ++Context(value).diagnostics.unlockCalls;
        GlobalUnlock(memory);
    }
    static int ConvertAnsi(void* value, UINT codePage, DWORD flags, LPCCH source, int sourceLength, LPWSTR target, int targetLength)
    {
        SyntheticClipboardContext& context = Context(value);
        ++context.diagnostics.conversionCalls;
        context.diagnostics.lastAnsiSourceLength = sourceLength;
        if (context.faultStage == L"conversion") return 0;
        return MultiByteToWideChar(codePage, flags, source, sourceLength, target, targetLength);
    }
    static void BeforeTextAllocation(void* value)
    {
        if (Context(value).faultStage == L"clipboard-allocation") throw std::bad_alloc();
    }

    SyntheticClipboardContext context_;
    ScopedGlobalMemory unicodeMemory_;
    ScopedGlobalMemory ansiMemory_;
};

bool GetNormalizedPasteLength(const std::wstring& text, std::size_t& normalizedLength) noexcept
{
    normalizedLength = text.size();
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (text[index] == L'\r' && index + 1U < text.size() && text[index + 1U] == L'\n') {
            ++index;
        } else if (text[index] != L'\r' && text[index] != L'\n') {
            continue;
        } else {
            if (normalizedLength == std::numeric_limits<std::size_t>::max()) return false;
            ++normalizedLength;
        }
    }
    return true;
}

std::wstring NormalizeClipboardTextForEditor(const std::wstring& text, std::size_t normalizedLength)
{
    std::wstring normalized;
    normalized.reserve(normalizedLength);
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            if (index + 1U < text.size() && text[index + 1U] == L'\n') ++index;
            normalized.push_back(L'\r');
            normalized.push_back(L'\n');
        } else if (text[index] == L'\n') {
            normalized.push_back(L'\r');
            normalized.push_back(L'\n');
        } else {
            normalized.push_back(text[index]);
        }
    }
    return normalized;
}

bool IsValidClipboardUtf16(const std::wstring& text) noexcept
{
    for (std::size_t index = 0U; index < text.size(); ++index) {
        const wchar_t unit = text[index];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (index + 1U >= text.size() || text[index + 1U] < 0xDC00 || text[index + 1U] > 0xDFFF) return false;
            ++index;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

std::vector<std::wstring> SplitPrintLines(const std::wstring& text)
{
    std::vector<std::wstring> lines;
    std::size_t lineStart = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != L'\r' && text[index] != L'\n') {
            continue;
        }

        lines.push_back(text.substr(lineStart, index - lineStart));
        if (text[index] == L'\r' && index + 1U < text.size() && text[index + 1U] == L'\n') {
            ++index;
        }

        lineStart = index + 1U;
    }

    if (lineStart < text.size() || text.empty()) {
        lines.push_back(text.substr(lineStart));
    }

    return lines;
}

std::wstring ExpandTabsForPrinting(const std::wstring& line)
{
    std::wstring expanded;
    expanded.reserve(line.size());
    int column = 0;
    for (wchar_t character : line) {
        if (character == L'\t') {
            const int spaces = kTabWidthSpaces - (column % kTabWidthSpaces);
            expanded.append(static_cast<std::size_t>(spaces), L' ');
            column += spaces;
        } else {
            expanded.push_back(character);
            ++column;
        }
    }

    return expanded;
}

std::wstring BuildPrintSinkText(
    const wchar_t* platform,
    const std::wstring& fontDescription,
    const RECT& margins,
    const std::wstring& text)
{
    std::wstring output;
    output += L"Classic Notepad Print Sink\r\n";
    output += L"Platform: ";
    output += platform;
    output += L"\r\n";
    output += L"Font: ";
    output += fontDescription;
    output += L"\r\n";
    output += L"Margins (thousandths inch): ";
    output += std::to_wstring(margins.left);
    output += L",";
    output += std::to_wstring(margins.top);
    output += L",";
    output += std::to_wstring(margins.right);
    output += L",";
    output += std::to_wstring(margins.bottom);
    output += L"\r\n";
    output += L"Pages: 1\r\n";
    output += L"--- Page 1 ---\r\n";
    output += text;
    if (!text.empty() && text.back() != L'\n' && text.back() != L'\r') {
        output += L"\r\n";
    }
    return output;
}

bool WriteUtf8TextFile(const std::wstring& path, const std::wstring& text, std::wstring& errorMessage)
{
    if (path.empty()) {
        errorMessage = L"Print sink path cannot be empty.";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!classic_notepad::EncodeTextBytes(text, classic_notepad::TextEncoding::Utf8NoBom, bytes, errorMessage)) {
        return false;
    }

    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errorMessage = L"The print sink could not be opened for writing.";
        return false;
    }

    bool ok = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
            errorMessage = L"The print sink could not be written.";
            ok = false;
            break;
        }

        offset += written;
    }

    if (!CloseHandle(file) && ok) {
        errorMessage = L"The print sink could not be closed.";
        ok = false;
    }

    return ok;
}

void ExpandTabsForMeasurementRange(
    const std::wstring& text,
    std::size_t start,
    std::size_t length,
    std::wstring& expanded)
{
    expanded.clear();
    expanded.reserve(length);

    int column = 0;
    for (std::size_t index = 0; index < length; ++index) {
        const wchar_t character = text[start + index];
        if (character == L'\t') {
            const int spaces = kTabWidthSpaces - (column % kTabWidthSpaces);
            expanded.append(static_cast<std::size_t>(spaces), L' ');
            column += spaces;
        } else {
            expanded.push_back(character);
            ++column;
        }
    }
}

int MeasureTextWidth(HDC deviceContext, const std::wstring& text, std::size_t start, std::size_t length)
{
    if (length == 0U) {
        return 0;
    }

    SIZE textSize{};
    const int safeLength = static_cast<int>(std::min<std::size_t>(
        length,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    if (!GetTextExtentPoint32W(deviceContext, text.c_str() + start, safeLength, &textSize)) {
        return 0;
    }

    return textSize.cx;
}

std::size_t FindFittingTextLength(
    HDC deviceContext,
    const std::wstring& text,
    std::size_t start,
    int availableWidth)
{
    const std::size_t remaining = text.size() - start;
    if (remaining == 0U) {
        return 0U;
    }

    std::size_t low = 1U;
    std::size_t high = std::min<std::size_t>(
        remaining,
        static_cast<std::size_t>(std::numeric_limits<int>::max()));
    std::size_t best = 1U;

    while (low <= high) {
        const std::size_t middle = low + ((high - low) / 2U);
        if (MeasureTextWidth(deviceContext, text, start, middle) <= availableWidth) {
            best = middle;
            low = middle + 1U;
        } else if (middle == 0U) {
            break;
        } else {
            high = middle - 1U;
        }
    }

    return best;
}

bool IsPrintWrapBreak(wchar_t character)
{
    return character == L' ' || character == L'-';
}

bool IsMissingFilePath(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool IsHighContrastEnabled()
{
    HIGHCONTRASTW highContrast{};
    highContrast.cbSize = sizeof(highContrast);
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0)) {
        return false;
    }

    return (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool SystemPrefersDarkMode()
{
    DWORD appsUseLightTheme = 1;
    DWORD valueSize = sizeof(appsUseLightTheme);
    const LSTATUS result = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &appsUseLightTheme,
        &valueSize);

    return result == ERROR_SUCCESS && appsUseLightTheme == 0;
}

classic_notepad::AppearanceTheme AppearanceThemeFromEnvironment()
{
    char value[32] {};
    const DWORD length = GetEnvironmentVariableA(
        classic_notepad::kAppearanceThemeEnvironmentVariable,
        value,
        static_cast<DWORD>(sizeof(value)));
    if (length == 0 || length >= sizeof(value)) {
        return classic_notepad::AppearanceTheme::System;
    }

    return classic_notepad::ParseAppearanceThemeOrSystem(value);
}

bool ShouldUseDarkMode(classic_notepad::AppearanceTheme theme)
{
    return classic_notepad::ResolveDarkMode(theme, SystemPrefersDarkMode(), IsHighContrastEnabled());
}

void ApplyDarkTitleBar(HWND window, bool useDarkMode)
{
    if (window == nullptr) {
        return;
    }

    const BOOL enabled = useDarkMode ? TRUE : FALSE;
    HRESULT result = DwmSetWindowAttribute(
        window,
        kDwmUseImmersiveDarkMode,
        &enabled,
        sizeof(enabled));

    if (FAILED(result)) {
        DwmSetWindowAttribute(
            window,
            kDwmUseImmersiveDarkModeBefore20H1,
            &enabled,
            sizeof(enabled));
    }

    const COLORREF borderColor = useDarkMode ? kDarkKeyLine : kDwmDefaultColor;
    DwmSetWindowAttribute(
        window,
        kDwmBorderColor,
        &borderColor,
        sizeof(borderColor));
}

int ScalePixelsForWindow(HWND window, int pixels)
{
    HDC deviceContext = GetDC(window);
    const int dpiY = deviceContext != nullptr ? std::max(1, GetDeviceCaps(deviceContext, LOGPIXELSY)) : 96;
    if (deviceContext != nullptr) {
        ReleaseDC(window, deviceContext);
    }

    return std::max(1, MulDiv(pixels, dpiY, 96));
}

SIZE MeasureText(HDC deviceContext, const std::wstring& text)
{
    SIZE textSize{};
    if (deviceContext != nullptr && !text.empty()) {
        GetTextExtentPoint32W(deviceContext, text.c_str(), static_cast<int>(text.size()), &textSize);
    }

    return textSize;
}

std::size_t FindWrapBreakLength(const std::wstring& text, std::size_t start, std::size_t maxLength)
{
    if (start + maxLength >= text.size()) {
        return maxLength;
    }

    for (std::size_t length = maxLength; length > 1U; --length) {
        if (IsPrintWrapBreak(text[start + length - 1U])) {
            return length;
        }
    }

    return maxLength;
}

RECT BuildPrintableTextRect(HDC printerDc, const RECT& marginsThousandths)
{
    const int dpiX = std::max(1, GetDeviceCaps(printerDc, LOGPIXELSX));
    const int dpiY = std::max(1, GetDeviceCaps(printerDc, LOGPIXELSY));
    const int horizontalResolution = std::max(1, GetDeviceCaps(printerDc, HORZRES));
    const int verticalResolution = std::max(1, GetDeviceCaps(printerDc, VERTRES));
    const int physicalWidth = std::max(horizontalResolution, GetDeviceCaps(printerDc, PHYSICALWIDTH));
    const int physicalHeight = std::max(verticalResolution, GetDeviceCaps(printerDc, PHYSICALHEIGHT));
    const int physicalOffsetX = std::max(0, GetDeviceCaps(printerDc, PHYSICALOFFSETX));
    const int physicalOffsetY = std::max(0, GetDeviceCaps(printerDc, PHYSICALOFFSETY));

    const int leftMargin = MulDiv(marginsThousandths.left, dpiX, 1000);
    const int topMargin = MulDiv(marginsThousandths.top, dpiY, 1000);
    const int rightMargin = MulDiv(marginsThousandths.right, dpiX, 1000);
    const int bottomMargin = MulDiv(marginsThousandths.bottom, dpiY, 1000);

    RECT textRect{};
    textRect.left = std::max(0, leftMargin - physicalOffsetX);
    textRect.top = std::max(0, topMargin - physicalOffsetY);
    textRect.right = std::min(horizontalResolution, physicalWidth - physicalOffsetX - rightMargin);
    textRect.bottom = std::min(verticalResolution, physicalHeight - physicalOffsetY - bottomMargin);

    if (textRect.right <= textRect.left) {
        textRect.left = 0;
        textRect.right = horizontalResolution;
    }

    if (textRect.bottom <= textRect.top) {
        textRect.top = 0;
        textRect.bottom = verticalResolution;
    }

    return textRect;
}

HFONT CreateDefaultEditorFont(HWND ownerWindow)
{
    HDC deviceContext = GetDC(ownerWindow);
    const int dpiY = deviceContext != nullptr ? std::max(1, GetDeviceCaps(deviceContext, LOGPIXELSY)) : 96;
    if (deviceContext != nullptr) {
        ReleaseDC(ownerWindow, deviceContext);
    }

    LOGFONTW logFont{};
    logFont.lfHeight = -MulDiv(kDefaultEditorFontPointSize, dpiY, 72);
    logFont.lfWeight = FW_NORMAL;
    logFont.lfCharSet = DEFAULT_CHARSET;
    logFont.lfOutPrecision = OUT_DEFAULT_PRECIS;
    logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    logFont.lfQuality = CLEARTYPE_QUALITY;
    logFont.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcscpy_s(logFont.lfFaceName, kDefaultEditorFontFace);

    return CreateFontIndirectW(&logFont);
}

std::wstring TrimWhitespace(const std::wstring& text)
{
    const auto first = std::find_if_not(text.begin(), text.end(), [](wchar_t character) {
        return std::iswspace(static_cast<wint_t>(character)) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](wchar_t character) {
        return std::iswspace(static_cast<wint_t>(character)) != 0;
    }).base();

    return first < last ? std::wstring(first, last) : std::wstring();
}

bool ParseAutomationFontDescription(const std::wstring& fontDescription, std::wstring& faceName, int& pointSize)
{
    const std::wstring trimmed = TrimWhitespace(fontDescription);
    if (trimmed.empty()) {
        return false;
    }

    std::size_t sizeEnd = trimmed.size();
    while (sizeEnd > 0U && std::iswspace(static_cast<wint_t>(trimmed[sizeEnd - 1U])) != 0) {
        --sizeEnd;
    }

    std::size_t sizeStart = sizeEnd;
    while (sizeStart > 0U && std::iswdigit(static_cast<wint_t>(trimmed[sizeStart - 1U])) != 0) {
        --sizeStart;
    }

    if (sizeStart == sizeEnd) {
        return false;
    }

    const std::wstring sizeText = trimmed.substr(sizeStart, sizeEnd - sizeStart);
    const long parsedSize = std::wcstol(sizeText.c_str(), nullptr, 10);
    if (parsedSize < 1 || parsedSize > 144) {
        return false;
    }

    faceName = TrimWhitespace(trimmed.substr(0, sizeStart));
    if (faceName.size() >= LF_FACESIZE || faceName.empty()) {
        return false;
    }

    pointSize = static_cast<int>(parsedSize);
    return true;
}

int PointSizeFromLogFont(HWND ownerWindow, const LOGFONTW& logFont)
{
    HDC deviceContext = GetDC(ownerWindow);
    const int dpiY = deviceContext != nullptr ? std::max(1, GetDeviceCaps(deviceContext, LOGPIXELSY)) : 96;
    if (deviceContext != nullptr) {
        ReleaseDC(ownerWindow, deviceContext);
    }

    const int pixelHeight = std::max(1, static_cast<int>(logFont.lfHeight < 0 ? -logFont.lfHeight : logFont.lfHeight));
    return std::max(1, MulDiv(pixelHeight, 72, dpiY));
}

bool IsFontFamilyAvailable(HDC deviceContext, const wchar_t* faceName)
{
    if (deviceContext == nullptr || faceName == nullptr || faceName[0] == L'\0') {
        return false;
    }

    LOGFONTW logFont{};
    logFont.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(logFont.lfFaceName, faceName);

    FontFamilySearch search{ faceName, false };
    EnumFontFamiliesExW(
        deviceContext,
        &logFont,
        EnumFontFamilyMatchProc,
        reinterpret_cast<LPARAM>(&search),
        0);

    return search.found;
}

HFONT CreateMenuFont(HWND ownerWindow)
{
    HDC deviceContext = GetDC(ownerWindow);
    const int dpiY = deviceContext != nullptr ? std::max(1, GetDeviceCaps(deviceContext, LOGPIXELSY)) : 96;
    const wchar_t* faceName = deviceContext != nullptr && IsFontFamilyAvailable(deviceContext, kWindows11MenuFontFace)
        ? kWindows11MenuFontFace
        : kFallbackMenuFontFace;

    if (deviceContext != nullptr) {
        ReleaseDC(ownerWindow, deviceContext);
    }

    LOGFONTW logFont{};
    logFont.lfHeight = -MulDiv(kWindows11MenuFontPixelSize, dpiY, 96);
    logFont.lfWeight = FW_NORMAL;
    logFont.lfCharSet = DEFAULT_CHARSET;
    logFont.lfOutPrecision = OUT_DEFAULT_PRECIS;
    logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    logFont.lfQuality = CLEARTYPE_QUALITY;
    logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
    wcscpy_s(logFont.lfFaceName, faceName);

    return CreateFontIndirectW(&logFont);
}

HFONT CreateMessageDialogFont(HWND ownerWindow)
{
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }

    LOGFONTW logFont{};
    if (GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(logFont), &logFont) != 0) {
        return CreateFontIndirectW(&logFont);
    }

    return CreateMenuFont(ownerWindow);
}

std::vector<MessageDialogButton> BuildMessageDialogButtons(UINT flags)
{
    std::vector<MessageDialogButton> buttons;
    switch (flags & MB_TYPEMASK) {
    case MB_OKCANCEL:
        AddMessageDialogButton(buttons, IDOK, L"OK");
        AddMessageDialogButton(buttons, IDCANCEL, L"Cancel");
        break;

    case MB_YESNO:
        AddMessageDialogButton(buttons, IDYES, L"Yes");
        AddMessageDialogButton(buttons, IDNO, L"No");
        break;

    case MB_YESNOCANCEL:
        AddMessageDialogButton(buttons, IDYES, L"Yes");
        AddMessageDialogButton(buttons, IDNO, L"No");
        AddMessageDialogButton(buttons, IDCANCEL, L"Cancel");
        break;

    default:
        AddMessageDialogButton(buttons, IDOK, L"OK");
        break;
    }

    const UINT defaultMask = flags & MB_DEFMASK;
    std::size_t defaultIndex = 0;
    if (defaultMask == MB_DEFBUTTON2) {
        defaultIndex = 1;
    } else if (defaultMask == MB_DEFBUTTON3) {
        defaultIndex = 2;
    } else if (defaultMask == MB_DEFBUTTON4) {
        defaultIndex = 3;
    }

    if (defaultIndex >= buttons.size()) {
        defaultIndex = 0;
    }

    buttons[defaultIndex].defaultButton = true;
    return buttons;
}

HICON LoadMessageDialogIcon(UINT flags)
{
    const UINT icon = flags & MB_ICONMASK;
    if (icon == MB_ICONERROR) {
        return LoadIconW(nullptr, IDI_ERROR);
    }
    if (icon == MB_ICONWARNING) {
        return LoadIconW(nullptr, IDI_WARNING);
    }
    if (icon == MB_ICONINFORMATION) {
        return LoadIconW(nullptr, IDI_INFORMATION);
    }
    if (icon == MB_ICONQUESTION) {
        return LoadIconW(nullptr, IDI_QUESTION);
    }

    return nullptr;
}

bool MessageDialogHasButton(const MessageDialogState& state, int id)
{
    for (const MessageDialogButton& button : state.buttons) {
        if (button.id == id) {
            return true;
        }
    }

    return false;
}

int GetMessageDialogDefaultResult(const MessageDialogState& state)
{
    for (const MessageDialogButton& button : state.buttons) {
        if (button.defaultButton) {
            return button.id;
        }
    }

    return state.buttons.empty() ? IDCANCEL : state.buttons.front().id;
}

int GetMessageDialogCloseResult(const MessageDialogState& state)
{
    if (MessageDialogHasButton(state, IDCANCEL)) {
        return IDCANCEL;
    }
    if (MessageDialogHasButton(state, IDNO)) {
        return IDNO;
    }
    if (MessageDialogHasButton(state, IDOK)) {
        return IDOK;
    }

    return state.buttons.empty() ? IDCANCEL : state.buttons.front().id;
}

MessageDialogButton* FindMessageDialogButton(MessageDialogState& state, int id)
{
    for (MessageDialogButton& button : state.buttons) {
        if (button.id == id) {
            return &button;
        }
    }

    return nullptr;
}

COLORREF MessageDialogBackgroundColor(bool darkMode)
{
    return darkMode ? kDarkMenuBackground : GetSysColor(COLOR_3DFACE);
}

COLORREF MessageDialogTextColor(bool darkMode)
{
    return darkMode ? kDarkMenuText : GetSysColor(COLOR_WINDOWTEXT);
}

HGDIOBJ MessageDialogFontObject(const MessageDialogState& state)
{
    return state.font != nullptr
        ? reinterpret_cast<HGDIOBJ>(state.font)
        : GetStockObject(DEFAULT_GUI_FONT);
}

void LayoutMessageDialog(MessageDialogState& state)
{
    const int paddingX = ScalePixelsForWindow(state.owner, 24);
    const int paddingY = ScalePixelsForWindow(state.owner, 22);
    const int iconTextGap = ScalePixelsForWindow(state.owner, 16);
    const int buttonGap = ScalePixelsForWindow(state.owner, 8);
    const int buttonWidth = ScalePixelsForWindow(state.owner, 88);
    const int buttonHeight = ScalePixelsForWindow(state.owner, 32);
    const int buttonTopGap = ScalePixelsForWindow(state.owner, 24);
    const int iconSize = state.icon != nullptr ? ScalePixelsForWindow(state.owner, 32) : 0;
    const int minClientWidth = ScalePixelsForWindow(state.owner, 330);
    const int maxTextWidth = ScalePixelsForWindow(state.owner, 460);

    int textHeight = ScalePixelsForWindow(state.owner, 18);
    int textWidth = ScalePixelsForWindow(state.owner, 1);
    HDC deviceContext = GetDC(state.owner);
    HGDIOBJ previousFont = nullptr;
    if (deviceContext != nullptr) {
        previousFont = SelectObject(
            deviceContext,
            MessageDialogFontObject(state));

        RECT measureRect{ 0, 0, maxTextWidth, 0 };
        DrawTextW(
            deviceContext,
            state.message.c_str(),
            -1,
            &measureRect,
            DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        textWidth = std::max(
            ScalePixelsForWindow(state.owner, 1),
            static_cast<int>(measureRect.right - measureRect.left));
        textHeight = std::max(
            ScalePixelsForWindow(state.owner, 18),
            static_cast<int>(measureRect.bottom - measureRect.top));

        if (previousFont != nullptr) {
            SelectObject(deviceContext, previousFont);
        }
        ReleaseDC(state.owner, deviceContext);
    }

    const int textLeft = paddingX + (iconSize > 0 ? iconSize + iconTextGap : 0);
    const int contentWidth = textLeft + textWidth + paddingX;
    const int buttonCount = static_cast<int>(state.buttons.size());
    const int totalButtonWidth =
        buttonCount > 0 ? (buttonCount * buttonWidth) + ((buttonCount - 1) * buttonGap) : 0;
    const int clientWidth = std::max(minClientWidth, std::max(contentWidth, totalButtonWidth + (paddingX * 2)));
    const int contentHeight = std::max(iconSize, textHeight);
    const int buttonY = paddingY + contentHeight + buttonTopGap;
    int buttonX = clientWidth - paddingX - totalButtonWidth;

    state.clientSize = { clientWidth, buttonY + buttonHeight + paddingY };
    state.iconRect = {
        paddingX,
        paddingY,
        paddingX + iconSize,
        paddingY + iconSize
    };
    state.messageRect = {
        textLeft,
        paddingY,
        clientWidth - paddingX,
        paddingY + textHeight
    };

    for (MessageDialogButton& button : state.buttons) {
        button.rect = { buttonX, buttonY, buttonX + buttonWidth, buttonY + buttonHeight };
        buttonX += buttonWidth + buttonGap;
    }
}

bool RegisterMessageDialogClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = MessageDialogWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kMessageDialogClass;
    windowClass.hbrBackground = nullptr;

    return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

POINT GetCenteredMessageDialogPosition(HWND owner, int width, int height)
{
    RECT workArea{};
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
        workArea = monitorInfo.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    }

    RECT ownerRect = workArea;
    if (owner != nullptr && IsWindow(owner)) {
        GetWindowRect(owner, &ownerRect);
    }

    POINT position{
        ownerRect.left + (((ownerRect.right - ownerRect.left) - width) / 2),
        ownerRect.top + (((ownerRect.bottom - ownerRect.top) - height) / 2)
    };

    const int minimumX = static_cast<int>(workArea.left);
    const int minimumY = static_cast<int>(workArea.top);
    const int maximumX = static_cast<int>(workArea.right) - width;
    const int maximumY = static_cast<int>(workArea.bottom) - height;
    position.x = std::max(minimumX, std::min(static_cast<int>(position.x), maximumX));
    position.y = std::max(minimumY, std::min(static_cast<int>(position.y), maximumY));
    return position;
}

void CompleteMessageDialog(HWND window, MessageDialogState& state, int result)
{
    state.result = result;
    state.done = true;
    DestroyWindow(window);
}

bool DrawMessageDialogButton(MessageDialogState& state, DRAWITEMSTRUCT* drawItem)
{
    if (drawItem == nullptr) {
        return false;
    }

    MessageDialogButton* button = FindMessageDialogButton(state, static_cast<int>(drawItem->CtlID));
    if (button == nullptr) {
        return false;
    }

    const bool pressed = (drawItem->itemState & ODS_SELECTED) != 0;
    const bool focused = (drawItem->itemState & ODS_FOCUS) != 0;
    const bool disabled = (drawItem->itemState & ODS_DISABLED) != 0;
    const COLORREF fillColor = state.darkMode
        ? (pressed ? kDarkMenuActiveBackground : kDarkMenuHotBackground)
        : GetSysColor(pressed ? COLOR_3DSHADOW : COLOR_BTNFACE);
    const COLORREF borderColor = button->defaultButton
        ? RGB(0, 120, 215)
        : (state.darkMode ? kDarkMenuSeparator : GetSysColor(COLOR_3DSHADOW));
    const COLORREF textColor = disabled
        ? (state.darkMode ? kDarkMenuDisabledText : GetSysColor(COLOR_GRAYTEXT))
        : MessageDialogTextColor(state.darkMode);

    HBRUSH fillBrush = CreateSolidBrush(fillColor);
    FillRect(drawItem->hDC, &drawItem->rcItem, fillBrush);
    DeleteObject(fillBrush);

    RECT borderRect = drawItem->rcItem;
    HBRUSH borderBrush = CreateSolidBrush(borderColor);
    FrameRect(drawItem->hDC, &borderRect, borderBrush);
    if (button->defaultButton) {
        InflateRect(&borderRect, -1, -1);
        FrameRect(drawItem->hDC, &borderRect, borderBrush);
    }
    DeleteObject(borderBrush);

    RECT textRect = drawItem->rcItem;
    InflateRect(&textRect, -ScalePixelsForWindow(state.owner, 8), 0);
    SetBkMode(drawItem->hDC, TRANSPARENT);
    SetTextColor(drawItem->hDC, textColor);
    HGDIOBJ previousFont = SelectObject(
        drawItem->hDC,
        MessageDialogFontObject(state));
    DrawTextW(
        drawItem->hDC,
        button->text.c_str(),
        -1,
        &textRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (previousFont != nullptr) {
        SelectObject(drawItem->hDC, previousFont);
    }

    if (focused) {
        RECT focusRect = drawItem->rcItem;
        InflateRect(&focusRect, -ScalePixelsForWindow(state.owner, 4), -ScalePixelsForWindow(state.owner, 4));
        DrawFocusRect(drawItem->hDC, &focusRect);
    }

    return true;
}

void DrawThemedPushButton(
    HDC deviceContext,
    const RECT& rect,
    const std::wstring& text,
    HFONT font,
    bool darkMode,
    bool defaultButton,
    UINT itemState,
    HWND scaleWindow)
{
    const bool pressed = (itemState & ODS_SELECTED) != 0;
    const bool focused = (itemState & ODS_FOCUS) != 0;
    const bool disabled = (itemState & ODS_DISABLED) != 0;
    const COLORREF fillColor = darkMode
        ? (pressed ? kDarkMenuActiveBackground : kDarkMenuHotBackground)
        : GetSysColor(pressed ? COLOR_3DSHADOW : COLOR_BTNFACE);
    const COLORREF borderColor = defaultButton
        ? RGB(0, 120, 215)
        : (darkMode ? kDarkMenuSeparator : GetSysColor(COLOR_3DSHADOW));
    const COLORREF textColor = disabled
        ? (darkMode ? kDarkMenuDisabledText : GetSysColor(COLOR_GRAYTEXT))
        : MessageDialogTextColor(darkMode);

    HBRUSH fillBrush = CreateSolidBrush(fillColor);
    FillRect(deviceContext, &rect, fillBrush);
    DeleteObject(fillBrush);

    RECT borderRect = rect;
    HBRUSH borderBrush = CreateSolidBrush(borderColor);
    FrameRect(deviceContext, &borderRect, borderBrush);
    if (defaultButton) {
        InflateRect(&borderRect, -1, -1);
        FrameRect(deviceContext, &borderRect, borderBrush);
    }
    DeleteObject(borderBrush);

    RECT textRect = rect;
    InflateRect(&textRect, -ScalePixelsForWindow(scaleWindow, 8), 0);
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, textColor);
    HGDIOBJ previousFont = SelectObject(
        deviceContext,
        font != nullptr ? reinterpret_cast<HGDIOBJ>(font) : GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(
        deviceContext,
        text.c_str(),
        -1,
        &textRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }

    if (focused) {
        RECT focusRect = rect;
        InflateRect(&focusRect, -ScalePixelsForWindow(scaleWindow, 4), -ScalePixelsForWindow(scaleWindow, 4));
        DrawFocusRect(deviceContext, &focusRect);
    }
}

void PaintMessageDialog(HWND window, MessageDialogState& state)
{
    PAINTSTRUCT paint{};
    HDC deviceContext = BeginPaint(window, &paint);

    RECT clientRect{};
    GetClientRect(window, &clientRect);
    HBRUSH backgroundBrush = CreateSolidBrush(MessageDialogBackgroundColor(state.darkMode));
    FillRect(deviceContext, &clientRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    if (state.icon != nullptr) {
        const int iconWidth = std::max(0, static_cast<int>(state.iconRect.right - state.iconRect.left));
        const int iconHeight = std::max(0, static_cast<int>(state.iconRect.bottom - state.iconRect.top));
        DrawIconEx(
            deviceContext,
            state.iconRect.left,
            state.iconRect.top,
            state.icon,
            iconWidth,
            iconHeight,
            0,
            nullptr,
            DI_NORMAL);
    }

    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, MessageDialogTextColor(state.darkMode));
    HGDIOBJ previousFont = SelectObject(
        deviceContext,
        MessageDialogFontObject(state));
    RECT textRect = state.messageRect;
    DrawTextW(
        deviceContext,
        state.message.c_str(),
        -1,
        &textRect,
        DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }

    EndPaint(window, &paint);
}

LRESULT CALLBACK MessageDialogWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    MessageDialogState* state = reinterpret_cast<MessageDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<MessageDialogState*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    if (state == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_CREATE:
        ApplyDarkTitleBar(window, state->darkMode);
        for (const MessageDialogButton& button : state->buttons) {
            HWND buttonWindow = CreateWindowExW(
                0,
                L"BUTTON",
                button.text.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW |
                    (button.defaultButton ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                button.rect.left,
                button.rect.top,
                button.rect.right - button.rect.left,
                button.rect.bottom - button.rect.top,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(button.id)),
                state->instance,
                nullptr);
            if (buttonWindow != nullptr && state->font != nullptr) {
                SendMessageW(buttonWindow, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        PaintMessageDialog(window, *state);
        return 0;

    case WM_DRAWITEM:
        if (DrawMessageDialogButton(*state, reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED && FindMessageDialogButton(*state, LOWORD(wParam)) != nullptr) {
            CompleteMessageDialog(window, *state, LOWORD(wParam));
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            CompleteMessageDialog(window, *state, GetMessageDialogCloseResult(*state));
            return 0;
        }
        if (wParam == VK_RETURN) {
            CompleteMessageDialog(window, *state, GetMessageDialogDefaultResult(*state));
            return 0;
        }
        break;

    case WM_CLOSE:
        CompleteMessageDialog(window, *state, GetMessageDialogCloseResult(*state));
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

int ShowThemedMessageDialog(
    HWND owner,
    HINSTANCE instance,
    const std::wstring& message,
    const std::wstring& title,
    UINT flags,
    bool darkMode)
{
    if (!darkMode) {
        return MessageBoxW(owner, message.c_str(), title.c_str(), flags);
    }

    MessageDialogState state{};
    state.instance = instance;
    state.owner = owner;
    state.title = title;
    state.message = message;
    state.flags = flags;
    state.darkMode = darkMode;
    state.buttons = BuildMessageDialogButtons(flags);
    state.result = GetMessageDialogCloseResult(state);
    state.font = CreateMessageDialogFont(owner);
    state.icon = LoadMessageDialogIcon(flags);
    LayoutMessageDialog(state);

    if (!RegisterMessageDialogClass(instance)) {
        if (state.font != nullptr) {
            DeleteObject(state.font);
        }
        return MessageBoxW(owner, message.c_str(), title.c_str(), flags);
    }

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT;
    RECT windowRect{ 0, 0, state.clientSize.cx, state.clientSize.cy };
    AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
    const int windowWidth = windowRect.right - windowRect.left;
    const int windowHeight = windowRect.bottom - windowRect.top;
    const POINT position = GetCenteredMessageDialogPosition(owner, windowWidth, windowHeight);

    HWND window = CreateWindowExW(
        exStyle,
        kMessageDialogClass,
        state.title.c_str(),
        style,
        position.x,
        position.y,
        windowWidth,
        windowHeight,
        owner,
        nullptr,
        instance,
        &state);

    if (window == nullptr) {
        if (state.font != nullptr) {
            DeleteObject(state.font);
        }
        return MessageBoxW(owner, message.c_str(), title.c_str(), flags);
    }

    const bool enableOwnerAfterDialog = owner != nullptr && IsWindow(owner) && IsWindowEnabled(owner) != FALSE;
    HWND previousFocus = GetFocus();
    if (enableOwnerAfterDialog) {
        EnableWindow(owner, FALSE);
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    for (const MessageDialogButton& button : state.buttons) {
        if (button.defaultButton) {
            HWND defaultButton = GetDlgItem(window, button.id);
            if (defaultButton != nullptr) {
                SetFocus(defaultButton);
            }
            break;
        }
    }

    MSG dialogMessage{};
    while (!state.done && IsWindow(window)) {
        const BOOL messageResult = GetMessageW(&dialogMessage, nullptr, 0, 0);
        if (messageResult == -1) {
            break;
        }
        if (messageResult == 0) {
            PostQuitMessage(static_cast<int>(dialogMessage.wParam));
            break;
        }

        if (!IsDialogMessageW(window, &dialogMessage)) {
            TranslateMessage(&dialogMessage);
            DispatchMessageW(&dialogMessage);
        }
    }

    if (IsWindow(window)) {
        DestroyWindow(window);
    }

    if (enableOwnerAfterDialog) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    if (previousFocus != nullptr && IsWindow(previousFocus)) {
        SetFocus(previousFocus);
    }
    if (state.font != nullptr) {
        DeleteObject(state.font);
    }

    return state.result;
}

HFONT CreatePrinterFont(HDC printerDc, HFONT sourceFont, HWND ownerWindow)
{
    LOGFONTW logFont{};
    if (sourceFont == nullptr || GetObjectW(sourceFont, sizeof(logFont), &logFont) == 0) {
        GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(logFont), &logFont);
    }

    HDC screenDc = GetDC(ownerWindow);
    const int screenDpiY = screenDc != nullptr ? std::max(1, GetDeviceCaps(screenDc, LOGPIXELSY)) : 96;
    if (screenDc != nullptr) {
        ReleaseDC(ownerWindow, screenDc);
    }

    const int printerDpiY = std::max(1, GetDeviceCaps(printerDc, LOGPIXELSY));
    const int pointTenths = logFont.lfHeight < 0
        ? MulDiv(-logFont.lfHeight, 720, screenDpiY)
        : MulDiv(logFont.lfHeight, 720, screenDpiY);
    logFont.lfHeight = -MulDiv(std::max(1, pointTenths), printerDpiY, 720);
    return CreateFontIndirectW(&logFont);
}

} // namespace

ClassicNotepadApp::ClassicNotepadApp(HINSTANCE instance)
    : instance_(instance)
    , findReplaceMessage_(RegisterWindowMessageW(kFindReplaceMessageName))
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInitialized_ = SUCCEEDED(hr);
    appearanceTheme_ = AppearanceThemeFromEnvironment();
    darkModeEnabled_ = ShouldUseDarkMode(appearanceTheme_);
    RecreateThemeBrushes();
}

ClassicNotepadApp::~ClassicNotepadApp()
{
    largeFileView_.reset();
    if (mainWindow_ != nullptr && spellCheckTimerId_ != 0) {
        KillTimer(mainWindow_, spellCheckTimerId_);
        spellCheckTimerId_ = 0;
    }

    DestroyOwnedEditorFont();
    DestroyOwnedMenuFont();
    DestroyPrintDialogHandles();
    DestroyThemeBrushes();

    spellChecker_.Reset();

    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

int ClassicNotepadApp::Run(int showCommand, const std::wstring& initialFilePath)
{
    if (!RegisterMainWindowClass() || !CreateMainWindow(showCommand)) {
        return -1;
    }

    accelerator_ = LoadAcceleratorsW(instance_, MAKEINTRESOURCEW(IDR_ACCELERATORS));

    if (!initialFilePath.empty()) {
        HandleInitialFilePath(initialFilePath);
    } else {
        UpdateTitle();
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (findDialog_ != nullptr && !IsWindow(findDialog_)) {
            findDialog_ = nullptr;
        }

        if (replaceDialog_ != nullptr && !IsWindow(replaceDialog_)) {
            replaceDialog_ = nullptr;
        }

        if (findDialog_ != nullptr && IsDialogMessageW(findDialog_, &message)) {
            continue;
        }

        if (replaceDialog_ != nullptr && IsDialogMessageW(replaceDialog_, &message)) {
            continue;
        }

        if (accelerator_ != nullptr && TranslateAcceleratorW(mainWindow_, accelerator_, &message) != 0) {
            continue;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}

int ClassicNotepadApp::RunAutomation(int showCommand, const std::wstring& initialFilePath, bool visible)
{
    automationMode_ = true;

    const int automationShowCommand = visible ? showCommand : SW_HIDE;
    if (!RegisterMainWindowClass() || !CreateMainWindow(automationShowCommand)) {
        return -1;
    }

    accelerator_ = LoadAcceleratorsW(instance_, MAKEINTRESOURCEW(IDR_ACCELERATORS));

    if (!initialFilePath.empty()) {
        std::wstring errorMessage;
        if (IsMissingFilePath(initialFilePath)) {
            CreateNewDocumentForPath(initialFilePath);
        } else if (!AutomationOpenFile(initialFilePath, errorMessage)) {
            UpdateTitle();
        }
    } else {
        UpdateTitle();
    }

    WindowsAutomationController controller(*this);
    const int result = controller.Run();

    if (mainWindow_ != nullptr) {
        DestroyWindow(mainWindow_);
        mainWindow_ = nullptr;
    }

    return result;
}

bool ClassicNotepadApp::RegisterMainWindowClass()
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = ClassicNotepadApp::WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    windowClass.lpszClassName = kMainWindowClass;
    windowClass.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));

    const bool mainWindowClassRegistered =
        RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return mainWindowClassRegistered && RegisterMenuBarClass() && RegisterScrollBarClass();
}

bool ClassicNotepadApp::RegisterMenuBarClass()
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = ClassicNotepadApp::MenuBarWindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kMenuBarClass;

    if (RegisterClassExW(&windowClass) != 0) {
        return true;
    }

    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ClassicNotepadApp::RegisterScrollBarClass()
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = ClassicNotepadApp::ScrollBarWindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kScrollBarClass;

    if (RegisterClassExW(&windowClass) != 0) {
        return true;
    }

    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ClassicNotepadApp::CreateMainWindow(int showCommand)
{
    mainWindow_ = CreateWindowExW(
        0,
        kMainWindowClass,
        kAppTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        650,
        nullptr,
        nullptr,
        instance_,
        this);

    if (mainWindow_ == nullptr) {
        return false;
    }

    if (menuFont_ == nullptr) {
        menuFont_ = CreateMenuFont(mainWindow_);
        ownsMenuFont_ = menuFont_ != nullptr;
        if (menuFont_ == nullptr) {
            menuFont_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            ownsMenuFont_ = false;
        }
    }

    ApplyThemeToWindows();
    ShowWindow(mainWindow_, showCommand);
    UpdateWindow(mainWindow_);
    return true;
}

HWND ClassicNotepadApp::CreateMenuBar()
{
    return CreateWindowExW(
        0,
        kMenuBarClass,
        nullptr,
        WS_CHILD | WS_VISIBLE,
        0,
        0,
        0,
        0,
        mainWindow_,
        nullptr,
        instance_,
        this);
}

HWND ClassicNotepadApp::CreateCustomScrollBar(ScrollBarOrientation orientation)
{
    (void)orientation;
    return CreateWindowExW(
        0,
        kScrollBarClass,
        nullptr,
        WS_CHILD,
        0,
        0,
        0,
        0,
        mainWindow_,
        nullptr,
        instance_,
        this);
}

HWND ClassicNotepadApp::CreateScrollCorner()
{
    return CreateWindowExW(
        0,
        kScrollBarClass,
        nullptr,
        WS_CHILD,
        0,
        0,
        0,
        0,
        mainWindow_,
        nullptr,
        instance_,
        this);
}

HWND ClassicNotepadApp::CreateEditor(bool initiallyVisible, bool wordWrap)
{
    const bool useCustomScrollBars = UseCustomScrollBars();
    const DWORD editorStyle =
        WS_CHILD | (initiallyVisible ? WS_VISIBLE : 0) |
        (useCustomScrollBars ? 0 : WS_VSCROLL) |
        (wordWrap || useCustomScrollBars ? 0 : WS_HSCROLL) |
        ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
        (wordWrap ? 0 : ES_AUTOHSCROLL) |
        ES_NOHIDESEL | ES_WANTRETURN;

    const DWORD editorExStyle = darkModeEnabled_ ? 0 : WS_EX_CLIENTEDGE;
    HWND editor = CreateWindowExW(
        editorExStyle,
        L"EDIT",
        nullptr,
        editorStyle,
        0,
        0,
        0,
        0,
        mainWindow_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MAIN_EDITOR)),
        instance_,
        nullptr);

    if (editor == nullptr) {
        return nullptr;
    }

    if (editorFont_ == nullptr) {
        editorFont_ = CreateDefaultEditorFont(mainWindow_);
        ownsEditorFont_ = editorFont_ != nullptr;
        if (editorFont_ == nullptr) {
            editorFont_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            ownsEditorFont_ = false;
        }
    }

    SendMessageW(editor, WM_SETFONT, reinterpret_cast<WPARAM>(editorFont_), TRUE);
    SendMessageW(editor, EM_SETLIMITTEXT, static_cast<WPARAM>(0x7FFFFFFE), 0);
    SetWindowLongPtrW(editor, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    originalEditorProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        editor,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(ClassicNotepadApp::EditorWindowProc)));

    return editor;
}

bool ClassicNotepadApp::TrySetEditorText(
    HWND editor,
    const std::wstring& text,
    std::wstring& errorMessage) const
{
    try {
    errorMessage.clear();
    if (editor == nullptr || !IsWindow(editor)) {
        errorMessage = L"The editor control is unavailable.";
        return false;
    }
    if (text.find(L'\0') != std::wstring::npos) {
        errorMessage = L"The text contains an embedded NUL character that the editor control cannot represent.";
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        const std::uint32_t unit = static_cast<std::uint16_t>(text[index]);
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            if (index + 1U >= text.size()) {
                errorMessage = L"The text contains an unmatched Unicode surrogate.";
                return false;
            }
            const std::uint32_t low = static_cast<std::uint16_t>(text[++index]);
            if (low < 0xDC00U || low > 0xDFFFU) {
                errorMessage = L"The text contains an unmatched Unicode surrogate.";
                return false;
            }
        } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
            errorMessage = L"The text contains an unmatched Unicode surrogate.";
            return false;
        }
    }
    constexpr std::size_t kEditControlLimit = 0x7FFFFFFEU;
    if (text.size() > kEditControlLimit || text.size() >= static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        errorMessage = L"The text exceeds the editor control's supported capacity.";
        return false;
    }

    if (automationMode_) {
        wchar_t injectedFailure[32]{};
        const DWORD failureLength = GetEnvironmentVariableW(
            L"CLASSIC_NOTEPAD_TEST_EDITOR_ADOPTION_FAILURE",
            injectedFailure,
            static_cast<DWORD>(std::size(injectedFailure)));
        if (failureLength != 0U && failureLength < std::size(injectedFailure)) {
            if (std::wstring(injectedFailure) == L"partial" && !text.empty()) {
                const std::wstring partial = text.substr(0U, text.size() / 2U);
                SetWindowTextW(editor, partial.c_str());
            }
            errorMessage = L"The test injected an editor text adoption failure.";
            return false;
        }
    }

    const int expectedLength = static_cast<int>(text.size());
    // Allocate verification storage before changing the candidate control so an
    // allocation failure can never occur after the text has been adopted.
    std::wstring adopted(static_cast<std::size_t>(expectedLength) + 1U, L'\0');

    SendMessageW(editor, EM_SETLIMITTEXT, static_cast<WPARAM>(kEditControlLimit), 0);
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowTextW(editor, text.c_str())) {
        errorMessage = L"The editor control could not accept the complete text.";
        const DWORD error = GetLastError();
        if (error != ERROR_SUCCESS) errorMessage += L" (Windows error " + std::to_wstring(error) + L").";
        return false;
    }

    if (IsInjectedPostAdoptionAllocationFailure(automationMode_)) throw std::bad_alloc();
    if (GetWindowTextLengthW(editor) != expectedLength) {
        errorMessage = L"The editor control accepted only part of the text.";
        return false;
    }
    const int copied = GetWindowTextW(editor, adopted.data(), static_cast<int>(adopted.size()));
    if (copied != expectedLength) {
        errorMessage = L"The editor control could not return the complete adopted text.";
        return false;
    }
    adopted.resize(static_cast<std::size_t>(copied));
    if (adopted != text) {
        errorMessage = L"The editor control changed the text during adoption.";
        return false;
    }
    return true;
    } catch (const std::bad_alloc&) {
        SetAllocationFailureMessage(errorMessage);
        return false;
    } catch (const std::length_error&) {
        SetAllocationFailureMessage(errorMessage);
        return false;
    }
}

HWND ClassicNotepadApp::CreateStatusBar()
{
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&commonControls);

    HWND statusBar = CreateWindowExW(
        0,
        STATUSCLASSNAMEW,
        nullptr,
        WS_CHILD | (statusBarVisible_ ? WS_VISIBLE : 0) |
            (darkModeEnabled_ ? CCS_NODIVIDER : SBARS_SIZEGRIP),
        0,
        0,
        0,
        0,
        mainWindow_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VIEW_STATUS_BAR)),
        instance_,
        nullptr);

    if (statusBar != nullptr) {
        if (menuFont_ != nullptr) {
            SendMessageW(statusBar, WM_SETFONT, reinterpret_cast<WPARAM>(menuFont_), FALSE);
        }
        SendMessageW(statusBar, SB_SETMINHEIGHT, static_cast<WPARAM>(ScalePixelsForWindow(mainWindow_, kStatusBarMinHeight)), 0);
        SendMessageW(statusBar, SB_SETBKCOLOR, 0, darkModeEnabled_ ? kDarkStatusBackground : CLR_DEFAULT);
        SetWindowSubclass(
            statusBar,
            ClassicNotepadApp::StatusBarSubclassProc,
            1,
            reinterpret_cast<DWORD_PTR>(this));
    }

    return statusBar;
}

int ClassicNotepadApp::GetMenuBarHeight() const
{
    return std::max(24, GetSystemMetrics(SM_CYMENU));
}

HMENU ClassicNotepadApp::ActiveMenu() const
{
    return mainMenu_ != nullptr ? mainMenu_ : GetMenu(mainWindow_);
}

int ClassicNotepadApp::GetTopLevelMenuCount() const
{
    HMENU menu = ActiveMenu();
    return menu != nullptr ? std::max(0, GetMenuItemCount(menu)) : 0;
}

std::wstring ClassicNotepadApp::GetTopLevelMenuText(int index) const
{
    HMENU menu = ActiveMenu();
    if (menu == nullptr || index < 0 || index >= GetTopLevelMenuCount()) {
        return {};
    }

    std::array<wchar_t, 128> buffer{};
    if (GetMenuStringW(menu, static_cast<UINT>(index), buffer.data(), static_cast<int>(buffer.size()), MF_BYPOSITION) == 0) {
        return {};
    }

    return buffer.data();
}

RECT ClassicNotepadApp::GetMenuBarItemRect(int index) const
{
    RECT itemRect{};
    if (menuBar_ == nullptr || index < 0 || index >= GetTopLevelMenuCount()) {
        return itemRect;
    }

    RECT clientRect{};
    GetClientRect(menuBar_, &clientRect);

    HDC deviceContext = GetDC(menuBar_);
    if (deviceContext == nullptr) {
        return itemRect;
    }

    HGDIOBJ previousFont = SelectObject(
        deviceContext,
        menuFont_ != nullptr ? menuFont_ : GetStockObject(DEFAULT_GUI_FONT));
    int x = clientRect.left;
    for (int itemIndex = 0; itemIndex <= index; ++itemIndex) {
        const std::wstring text = classic_notepad::windows::StripMenuMnemonics(
            GetTopLevelMenuText(itemIndex));
        SIZE textSize{};
        if (!text.empty()) {
            GetTextExtentPoint32W(deviceContext, text.c_str(), static_cast<int>(text.size()), &textSize);
        }

        const int itemWidth = textSize.cx + (kMenuHorizontalPadding * 2);
        if (itemIndex == index) {
            itemRect = RECT{ x, clientRect.top, x + itemWidth, clientRect.bottom };
            break;
        }

        x += itemWidth;
    }

    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }
    ReleaseDC(menuBar_, deviceContext);

    return itemRect;
}

int ClassicNotepadApp::MenuIndexFromPoint(POINT point) const
{
    const int itemCount = GetTopLevelMenuCount();
    for (int index = 0; index < itemCount; ++index) {
        const RECT itemRect = GetMenuBarItemRect(index);
        if (!IsRectEmpty(&itemRect) && PtInRect(&itemRect, point)) {
            return index;
        }
    }

    return -1;
}

void ClassicNotepadApp::PaintMenuBar(HDC deviceContext) const
{
    if (deviceContext == nullptr || menuBar_ == nullptr) {
        return;
    }

    RECT clientRect{};
    GetClientRect(menuBar_, &clientRect);

    const COLORREF backgroundColor = darkModeEnabled_ ? kDarkMenuBackground : GetSysColor(COLOR_MENU);
    const COLORREF hotBackgroundColor = darkModeEnabled_ ? kDarkMenuHotBackground : GetSysColor(COLOR_MENUHILIGHT);
    const COLORREF activeBackgroundColor = darkModeEnabled_ ? kDarkMenuActiveBackground : hotBackgroundColor;
    const COLORREF textColor = darkModeEnabled_ ? kDarkMenuText : GetSysColor(COLOR_MENUTEXT);

    HBRUSH backgroundBrush = CreateSolidBrush(backgroundColor);
    if (backgroundBrush != nullptr) {
        FillRect(deviceContext, &clientRect, backgroundBrush);
        DeleteObject(backgroundBrush);
    }

    HGDIOBJ previousFont = SelectObject(
        deviceContext,
        menuFont_ != nullptr ? menuFont_ : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, textColor);

    HBRUSH hotBrush = CreateSolidBrush(hotBackgroundColor);
    HBRUSH activeBrush = CreateSolidBrush(activeBackgroundColor);
    const int itemCount = GetTopLevelMenuCount();
    for (int index = 0; index < itemCount; ++index) {
        RECT itemRect = GetMenuBarItemRect(index);
        if (IsRectEmpty(&itemRect)) {
            continue;
        }

        if (index == activeMenuIndex_ && activeBrush != nullptr) {
            FillRect(deviceContext, &itemRect, activeBrush);
        } else if (index == hotMenuIndex_ && hotBrush != nullptr) {
            FillRect(deviceContext, &itemRect, hotBrush);
        }

        RECT textRect = itemRect;
        textRect.left += kMenuHorizontalPadding;
        textRect.right -= kMenuHorizontalPadding;
        const std::wstring text = classic_notepad::windows::StripMenuMnemonics(
            GetTopLevelMenuText(index));
        SetTextColor(
            deviceContext,
            !darkModeEnabled_ && (index == activeMenuIndex_ || index == hotMenuIndex_)
                ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                : textColor);
        DrawTextW(
            deviceContext,
            text.c_str(),
            -1,
            &textRect,
            DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }

    if (hotBrush != nullptr) {
        DeleteObject(hotBrush);
    }
    if (activeBrush != nullptr) {
        DeleteObject(activeBrush);
    }
    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }
}

void ClassicNotepadApp::SetHotMenuIndex(int index)
{
    if (index == hotMenuIndex_) {
        return;
    }

    hotMenuIndex_ = index;
    if (menuBar_ != nullptr) {
        InvalidateRect(menuBar_, nullptr, FALSE);
    }
}

void ClassicNotepadApp::ClearMenuMode()
{
    hotMenuIndex_ = -1;
    activeMenuIndex_ = -1;
    menuKeyboardActive_ = false;
    if (menuBar_ != nullptr) {
        InvalidateRect(menuBar_, nullptr, FALSE);
    }
}

void ClassicNotepadApp::ShowMenuPopup(int index, bool fromKeyboard)
{
    HMENU menu = ActiveMenu();
    if (menu == nullptr || menuBar_ == nullptr || index < 0 || index >= GetTopLevelMenuCount()) {
        return;
    }

    HMENU submenu = GetSubMenu(menu, index);
    if (submenu == nullptr) {
        return;
    }

    UpdateMenuState(menu);
    hotMenuIndex_ = index;
    activeMenuIndex_ = index;
    menuKeyboardActive_ = fromKeyboard;
    InvalidateRect(menuBar_, nullptr, FALSE);

    RECT itemRect = GetMenuBarItemRect(index);
    POINT popupPoint{ itemRect.left, itemRect.bottom };
    ClientToScreen(menuBar_, &popupPoint);

    SetForegroundWindow(mainWindow_);
    TrackPopupMenuEx(
        submenu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        popupPoint.x,
        popupPoint.y,
        mainWindow_,
        nullptr);
    PostMessageW(mainWindow_, WM_NULL, 0, 0);

    ClearMenuMode();
    FocusActiveEditor();
}

bool ClassicNotepadApp::ActivateMenuBarFromKeyboard()
{
    if (menuBar_ == nullptr || !IsWindowVisible(menuBar_) || GetTopLevelMenuCount() == 0) {
        return false;
    }

    menuKeyboardActive_ = true;
    SetHotMenuIndex(hotMenuIndex_ >= 0 ? hotMenuIndex_ : 0);
    SetFocus(menuBar_);
    return true;
}

bool ClassicNotepadApp::ActivateMenuMnemonic(wchar_t mnemonic)
{
    if (menuBar_ == nullptr || !IsWindowVisible(menuBar_)) {
        return false;
    }

    const wchar_t target = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(mnemonic)));
    const int itemCount = GetTopLevelMenuCount();
    for (int index = 0; index < itemCount; ++index) {
        if (classic_notepad::windows::FindMenuMnemonic(GetTopLevelMenuText(index)) == target) {
            ShowMenuPopup(index, true);
            return true;
        }
    }

    return false;
}

ClassicNotepadApp::OwnerDrawMenuItem* ClassicNotepadApp::StoreOwnerDrawMenuItem(
    std::vector<std::unique_ptr<OwnerDrawMenuItem>>& storage,
    const std::wstring& text,
    bool separator,
    bool reserveIconSpace) const
{
    auto item = std::make_unique<OwnerDrawMenuItem>();
    item->text = text;
    item->separator = separator;
    item->reserveIconSpace = reserveIconSpace;
    OwnerDrawMenuItem* storedItem = item.get();
    storage.push_back(std::move(item));
    return storedItem;
}

bool ClassicNotepadApp::AppendOwnerDrawMenuItem(
    HMENU menu,
    UINT flags,
    UINT_PTR itemId,
    const std::wstring& text,
    std::vector<std::unique_ptr<OwnerDrawMenuItem>>& storage,
    bool reserveIconSpace) const
{
    OwnerDrawMenuItem* item = StoreOwnerDrawMenuItem(storage, text, false, reserveIconSpace);
    return AppendMenuW(
        menu,
        MF_OWNERDRAW | flags,
        itemId,
        reinterpret_cast<LPCWSTR>(item)) != 0;
}

bool ClassicNotepadApp::AppendOwnerDrawMenuSeparator(
    HMENU menu,
    std::vector<std::unique_ptr<OwnerDrawMenuItem>>& storage,
    bool reserveIconSpace) const
{
    OwnerDrawMenuItem* item = StoreOwnerDrawMenuItem(storage, {}, true, reserveIconSpace);
    return AppendMenuW(
        menu,
        MF_OWNERDRAW | MF_DISABLED,
        0,
        reinterpret_cast<LPCWSTR>(item)) != 0;
}

void ClassicNotepadApp::ApplyMenuBackground(HMENU menu) const
{
    if (menu == nullptr) {
        return;
    }

    MENUINFO menuInfo{};
    menuInfo.cbSize = sizeof(menuInfo);
    menuInfo.fMask = MIM_BACKGROUND;
    menuInfo.hbrBack = darkModeEnabled_ && darkMenuBackgroundBrush_ != nullptr
        ? darkMenuBackgroundBrush_
        : GetSysColorBrush(COLOR_MENU);
    SetMenuInfo(menu, &menuInfo);
}

void ClassicNotepadApp::ApplyOwnerDrawToPopupMenu(
    HMENU menu,
    std::vector<std::unique_ptr<OwnerDrawMenuItem>>& storage) const
{
    if (menu == nullptr) {
        return;
    }

    ApplyMenuBackground(menu);

    const int itemCount = GetMenuItemCount(menu);
    bool reserveIconSpace = false;
    for (int index = 0; index < itemCount; ++index) {
        MENUITEMINFOW itemInfo{};
        itemInfo.cbSize = sizeof(itemInfo);
        itemInfo.fMask = MIIM_ID;
        if (GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &itemInfo) &&
            (itemInfo.wID == ID_FORMAT_WORD_WRAP || itemInfo.wID == ID_VIEW_STATUS_BAR)) {
            reserveIconSpace = true;
            break;
        }
    }

    for (int index = 0; index < itemCount; ++index) {
        const UINT state = GetMenuState(menu, static_cast<UINT>(index), MF_BYPOSITION);
        const bool separator = (state & MF_SEPARATOR) != 0;

        std::wstring text;
        if (!separator) {
            std::array<wchar_t, 256> textBuffer{};
            GetMenuStringW(
                menu,
                static_cast<UINT>(index),
                textBuffer.data(),
                static_cast<int>(textBuffer.size()),
                MF_BYPOSITION);
            text = textBuffer.data();
        }

        OwnerDrawMenuItem* ownerDrawItem = StoreOwnerDrawMenuItem(storage, text, separator, reserveIconSpace);
        MENUITEMINFOW itemInfo{};
        itemInfo.cbSize = sizeof(itemInfo);
        itemInfo.fMask = MIIM_FTYPE | MIIM_DATA;
        itemInfo.fType = MFT_OWNERDRAW;
        itemInfo.dwItemData = reinterpret_cast<ULONG_PTR>(ownerDrawItem);
        if (separator) {
            itemInfo.fMask |= MIIM_STATE;
            itemInfo.fState = MFS_DISABLED;
        }
        SetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &itemInfo);

        HMENU submenu = GetSubMenu(menu, index);
        if (submenu != nullptr) {
            ApplyOwnerDrawToPopupMenu(submenu, storage);
        }
    }
}

void ClassicNotepadApp::ApplyOwnerDrawToMainMenu()
{
    if (mainMenu_ == nullptr || mainMenuOwnerDrawApplied_) {
        return;
    }

    mainOwnerDrawMenuItems_.clear();

    const int itemCount = GetMenuItemCount(mainMenu_);
    for (int index = 0; index < itemCount; ++index) {
        HMENU submenu = GetSubMenu(mainMenu_, index);
        if (submenu != nullptr) {
            ApplyOwnerDrawToPopupMenu(submenu, mainOwnerDrawMenuItems_);
        }
    }

    mainMenuOwnerDrawApplied_ = true;
}

void ClassicNotepadApp::UpdateMenuChrome()
{
    if (mainWindow_ == nullptr) {
        return;
    }

    if (mainMenu_ == nullptr) {
        mainMenu_ = GetMenu(mainWindow_);
    }

    ApplyOwnerDrawToMainMenu();
    if (mainMenu_ != nullptr) {
        const int itemCount = GetMenuItemCount(mainMenu_);
        for (int index = 0; index < itemCount; ++index) {
            HMENU submenu = GetSubMenu(mainMenu_, index);
            if (submenu != nullptr) {
                ApplyMenuBackground(submenu);
            }
        }
    }

    if (GetMenu(mainWindow_) != nullptr) {
        SetMenu(mainWindow_, nullptr);
    }
    if (menuBar_ != nullptr) {
        ShowWindow(menuBar_, SW_SHOWNA);
        InvalidateRect(menuBar_, nullptr, FALSE);
    }

    DrawMenuBar(mainWindow_);
    ResizeEditor();
}

void ClassicNotepadApp::UpdateThemeFromSystem()
{
    BeginEditorViewportAnchorPreservation();
    const bool newDarkMode = ShouldUseDarkMode(appearanceTheme_);
    if (darkModeEnabled_ != newDarkMode) {
        darkModeEnabled_ = newDarkMode;
        RecreateThemeBrushes();
    }

    ApplyThemeToWindows();
    EndEditorViewportAnchorPreservation();
}

void ClassicNotepadApp::ApplyThemeToWindows()
{
    ApplyDarkTitleBar(mainWindow_, darkModeEnabled_);

    if (mainWindow_ != nullptr) {
        UpdateMenuChrome();
        InvalidateRect(mainWindow_, nullptr, TRUE);
    }

    if (editor_ != nullptr) {
        UpdateEditorFrameStyle();
        InvalidateRect(editor_, nullptr, TRUE);
    }
    if (largeFileView_ != nullptr) {
        largeFileView_->SetTheme(editorFont_, darkModeEnabled_);
    }

    if (statusBar_ != nullptr) {
        UpdateStatusBarSizeGripStyle();
        SendMessageW(statusBar_, SB_SETBKCOLOR, 0, darkModeEnabled_ ? kDarkStatusBackground : CLR_DEFAULT);
        if (editor_ != nullptr) {
            UpdateStatusBar();
        }
        InvalidateRect(statusBar_, nullptr, TRUE);
    }
}

void ClassicNotepadApp::RecreateThemeBrushes()
{
    DestroyThemeBrushes();
    if (darkModeEnabled_) {
        darkEditorBackgroundBrush_ = CreateSolidBrush(kDarkEditorBackground);
        darkStatusBackgroundBrush_ = CreateSolidBrush(kDarkStatusBackground);
        darkMenuBackgroundBrush_ = CreateSolidBrush(kDarkMenuBackground);
    }
}

void ClassicNotepadApp::DestroyThemeBrushes()
{
    if (darkEditorBackgroundBrush_ != nullptr) {
        DeleteObject(darkEditorBackgroundBrush_);
        darkEditorBackgroundBrush_ = nullptr;
    }

    if (darkStatusBackgroundBrush_ != nullptr) {
        DeleteObject(darkStatusBackgroundBrush_);
        darkStatusBackgroundBrush_ = nullptr;
    }

    if (darkMenuBackgroundBrush_ != nullptr) {
        DeleteObject(darkMenuBackgroundBrush_);
        darkMenuBackgroundBrush_ = nullptr;
    }
}

LRESULT ClassicNotepadApp::HandleControlColor(HDC deviceContext, HWND controlWindow) const
{
    if (!darkModeEnabled_ || controlWindow != editor_ || darkEditorBackgroundBrush_ == nullptr) {
        return 0;
    }

    SetTextColor(deviceContext, kDarkEditorText);
    SetBkColor(deviceContext, kDarkEditorBackground);
    return reinterpret_cast<LRESULT>(darkEditorBackgroundBrush_);
}

LRESULT ClassicNotepadApp::HandleDrawItem(WPARAM controlId, LPARAM lParam) const
{
    const auto* drawItem = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
    if (drawItem == nullptr || drawItem->hDC == nullptr) {
        return 0;
    }

    if (drawItem->CtlType == ODT_MENU) {
        const auto* menuItem = reinterpret_cast<const OwnerDrawMenuItem*>(drawItem->itemData);
        if (menuItem == nullptr) {
            return 0;
        }

        const bool selected = (drawItem->itemState & ODS_SELECTED) != 0;
        const bool disabled = (drawItem->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
        const bool checked = (drawItem->itemState & ODS_CHECKED) != 0;

        const COLORREF backgroundColor = darkModeEnabled_
            ? (selected ? kDarkMenuHotBackground : kDarkMenuBackground)
            : GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_MENU);
        const COLORREF textColor = darkModeEnabled_
            ? (disabled ? kDarkMenuDisabledText : kDarkMenuText)
            : GetSysColor(disabled ? COLOR_GRAYTEXT : (selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT));
        const COLORREF separatorColor = darkModeEnabled_ ? kDarkMenuSeparator : GetSysColor(COLOR_3DSHADOW);

        HBRUSH backgroundBrush = CreateSolidBrush(backgroundColor);
        if (backgroundBrush != nullptr) {
            FillRect(drawItem->hDC, &drawItem->rcItem, backgroundBrush);
            DeleteObject(backgroundBrush);
        }

        if (menuItem->separator) {
            const int checkWidth = menuItem->reserveIconSpace
                ? ScalePixelsForWindow(mainWindow_, kPopupMenuCheckWidth)
                : 0;
            const int padding = ScalePixelsForWindow(mainWindow_, kPopupMenuHorizontalPadding);
            const int y = drawItem->rcItem.top + ((drawItem->rcItem.bottom - drawItem->rcItem.top) / 2);
            HPEN separatorPen = CreatePen(PS_SOLID, 1, separatorColor);
            HGDIOBJ previousPen = separatorPen != nullptr ? SelectObject(drawItem->hDC, separatorPen) : nullptr;
            MoveToEx(drawItem->hDC, drawItem->rcItem.left + checkWidth + padding, y, nullptr);
            LineTo(drawItem->hDC, drawItem->rcItem.right - padding, y);
            if (previousPen != nullptr) {
                SelectObject(drawItem->hDC, previousPen);
            }
            if (separatorPen != nullptr) {
                DeleteObject(separatorPen);
            }
            return TRUE;
        }

        HGDIOBJ previousFont = SelectObject(
            drawItem->hDC,
            menuFont_ != nullptr ? menuFont_ : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(drawItem->hDC, TRANSPARENT);
        SetTextColor(drawItem->hDC, textColor);

        const int checkWidth = menuItem->reserveIconSpace
            ? ScalePixelsForWindow(mainWindow_, kPopupMenuCheckWidth)
            : 0;
        const int padding = ScalePixelsForWindow(mainWindow_, kPopupMenuHorizontalPadding);
        const int acceleratorGap = ScalePixelsForWindow(mainWindow_, kPopupMenuAcceleratorGap);

        if (checked && menuItem->reserveIconSpace) {
            RECT checkRect = drawItem->rcItem;
            checkRect.right = checkRect.left + checkWidth;
            const wchar_t checkMark[] = { 0x2713, L'\0' };
            DrawTextW(
                drawItem->hDC,
                checkMark,
                -1,
                &checkRect,
                DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        }

        std::wstring label;
        std::wstring accelerator;
        classic_notepad::windows::SplitMenuItemText(menuItem->text, label, accelerator);

        RECT labelRect = drawItem->rcItem;
        labelRect.left += checkWidth + padding;
        labelRect.right -= padding;
        if (!accelerator.empty()) {
            labelRect.right -= acceleratorGap + MeasureText(drawItem->hDC, accelerator).cx;
        }

        DrawTextW(
            drawItem->hDC,
            label.c_str(),
            -1,
            &labelRect,
            DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

        if (!accelerator.empty()) {
            RECT acceleratorRect = drawItem->rcItem;
            acceleratorRect.left = labelRect.right + acceleratorGap;
            acceleratorRect.right -= padding;
            DrawTextW(
                drawItem->hDC,
                accelerator.c_str(),
                -1,
                &acceleratorRect,
                DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        }

        if (previousFont != nullptr) {
            SelectObject(drawItem->hDC, previousFont);
        }

        return TRUE;
    }

    if (controlId != ID_VIEW_STATUS_BAR || statusBar_ == nullptr) {
        return 0;
    }

    if (drawItem->hwndItem != statusBar_) {
        return 0;
    }

    std::size_t partIndex = static_cast<std::size_t>(drawItem->itemID);
    if (partIndex >= statusBarParts_.size() && drawItem->itemData < statusBarParts_.size()) {
        partIndex = static_cast<std::size_t>(drawItem->itemData);
    }

    if (partIndex >= statusBarParts_.size()) {
        return TRUE;
    }

    const COLORREF backgroundColor = darkModeEnabled_ ? kDarkStatusBackground : GetSysColor(COLOR_3DFACE);
    const COLORREF textColor = darkModeEnabled_ ? kDarkStatusText : GetSysColor(COLOR_BTNTEXT);
    const COLORREF separatorColor = darkModeEnabled_ ? kDarkStatusSeparator : kLightStatusSeparator;

    HBRUSH backgroundBrush = CreateSolidBrush(backgroundColor);
    if (backgroundBrush != nullptr) {
        FillRect(drawItem->hDC, &drawItem->rcItem, backgroundBrush);
        DeleteObject(backgroundBrush);
    }

    if (partIndex > 0U) {
        HPEN separatorPen = CreatePen(PS_SOLID, 1, separatorColor);
        HGDIOBJ previousPen = separatorPen != nullptr ? SelectObject(drawItem->hDC, separatorPen) : nullptr;
        const int separatorInset = ScalePixelsForWindow(statusBar_, kStatusBarSeparatorInset);
        const int separatorX = drawItem->rcItem.left;
        MoveToEx(drawItem->hDC, separatorX, drawItem->rcItem.top + separatorInset, nullptr);
        LineTo(drawItem->hDC, separatorX, std::max(drawItem->rcItem.top + separatorInset, drawItem->rcItem.bottom - separatorInset));
        if (previousPen != nullptr) {
            SelectObject(drawItem->hDC, previousPen);
        }
        if (separatorPen != nullptr) {
            DeleteObject(separatorPen);
        }
    }

    HGDIOBJ previousFont = SelectObject(
        drawItem->hDC,
        menuFont_ != nullptr ? menuFont_ : GetStockObject(DEFAULT_GUI_FONT));

    RECT itemRect = drawItem->rcItem;
    const int horizontalPadding = ScalePixelsForWindow(statusBar_, kStatusBarHorizontalPadding);
    itemRect.left += horizontalPadding;
    itemRect.right -= horizontalPadding;
    if (partIndex + 1U == statusBarParts_.size()) {
        const RECT gripRect = GetStatusBarResizeGripRect();
        if (!IsRectEmpty(&gripRect)) {
            itemRect.right = std::min(itemRect.right, gripRect.left - horizontalPadding);
        }
    }

    SetBkMode(drawItem->hDC, TRANSPARENT);
    SetTextColor(drawItem->hDC, textColor);
    DrawTextW(
        drawItem->hDC,
        statusBarParts_[partIndex].c_str(),
        -1,
        &itemRect,
        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (previousFont != nullptr) {
        SelectObject(drawItem->hDC, previousFont);
    }
    return TRUE;
}

LRESULT ClassicNotepadApp::HandleMeasureItem(WPARAM, LPARAM lParam) const
{
    auto* measureItem = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
    if (measureItem == nullptr || measureItem->CtlType != ODT_MENU) {
        return 0;
    }

    const auto* menuItem = reinterpret_cast<const OwnerDrawMenuItem*>(measureItem->itemData);
    if (menuItem == nullptr) {
        return 0;
    }

    if (menuItem->separator) {
        measureItem->itemWidth = static_cast<UINT>(ScalePixelsForWindow(mainWindow_, 96));
        measureItem->itemHeight = static_cast<UINT>(ScalePixelsForWindow(mainWindow_, kPopupMenuSeparatorHeight));
        return TRUE;
    }

    HDC deviceContext = GetDC(mainWindow_);
    if (deviceContext == nullptr) {
        measureItem->itemWidth = static_cast<UINT>(ScalePixelsForWindow(mainWindow_, 160));
        measureItem->itemHeight = static_cast<UINT>(ScalePixelsForWindow(mainWindow_, kPopupMenuItemHeight));
        return TRUE;
    }

    HGDIOBJ previousFont = SelectObject(
        deviceContext,
        menuFont_ != nullptr ? menuFont_ : GetStockObject(DEFAULT_GUI_FONT));

    std::wstring label;
    std::wstring accelerator;
    classic_notepad::windows::SplitMenuItemText(menuItem->text, label, accelerator);
    const SIZE labelSize = MeasureText(deviceContext, label);
    const SIZE acceleratorSize = MeasureText(deviceContext, accelerator);

    TEXTMETRICW textMetrics{};
    GetTextMetricsW(deviceContext, &textMetrics);

    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }
    ReleaseDC(mainWindow_, deviceContext);

    const int padding = ScalePixelsForWindow(mainWindow_, kPopupMenuHorizontalPadding);
    const int checkWidth = menuItem->reserveIconSpace
        ? ScalePixelsForWindow(mainWindow_, kPopupMenuCheckWidth)
        : 0;
    const int acceleratorGap = accelerator.empty() ? 0 : ScalePixelsForWindow(mainWindow_, kPopupMenuAcceleratorGap);
    const int minimumHeight = ScalePixelsForWindow(mainWindow_, kPopupMenuItemHeight);

    measureItem->itemWidth = static_cast<UINT>(
        checkWidth +
        (padding * 2) +
        labelSize.cx +
        acceleratorGap +
        acceleratorSize.cx);
    measureItem->itemHeight = static_cast<UINT>(std::max(
        minimumHeight,
        static_cast<int>(textMetrics.tmHeight + textMetrics.tmExternalLeading + ScalePixelsForWindow(mainWindow_, 10))));
    return TRUE;
}

LRESULT ClassicNotepadApp::HandleNotify(LPARAM lParam) const
{
    const auto* header = reinterpret_cast<const NMHDR*>(lParam);
    if (!darkModeEnabled_ || header == nullptr || header->hwndFrom != statusBar_ ||
        header->code != kStatusBarCustomDrawCode) {
        return 0;
    }

    auto* customDraw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
    switch (customDraw->dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT:
        SetTextColor(customDraw->hdc, kDarkStatusText);
        SetBkColor(customDraw->hdc, kDarkStatusBackground);
        return CDRF_DODEFAULT;

    default:
        break;
    }

    return CDRF_DODEFAULT;
}

void ClassicNotepadApp::ResizeEditor()
{
    BeginEditorViewportAnchorPreservation();
    if (IsLargeFileMode()) {
        AdvanceSpellingLayoutRevision();
        const RECT hostRect = GetEditorHostRect();
        largeFileView_->SetBounds(hostRect);
        if (verticalScrollBar_ != nullptr) {
            ShowWindow(verticalScrollBar_, SW_HIDE);
        }
        if (horizontalScrollBar_ != nullptr) {
            ShowWindow(horizontalScrollBar_, SW_HIDE);
        }
        if (scrollCorner_ != nullptr) {
            ShowWindow(scrollCorner_, SW_HIDE);
        }
        EndEditorViewportAnchorPreservation();
        RefreshSpellCheck(false);
        return;
    }
    if (editor_ == nullptr) {
        EndEditorViewportAnchorPreservation();
        return;
    }

    AdvanceSpellingLayoutRevision();
    UpdateScrollBars();
    RefreshSpellCheck(false);
    EndEditorViewportAnchorPreservation();
}

RECT ClassicNotepadApp::GetEditorHostRect()
{
    RECT hostRect{};
    if (mainWindow_ == nullptr) {
        return hostRect;
    }

    RECT clientArea{};
    GetClientRect(mainWindow_, &clientArea);

    int statusHeight = 0;
    if (statusBar_ != nullptr) {
        SendMessageW(statusBar_, WM_SIZE, 0, 0);
        if (statusBarVisible_) {
            RECT statusRect{};
            GetWindowRect(statusBar_, &statusRect);
            statusHeight = statusRect.bottom - statusRect.top;
        }
    }

    const int width = static_cast<int>(clientArea.right - clientArea.left);
    int menuHeight = 0;
    if (darkModeEnabled_ && menuBar_ != nullptr && IsWindowVisible(menuBar_)) {
        menuHeight = GetMenuBarHeight();
        MoveChildWindowIfNeeded(menuBar_, RECT{ 0, 0, width, menuHeight }, true);
    }

    hostRect.left = 0;
    hostRect.top = menuHeight;
    hostRect.right = width;
    hostRect.bottom = std::max(menuHeight, static_cast<int>(clientArea.bottom - clientArea.top) - statusHeight);
    return hostRect;
}

RECT ClassicNotepadApp::GetEditorRectForScrollBars(
    const RECT& hostRect,
    bool verticalVisible,
    bool horizontalVisible) const
{
    RECT editorRect = hostRect;
    if (!UseCustomScrollBars()) {
        return editorRect;
    }

    if (verticalVisible) {
        editorRect.right = std::max(editorRect.left, editorRect.right - GetScrollBarThickness(ScrollBarOrientation::Vertical));
    }
    if (horizontalVisible) {
        editorRect.bottom = std::max(editorRect.top, editorRect.bottom - GetScrollBarThickness(ScrollBarOrientation::Horizontal));
    }

    return editorRect;
}

void ClassicNotepadApp::ApplyEditorAndScrollBarLayout(
    const RECT& hostRect,
    bool verticalVisible,
    bool horizontalVisible,
    bool repaint)
{
    if (editor_ == nullptr) {
        return;
    }

    const RECT editorRect = GetEditorRectForScrollBars(hostRect, verticalVisible, horizontalVisible);
    MoveChildWindowIfNeeded(editor_, editorRect, repaint);

    if (verticalScrollBar_ == nullptr || horizontalScrollBar_ == nullptr || scrollCorner_ == nullptr) {
        return;
    }

    if (!UseCustomScrollBars()) {
        ShowWindow(verticalScrollBar_, SW_HIDE);
        ShowWindow(horizontalScrollBar_, SW_HIDE);
        ShowWindow(scrollCorner_, SW_HIDE);
        return;
    }

    const int verticalThickness = GetScrollBarThickness(ScrollBarOrientation::Vertical);
    const int horizontalThickness = GetScrollBarThickness(ScrollBarOrientation::Horizontal);
    const int editorWidth = std::max(0, static_cast<int>(editorRect.right - editorRect.left));
    const int editorHeight = std::max(0, static_cast<int>(editorRect.bottom - editorRect.top));

    if (verticalVisible && editorHeight > 0) {
        MoveChildWindowIfNeeded(
            verticalScrollBar_,
            RECT{ editorRect.right, hostRect.top, editorRect.right + verticalThickness, hostRect.top + editorHeight },
            repaint);
        if (!IsWindowVisible(verticalScrollBar_)) {
            SetWindowPos(
                verticalScrollBar_,
                HWND_TOP,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    } else {
        ShowWindow(verticalScrollBar_, SW_HIDE);
    }

    if (horizontalVisible && editorWidth > 0) {
        MoveChildWindowIfNeeded(
            horizontalScrollBar_,
            RECT{ hostRect.left, editorRect.bottom, hostRect.left + editorWidth, editorRect.bottom + horizontalThickness },
            repaint);
        if (!IsWindowVisible(horizontalScrollBar_)) {
            SetWindowPos(
                horizontalScrollBar_,
                HWND_TOP,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    } else {
        ShowWindow(horizontalScrollBar_, SW_HIDE);
    }

    if (verticalVisible && horizontalVisible && editorWidth > 0 && editorHeight > 0) {
        MoveChildWindowIfNeeded(
            scrollCorner_,
            RECT{ editorRect.right, editorRect.bottom, editorRect.right + verticalThickness, editorRect.bottom + horizontalThickness },
            repaint);
        if (!IsWindowVisible(scrollCorner_)) {
            SetWindowPos(
                scrollCorner_,
                HWND_TOP,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    } else {
        ShowWindow(scrollCorner_, SW_HIDE);
    }
}

int ClassicNotepadApp::GetScrollBarThickness(ScrollBarOrientation orientation) const
{
    return orientation == ScrollBarOrientation::Vertical
        ? std::max(1, GetSystemMetrics(SM_CXVSCROLL))
        : std::max(1, GetSystemMetrics(SM_CYHSCROLL));
}

bool ClassicNotepadApp::UseCustomScrollBars() const
{
    return darkModeEnabled_;
}

void ClassicNotepadApp::UpdateEditorFrameStyle()
{
    if (editor_ == nullptr) {
        return;
    }

    const LONG_PTR currentStyle = GetWindowLongPtrW(editor_, GWL_EXSTYLE);
    const LONG_PTR wantedStyle = darkModeEnabled_
        ? (currentStyle & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE))
        : (currentStyle | static_cast<LONG_PTR>(WS_EX_CLIENTEDGE));

    if (wantedStyle == currentStyle) {
        return;
    }

    BeginEditorViewportAnchorPreservation();
    SetWindowLongPtrW(editor_, GWL_EXSTYLE, wantedStyle);
    SetWindowPos(
        editor_,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ResizeEditor();
    EndEditorViewportAnchorPreservation();
}

void ClassicNotepadApp::UpdateScrollBars()
{
    if (IsLargeFileMode()) {
        if (verticalScrollBar_ != nullptr) {
            ShowWindow(verticalScrollBar_, SW_HIDE);
        }
        if (horizontalScrollBar_ != nullptr) {
            ShowWindow(horizontalScrollBar_, SW_HIDE);
        }
        if (scrollCorner_ != nullptr) {
            ShowWindow(scrollCorner_, SW_HIDE);
        }
        return;
    }
    if (editor_ == nullptr) {
        return;
    }

    if (updatingScrollBars_) {
        return;
    }

    updatingScrollBars_ = true;
    const RECT hostRect = GetEditorHostRect();
    const bool useCustomScrollBars = UseCustomScrollBars();

    if (useCustomScrollBars) {
        SetNativeEditorScrollBarVisibility(false, false);

        bool verticalVisible = verticalScrollBarVisible_;
        bool horizontalVisible = horizontalScrollBarVisible_ && !wordWrap_;
        for (int pass = 0; pass < 4; ++pass) {
            ApplyEditorAndScrollBarLayout(hostRect, verticalVisible, horizontalVisible, false);
            const RECT editorRect = GetEditorRectForScrollBars(hostRect, verticalVisible, horizontalVisible);
            const int availableWidth = std::max(0, static_cast<int>(editorRect.right - editorRect.left));
            const int availableHeight = std::max(0, static_cast<int>(editorRect.bottom - editorRect.top));
            const bool nextHorizontalVisible = !wordWrap_ && NeedsHorizontalScrollBar(availableWidth);
            const bool nextVerticalVisible = NeedsVerticalScrollBar(availableHeight);

            if (nextVerticalVisible == verticalVisible && nextHorizontalVisible == horizontalVisible) {
                break;
            }

            verticalVisible = nextVerticalVisible;
            horizontalVisible = nextHorizontalVisible;
        }

        ApplyEditorAndScrollBarLayout(hostRect, verticalVisible, horizontalVisible, true);
        SetEditorScrollBarVisibility(verticalVisible, horizontalVisible);
        updatingScrollBars_ = false;
        return;
    }

    ApplyEditorAndScrollBarLayout(hostRect, false, false, false);

    RECT clientRect{};
    GetClientRect(editor_, &clientRect);

    const int verticalMetric = GetSystemMetrics(SM_CXVSCROLL);
    const int horizontalMetric = GetSystemMetrics(SM_CYHSCROLL);
    const int baseWidth = std::max(
        0,
        static_cast<int>(clientRect.right - clientRect.left) +
            (verticalScrollBarVisible_ ? verticalMetric : 0));
    const int baseHeight = std::max(
        0,
        static_cast<int>(clientRect.bottom - clientRect.top) +
            (horizontalScrollBarVisible_ ? horizontalMetric : 0));

    bool verticalVisible = false;
    bool horizontalVisible = false;
    for (int pass = 0; pass < 3; ++pass) {
        const int availableWidth = std::max(0, baseWidth - (verticalVisible ? verticalMetric : 0));
        const int availableHeight = std::max(0, baseHeight - (horizontalVisible ? horizontalMetric : 0));
        const bool nextVerticalVisible = NeedsVerticalScrollBar(availableHeight);
        const bool nextHorizontalVisible = !wordWrap_ && NeedsHorizontalScrollBar(availableWidth);

        if (nextVerticalVisible == verticalVisible && nextHorizontalVisible == horizontalVisible) {
            break;
        }

        verticalVisible = nextVerticalVisible;
        horizontalVisible = nextHorizontalVisible;
    }

    SetEditorScrollBarVisibility(verticalVisible, horizontalVisible);
    ApplyEditorAndScrollBarLayout(hostRect, verticalVisible, horizontalVisible, true);
    updatingScrollBars_ = false;
}

void ClassicNotepadApp::InvalidateWidestLineCache()
{
    BeginEditorWidthIndexRebuild();
}

int ClassicNotepadApp::GetVisibleEditorLineCount(int lineMargin) const
{
    if (editor_ == nullptr) {
        return std::max(1, lineMargin * 2 + 1);
    }

    RECT clientRect{};
    GetClientRect(editor_, &clientRect);
    const int clientHeight = std::max(0, static_cast<int>(clientRect.bottom - clientRect.top));
    const int lineHeight = GetEditorLineHeight();
    const int visibleLines = std::max(1, (clientHeight + lineHeight - 1) / lineHeight + 1);
    return visibleLines + std::max(0, lineMargin) * 2;
}

std::wstring ClassicNotepadApp::GetEditorLineText(int line, int lengthLimit) const
{
    if (editor_ == nullptr || lengthLimit <= 0) {
        return {};
    }

    const int bufferLength = std::clamp(lengthLimit, 0, kMaxEditorLineFetchChars);
    if (bufferLength <= 0) {
        return {};
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(bufferLength) + 1U, L'\0');
    *reinterpret_cast<WORD*>(buffer.data()) = static_cast<WORD>(bufferLength);
    const LRESULT copied = SendMessageW(editor_, EM_GETLINE, static_cast<WPARAM>(line), reinterpret_cast<LPARAM>(buffer.data()));
    if (copied <= 0) {
        return {};
    }

    return std::wstring(
        buffer.data(),
        static_cast<std::size_t>(std::min<int>(static_cast<int>(copied), bufferLength)));
}

bool ClassicNotepadApp::GetVisibleEditorTextWindows(
    std::vector<classic_notepad::SpellingTextWindow>& windows) const
{
    windows.clear();
    if (editor_ == nullptr) return false;

    const int textLength = GetWindowTextLengthW(editor_);
    if (textLength <= 0) return false;

    RECT client{};
    if (!GetClientRect(editor_, &client) || IsRectEmpty(&client)) return false;
    const int lineHeight = std::max(1, GetEditorLineHeight());
    const int lineCount = std::max<int>(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    const int firstVisibleLine = std::clamp(
        static_cast<int>(SendMessageW(editor_, EM_GETFIRSTVISIBLELINE, 0, 0)), 0, lineCount - 1);
    const int visibleRows = std::max(1,
        (static_cast<int>(client.bottom - client.top) + lineHeight - 1) / lineHeight + 1);
    const int firstRow = firstVisibleLine;
    const int lastRow = std::min(lineCount - 1, firstVisibleLine + visibleRows - 1);
    const int maximumAdvance = std::max(1, GetEditorFontMaximumAdvance());

    struct NativeVisibleSegment {
        std::uint64_t rowStart = 0;
        std::uint64_t rowEnd = 0;
        std::uint64_t visibleStart = 0;
        std::uint64_t visibleEnd = 0;
    };
    std::vector<NativeVisibleSegment> segments;
    segments.reserve(static_cast<std::size_t>(std::max(0, lastRow - firstRow + 1)));

    const auto positionX = [this](std::uint64_t index) {
        const POINT point = PointFromEditorCharPosition(editor_, static_cast<DWORD>(index));
        return static_cast<int>(point.x);
    };
    const auto lowerBoundX = [&positionX](std::uint64_t start, std::uint64_t length, int target) {
        std::uint64_t low = 0;
        std::uint64_t high = length;
        while (low < high) {
            const std::uint64_t middle = low + (high - low) / 2U;
            if (positionX(start + middle) < target) low = middle + 1U;
            else high = middle;
        }
        return start + low;
    };

    for (int row = firstRow; row <= lastRow; ++row) {
        const LRESULT rowStartResult = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(row), 0);
        if (rowStartResult < 0) continue;
        const std::uint64_t rowStart = static_cast<std::uint64_t>(rowStartResult);
        if (rowStart >= static_cast<std::uint64_t>(textLength)) continue;
        const LRESULT rowLengthResult = SendMessageW(editor_, EM_LINELENGTH,
            static_cast<WPARAM>(rowStart), 0);
        if (rowLengthResult <= 0) continue;
        const std::uint64_t rowLength = static_cast<std::uint64_t>(rowLengthResult);
        if (rowLength > static_cast<std::uint64_t>(textLength) - rowStart) continue;
        const std::uint64_t rowEnd = rowStart + rowLength;

        // Use full-width EM_POSFROMCHAR indices to find the text segment whose
        // character origins intersect the client. EM_CHARFROMPOS truncates its
        // returned character index and is not suitable for this extraction.
        const std::uint64_t visibleStart = lowerBoundX(
            rowStart, rowLength, static_cast<int>(client.left) - maximumAdvance);
        const std::uint64_t visibleEnd = lowerBoundX(
            rowStart, rowLength, static_cast<int>(client.right) + 1);
        if (visibleStart >= visibleEnd || visibleStart >= rowEnd) continue;
        segments.push_back({rowStart, rowEnd, visibleStart, std::min(visibleEnd, rowEnd)});
    }
    if (segments.empty()) return false;

    const HLOCAL textHandle = reinterpret_cast<HLOCAL>(SendMessageW(editor_, EM_GETHANDLE, 0, 0));
    if (textHandle == nullptr) return false;
    const auto unlockLocalText = [textHandle](const wchar_t*) { LocalUnlock(textHandle); };
    std::unique_ptr<const wchar_t, decltype(unlockLocalText)> lockedText(
        static_cast<const wchar_t*>(LocalLock(textHandle)), unlockLocalText);
    if (!lockedText) return false;

    const classic_notepad::SpellingRevision revision = CurrentSpellingRevision();
    std::size_t remainingUnits = kMaxVisibleSpellCheckChars;
    windows.reserve(segments.size());
    for (const NativeVisibleSegment& segment : segments) {
        std::uint64_t start = segment.visibleStart;
        std::uint64_t end = segment.visibleEnd;
        if (start > 0U && start < static_cast<std::uint64_t>(textLength) &&
            IsHighSurrogateUnit(lockedText.get()[start - 1U]) &&
            IsLowSurrogateUnit(lockedText.get()[start])) {
            --start;
        }
        if (end > 0U && end < static_cast<std::uint64_t>(textLength) &&
            IsHighSurrogateUnit(lockedText.get()[end - 1U]) &&
            IsLowSurrogateUnit(lockedText.get()[end])) {
            ++end;
        }

        const std::uint64_t available = end - start;
        if (available == 0U || remainingUnits == 0U) continue;
        if (available > remainingUnits) {
            end = start + remainingUnits;
            if (end > 0U && end < static_cast<std::uint64_t>(textLength) &&
                IsHighSurrogateUnit(lockedText.get()[end - 1U]) &&
                IsLowSurrogateUnit(lockedText.get()[end])) {
                --end;
            }
            if (end <= start) continue;
        }
        const std::size_t copiedUnits = static_cast<std::size_t>(end - start);
        if (copiedUnits == 0U) continue;

        classic_notepad::SpellingTextWindow window;
        window.revision = revision;
        window.absoluteStart = start;
        window.text.assign(lockedText.get() + start, copiedUnits);
        // A native EDIT display row can split an unbroken word. Check the
        // adjacent document units, rather than the display-row bounds, so a
        // wrapped row boundary does not make a clipped word fragment eligible.
        const bool leftBoundaryContinuesWord = start > 0U &&
            classic_notepad::IsSpellingWordContinuationUnit(lockedText.get()[start - 1U]);
        const bool rightBoundaryContinuesWord = end < static_cast<std::uint64_t>(textLength) &&
            classic_notepad::IsSpellingWordContinuationUnit(lockedText.get()[end]);
        classic_notepad::SetSpellingEligibleBounds(
            window, leftBoundaryContinuesWord, rightBoundaryContinuesWord);
        if (!classic_notepad::IsUtf16ScalarBoundary(window.text, window.eligibleStart) ||
            !classic_notepad::IsUtf16ScalarBoundary(window.text, window.eligibleEnd)) {
            windows.clear();
            return false;
        }
        remainingUnits -= copiedUnits;
        windows.push_back(std::move(window));
    }
    return !windows.empty();
}

bool ClassicNotepadApp::ResolveEditorSpellingContextTarget(
    POINT clientPoint,
    bool keyboardInvocation,
    classic_notepad::SpellingContextTarget& target) const
{
    target = {};
    if (editor_ == nullptr || !spellCheckAvailable_) return false;
    const classic_notepad::SpellingRevision revision = CurrentSpellingRevision();
    if (!classic_notepad::SpellingBatchIsCurrent(spellingBatch_, revision)) return false;

    std::vector<classic_notepad::SpellingTextWindow> windows;
    if (!GetVisibleEditorTextWindows(windows)) return false;
    if (keyboardInvocation) {
        DWORD selectionStart = 0U;
        DWORD selectionEnd = 0U;
        GetSelectionRange(selectionStart, selectionEnd);
        std::uint64_t caretPosition = selectionEnd;
        if (selectionStart != selectionEnd) {
            POINT caretPoint{};
            if (!GetCaretPos(&caretPoint)) return false;
            const POINT startPoint = PointFromEditorCharPosition(editor_, selectionStart);
            const POINT endPoint = PointFromEditorCharPosition(editor_, selectionEnd);
            const bool atStart = caretPoint.x == startPoint.x && caretPoint.y == startPoint.y;
            const bool atEnd = caretPoint.x == endPoint.x && caretPoint.y == endPoint.y;
            if (atStart == atEnd) return false;
            caretPosition = atStart ? selectionStart : selectionEnd;
        }
        return classic_notepad::ResolveSpellingContextTarget(
            windows, revision, caretPosition, true, target);
    }

    RECT client{};
    if (!GetClientRect(editor_, &client) || !PtInRect(&client, clientPoint)) return false;
    const int textLength = GetWindowTextLengthW(editor_);
    const int lineCount = std::max<int>(1,
        static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    const int firstVisibleLine = std::clamp(
        static_cast<int>(SendMessageW(editor_, EM_GETFIRSTVISIBLELINE, 0, 0)), 0, lineCount - 1);
    const int lineHeight = std::max(1, GetEditorLineHeight());
    const int visibleRows = std::max(1,
        (static_cast<int>(client.bottom - client.top) + lineHeight - 1) / lineHeight + 1);
    const int lastVisibleLine = std::min(lineCount - 1, firstVisibleLine + visibleRows);
    bool foundPosition = false;
    std::uint64_t characterPosition = 0U;
    for (int displayLine = firstVisibleLine; displayLine <= lastVisibleLine; ++displayLine) {
        const LRESULT rowStartResult = SendMessageW(
            editor_, EM_LINEINDEX, static_cast<WPARAM>(displayLine), 0);
        if (rowStartResult < 0) continue;
        const std::uint64_t rowStart = static_cast<std::uint64_t>(rowStartResult);
        if (rowStart >= static_cast<std::uint64_t>(textLength)) continue;
        const LRESULT rowLengthResult = SendMessageW(
            editor_, EM_LINELENGTH, static_cast<WPARAM>(rowStart), 0);
        if (rowLengthResult <= 0) continue;
        const std::uint64_t rowLength = static_cast<std::uint64_t>(rowLengthResult);
        if (rowLength > static_cast<std::uint64_t>(textLength) - rowStart) continue;
        const std::uint64_t rowEnd = rowStart + rowLength;
        const POINT rowOrigin = PointFromEditorCharPosition(editor_, static_cast<DWORD>(rowStart));
        if (clientPoint.y < rowOrigin.y ||
            clientPoint.y >= rowOrigin.y + lineHeight) continue;

        for (const classic_notepad::SpellingTextWindow& window : windows) {
            if (!classic_notepad::SpellingRevisionMatches(window.revision, revision) ||
                window.text.size() > (std::numeric_limits<std::uint64_t>::max)() - window.absoluteStart) {
                return false;
            }
            const std::uint64_t windowEnd = window.absoluteStart +
                static_cast<std::uint64_t>(window.text.size());
            std::uint64_t index = (std::max)(rowStart, window.absoluteStart);
            const std::uint64_t segmentEnd = (std::min)(rowEnd, windowEnd);
            while (index < segmentEnd) {
                const std::size_t local = static_cast<std::size_t>(index - window.absoluteStart);
                if (!classic_notepad::IsUtf16ScalarBoundary(window.text, local)) {
                    ++index;
                    continue;
                }
                std::uint64_t next = index + 1U;
                if (next < windowEnd && local + 1U < window.text.size() &&
                    IsHighSurrogateUnit(window.text[local]) && IsLowSurrogateUnit(window.text[local + 1U])) {
                    ++next;
                }
                if (next > segmentEnd || next > windowEnd) break;

                const POINT startPoint = PointFromEditorCharPosition(editor_, static_cast<DWORD>(index));
                const POINT endPoint = PointFromEditorCharPosition(editor_, static_cast<DWORD>(next));
                if (startPoint.y == rowOrigin.y && endPoint.y == rowOrigin.y &&
                    endPoint.x > startPoint.x && clientPoint.x >= startPoint.x &&
                    clientPoint.x < endPoint.x) {
                    if (foundPosition && characterPosition != index) return false;
                    characterPosition = index;
                    foundPosition = true;
                    break;
                }
                index = next;
            }
        }
    }
    return foundPosition && classic_notepad::ResolveSpellingContextTarget(
        windows, revision, characterPosition, false, target);
}

classic_notepad::SpellingRevision ClassicNotepadApp::CurrentSpellingRevision() const
{
    if (largeFileView_ != nullptr) return largeFileView_->GetSpellingRevision();
    return {spellingSessionId_, spellingContentRevision_, spellingLayoutRevision_};
}

void ClassicNotepadApp::ClearSpellingAnnotations()
{
    spellingBatch_ = {};
    if (largeFileView_ != nullptr) largeFileView_->SetSpellingBatch({});
    if (editor_ != nullptr) InvalidateRect(editor_, nullptr, FALSE);
}

void ClassicNotepadApp::AdvanceSpellingSession()
{
    if (++spellingSessionId_ == 0U) spellingSessionId_ = 1U;
    spellingContentRevision_ = 1U;
    spellingLayoutRevision_ = 1U;
    ClearIgnoredSpellingOccurrences();
    ClearSpellingAnnotations();
}

void ClassicNotepadApp::AdvanceSpellingContentRevision()
{
    if (++spellingContentRevision_ == 0U) spellingContentRevision_ = 1U;
    ClearIgnoredSpellingOccurrences();
    ClearSpellingAnnotations();
}

void ClassicNotepadApp::AdvanceSpellingLayoutRevision()
{
    if (++spellingLayoutRevision_ == 0U) spellingLayoutRevision_ = 1U;
    ClearSpellingAnnotations();
}

int ClassicNotepadApp::GetEditorLineHeight() const
{
    if (editor_ == nullptr) {
        return 1;
    }

    HDC deviceContext = GetDC(editor_);
    if (deviceContext == nullptr) {
        return 1;
    }

    HGDIOBJ previousFont = nullptr;
    if (editorFont_ != nullptr) {
        previousFont = SelectObject(deviceContext, editorFont_);
    }

    TEXTMETRICW textMetrics{};
    const bool measured = GetTextMetricsW(deviceContext, &textMetrics) != 0;

    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }
    ReleaseDC(editor_, deviceContext);

    if (!measured) {
        return 1;
    }

    return std::max(1, static_cast<int>(textMetrics.tmHeight + textMetrics.tmExternalLeading));
}

int ClassicNotepadApp::MeasureWidestEditorLine() const
{
    if (editor_ == nullptr) {
        return 0;
    }

    int widestLine = measuredEditorLineWidths_.empty() ? 0 : *measuredEditorLineWidths_.rbegin();
    if (editorWidthIndexNeedsRebuild_ || editorWidthIndexAllocationFailed_ || editorWidthUnknownLines_ != 0U) {
        widestLine = std::max(widestLine, GetEditorWidthFallback());
    }
    cachedWidestLineWidth_ = widestLine;
    widestLineCacheDirty_ = false;
    return widestLine;
}

int ClassicNotepadApp::GetEditorFontMaximumAdvance() const
{
    if (editor_ == nullptr) {
        return 1;
    }

    HDC deviceContext = GetDC(editor_);
    if (deviceContext == nullptr) {
        return std::max(1, GetEditorHorizontalScrollUnit());
    }

    HGDIOBJ previousFont = editorFont_ != nullptr ? SelectObject(deviceContext, editorFont_) : nullptr;
    TEXTMETRICW textMetrics{};
    const bool measured = GetTextMetricsW(deviceContext, &textMetrics) != 0;
    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }
    ReleaseDC(editor_, deviceContext);
    return measured ? std::max(1, static_cast<int>(textMetrics.tmMaxCharWidth)) : 1;
}

int ClassicNotepadApp::GetEditorTabInterval() const
{
    return SaturatingTextWidthEstimate(kTabWidthSpaces, std::max(1, GetEditorHorizontalScrollUnit()));
}

int ClassicNotepadApp::ConservativeEditorLineWidth(int lineLength) const
{
    if (lineLength <= 0) {
        return 0;
    }
    // A tab may advance to the next eight-average-character stop. Treat every
    // unit as the larger of that interval and the font's widest glyph advance.
    return SaturatingTextWidthEstimate(
        lineLength,
        std::max(GetEditorFontMaximumAdvance(), GetEditorTabInterval()));
}

int ClassicNotepadApp::GetEditorWidthFallback() const
{
    if (editor_ == nullptr) {
        return 0;
    }
    const int textLength = std::max(0, GetWindowTextLengthW(editor_));
    return SaturatingTextWidthEstimate(
        textLength,
        std::max(GetEditorFontMaximumAdvance(), GetEditorTabInterval()));
}

int ClassicNotepadApp::MeasureEditorLineWidth(int line, int lineLength, bool& conservative) const
{
    conservative = false;
    if (lineLength <= 0) {
        return 0;
    }
    if (lineLength > kExactLineWidthMeasureCharLimit) {
        conservative = true;
        return ConservativeEditorLineWidth(lineLength);
    }

    const std::wstring text = GetEditorLineText(line, lineLength);
    if (text.size() != static_cast<std::size_t>(lineLength)) {
        conservative = true;
        return ConservativeEditorLineWidth(lineLength);
    }

    return MeasureEditorTextWidth(text, conservative);
}

int ClassicNotepadApp::MeasureEditorTextWidth(const std::wstring& text, bool& conservative) const
{
    conservative = false;
    if (text.empty()) {
        return 0;
    }
    HDC deviceContext = GetDC(editor_);
    if (deviceContext == nullptr) {
        conservative = true;
        return ConservativeEditorLineWidth(static_cast<int>(std::min<std::size_t>(
            text.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)()))));
    }
    HGDIOBJ previousFont = editorFont_ != nullptr ? SelectObject(deviceContext, editorFont_) : nullptr;
    const int tabInterval = std::max(1, GetEditorTabInterval());
    int width = 0;
    std::size_t segmentStart = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index < text.size() && text[index] != L'\t') {
            continue;
        }
        if (index > segmentStart) {
            SIZE extent{};
            const int segmentLength = static_cast<int>(index - segmentStart);
            if (!GetTextExtentPoint32W(deviceContext, text.data() + segmentStart, segmentLength, &extent)) {
                conservative = true;
                width = ConservativeEditorLineWidth(static_cast<int>(std::min<std::size_t>(
                    text.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)()))));
                break;
            }
            width = (extent.cx > 0 && width > (std::numeric_limits<int>::max)() - extent.cx)
                ? (std::numeric_limits<int>::max)()
                : width + std::max(0, static_cast<int>(extent.cx));
        }
        if (index == text.size()) {
            break;
        }
        const int remainder = width % tabInterval;
        const int advance = remainder == 0 ? tabInterval : tabInterval - remainder;
        width = width > (std::numeric_limits<int>::max)() - advance
            ? (std::numeric_limits<int>::max)()
            : width + advance;
        segmentStart = index + 1U;
    }
    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }
    ReleaseDC(editor_, deviceContext);
    return width;
}

void ClassicNotepadApp::SetEditorLineWidth(std::size_t line, int width)
{
    if (line >= editorLineWidths_.size()) {
        return;
    }
    int& previous = editorLineWidths_[line];
    if (previous >= 0) {
        const auto found = measuredEditorLineWidths_.find(previous);
        if (found != measuredEditorLineWidths_.end()) {
            measuredEditorLineWidths_.erase(found);
        }
    } else if (width >= 0 && editorWidthUnknownLines_ > 0U) {
        --editorWidthUnknownLines_;
    }
    previous = width;
    if (width >= 0) {
        measuredEditorLineWidths_.insert(width);
    }
    widestLineCacheDirty_ = true;
}

void ClassicNotepadApp::BeginEditorWidthIndexRebuild()
{
    ++editorWidthGeneration_;
    editorWidthMaximumBatchDiscoveredLines_ = 0;
    editorWidthMaximumBatchPublishedLines_ = 0;
    editorWidthMaximumBatchTextUnits_ = 0;
    editorWidthScanLine_ = 0;
    editorWidthLogicalLineStart_ = 0;
    editorWidthLogicalLineStartOffset_ = 0;
    editorWidthLogicalLineLength_ = 0;
    editorWidthLogicalLineDisplayLines_ = 0;
    editorWidthLogicalLineText_.clear();
    editorWidthLogicalLineTextExact_ = true;
    editorWidthLogicalLineActive_ = false;
    editorWidthLogicalLineReady_ = false;
    editorWidthMaximumEditDisplayLineLength_ = 0;
    editorWidthUnknownLines_ = 0;
    editorLineWidths_.clear();
    measuredEditorLineWidths_.clear();
    editorWidthIndexNeedsRebuild_ = true;
    editorWidthIndexAllocationFailed_ = false;
    cachedWidestLineWidth_ = 0;
    widestLineCacheDirty_ = true;

    if (editor_ != nullptr && !wordWrap_) {
        const std::size_t lineCount = static_cast<std::size_t>(std::max<LRESULT>(
            1,
            SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
        try {
            editorLineWidths_.assign(lineCount, -1);
            editorWidthUnknownLines_ = lineCount;
            editorWidthIndexNeedsRebuild_ = false;
            if (mainWindow_ != nullptr) {
                SetTimer(mainWindow_, kEditorWidthIndexTimerId, kEditorWidthIndexDelayMs, nullptr);
            }
        } catch (const std::bad_alloc&) {
            editorLineWidths_.clear();
            editorWidthIndexAllocationFailed_ = true;
        }
    }
}

void ClassicNotepadApp::MeasureEditorWidthBatch()
{
    if (editor_ == nullptr || IsLargeFileMode() || wordWrap_ || editorWidthIndexAllocationFailed_) {
        if (mainWindow_ != nullptr) {
            KillTimer(mainWindow_, kEditorWidthIndexTimerId);
        }
        return;
    }
    if (editorWidthIndexNeedsRebuild_) {
        BeginEditorWidthIndexRebuild();
    }
    if (editorWidthUnknownLines_ == 0U) {
        if (mainWindow_ != nullptr) {
            KillTimer(mainWindow_, kEditorWidthIndexTimerId);
        }
        return;
    }

    BeginEditorViewportAnchorPreservation();
    ++editorWidthMeasurementBatches_;
    editorWidthLastBatchDiscoveredLines_ = 0;
    editorWidthLastBatchPublishedLines_ = 0;
    editorWidthLastBatchDiscoveredTextUnits_ = 0;
    editorWidthLastBatchMeasuredTextUnits_ = 0;
    std::size_t linesProcessed = 0;
    std::size_t textUnitsProcessed = 0;
    const int lineCount = std::max<int>(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    if (static_cast<std::size_t>(lineCount) != editorLineWidths_.size()) {
        InvalidateEditorWidthIndex();
        UpdateScrollBars();
        EndEditorViewportAnchorPreservation();
        return;
    }
    auto publishLineWidth = [&](std::size_t line, int width) {
        SetEditorLineWidth(line, width);
        ++editorWidthLastBatchPublishedLines_;
    };
    auto clearLogicalLine = [&]() {
        editorWidthLogicalLineActive_ = false;
        editorWidthLogicalLineReady_ = false;
        editorWidthLogicalLineLength_ = 0;
        editorWidthLogicalLineDisplayLines_ = 0;
        editorWidthLogicalLineText_.clear();
        editorWidthLogicalLineTextExact_ = true;
    };
    auto finishLogicalLine = [&]() -> bool {
        if (!editorWidthLogicalLineActive_ || !editorWidthLogicalLineReady_ ||
            editorWidthLogicalLineDisplayLines_ == 0U) {
            return false;
        }
        if (editorWidthLastBatchPublishedLines_ >= kEditorWidthIndexBatchLineLimit) {
            return false;
        }

        const int logicalLength = static_cast<int>(std::min<std::uint64_t>(
            editorWidthLogicalLineLength_, static_cast<std::uint64_t>((std::numeric_limits<int>::max)())));
        const bool canMeasureExactly = logicalLength <= kExactLineWidthMeasureCharLimit &&
            editorWidthLogicalLineDisplayLines_ <= kEditorWidthIndexBatchLineLimit &&
            editorWidthLogicalLineTextExact_ &&
            editorWidthLogicalLineText_.size() == static_cast<std::size_t>(logicalLength);
        bool conservative = !canMeasureExactly;
        int width = 0;
        if (!canMeasureExactly) {
            width = ConservativeEditorLineWidth(logicalLength);
            ++editorWidthConservativeLines_;
        } else {
            const std::size_t measuredLength = static_cast<std::size_t>(logicalLength);
            textUnitsProcessed += measuredLength;
            editorWidthMeasuredTextUnits_ += measuredLength;
            editorWidthLastBatchMeasuredTextUnits_ += measuredLength;
            width = MeasureEditorTextWidth(editorWidthLogicalLineText_, conservative);
            if (conservative) {
                width = ConservativeEditorLineWidth(logicalLength);
                ++editorWidthConservativeLines_;
            }
        }
        publishLineWidth(editorWidthLogicalLineStart_, width);
        editorWidthMeasuredLines_ += editorWidthLogicalLineDisplayLines_;
        clearLogicalLine();
        return true;
    };

    try {
        if (editorWidthLogicalLineReady_) {
            finishLogicalLine();
        }

        while (editorWidthScanLine_ < editorLineWidths_.size() &&
            linesProcessed < kEditorWidthIndexBatchLineLimit &&
            editorWidthLastBatchPublishedLines_ < kEditorWidthIndexBatchLineLimit &&
            textUnitsProcessed < kEditorWidthIndexBatchTextUnitLimit) {
            const std::size_t line = editorWidthScanLine_++;
            ++linesProcessed;
            if (editorLineWidths_[line] >= 0) {
                continue;
            }

            const LRESULT start = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(line), 0);
            if (start < 0) {
                publishLineWidth(line, 0);
                ++editorWidthMeasuredLines_;
                continue;
            }
            const int lineLength = std::max(0, static_cast<int>(SendMessageW(
                editor_, EM_LINELENGTH, static_cast<WPARAM>(start), 0)));
            editorWidthMaximumEditDisplayLineLength_ = std::max(
                editorWidthMaximumEditDisplayLineLength_, static_cast<std::uint64_t>(lineLength));

            const bool startingLogicalLine = !editorWidthLogicalLineActive_;
            if (startingLogicalLine) {
                const std::size_t reservedExactUnits = static_cast<std::size_t>(
                    kExactLineWidthMeasureCharLimit) * 2U;
                if (lineLength > 0 &&
                    textUnitsProcessed + reservedExactUnits > kEditorWidthIndexBatchTextUnitLimit &&
                    linesProcessed > 1U) {
                    --editorWidthScanLine_;
                    --linesProcessed;
                    break;
                }
                editorWidthLogicalLineStart_ = line;
                editorWidthLogicalLineStartOffset_ = static_cast<std::uint64_t>(start);
                editorWidthLogicalLineLength_ = 0;
                editorWidthLogicalLineDisplayLines_ = 0;
                editorWidthLogicalLineText_.clear();
                editorWidthLogicalLineTextExact_ = true;
                editorWidthLogicalLineActive_ = true;
            }
            if (line != editorWidthLogicalLineStart_ + editorWidthLogicalLineDisplayLines_ ||
                static_cast<std::uint64_t>(start) !=
                    editorWidthLogicalLineStartOffset_ + editorWidthLogicalLineLength_) {
                InvalidateEditorWidthIndex();
                UpdateScrollBars();
                EndEditorViewportAnchorPreservation();
                return;
            }

            const std::uint64_t nextLogicalLength = editorWidthLogicalLineLength_ +
                static_cast<std::uint64_t>(lineLength);
            const std::size_t nextDisplayLineCount = editorWidthLogicalLineDisplayLines_ + 1U;
            const bool exactCandidate = editorWidthLogicalLineTextExact_ &&
                nextLogicalLength <= static_cast<std::uint64_t>(kExactLineWidthMeasureCharLimit) &&
                nextDisplayLineCount <= kEditorWidthIndexBatchLineLimit;
            if (exactCandidate && lineLength > 0 &&
                textUnitsProcessed + static_cast<std::size_t>(lineLength) +
                    static_cast<std::size_t>(kExactLineWidthMeasureCharLimit) >
                    kEditorWidthIndexBatchTextUnitLimit) {
                --editorWidthScanLine_;
                --linesProcessed;
                if (startingLogicalLine) {
                    clearLogicalLine();
                }
                break;
            }
            if (exactCandidate) {
                const std::wstring segmentText = GetEditorLineText(static_cast<int>(line), lineLength);
                textUnitsProcessed += static_cast<std::size_t>(lineLength);
                editorWidthLastBatchDiscoveredTextUnits_ += static_cast<std::uint64_t>(lineLength);
                if (segmentText.size() == static_cast<std::size_t>(lineLength)) {
                    editorWidthLogicalLineText_ += segmentText;
                } else {
                    editorWidthLogicalLineTextExact_ = false;
                    editorWidthLogicalLineText_.clear();
                }
            } else {
                editorWidthLogicalLineTextExact_ = false;
                editorWidthLogicalLineText_.clear();
            }

            editorWidthLogicalLineLength_ += static_cast<std::uint64_t>(lineLength);
            ++editorWidthLogicalLineDisplayLines_;
            if (line != editorWidthLogicalLineStart_) {
                // Keep one unknown sentinel at the logical row's first display
                // line. The document fallback remains conservative as each
                // continuation is published within this batch's line budget.
                publishLineWidth(line, 0);
            }
            const std::size_t nextLine = line + 1U;
            const LRESULT nextStart = nextLine < editorLineWidths_.size()
                ? SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(nextLine), 0)
                : -1;
            const std::uint64_t currentEnd = static_cast<std::uint64_t>(start) +
                static_cast<std::uint64_t>(lineLength);
            if (nextLine >= editorLineWidths_.size() || nextStart < 0 ||
                static_cast<std::uint64_t>(nextStart) != currentEnd) {
                editorWidthLogicalLineReady_ = true;
                if (!finishLogicalLine()) {
                    break;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        InvalidateEditorWidthIndex();
        EndEditorViewportAnchorPreservation();
        return;
    }
    editorWidthLastBatchDiscoveredLines_ = linesProcessed;
    editorWidthMaximumBatchDiscoveredLines_ = std::max(
        editorWidthMaximumBatchDiscoveredLines_, static_cast<std::uint64_t>(linesProcessed));
    editorWidthMaximumBatchPublishedLines_ = std::max(
        editorWidthMaximumBatchPublishedLines_, editorWidthLastBatchPublishedLines_);
    editorWidthMaximumBatchTextUnits_ = std::max(
        editorWidthMaximumBatchTextUnits_,
        editorWidthLastBatchDiscoveredTextUnits_ + editorWidthLastBatchMeasuredTextUnits_);

    if (editorWidthUnknownLines_ == 0U) {
        if (mainWindow_ != nullptr) {
            KillTimer(mainWindow_, kEditorWidthIndexTimerId);
        }
    } else if (editorWidthScanLine_ >= editorLineWidths_.size()) {
        if (!editorWidthLogicalLineReady_) {
            // A text change that bypassed the subclass notification can change
            // line count. Reject this partial index and rebuild from the live EDIT.
            InvalidateEditorWidthIndex();
        } else if (mainWindow_ != nullptr) {
            // All display rows were discovered, but publication may have used
            // the final batch slot on a continuation placeholder. The next
            // batch must publish the logical line's first sentinel.
            SetTimer(mainWindow_, kEditorWidthIndexTimerId, kEditorWidthIndexDelayMs, nullptr);
        }
    }

    widestLineCacheDirty_ = true;
    UpdateScrollBars();
    EndEditorViewportAnchorPreservation();
}

void ClassicNotepadApp::InvalidateEditorWidthIndex()
{
    BeginEditorWidthIndexRebuild();
}

bool ClassicNotepadApp::IsEditorDisplayLineWrapped(int line, int lineCount) const
{
    if (editor_ == nullptr || line < 0 || line >= lineCount) {
        return false;
    }
    const LRESULT start = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(line), 0);
    if (start < 0) {
        return false;
    }
    const int length = std::max(0, static_cast<int>(SendMessageW(
        editor_, EM_LINELENGTH, static_cast<WPARAM>(start), 0)));
    const LRESULT end = start + length;
    if (line + 1 < lineCount) {
        const LRESULT nextStart = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(line + 1), 0);
        if (nextStart == end) {
            return true;
        }
    }
    if (line > 0) {
        const LRESULT previousStart = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(line - 1), 0);
        if (previousStart >= 0) {
            const int previousLength = std::max(0, static_cast<int>(SendMessageW(
                editor_, EM_LINELENGTH, static_cast<WPARAM>(previousStart), 0)));
            if (previousStart + previousLength == start) {
                return true;
            }
        }
    }
    return false;
}

void ClassicNotepadApp::BeginEditorMutationTracking(UINT message, WPARAM wParam)
{
    if (editorWidthIndexMutationActive_) {
        return;
    }
    bool wholeDocument = message == WM_SETTEXT || message == WM_UNDO || message == EM_UNDO;
    const bool likelyMutation = wholeDocument || message == WM_CHAR || message == WM_CUT || message == WM_CLEAR ||
        message == WM_PASTE || message == EM_REPLACESEL ||
        (message == WM_KEYDOWN && (wParam == VK_DELETE || wParam == VK_BACK));
    if (!likelyMutation) {
        return;
    }

    if ((message == WM_CHAR && (wParam == 26U || wParam == 25U)) ||
        (message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (wParam == VK_DELETE || wParam == VK_BACK))) {
        wholeDocument = true;
    }

    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);
    if (selectionEnd < selectionStart) {
        std::swap(selectionStart, selectionEnd);
    }
    int lineCount = std::max<int>(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    int startLine = std::clamp(static_cast<int>(SendMessageW(
        editor_, EM_LINEFROMCHAR, selectionStart, 0)), 0, lineCount - 1);
    int endLine = std::clamp(static_cast<int>(SendMessageW(
        editor_, EM_LINEFROMCHAR, selectionEnd, 0)), startLine, lineCount - 1);

    if (selectionStart == selectionEnd && message == WM_CHAR && wParam == 8U && startLine > 0) {
        --startLine;
    } else if (selectionStart == selectionEnd && message == WM_KEYDOWN && wParam == VK_DELETE) {
        const LRESULT lineStart = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(startLine), 0);
        const int lineLength = lineStart < 0 ? 0 : static_cast<int>(SendMessageW(
            editor_, EM_LINELENGTH, static_cast<WPARAM>(lineStart), 0));
        if (static_cast<int>(selectionStart) >= static_cast<int>(lineStart) + lineLength && startLine + 1 < lineCount) {
            ++endLine;
        }
    }

    if (static_cast<std::size_t>(endLine - startLine + 1) > kEditorWidthIndexLocalLineLimit) {
        wholeDocument = true;
    }
    if (!wholeDocument) {
        for (int line = startLine; line <= endLine; ++line) {
            if (IsEditorDisplayLineWrapped(line, lineCount)) {
                wholeDocument = true;
                break;
            }
        }
    }

    editorWidthIndexMutationActive_ = true;
    editorWidthIndexMutationHandled_ = false;
    editorWidthIndexMutationWholeDocument_ = wholeDocument;
    editorWidthIndexMutationStartLine_ = startLine;
    editorWidthIndexMutationEndLine_ = endLine;
    editorWidthIndexMutationGeneration_ = editorWidthGeneration_;
}

void ClassicNotepadApp::EndEditorMutationTracking()
{
    editorWidthIndexMutationActive_ = false;
    editorWidthIndexMutationHandled_ = false;
    editorWidthIndexMutationWholeDocument_ = false;
}

void ClassicNotepadApp::UpdateEditorWidthIndexForMutation()
{
    if (editorWidthIndexMutationHandled_) {
        return;
    }
    editorWidthIndexMutationHandled_ = true;
    if (wordWrap_) {
        editorWidthIndexNeedsRebuild_ = true;
        return;
    }
    if (!editorWidthIndexMutationActive_ || editorWidthIndexMutationWholeDocument_ ||
        editorWidthIndexMutationGeneration_ != editorWidthGeneration_ ||
        editorWidthIndexNeedsRebuild_ || editorWidthIndexAllocationFailed_) {
        InvalidateEditorWidthIndex();
        return;
    }

    const int lineCount = std::max<int>(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    const int startLine = std::clamp(editorWidthIndexMutationStartLine_, 0, lineCount - 1);
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);
    const int endLine = std::clamp(static_cast<int>(SendMessageW(
        editor_, EM_LINEFROMCHAR, static_cast<WPARAM>(std::max(selectionStart, selectionEnd)), 0)), startLine, lineCount - 1);
    const int oldStartLine = std::clamp(editorWidthIndexMutationStartLine_, 0,
        std::max(0, static_cast<int>(editorLineWidths_.size()) - 1));
    const int oldEndLine = std::clamp(editorWidthIndexMutationEndLine_, oldStartLine,
        std::max(oldStartLine, static_cast<int>(editorLineWidths_.size()) - 1));
    const std::size_t oldCount = static_cast<std::size_t>(oldEndLine - oldStartLine + 1);
    const std::size_t newCount = static_cast<std::size_t>(endLine - startLine + 1);
    if (oldCount > kEditorWidthIndexLocalLineLimit || newCount > kEditorWidthIndexLocalLineLimit ||
        editorLineWidths_.size() < oldCount + static_cast<std::size_t>(oldStartLine) ||
        editorLineWidths_.size() - oldCount + newCount != static_cast<std::size_t>(lineCount)) {
        InvalidateEditorWidthIndex();
        return;
    }
    for (int line = startLine; line <= endLine; ++line) {
        if (IsEditorDisplayLineWrapped(line, lineCount)) {
            InvalidateEditorWidthIndex();
            return;
        }
    }

    std::vector<int> replacementWidths;
    try {
        replacementWidths.assign(newCount, -1);
        std::size_t localUnits = 0;
        for (std::size_t offset = 0; offset < newCount; ++offset) {
            const int line = startLine + static_cast<int>(offset);
            const LRESULT lineStart = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(line), 0);
            const int lineLength = lineStart < 0 ? 0 : std::max(0, static_cast<int>(SendMessageW(
                editor_, EM_LINELENGTH, static_cast<WPARAM>(lineStart), 0)));
            if (lineLength > kExactLineWidthMeasureCharLimit) {
                bool conservative = false;
                replacementWidths[offset] = MeasureEditorLineWidth(line, lineLength, conservative);
                if (conservative) {
                    ++editorWidthConservativeLines_;
                }
                ++editorWidthLocalEditMeasurements_;
                continue;
            }
            if (localUnits + static_cast<std::size_t>(lineLength) > kEditorWidthIndexLocalTextUnitLimit) {
                replacementWidths[offset] = ConservativeEditorLineWidth(lineLength);
                ++editorWidthConservativeLines_;
                ++editorWidthLocalEditMeasurements_;
                continue;
            }
            bool conservative = false;
            replacementWidths[offset] = MeasureEditorLineWidth(line, lineLength, conservative);
            ++editorWidthLocalEditMeasurements_;
            localUnits += static_cast<std::size_t>(lineLength);
            editorWidthLocalEditTextUnits_ += static_cast<std::uint64_t>(lineLength);
            if (conservative) {
                ++editorWidthConservativeLines_;
            }
        }

        const std::size_t oldScanLine = editorWidthScanLine_;
        for (std::size_t offset = 0; offset < oldCount; ++offset) {
            const int width = editorLineWidths_[static_cast<std::size_t>(oldStartLine) + offset];
            if (width >= 0) {
                const auto found = measuredEditorLineWidths_.find(width);
                if (found != measuredEditorLineWidths_.end()) {
                    measuredEditorLineWidths_.erase(found);
                }
            } else if (editorWidthUnknownLines_ > 0U) {
                --editorWidthUnknownLines_;
            }
        }
        auto eraseBegin = editorLineWidths_.begin() + oldStartLine;
        auto eraseEnd = eraseBegin + static_cast<std::ptrdiff_t>(oldCount);
        eraseBegin = editorLineWidths_.erase(eraseBegin, eraseEnd);
        editorLineWidths_.insert(eraseBegin, replacementWidths.begin(), replacementWidths.end());
        for (const int width : replacementWidths) {
            if (width >= 0) {
                measuredEditorLineWidths_.insert(width);
            } else {
                ++editorWidthUnknownLines_;
            }
        }
        const std::size_t oldStart = static_cast<std::size_t>(oldStartLine);
        const std::size_t oldEnd = oldStart + oldCount;
        if (oldScanLine > oldEnd) {
            editorWidthScanLine_ = oldScanLine - oldCount + newCount;
        } else if (oldScanLine >= oldStart) {
            editorWidthScanLine_ = oldStart + newCount;
        }
        editorWidthIndexNeedsRebuild_ = false;
        editorWidthIndexAllocationFailed_ = false;
        if (editorWidthUnknownLines_ == 0U) {
            if (mainWindow_ != nullptr) {
                KillTimer(mainWindow_, kEditorWidthIndexTimerId);
            }
        } else if (mainWindow_ != nullptr) {
            SetTimer(mainWindow_, kEditorWidthIndexTimerId, kEditorWidthIndexDelayMs, nullptr);
        }
        widestLineCacheDirty_ = true;
    } catch (const std::bad_alloc&) {
        InvalidateEditorWidthIndex();
    }
}

bool ClassicNotepadApp::NeedsVerticalScrollBar(int availableHeight) const
{
    if (editor_ == nullptr || availableHeight <= 0) {
        return false;
    }

    const int lineCount = std::max<int>(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    return static_cast<long long>(lineCount) * GetEditorLineHeight() > availableHeight;
}

bool ClassicNotepadApp::NeedsHorizontalScrollBar(int availableWidth) const
{
    if (editor_ == nullptr || availableWidth <= 0) {
        return false;
    }

    return MeasureWidestEditorLine() > std::max(0, availableWidth - 8);
}

void ClassicNotepadApp::SetEditorScrollBarVisibility(bool verticalVisible, bool horizontalVisible)
{
    if (editor_ == nullptr) {
        verticalScrollBarVisible_ = verticalVisible;
        horizontalScrollBarVisible_ = horizontalVisible;
        return;
    }

    horizontalVisible = horizontalVisible && !wordWrap_;
    const bool verticalChanged = verticalScrollBarVisible_ != verticalVisible;
    const bool horizontalChanged = horizontalScrollBarVisible_ != horizontalVisible;

    // ShowScrollBar can synchronously send WM_SIZE back through the edit callback.
    // Store the requested state first so a re-entrant UpdateScrollBars call sees
    // the new truth instead of repeatedly asking USER32 for the same transition.
    verticalScrollBarVisible_ = verticalVisible;
    horizontalScrollBarVisible_ = horizontalVisible;

    const bool useCustomScrollBars = UseCustomScrollBars();
    const bool nativeVerticalVisible = !useCustomScrollBars && verticalVisible;
    const bool nativeHorizontalVisible = !useCustomScrollBars && horizontalVisible;

    SetNativeEditorScrollBarVisibility(nativeVerticalVisible, nativeHorizontalVisible);

    if (verticalChanged && !verticalVisible) {
        const int firstVisibleLine = static_cast<int>(SendMessageW(editor_, EM_GETFIRSTVISIBLELINE, 0, 0));
        if (firstVisibleLine > 0) {
            SendMessageW(editor_, EM_LINESCROLL, 0, -firstVisibleLine);
        }
    }

    if (useCustomScrollBars && horizontalVisible) {
        const ScrollBarMetrics horizontalMetrics = GetCustomScrollBarMetrics(ScrollBarOrientation::Horizontal);
        const int previousPosition = horizontalScrollPosition_;
        horizontalScrollPosition_ = horizontalMetrics.position;
        const int delta = horizontalScrollPosition_ - previousPosition;
        if (delta != 0) {
            SendMessageW(editor_, EM_LINESCROLL, delta, 0);
        }
    } else if (horizontalVisible) {
        const int unit = std::max(1, GetEditorHorizontalScrollUnit());
        RECT clientRect{};
        GetClientRect(editor_, &clientRect);
        const int viewportUnits = std::max(1, std::max(0,
            static_cast<int>(clientRect.right - clientRect.left) - 8) / unit);
        const int contentWidth = MeasureWidestEditorLine();
        const int contentUnits = contentWidth / unit + ((contentWidth % unit) != 0 ? 1 : 0);
        const int maximumPosition = std::max(0, contentUnits - viewportUnits);
        SCROLLINFO scrollInfo{};
        scrollInfo.cbSize = sizeof(scrollInfo);
        scrollInfo.fMask = SIF_POS;
        if (GetScrollInfo(editor_, SB_HORZ, &scrollInfo) && scrollInfo.nPos > maximumPosition) {
            SendMessageW(editor_, EM_LINESCROLL, maximumPosition - scrollInfo.nPos, 0);
        }
    }

    if (horizontalChanged && !horizontalVisible && !wordWrap_) {
        RECT clientRect{};
        GetClientRect(editor_, &clientRect);
        const int viewportWidth = std::max(0, static_cast<int>(clientRect.right - clientRect.left) - 8);
        if (MeasureWidestEditorLine() <= viewportWidth) {
            if (useCustomScrollBars) {
                const int oldPosition = horizontalScrollPosition_;
                horizontalScrollPosition_ = 0;
                if (oldPosition != 0) {
                    SendMessageW(editor_, EM_LINESCROLL, -oldPosition, 0);
                }
            } else {
                SendMessageW(editor_, WM_HSCROLL, SB_LEFT, 0);
            }
        }
    }

    if (useCustomScrollBars) {
        const RECT verticalThumb = verticalVisible ? GetCustomScrollBarThumbRect(ScrollBarOrientation::Vertical) : RECT{};
        const RECT horizontalThumb = horizontalVisible ? GetCustomScrollBarThumbRect(ScrollBarOrientation::Horizontal) : RECT{};

        if (verticalScrollBar_ != nullptr &&
            (lastVerticalScrollBarPainted_ != verticalVisible ||
                !EqualRect(&lastVerticalScrollBarThumb_, &verticalThumb))) {
            InvalidateRect(verticalScrollBar_, nullptr, FALSE);
        }
        if (horizontalScrollBar_ != nullptr &&
            (lastHorizontalScrollBarPainted_ != horizontalVisible ||
                !EqualRect(&lastHorizontalScrollBarThumb_, &horizontalThumb))) {
            InvalidateRect(horizontalScrollBar_, nullptr, FALSE);
        }
        if (scrollCorner_ != nullptr && (verticalChanged || horizontalChanged)) {
            InvalidateRect(scrollCorner_, nullptr, FALSE);
        }

        lastVerticalScrollBarThumb_ = verticalThumb;
        lastHorizontalScrollBarThumb_ = horizontalThumb;
        lastVerticalScrollBarPainted_ = verticalVisible;
        lastHorizontalScrollBarPainted_ = horizontalVisible;
    } else {
        lastVerticalScrollBarThumb_ = RECT{};
        lastHorizontalScrollBarThumb_ = RECT{};
        lastVerticalScrollBarPainted_ = false;
        lastHorizontalScrollBarPainted_ = false;
    }
}

void ClassicNotepadApp::SetNativeEditorScrollBarVisibility(bool verticalVisible, bool horizontalVisible)
{
    if (editor_ == nullptr) {
        nativeVerticalScrollBarVisible_ = verticalVisible;
        nativeHorizontalScrollBarVisible_ = horizontalVisible;
        return;
    }

    if (nativeVerticalScrollBarVisible_ != verticalVisible) {
        nativeVerticalScrollBarVisible_ = verticalVisible;
        ShowScrollBar(editor_, SB_VERT, verticalVisible ? TRUE : FALSE);
    }

    if (nativeHorizontalScrollBarVisible_ != horizontalVisible) {
        nativeHorizontalScrollBarVisible_ = horizontalVisible;
        ShowScrollBar(editor_, SB_HORZ, horizontalVisible ? TRUE : FALSE);
    }
}

ClassicNotepadApp::ScrollBarMetrics ClassicNotepadApp::GetCustomScrollBarMetrics(ScrollBarOrientation orientation) const
{
    ScrollBarMetrics metrics{};
    if (editor_ == nullptr) {
        return metrics;
    }

    RECT clientRect{};
    GetClientRect(editor_, &clientRect);

    if (orientation == ScrollBarOrientation::Vertical) {
        const int lineHeight = GetEditorLineHeight();
        const int clientHeight = std::max(0, static_cast<int>(clientRect.bottom - clientRect.top));
        const int lineCount = std::max<int>(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
        metrics.minimum = 0;
        metrics.maximum = std::max(0, lineCount - 1);
        metrics.page = std::max(1, clientHeight / lineHeight);
        const int maxPosition = std::max(metrics.minimum, metrics.maximum - metrics.page + 1);
        const int firstVisibleLine = static_cast<int>(SendMessageW(editor_, EM_GETFIRSTVISIBLELINE, 0, 0));
        metrics.position = std::clamp(firstVisibleLine, metrics.minimum, maxPosition);
        return metrics;
    }

    const int unit = GetEditorHorizontalScrollUnit();
    const int clientWidth = std::max(0, static_cast<int>(clientRect.right - clientRect.left));
    const int widestLine = MeasureWidestEditorLine();
    const int viewportWidth = std::max(0, clientWidth - 8);
    const int rangeUnits = std::max(1, widestLine / unit + ((widestLine % unit) != 0 ? 1 : 0));
    metrics.minimum = 0;
    metrics.maximum = std::max(0, rangeUnits - 1);
    metrics.page = std::max(1, viewportWidth / unit);
    const int maxPosition = std::max(metrics.minimum, metrics.maximum - metrics.page + 1);
    metrics.position = std::clamp(horizontalScrollPosition_, metrics.minimum, maxPosition);
    return metrics;
}

int ClassicNotepadApp::GetEditorHorizontalScrollUnit() const
{
    if (editor_ == nullptr) {
        return 1;
    }

    HDC deviceContext = GetDC(editor_);
    if (deviceContext == nullptr) {
        return 1;
    }

    HGDIOBJ previousFont = nullptr;
    if (editorFont_ != nullptr) {
        previousFont = SelectObject(deviceContext, editorFont_);
    }

    TEXTMETRICW textMetrics{};
    const bool measured = GetTextMetricsW(deviceContext, &textMetrics) != 0;

    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }
    ReleaseDC(editor_, deviceContext);

    if (!measured) {
        return 1;
    }

    return std::max(1, static_cast<int>(textMetrics.tmAveCharWidth));
}

int ClassicNotepadApp::GetHorizontalAnchorPixels() const
{
    if (editor_ == nullptr) {
        return 0;
    }
    const int unit = std::max(1, GetEditorHorizontalScrollUnit());
    int nativeMaximumPixels = -1;
    const int nativePositionPixels = GetNativeEditorHorizontalPosition(&nativeMaximumPixels);
    if (nativeMaximumPixels >= 0) {
        return std::max(0, nativePositionPixels);
    }
    if (horizontalScrollPosition_ <= 0) {
        return 0;
    }
    return horizontalScrollPosition_ > (std::numeric_limits<int>::max)() / unit
        ? (std::numeric_limits<int>::max)()
        : horizontalScrollPosition_ * unit;
}

int ClassicNotepadApp::GetNativeEditorHorizontalPosition(int* maximumPosition) const
{
    if (maximumPosition != nullptr) {
        *maximumPosition = -1;
    }
    if (editor_ == nullptr) {
        return 0;
    }

    SCROLLINFO scrollInfo{};
    scrollInfo.cbSize = sizeof(scrollInfo);
    scrollInfo.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (!GetScrollInfo(editor_, SB_HORZ, &scrollInfo)) {
        return horizontalScrollPosition_;
    }

    const long long rangeMaximum = static_cast<long long>(scrollInfo.nMax) - scrollInfo.nMin -
        static_cast<long long>(std::max<UINT>(1U, scrollInfo.nPage)) + 1LL;
    const int boundedMaximum = static_cast<int>(std::clamp(
        rangeMaximum,
        0LL,
        static_cast<long long>((std::numeric_limits<int>::max)())));
    if (maximumPosition != nullptr &&
        (scrollInfo.nMax > scrollInfo.nMin || scrollInfo.nPos > 0 || scrollInfo.nPage > 0)) {
        *maximumPosition = boundedMaximum;
    }
    if (scrollInfo.nMax > scrollInfo.nMin || scrollInfo.nPos > 0 || scrollInfo.nPage > 0) {
        return std::max(0, scrollInfo.nPos);
    }
    return horizontalScrollPosition_;
}

void ClassicNotepadApp::BeginEditorViewportAnchorPreservation()
{
    if (editorViewportAnchorDepth_++ != 0U) {
        return;
    }

    editorViewportAnchor_ = {};
    if (editor_ == nullptr || IsLargeFileMode()) {
        return;
    }

    const int unit = std::max(1, GetEditorHorizontalScrollUnit());
    int horizontalMaximumPixels = -1;
    const int horizontalPositionPixels = GetNativeEditorHorizontalPosition(&horizontalMaximumPixels);
    const int horizontalPixels = horizontalMaximumPixels >= 0
        ? std::max(0, horizontalPositionPixels)
        : GetHorizontalAnchorPixels();

    const int lineCount = std::max(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    const int firstVisibleLine = std::clamp(
        static_cast<int>(SendMessageW(editor_, EM_GETFIRSTVISIBLELINE, 0, 0)),
        0,
        lineCount - 1);
    const LRESULT topLineStart = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(firstVisibleLine), 0);
    const int textLength = std::max(0, GetWindowTextLengthW(editor_));

    editorViewportAnchor_.horizontalPixels = horizontalPixels;
    editorViewportAnchor_.verticalContentPosition = topLineStart < 0
        ? 0
        : std::clamp(static_cast<int>(topLineStart), 0, textLength);
    editorViewportAnchor_.valid = true;
}

void ClassicNotepadApp::EndEditorViewportAnchorPreservation()
{
    if (editorViewportAnchorDepth_ == 0U) {
        return;
    }
    --editorViewportAnchorDepth_;
    if (editorViewportAnchorDepth_ != 0U) {
        return;
    }

    const EditorViewportAnchor anchor = editorViewportAnchor_;
    editorViewportAnchor_ = {};
    if (anchor.valid) {
        RestoreEditorViewportAnchor(anchor);
    }
}

void ClassicNotepadApp::RestoreEditorViewportAnchor(const EditorViewportAnchor& anchor)
{
    if (editor_ == nullptr || IsLargeFileMode() || !anchor.valid) {
        return;
    }

    const int unit = std::max(1, GetEditorHorizontalScrollUnit());
    const int wantedHorizontalPosition = std::max(0, anchor.horizontalPixels) / unit;
    int reachableHorizontalMaximumPixels = -1;
    const int currentHorizontalPositionPixels = GetNativeEditorHorizontalPosition(&reachableHorizontalMaximumPixels);
    int currentHorizontalPosition = reachableHorizontalMaximumPixels >= 0
        ? std::max(0, currentHorizontalPositionPixels) / unit
        : std::max(0, horizontalScrollPosition_);
    int reachableHorizontalMaximum = reachableHorizontalMaximumPixels >= 0
        ? reachableHorizontalMaximumPixels / unit
        : -1;
    if (reachableHorizontalMaximum < 0 && UseCustomScrollBars()) {
        const ScrollBarMetrics metrics = GetCustomScrollBarMetrics(ScrollBarOrientation::Horizontal);
        reachableHorizontalMaximum = std::max(metrics.minimum, metrics.maximum - metrics.page + 1);
    }
    if (reachableHorizontalMaximum < 0) {
        const int contentWidth = MeasureWidestEditorLine();
        RECT clientRect{};
        GetClientRect(editor_, &clientRect);
        const int viewportWidth = std::max(0, static_cast<int>(clientRect.right - clientRect.left) - 8);
        const int contentUnits = contentWidth / unit + ((contentWidth % unit) != 0 ? 1 : 0);
        reachableHorizontalMaximum = std::max(0, contentUnits - std::max(1, viewportWidth / unit));
    }
    const int targetHorizontalPosition = std::clamp(
        wantedHorizontalPosition,
        0,
        std::max(0, reachableHorizontalMaximum));
    const int horizontalDelta = targetHorizontalPosition - currentHorizontalPosition;
    if (horizontalDelta != 0) {
        SendMessageW(editor_, EM_LINESCROLL, horizontalDelta, 0);
    }

    int actualHorizontalMaximumPixels = -1;
    const int actualHorizontalPositionPixels = GetNativeEditorHorizontalPosition(&actualHorizontalMaximumPixels);
    horizontalScrollPosition_ = actualHorizontalMaximumPixels >= 0
        ? std::max(0, actualHorizontalPositionPixels) / unit
        : targetHorizontalPosition;

    const int lineCount = std::max(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    const int textLength = std::max(0, GetWindowTextLengthW(editor_));
    const int anchorCharacter = std::clamp(anchor.verticalContentPosition, 0, textLength);
    int targetVerticalLine = static_cast<int>(SendMessageW(
        editor_, EM_LINEFROMCHAR, static_cast<WPARAM>(anchorCharacter), 0));
    targetVerticalLine = std::clamp(targetVerticalLine, 0, lineCount - 1);

    RECT clientRect{};
    GetClientRect(editor_, &clientRect);
    const int pageLines = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top) /
        std::max(1, GetEditorLineHeight()));
    const int maximumVerticalPosition = std::max(0, lineCount - pageLines);
    targetVerticalLine = std::clamp(targetVerticalLine, 0, maximumVerticalPosition);
    const int currentVerticalLine = std::max(0, static_cast<int>(SendMessageW(
        editor_, EM_GETFIRSTVISIBLELINE, 0, 0)));
    const int verticalDelta = targetVerticalLine - currentVerticalLine;
    if (verticalDelta != 0) {
        SendMessageW(editor_, EM_LINESCROLL, 0, verticalDelta);
    }
}

RECT ClassicNotepadApp::GetCustomScrollBarThumbRect(ScrollBarOrientation orientation) const
{
    HWND scrollBar = orientation == ScrollBarOrientation::Vertical ? verticalScrollBar_ : horizontalScrollBar_;
    RECT clientRect{};
    if (scrollBar == nullptr) {
        return clientRect;
    }

    GetClientRect(scrollBar, &clientRect);
    if (IsRectEmpty(&clientRect)) {
        return clientRect;
    }

    const ScrollBarMetrics metrics = GetCustomScrollBarMetrics(orientation);
    const int minimum = metrics.minimum;
    const int maximum = metrics.maximum;
    const int page = std::max(1, metrics.page);
    const int range = std::max(1, maximum - minimum + 1);
    const int maxPosition = std::max(minimum, maximum - page + 1);
    const int position = std::clamp(metrics.position, minimum, maxPosition);
    if (maxPosition <= minimum) {
        return RECT{};
    }

    const int trackLength = orientation == ScrollBarOrientation::Vertical
        ? std::max(0, static_cast<int>(clientRect.bottom - clientRect.top))
        : std::max(0, static_cast<int>(clientRect.right - clientRect.left));
    if (trackLength <= 0) {
        return clientRect;
    }

    int thumbLength = MulDiv(trackLength, std::min(page, range), range);
    thumbLength = std::clamp(thumbLength, std::min(trackLength, kMinimumScrollThumbLength), trackLength);

    const int movableLength = std::max(0, trackLength - thumbLength);
    const int scrollableRange = std::max(1, maxPosition - minimum);
    const int thumbOffset = maxPosition > minimum
        ? MulDiv(position - minimum, movableLength, scrollableRange)
        : 0;

    RECT thumbRect = clientRect;
    if (orientation == ScrollBarOrientation::Vertical) {
        thumbRect.top = clientRect.top + thumbOffset;
        thumbRect.bottom = thumbRect.top + thumbLength;
        InflateRect(&thumbRect, -3, -2);
    } else {
        thumbRect.left = clientRect.left + thumbOffset;
        thumbRect.right = thumbRect.left + thumbLength;
        InflateRect(&thumbRect, -2, -3);
    }

    return thumbRect;
}

void ClassicNotepadApp::PaintCustomScrollBar(HWND scrollBar, HDC deviceContext) const
{
    if (scrollBar == nullptr || deviceContext == nullptr) {
        return;
    }

    RECT clientRect{};
    GetClientRect(scrollBar, &clientRect);
    HBRUSH trackBrush = CreateSolidBrush(kDarkScrollTrack);
    if (trackBrush != nullptr) {
        FillRect(deviceContext, &clientRect, trackBrush);
        DeleteObject(trackBrush);
    }

    if (scrollBar == scrollCorner_) {
        return;
    }

    const ScrollBarOrientation orientation = scrollBar == verticalScrollBar_
        ? ScrollBarOrientation::Vertical
        : ScrollBarOrientation::Horizontal;
    RECT thumbRect = GetCustomScrollBarThumbRect(orientation);
    if (IsRectEmpty(&thumbRect)) {
        return;
    }

    HBRUSH thumbBrush = CreateSolidBrush(customScrollBarDragging_ && activeScrollBar_ == orientation
        ? kDarkScrollThumbActive
        : kDarkScrollThumb);
    if (thumbBrush != nullptr) {
        FillRect(deviceContext, &thumbRect, thumbBrush);
        DeleteObject(thumbBrush);
    }
}

void ClassicNotepadApp::ScrollCustomScrollBarByPage(ScrollBarOrientation orientation, bool forward)
{
    if (editor_ == nullptr) {
        return;
    }

    const ScrollBarMetrics metrics = GetCustomScrollBarMetrics(orientation);
    const int maxPosition = std::max(metrics.minimum, metrics.maximum - metrics.page + 1);
    const int delta = std::max(1, metrics.page);
    const int target = metrics.position + (forward ? delta : -delta);
    ScrollCustomScrollBarToPosition(orientation, std::clamp(target, metrics.minimum, maxPosition));
}

void ClassicNotepadApp::ScrollCustomScrollBarToPosition(ScrollBarOrientation orientation, int targetPosition)
{
    if (editor_ == nullptr) {
        return;
    }

    const ScrollBarMetrics metrics = GetCustomScrollBarMetrics(orientation);
    const int minimum = metrics.minimum;
    const int maximum = metrics.maximum;
    const int page = std::max(1, metrics.page);
    const int maxPosition = std::max(minimum, maximum - page + 1);
    const int target = std::clamp(targetPosition, minimum, maxPosition);
    const int delta = target - metrics.position;
    if (delta == 0) {
        return;
    }

    if (orientation == ScrollBarOrientation::Vertical) {
        SendMessageW(editor_, EM_LINESCROLL, 0, delta);
    } else {
        horizontalScrollPosition_ = target;
        SendMessageW(editor_, EM_LINESCROLL, delta, 0);
        int nativeMaximumPixels = -1;
        const int actualPositionPixels = GetNativeEditorHorizontalPosition(&nativeMaximumPixels);
        if (nativeMaximumPixels >= 0) {
            horizontalScrollPosition_ = std::max(0, actualPositionPixels) /
                std::max(1, GetEditorHorizontalScrollUnit());
        }
    }

    UpdateStatusBar();
    UpdateScrollBars();
    AdvanceSpellingLayoutRevision();
    RefreshSpellCheck(false);
    InvalidateRect(editor_, nullptr, FALSE);
}

bool ClassicNotepadApp::HandleMouseWheel(WPARAM wParam)
{
    if (editor_ == nullptr || !UseCustomScrollBars()) {
        return false;
    }

    const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
    if (wheelDelta == 0) {
        return true;
    }

    UINT scrollLines = 3;
    if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &scrollLines, 0)) {
        scrollLines = 3;
    }

    if (scrollLines == 0U) {
        mouseWheelRemainder_ = 0;
        return true;
    }

    const ScrollBarMetrics metrics = GetCustomScrollBarMetrics(ScrollBarOrientation::Vertical);
    const int page = std::max(1, metrics.page);
    const int maxPosition = std::max(metrics.minimum, metrics.maximum - page + 1);
    if (maxPosition <= metrics.minimum) {
        mouseWheelRemainder_ = 0;
        return true;
    }

    if (scrollLines == WHEEL_PAGESCROLL) {
        const int wheelUnits = mouseWheelRemainder_ + wheelDelta;
        const int pages = wheelUnits / WHEEL_DELTA;
        mouseWheelRemainder_ = wheelUnits % WHEEL_DELTA;
        if (pages != 0) {
            const long long target =
                static_cast<long long>(metrics.position) - static_cast<long long>(pages) * page;
            ScrollCustomScrollBarToPosition(
                ScrollBarOrientation::Vertical,
                static_cast<int>(std::clamp(
                    target,
                    static_cast<long long>(std::numeric_limits<int>::min()),
                    static_cast<long long>(std::numeric_limits<int>::max()))));
        }
        return true;
    }

    const int linesPerWheelDelta = static_cast<int>(std::min<UINT>(scrollLines, 1000U));
    const int wheelUnits = mouseWheelRemainder_ + wheelDelta * linesPerWheelDelta;
    const int lines = wheelUnits / WHEEL_DELTA;
    mouseWheelRemainder_ = wheelUnits % WHEEL_DELTA;
    if (lines != 0) {
        ScrollCustomScrollBarToPosition(ScrollBarOrientation::Vertical, metrics.position - lines);
    }

    return true;
}

void ClassicNotepadApp::BeginCustomScrollBarInteraction(HWND scrollBar, LPARAM lParam)
{
    if (scrollBar != verticalScrollBar_ && scrollBar != horizontalScrollBar_) {
        return;
    }

    const ScrollBarOrientation orientation = scrollBar == verticalScrollBar_
        ? ScrollBarOrientation::Vertical
        : ScrollBarOrientation::Horizontal;
    const POINT point{
        static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
        static_cast<LONG>(static_cast<short>(HIWORD(lParam)))
    };
    const RECT thumbRect = GetCustomScrollBarThumbRect(orientation);
    if (!IsRectEmpty(&thumbRect) && PtInRect(&thumbRect, point)) {
        const ScrollBarMetrics metrics = GetCustomScrollBarMetrics(orientation);
        customScrollBarDragging_ = true;
        activeScrollBar_ = orientation;
        scrollBarDragStartMouse_ = orientation == ScrollBarOrientation::Vertical ? point.y : point.x;
        scrollBarDragStartPosition_ = metrics.position;
        SetCapture(scrollBar);
        InvalidateRect(scrollBar, nullptr, FALSE);
        return;
    }

    const int coordinate = orientation == ScrollBarOrientation::Vertical ? point.y : point.x;
    const int thumbStart = orientation == ScrollBarOrientation::Vertical ? thumbRect.top : thumbRect.left;
    ScrollCustomScrollBarByPage(orientation, coordinate > thumbStart);
}

void ClassicNotepadApp::UpdateCustomScrollBarDrag(HWND scrollBar, LPARAM lParam)
{
    if (!customScrollBarDragging_ ||
        (activeScrollBar_ == ScrollBarOrientation::Vertical && scrollBar != verticalScrollBar_) ||
        (activeScrollBar_ == ScrollBarOrientation::Horizontal && scrollBar != horizontalScrollBar_)) {
        return;
    }

    RECT clientRect{};
    GetClientRect(scrollBar, &clientRect);
    const int trackLength = activeScrollBar_ == ScrollBarOrientation::Vertical
        ? std::max(0, static_cast<int>(clientRect.bottom - clientRect.top))
        : std::max(0, static_cast<int>(clientRect.right - clientRect.left));

    RECT thumbRect = GetCustomScrollBarThumbRect(activeScrollBar_);
    const int thumbLength = activeScrollBar_ == ScrollBarOrientation::Vertical
        ? std::max(1, static_cast<int>(thumbRect.bottom - thumbRect.top))
        : std::max(1, static_cast<int>(thumbRect.right - thumbRect.left));
    const int movableLength = std::max(1, trackLength - thumbLength);

    const ScrollBarMetrics metrics = GetCustomScrollBarMetrics(activeScrollBar_);
    const int minimum = metrics.minimum;
    const int maximum = metrics.maximum;
    const int page = std::max(1, metrics.page);
    const int maxPosition = std::max(minimum, maximum - page + 1);
    const int scrollableRange = std::max(1, maxPosition - minimum);

    const int mousePosition = activeScrollBar_ == ScrollBarOrientation::Vertical
        ? static_cast<int>(static_cast<short>(HIWORD(lParam)))
        : static_cast<int>(static_cast<short>(LOWORD(lParam)));
    const int mouseDelta = mousePosition - scrollBarDragStartMouse_;
    const int positionDelta = MulDiv(mouseDelta, scrollableRange, movableLength);
    ScrollCustomScrollBarToPosition(activeScrollBar_, scrollBarDragStartPosition_ + positionDelta);
}

void ClassicNotepadApp::EndCustomScrollBarDrag(HWND scrollBar)
{
    if (!customScrollBarDragging_) {
        return;
    }

    customScrollBarDragging_ = false;
    if (GetCapture() == scrollBar) {
        ReleaseCapture();
    }

    if (verticalScrollBar_ != nullptr) {
        InvalidateRect(verticalScrollBar_, nullptr, FALSE);
    }
    if (horizontalScrollBar_ != nullptr) {
        InvalidateRect(horizontalScrollBar_, nullptr, FALSE);
    }
}

void ClassicNotepadApp::UpdateTitle()
{
    std::wstring title;
    if (document_.IsModified()) {
        title += L"*";
    }

    title += document_.DisplayName();
    if (IsLargeFileMode()) {
        title += L" [Large File]";
    }
    title += L" - Classic Notepad";
    SetWindowTextW(mainWindow_, title.c_str());
}

void ClassicNotepadApp::UpdateMenuState(HMENU menu)
{
    if (menu == nullptr) {
        return;
    }

    if (IsLargeFileMode()) {
        const LargeFileViewInfo info = largeFileView_->GetInfo();
        const UINT editState = info.editable ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(menu, ID_FILE_SAVE, MF_BYCOMMAND | (info.modified ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menu, ID_FILE_SAVE_AS, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_FILE_PAGE_SETUP, MF_BYCOMMAND | MF_ENABLED);
        EnableMenuItem(menu, ID_FILE_PRINT, MF_BYCOMMAND | MF_GRAYED);
        EnableMenuItem(menu, ID_EDIT_UNDO, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_EDIT_CUT, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_EDIT_COPY, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_EDIT_PASTE, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_EDIT_DELETE, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_EDIT_FIND, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_EDIT_FIND_NEXT, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_EDIT_REPLACE, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_EDIT_GO_TO, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_EDIT_SELECT_ALL, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_EDIT_TIME_DATE, MF_BYCOMMAND | editState);
        EnableMenuItem(menu, ID_FORMAT_WORD_WRAP, MF_BYCOMMAND | MF_ENABLED);
        EnableMenuItem(menu, ID_FORMAT_FONT, MF_BYCOMMAND | MF_ENABLED);
        EnableMenuItem(menu, ID_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_ENABLED);
        CheckMenuItem(menu, ID_FORMAT_WORD_WRAP, MF_BYCOMMAND | (wordWrap_ ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(menu, ID_VIEW_STATUS_BAR, MF_BYCOMMAND | (statusBarVisible_ ? MF_CHECKED : MF_UNCHECKED));
        return;
    }
    if (editor_ == nullptr) {
        return;
    }

    const bool hasSelection = HasSelection();
    const bool hasText = GetWindowTextLengthW(editor_) > 0;
    const bool canUndo = SendMessageW(editor_, EM_CANUNDO, 0, 0) != 0;
    const bool canPaste = CanPasteText();
    const bool hasFindText = findBuffer_[0] != L'\0';

    EnableMenuItem(menu, ID_FILE_PAGE_SETUP, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, ID_FILE_PRINT, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, ID_EDIT_UNDO, MF_BYCOMMAND | (canUndo ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_CUT, MF_BYCOMMAND | (hasSelection ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_COPY, MF_BYCOMMAND | (hasSelection ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_PASTE, MF_BYCOMMAND | (canPaste ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_DELETE, MF_BYCOMMAND | (hasSelection ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_FIND, MF_BYCOMMAND | (hasText ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_FIND_NEXT, MF_BYCOMMAND | (hasText && hasFindText ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_REPLACE, MF_BYCOMMAND | (hasText ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_GO_TO, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, ID_EDIT_SELECT_ALL, MF_BYCOMMAND | (hasText ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, ID_EDIT_TIME_DATE, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, ID_FORMAT_WORD_WRAP, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, ID_FORMAT_FONT, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, ID_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_ENABLED);
    CheckMenuItem(menu, ID_FORMAT_WORD_WRAP, MF_BYCOMMAND | (wordWrap_ ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_VIEW_STATUS_BAR, MF_BYCOMMAND | (statusBarVisible_ ? MF_CHECKED : MF_UNCHECKED));
}

void ClassicNotepadApp::UpdateStatusBar()
{
    if (statusBar_ == nullptr || editor_ == nullptr) {
        return;
    }

    if (IsLargeFileMode()) {
        const LargeFileViewInfo info = largeFileView_->GetInfo();
        const std::uint64_t indexPercent = info.fileSize == 0U
            ? 0U
            : std::min<std::uint64_t>(100U, static_cast<std::uint64_t>(
                static_cast<long double>(info.indexedBytes) * 100.0L / static_cast<long double>(info.fileSize)));
        statusBarParts_[0] = info.modified ? L"Large file (modified)" : L"Large file (editable)";
        statusBarParts_[1] = info.indexingComplete && info.displayedCharacterCountKnown
            ? classic_notepad::FormatCharacterCount(static_cast<std::size_t>(info.displayedCharacterCount))
            : L"Indexing " + std::to_wstring(indexPercent) + L"%";
        statusBarParts_[2] = L"Ln " + classic_notepad::FormatNumberWithSeparators(static_cast<std::size_t>(info.caretLine + 1U)) +
            L", Col " + classic_notepad::FormatNumberWithSeparators(static_cast<std::size_t>(info.caretColumn + 1U)) +
            L", Char " + classic_notepad::FormatNumberWithSeparators(static_cast<std::size_t>(info.caret));
        statusBarParts_[3] = classic_notepad::FormatEncoding(document_.Encoding()) + L" | " +
            classic_notepad::FormatLineEnding(document_.LineEnding());
        UpdateStatusBarPartLayout();
        for (std::size_t index = 0; index < statusBarParts_.size(); ++index) {
            const WPARAM part = static_cast<WPARAM>(index) | SBT_OWNERDRAW | SBT_NOBORDERS;
            SendMessageW(statusBar_, SB_SETTEXTW, part, static_cast<LPARAM>(index));
        }
        return;
    }

    const DWORD activeCaretPosition = GetActiveCaretPosition();

    std::uint64_t logicalLine = 0U;
    std::uint64_t logicalColumn = 0U;
    statusBarParts_[0] = L"Ln ";
    if (TryGetNativeLogicalCoordinate(
            static_cast<int>(activeCaretPosition), logicalLine, logicalColumn)) {
        statusBarParts_[0] += classic_notepad::FormatNumberWithSeparators(
            static_cast<std::size_t>(logicalLine));
        statusBarParts_[0] += L", Col ";
        statusBarParts_[0] += classic_notepad::FormatNumberWithSeparators(
            static_cast<std::size_t>(logicalColumn));
    } else if (!statusCharacterCountAvailable_) {
        statusBarParts_[0] += L"unavailable, Col unavailable";
    } else {
        statusBarParts_[0] += L"indexing, Col indexing";
    }
    GetStatusCharacterCount();
    statusBarParts_[1] = GetStatusCharacterCountLabel();
    statusBarParts_[2] = classic_notepad::FormatLineEnding(document_.LineEnding());
    statusBarParts_[3] = classic_notepad::FormatEncoding(document_.Encoding());

    UpdateStatusBarPartLayout();

    for (std::size_t index = 0; index < statusBarParts_.size(); ++index) {
        const WPARAM part = static_cast<WPARAM>(index) | SBT_OWNERDRAW | SBT_NOBORDERS;
        SendMessageW(statusBar_, SB_SETTEXTW, part, static_cast<LPARAM>(index));
    }
}

void ClassicNotepadApp::UpdateStatusBarPartLayout()
{
    if (statusBar_ == nullptr) {
        return;
    }

    HDC deviceContext = GetDC(statusBar_);
    HGDIOBJ previousFont = nullptr;
    if (deviceContext != nullptr) {
        previousFont = SelectObject(
            deviceContext,
            menuFont_ != nullptr ? menuFont_ : GetStockObject(DEFAULT_GUI_FONT));
    }

    std::array<int, kStatusBarPartCount> partRights{};
    const int horizontalPadding = ScalePixelsForWindow(statusBar_, kStatusBarHorizontalPadding);
    const int extraPartRoom = ScalePixelsForWindow(statusBar_, kStatusBarExtraPartRoom);

    std::array<int, kStatusBarPartCount> minimumWidths{};
    for (int index = 0; index < kStatusBarPartCount; ++index) {
        const SIZE textSize = MeasureText(deviceContext, statusBarParts_[static_cast<std::size_t>(index)]);
        minimumWidths[static_cast<std::size_t>(index)] = textSize.cx + (horizontalPadding * 2) + extraPartRoom;
    }

    RECT statusRect{};
    GetClientRect(statusBar_, &statusRect);
    const int statusWidth = std::max(0, static_cast<int>(statusRect.right - statusRect.left));
    const RECT gripRect = GetStatusBarResizeGripRect();
    const int gripWidth = IsRectEmpty(&gripRect)
        ? 0
        : std::max(0, static_cast<int>(gripRect.right - gripRect.left));
    const int usableWidth = std::max(0, statusWidth - gripWidth);

    int previousRight = 0;
    for (int index = 0; index < kStatusBarPartCount - 1; ++index) {
        int remainingMinimum = minimumWidths.back();
        for (int nextIndex = index + 1; nextIndex < kStatusBarPartCount - 1; ++nextIndex) {
            remainingMinimum += minimumWidths[static_cast<std::size_t>(nextIndex)];
        }

        const int minimumRight = previousRight + minimumWidths[static_cast<std::size_t>(index)];
        const int maximumRight = usableWidth > 0 ? usableWidth - remainingMinimum : minimumRight;
        const int preferredRight = usableWidth > 0
            ? MulDiv(usableWidth, kStatusBarPartRightPercents[static_cast<std::size_t>(index)], 100)
            : minimumRight;
        const int right = maximumRight >= minimumRight
            ? std::clamp(preferredRight, minimumRight, maximumRight)
            : minimumRight;

        partRights[static_cast<std::size_t>(index)] = right;
        previousRight = right;
    }
    partRights.back() = -1;

    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }
    if (deviceContext != nullptr) {
        ReleaseDC(statusBar_, deviceContext);
    }

    SendMessageW(
        statusBar_,
        SB_SETPARTS,
        static_cast<WPARAM>(partRights.size()),
        reinterpret_cast<LPARAM>(partRights.data()));
}

void ClassicNotepadApp::SetStatusCharacterCountFromText(const std::wstring& text)
{
    statusCharacterCount_ = 0U;
    statusCharacterTextLength_ = static_cast<int>(std::min<std::size_t>(
        text.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    statusCharacterHasPreviousUnit_ = false;
    statusCharacterPreviousUnit_ = 0;
    statusLogicalLineBreakCount_ = 0U;
    statusLogicalLineStartPosition_ = 0;
    statusLogicalPreviousWasCarriageReturn_ = false;
    statusLogicalLineIndexAvailable_ = true;
    nativeStatusLineCheckpoints_.clear();
    try {
        nativeStatusLineCheckpoints_.reserve(
            static_cast<std::size_t>(statusCharacterTextLength_) / kStatusCharacterCountBatchUnitLimit + 2U);
        if (!AppendNativeStatusCheckpoint(0)) throw std::bad_alloc();
        for (int position = 0; position < statusCharacterTextLength_; ++position) {
            if (position != 0 &&
                position % static_cast<int>(kStatusCharacterCountBatchUnitLimit) == 0 &&
                !AppendNativeStatusCheckpoint(position)) {
                throw std::bad_alloc();
            }
            AccumulateNativeStatusCodeUnit(position, text[static_cast<std::size_t>(position)]);
        }
        if (nativeStatusLineCheckpoints_.empty() ||
            nativeStatusLineCheckpoints_.back().textPosition != statusCharacterTextLength_) {
            if (!AppendNativeStatusCheckpoint(statusCharacterTextLength_)) throw std::bad_alloc();
        }
    } catch (const std::bad_alloc&) {
        nativeStatusLineCheckpoints_.clear();
        statusCharacterCount_ = classic_notepad::CountStatusCharacters(text);
        statusLogicalLineBreakCount_ = 0U;
        statusLogicalLineStartPosition_ = 0;
        statusLogicalPreviousWasCarriageReturn_ = false;
        statusLogicalLineIndexAvailable_ = false;
    }
    statusCharacterScanPosition_ = statusCharacterTextLength_;
    statusCharacterCountComplete_ = true;
    statusCharacterCountAvailable_ = true;
    statusLogicalLineIndexAvailable_ = statusLogicalLineIndexAvailable_ &&
        !nativeStatusLineCheckpoints_.empty();
    statusCharacterCountDirty_ = false;
    if (mainWindow_ != nullptr) KillTimer(mainWindow_, kStatusCharacterCountTimerId);
}

void ClassicNotepadApp::ResetNativeStatusSummary(int textLength)
{
    statusCharacterCount_ = 0U;
    statusCharacterTextLength_ = std::max(0, textLength);
    statusCharacterScanPosition_ = 0;
    statusCharacterPreviousUnit_ = 0;
    statusCharacterHasPreviousUnit_ = false;
    statusLogicalLineBreakCount_ = 0U;
    statusLogicalLineStartPosition_ = 0;
    statusLogicalPreviousWasCarriageReturn_ = false;
    statusLogicalLineIndexAvailable_ = true;
    statusCharacterCountComplete_ = textLength == 0;
    statusCharacterCountAvailable_ = true;
    statusCharacterCountDirty_ = false;
    nativeStatusLineCheckpoints_.clear();
    try {
        nativeStatusLineCheckpoints_.reserve(
            static_cast<std::size_t>(std::max(0, textLength)) / kStatusCharacterCountBatchUnitLimit + 2U);
        if (!AppendNativeStatusCheckpoint(0)) throw std::bad_alloc();
    } catch (const std::bad_alloc&) {
        nativeStatusLineCheckpoints_.clear();
        statusLogicalLineIndexAvailable_ = false;
    }
}

bool ClassicNotepadApp::AppendNativeStatusCheckpoint(int textPosition)
{
    try {
        nativeStatusLineCheckpoints_.push_back({
            textPosition,
            statusLogicalLineBreakCount_,
            statusLogicalLineStartPosition_,
            statusLogicalPreviousWasCarriageReturn_});
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void ClassicNotepadApp::AccumulateNativeStatusCodeUnit(int textPosition, wchar_t character)
{
    const bool joinsPrevious = statusCharacterHasPreviousUnit_ &&
        ((statusCharacterPreviousUnit_ == L'\r' && character == L'\n') ||
            (classic_notepad::IsHighSurrogateUnit(statusCharacterPreviousUnit_) &&
                classic_notepad::IsLowSurrogateUnit(character)));
    ++statusCharacterCount_;
    if (joinsPrevious) --statusCharacterCount_;
    statusCharacterPreviousUnit_ = character;
    statusCharacterHasPreviousUnit_ = true;

    if (character == L'\r') {
        ++statusLogicalLineBreakCount_;
        statusLogicalLineStartPosition_ = textPosition + 1;
        statusLogicalPreviousWasCarriageReturn_ = true;
    } else if (character == L'\n') {
        if (!statusLogicalPreviousWasCarriageReturn_) ++statusLogicalLineBreakCount_;
        statusLogicalLineStartPosition_ = textPosition + 1;
        statusLogicalPreviousWasCarriageReturn_ = false;
    } else {
        statusLogicalPreviousWasCarriageReturn_ = false;
    }
}

bool ClassicNotepadApp::TryGetNativeLogicalCoordinate(
    int textPosition, std::uint64_t& oneBasedLine, std::uint64_t& oneBasedColumn) const
{
    if (editor_ == nullptr || !statusCharacterCountAvailable_ || !statusCharacterCountComplete_ ||
        !statusLogicalLineIndexAvailable_ ||
        textPosition < 0 || textPosition > statusCharacterTextLength_ ||
        nativeStatusLineCheckpoints_.empty()) return false;

    const auto checkpoint = std::upper_bound(
        nativeStatusLineCheckpoints_.begin(), nativeStatusLineCheckpoints_.end(), textPosition,
        [](int position, const NativeStatusLineCheckpoint& candidate) {
            return position < candidate.textPosition;
        });
    if (checkpoint == nativeStatusLineCheckpoints_.begin()) return false;
    const NativeStatusLineCheckpoint& base = *std::prev(checkpoint);
    std::uint64_t lineBreaks = base.lineBreaks;
    int lineStart = base.lineStartPosition;
    bool previousWasCr = base.previousWasCarriageReturn;
    {
        const HLOCAL buffer = reinterpret_cast<HLOCAL>(SendMessageW(editor_, EM_GETHANDLE, 0, 0));
        ScopedLocalBufferLock lockedBuffer(buffer);
        if (lockedBuffer.Get() == nullptr) return false;
        const wchar_t* text = lockedBuffer.Get();
        for (int position = base.textPosition; position < textPosition; ++position) {
            const wchar_t character = text[position];
            if (character == L'\r') {
                ++lineBreaks;
                lineStart = position + 1;
                previousWasCr = true;
            } else if (character == L'\n') {
                if (!previousWasCr) ++lineBreaks;
                lineStart = position + 1;
                previousWasCr = false;
            } else {
                previousWasCr = false;
            }
        }
    }
    oneBasedLine = lineBreaks + 1U;
    oneBasedColumn = textPosition >= lineStart
        ? static_cast<std::uint64_t>(textPosition - lineStart) + 1U
        : 1U;
    return true;
}

bool ClassicNotepadApp::EnsureNativeStatusSummaryComplete()
{
    if (editor_ == nullptr || IsLargeFileMode()) return false;
    if (statusCharacterCountComplete_ && statusCharacterCountAvailable_ && statusLogicalLineIndexAvailable_) return true;

    const int textLength = std::max(0, GetWindowTextLengthW(editor_));
    ResetNativeStatusSummary(textLength);
    if (!statusLogicalLineIndexAvailable_) return false;
    bool okay = false;
    {
        const HLOCAL buffer = reinterpret_cast<HLOCAL>(SendMessageW(editor_, EM_GETHANDLE, 0, 0));
        ScopedLocalBufferLock lockedBuffer(buffer);
        if (lockedBuffer.Get() != nullptr) {
            const wchar_t* text = lockedBuffer.Get();
            for (int position = 0; position < textLength; ++position) {
                if (position != 0 &&
                    position % static_cast<int>(kStatusCharacterCountBatchUnitLimit) == 0 &&
                    !AppendNativeStatusCheckpoint(position)) {
                    break;
                }
                AccumulateNativeStatusCodeUnit(position, text[position]);
                statusCharacterScanPosition_ = position + 1;
            }
            if (statusCharacterScanPosition_ == textLength &&
                (nativeStatusLineCheckpoints_.empty() ||
                    nativeStatusLineCheckpoints_.back().textPosition != textLength)) {
                okay = AppendNativeStatusCheckpoint(textLength);
            } else {
                okay = statusCharacterScanPosition_ == textLength;
            }
        }
    }
    statusCharacterCountAvailable_ = okay;
    statusLogicalLineIndexAvailable_ = okay;
    statusCharacterCountComplete_ = okay;
    statusCharacterCountDirty_ = false;
    if (mainWindow_ != nullptr) {
        KillTimer(mainWindow_, kStatusCharacterCountTimerId);
        UpdateStatusBar();
    }
    return okay;
}

bool ClassicNotepadApp::GetNativeLogicalLineStart(std::uint64_t oneBasedLine, DWORD& textPosition) const
{
    if (oneBasedLine == 0U || !statusCharacterCountAvailable_ || !statusCharacterCountComplete_ ||
        !statusLogicalLineIndexAvailable_ ||
        nativeStatusLineCheckpoints_.empty() || oneBasedLine > statusLogicalLineBreakCount_ + 1U) return false;
    const std::uint64_t targetBreaks = oneBasedLine - 1U;
    if (targetBreaks == 0U) {
        textPosition = 0U;
        return true;
    }
    const auto checkpoint = std::upper_bound(
        nativeStatusLineCheckpoints_.begin(), nativeStatusLineCheckpoints_.end(), targetBreaks,
        [](std::uint64_t breaks, const NativeStatusLineCheckpoint& candidate) {
            return breaks < candidate.lineBreaks;
        });
    if (checkpoint == nativeStatusLineCheckpoints_.begin()) return false;
    const NativeStatusLineCheckpoint& base = *std::prev(checkpoint);
    std::uint64_t lineBreaks = base.lineBreaks;
    int lineStart = base.lineStartPosition;
    bool previousWasCr = base.previousWasCarriageReturn;
    const int textLength = statusCharacterTextLength_;
    {
        const HLOCAL buffer = reinterpret_cast<HLOCAL>(SendMessageW(editor_, EM_GETHANDLE, 0, 0));
        ScopedLocalBufferLock lockedBuffer(buffer);
        if (lockedBuffer.Get() == nullptr) return false;
        const wchar_t* text = lockedBuffer.Get();
        int position = base.textPosition;
        while (position < textLength && lineBreaks < targetBreaks) {
            const wchar_t character = text[position];
            if (character == L'\r') {
                ++lineBreaks;
                lineStart = position + 1;
                previousWasCr = true;
            } else if (character == L'\n') {
                if (!previousWasCr) ++lineBreaks;
                lineStart = position + 1;
                previousWasCr = false;
            } else {
                previousWasCr = false;
            }
            ++position;
        }
        if (lineBreaks != targetBreaks) return false;
        if (previousWasCr && lineStart < textLength && text[lineStart] == L'\n') ++lineStart;
    }
    if (lineStart < 0 || static_cast<std::uint64_t>(lineStart) > (std::numeric_limits<DWORD>::max)()) return false;
    textPosition = static_cast<DWORD>(lineStart);
    return true;
}

std::size_t ClassicNotepadApp::GetStatusCharacterCount()
{
    if (editor_ == nullptr) {
        statusCharacterCount_ = 0;
        statusCharacterTextLength_ = 0;
        statusCharacterScanPosition_ = 0;
        statusCharacterCountComplete_ = true;
        statusCharacterCountAvailable_ = true;
        statusCharacterCountDirty_ = false;
        statusLogicalLineBreakCount_ = 0U;
        statusLogicalLineStartPosition_ = 0;
        statusLogicalPreviousWasCarriageReturn_ = false;
        statusLogicalLineIndexAvailable_ = true;
        nativeStatusLineCheckpoints_.clear();
        AppendNativeStatusCheckpoint(0);
        return statusCharacterCount_;
    }

    const int textLength = std::max(0, GetWindowTextLengthW(editor_));
    if (!statusCharacterCountDirty_ && textLength == statusCharacterTextLength_ &&
        statusCharacterCountAvailable_) {
        const bool scanInProgress = textLength > 0 && !statusCharacterCountComplete_;
        const bool completedIndexReady = statusCharacterCountComplete_ &&
            statusLogicalLineIndexAvailable_ && !nativeStatusLineCheckpoints_.empty();
        if (scanInProgress || completedIndexReady) return statusCharacterCount_;
    }

    ResetNativeStatusSummary(textLength);
    if (textLength == 0) {
        if (mainWindow_ != nullptr) KillTimer(mainWindow_, kStatusCharacterCountTimerId);
    } else if (textLength <= kStatusCharacterCountBatchUnitLimit) {
        SetStatusCharacterCountFromText(GetEditorText());
        if (mainWindow_ != nullptr) KillTimer(mainWindow_, kStatusCharacterCountTimerId);
    } else if (mainWindow_ == nullptr ||
        SetTimer(mainWindow_, kStatusCharacterCountTimerId, 1U, nullptr) == 0U) {
        statusCharacterCountAvailable_ = false;
    }
    return statusCharacterCount_;
}

std::wstring ClassicNotepadApp::GetStatusCharacterCountLabel() const
{
    if (!statusCharacterCountAvailable_) return L"Character count unavailable";
    if (!statusCharacterCountComplete_) {
        const int percent = statusCharacterTextLength_ <= 0 ? 0 : std::clamp(
            static_cast<int>(static_cast<std::int64_t>(statusCharacterScanPosition_) * 100 /
                statusCharacterTextLength_), 0, 100);
        return L"Counting characters " + std::to_wstring(percent) + L"%";
    }
    return classic_notepad::FormatCharacterCount(statusCharacterCount_);
}

void ClassicNotepadApp::MeasureStatusCharacterCountBatch()
{
    if (mainWindow_ == nullptr || editor_ == nullptr || IsLargeFileMode()) {
        if (mainWindow_ != nullptr) KillTimer(mainWindow_, kStatusCharacterCountTimerId);
        return;
    }

    const int textLength = std::max(0, GetWindowTextLengthW(editor_));
    if (statusCharacterCountDirty_ || textLength != statusCharacterTextLength_) {
        GetStatusCharacterCount();
    }
    if (!statusCharacterCountAvailable_ || statusCharacterCountComplete_) {
        KillTimer(mainWindow_, kStatusCharacterCountTimerId);
        UpdateStatusBar();
        return;
    }

    bool lockFailed = false;
    {
        const HLOCAL buffer = reinterpret_cast<HLOCAL>(SendMessageW(editor_, EM_GETHANDLE, 0, 0));
        ScopedLocalBufferLock lockedBuffer(buffer);
        if (lockedBuffer.Get() == nullptr) {
            lockFailed = true;
        } else {
            const int limit = statusCharacterScanPosition_ + std::min(
                textLength - statusCharacterScanPosition_, kStatusCharacterCountBatchUnitLimit);
            const wchar_t* text = lockedBuffer.Get();
            if (statusLogicalLineIndexAvailable_ &&
                (nativeStatusLineCheckpoints_.empty() ||
                    nativeStatusLineCheckpoints_.back().textPosition != statusCharacterScanPosition_) &&
                !AppendNativeStatusCheckpoint(statusCharacterScanPosition_)) {
                statusLogicalLineIndexAvailable_ = false;
                nativeStatusLineCheckpoints_.clear();
            }
            while (statusCharacterScanPosition_ < limit) {
                AccumulateNativeStatusCodeUnit(
                    statusCharacterScanPosition_, text[statusCharacterScanPosition_]);
                ++statusCharacterScanPosition_;
            }
            if (statusLogicalLineIndexAvailable_ &&
                (nativeStatusLineCheckpoints_.empty() ||
                    nativeStatusLineCheckpoints_.back().textPosition != statusCharacterScanPosition_) &&
                !AppendNativeStatusCheckpoint(statusCharacterScanPosition_)) {
                statusLogicalLineIndexAvailable_ = false;
                nativeStatusLineCheckpoints_.clear();
            }
        }
    }
    if (lockFailed) {
        statusCharacterCountAvailable_ = false;
        statusCharacterCountComplete_ = false;
        statusLogicalLineIndexAvailable_ = false;
        nativeStatusLineCheckpoints_.clear();
        KillTimer(mainWindow_, kStatusCharacterCountTimerId);
        UpdateStatusBar();
        return;
    }

    statusCharacterCountComplete_ = statusCharacterScanPosition_ >= textLength;
    if (statusCharacterCountComplete_) KillTimer(mainWindow_, kStatusCharacterCountTimerId);
    UpdateStatusBar();
}

void ClassicNotepadApp::UpdateStatusBarSizeGripStyle()
{
    if (statusBar_ == nullptr) {
        return;
    }

    const LONG_PTR currentStyle = GetWindowLongPtrW(statusBar_, GWL_STYLE);
    const LONG_PTR wantedStyle = darkModeEnabled_
        ? ((currentStyle & ~static_cast<LONG_PTR>(SBARS_SIZEGRIP)) | static_cast<LONG_PTR>(CCS_NODIVIDER))
        : ((currentStyle | static_cast<LONG_PTR>(SBARS_SIZEGRIP)) & ~static_cast<LONG_PTR>(CCS_NODIVIDER));

    if (wantedStyle != currentStyle) {
        SetWindowLongPtrW(statusBar_, GWL_STYLE, wantedStyle);
        SetWindowPos(
            statusBar_,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        SendMessageW(statusBar_, WM_SIZE, 0, 0);
    }
}

RECT ClassicNotepadApp::GetStatusBarResizeGripRect() const
{
    RECT gripRect{};
    if (statusBar_ == nullptr || !statusBarVisible_) {
        return gripRect;
    }

    GetClientRect(statusBar_, &gripRect);
    const int statusHeight = std::max(0, static_cast<int>(gripRect.bottom - gripRect.top));
    const int statusWidth = std::max(0, static_cast<int>(gripRect.right - gripRect.left));
    const int gripSize = std::min(
        statusWidth,
        std::max(statusHeight, std::max(GetSystemMetrics(SM_CXVSCROLL), GetSystemMetrics(SM_CYHSCROLL))));
    gripRect.left = std::max(gripRect.left, gripRect.right - gripSize);
    gripRect.top = std::max(gripRect.top, gripRect.bottom - gripSize);
    return gripRect;
}

RECT ClassicNotepadApp::GetResizeGripRect() const
{
    RECT gripRect = GetStatusBarResizeGripRect();
    if (statusBar_ == nullptr || IsRectEmpty(&gripRect)) {
        return RECT{};
    }

    MapWindowPoints(statusBar_, mainWindow_, reinterpret_cast<POINT*>(&gripRect), 2);
    return gripRect;
}

bool ClassicNotepadApp::IsResizableFromGrip() const
{
    if (mainWindow_ == nullptr || IsZoomed(mainWindow_)) {
        return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(mainWindow_, GWL_STYLE);
    return (style & WS_THICKFRAME) != 0;
}

bool ClassicNotepadApp::IsPointInStatusBarResizeGrip(POINT point) const
{
    if (!darkModeEnabled_ || !IsResizableFromGrip()) {
        return false;
    }

    const RECT gripRect = GetStatusBarResizeGripRect();
    return !IsRectEmpty(&gripRect) && PtInRect(&gripRect, point) != 0;
}

void ClassicNotepadApp::DrawDarkResizeGrip(HDC deviceContext) const
{
    if (!darkModeEnabled_ || deviceContext == nullptr || !IsResizableFromGrip()) {
        return;
    }

    const RECT gripRect = GetStatusBarResizeGripRect();
    if (IsRectEmpty(&gripRect)) {
        return;
    }

    RECT paintRect = gripRect;
    RECT statusRect{};
    GetClientRect(statusBar_, &statusRect);
    paintRect.left = std::max(statusRect.left, paintRect.left - 2);
    paintRect.top = statusRect.top;
    paintRect.right = statusRect.right;
    paintRect.bottom = statusRect.bottom;

    HBRUSH backgroundBrush = darkStatusBackgroundBrush_ != nullptr
        ? darkStatusBackgroundBrush_
        : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    FillRect(deviceContext, &paintRect, backgroundBrush);

    HPEN pen = CreatePen(PS_SOLID, 1, kDarkResizeGrip);
    if (pen == nullptr) {
        return;
    }

    HGDIOBJ previousPen = SelectObject(deviceContext, pen);
    for (int index = 0; index < 3; ++index) {
        const int inset = 4 + index * 4;
        MoveToEx(deviceContext, paintRect.right - inset, paintRect.bottom - 3, nullptr);
        LineTo(deviceContext, paintRect.right - 3, paintRect.bottom - inset);
    }

    if (previousPen != nullptr) {
        SelectObject(deviceContext, previousPen);
    }
    DeleteObject(pen);
}

void ClassicNotepadApp::DrawDarkStatusBarChrome(HDC deviceContext) const
{
    if (!darkModeEnabled_ || deviceContext == nullptr || statusBar_ == nullptr) {
        return;
    }

    RECT statusRect{};
    GetClientRect(statusBar_, &statusRect);
    if (IsRectEmpty(&statusRect)) {
        return;
    }

    DrawDarkResizeGrip(deviceContext);

    HBRUSH lineBrush = CreateSolidBrush(kDarkStatusSeparator);
    if (lineBrush != nullptr) {
        RECT topLine = statusRect;
        topLine.bottom = topLine.top + 1;
        FillRect(deviceContext, &topLine, lineBrush);

        RECT bottomLine = statusRect;
        bottomLine.top = std::max(bottomLine.top, bottomLine.bottom - 1);
        FillRect(deviceContext, &bottomLine, lineBrush);

        const RECT gripRect = GetStatusBarResizeGripRect();
        if (!IsRectEmpty(&gripRect)) {
            RECT gripLeftLine = gripRect;
            gripLeftLine.right = gripLeftLine.left + 1;
            FillRect(deviceContext, &gripLeftLine, lineBrush);
        }

        DeleteObject(lineBrush);
    }
}

bool ClassicNotepadApp::StartResizeFromStatusGrip(HWND statusBar, LPARAM lParam) const
{
    if (statusBar != statusBar_ || !IsResizableFromGrip()) {
        return false;
    }

    POINT point{
        static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
        static_cast<LONG>(static_cast<short>(HIWORD(lParam)))
    };

    if (!IsPointInStatusBarResizeGrip(point)) {
        return false;
    }

    POINT screenPoint{};
    GetCursorPos(&screenPoint);
    ReleaseCapture();
    SendMessageW(
        mainWindow_,
        WM_NCLBUTTONDOWN,
        HTBOTTOMRIGHT,
        MAKELPARAM(static_cast<short>(screenPoint.x), static_cast<short>(screenPoint.y)));
    return true;
}

void ClassicNotepadApp::RefreshSpellCheck(bool immediate)
{
    if (!spellCheckAvailable_) {
        ClearSpellingAnnotations();
        return;
    }

    if (immediate) {
        RunSpellCheckNow();
        return;
    }

    ScheduleSpellCheck();
}

void ClassicNotepadApp::ScheduleSpellCheck()
{
    if (!spellCheckAvailable_ || mainWindow_ == nullptr) {
        return;
    }

    if (spellCheckTimerId_ != 0) {
        KillTimer(mainWindow_, spellCheckTimerId_);
    }

    spellCheckTimerId_ = SetTimer(mainWindow_, kSpellCheckTimerId, kSpellCheckDelayMs, nullptr);
}

void ClassicNotepadApp::RunSpellCheckNow()
{
    if (mainWindow_ != nullptr && spellCheckTimerId_ != 0) {
        KillTimer(mainWindow_, spellCheckTimerId_);
        spellCheckTimerId_ = 0;
    }

    const classic_notepad::SpellingRevision currentRevision = CurrentSpellingRevision();
    ignoredSpellingOccurrences_.erase(
        std::remove_if(ignoredSpellingOccurrences_.begin(), ignoredSpellingOccurrences_.end(),
            [&currentRevision](const classic_notepad::windows::IgnoredSpellingOccurrence& occurrence) {
                return occurrence.revision.session != currentRevision.session ||
                    occurrence.revision.content != currentRevision.content;
            }),
        ignoredSpellingOccurrences_.end());
    ClearSpellingAnnotations();
    if (!spellCheckAvailable_) return;

    std::vector<classic_notepad::SpellingTextWindow> windows;
    if (largeFileView_ != nullptr) {
        std::wstring error;
        if (!largeFileView_->GetVisibleTextWindows(windows, error)) return;
    } else if (!GetVisibleEditorTextWindows(windows)) {
        return;
    }

    SpellOperationStatus checkStatus = SpellOperationStatus::Success;
    HRESULT checkHresult = S_OK;
    const auto checkBackend = [this, &checkStatus, &checkHresult](const std::wstring& text) {
        if (checkStatus != SpellOperationStatus::Success) return std::vector<classic_notepad::TextRange>{};
        const SpellCheckResult backend = spellChecker_.Check(text);
        if (backend.status != SpellOperationStatus::Success) {
            checkStatus = backend.status;
            checkHresult = backend.hresult;
            return std::vector<classic_notepad::TextRange>{};
        }
        std::vector<classic_notepad::TextRange> ranges;
        ranges.reserve(backend.value.size());
        for (const SpellingErrorRange& result : backend.value) {
            ranges.push_back({static_cast<std::size_t>(result.start),
                static_cast<std::size_t>(result.length)});
        }
        return ranges;
    };
    classic_notepad::SpellingAnnotationBatch candidate;
    const bool checked = classic_notepad::CheckSpellingWindows(windows, checkBackend, candidate);
    if (checkStatus != SpellOperationStatus::Success) {
        ReportSpellCheckFailure(checkStatus, checkHresult);
        return;
    }
    if (!checked ||
        !classic_notepad::SpellingRevisionMatches(candidate.revision, CurrentSpellingRevision())) {
        ReportSpellCheckFailure(SpellOperationStatus::Failure, E_UNEXPECTED);
        return;
    }

    candidate.annotations.erase(
        std::remove_if(candidate.annotations.begin(), candidate.annotations.end(),
            [this, &windows, &candidate](const classic_notepad::SpellingAnnotation& annotation) {
                return IsIgnoredSpellingOccurrence(annotation, candidate.revision, windows);
            }),
        candidate.annotations.end());

    if (!classic_notepad::TryPublishSpellingBatch(
            spellingBatch_, candidate, CurrentSpellingRevision())) {
        ReportSpellCheckFailure(SpellOperationStatus::Failure, E_ABORT);
        return;
    }
    spellCheckFailureDialogShown_ = false;
    if (largeFileView_ != nullptr) largeFileView_->SetSpellingBatch(spellingBatch_);
    else if (editor_ != nullptr) InvalidateRect(editor_, nullptr, FALSE);
}

bool ClassicNotepadApp::IsIgnoredSpellingOccurrence(
    const classic_notepad::SpellingAnnotation& annotation,
    const classic_notepad::SpellingRevision& revision,
    const std::vector<classic_notepad::SpellingTextWindow>& windows) const
{
    return classic_notepad::windows::IsIgnoredSpellingOccurrence(
        ignoredSpellingOccurrences_, annotation, revision, windows);
}

void ClassicNotepadApp::ClearIgnoredSpellingOccurrences()
{
    ignoredSpellingOccurrences_.clear();
}

void ClassicNotepadApp::ReportSpellCheckFailure(SpellOperationStatus status, HRESULT hresult)
{
    if (spellCheckFailureDialogShown_ || automationMode_) return;
    spellCheckFailureDialogShown_ = true;
    std::wstring message = status == SpellOperationStatus::Unavailable
        ? L"British English spell checking became unavailable. No spelling results were published."
        : L"Spell checking failed. No spelling results were published. HRESULT ";
    if (status != SpellOperationStatus::Unavailable) {
        message += FormatSpellHRESULT(hresult);
        message += L".";
    }
    ShowError(message);
}

bool ClassicNotepadApp::ReadCurrentSpellingTextRange(
    std::uint64_t start,
    std::uint64_t length,
    std::wstring& text,
    std::wstring& errorMessage) const
{
    text.clear();
    if (length == 0U || length > classic_notepad::kMaximumSpellingWindowUnits ||
        length > (std::numeric_limits<std::uint64_t>::max)() - start) {
        errorMessage = L"The spelling target range is invalid or exceeds the bounded word limit.";
        return false;
    }

    if (largeFileView_ != nullptr) {
        return largeFileView_->GetTextRange(start, length, text, errorMessage);
    }
    if (editor_ == nullptr || start > static_cast<std::uint64_t>((std::numeric_limits<LONG>::max)()) ||
        start + length > static_cast<std::uint64_t>((std::numeric_limits<LONG>::max)())) {
        errorMessage = L"The spelling target is outside the native editor range.";
        return false;
    }

    const int nativeTextLength = GetWindowTextLengthW(editor_);
    if (nativeTextLength < 0 || start + length > static_cast<std::uint64_t>(nativeTextLength)) {
        errorMessage = L"The spelling target is no longer present in the native editor.";
        return false;
    }

    const HLOCAL memory = reinterpret_cast<HLOCAL>(SendMessageW(editor_, EM_GETHANDLE, 0, 0));
    if (memory == nullptr) {
        errorMessage = L"The native editor text buffer is unavailable.";
        return false;
    }
    ScopedLocalBufferLock lockedBuffer(memory);
    const wchar_t* buffer = lockedBuffer.Get();
    if (buffer == nullptr) {
        errorMessage = L"The native editor text buffer could not be locked.";
        return false;
    }

    const std::size_t begin = static_cast<std::size_t>(start);
    const std::size_t count = static_cast<std::size_t>(length);
    const auto isHigh = [](wchar_t value) {
        const std::uint32_t unit = static_cast<std::uint32_t>(value);
        return unit >= 0xD800U && unit <= 0xDBFFU;
    };
    const auto isLow = [](wchar_t value) {
        const std::uint32_t unit = static_cast<std::uint32_t>(value);
        return unit >= 0xDC00U && unit <= 0xDFFFU;
    };
    if ((begin > 0U && isHigh(buffer[begin - 1U]) && isLow(buffer[begin])) ||
        (begin + count < static_cast<std::size_t>(nativeTextLength) &&
            isHigh(buffer[begin + count - 1U]) && isLow(buffer[begin + count]))) {
        errorMessage = L"The spelling target ends inside a Unicode character.";
        return false;
    }
    text.assign(buffer + begin, count);
    return true;
}

bool ClassicNotepadApp::IsSpellingTargetCurrent(
    const classic_notepad::SpellingContextTarget& target,
    std::wstring& currentText,
    std::wstring& errorMessage) const
{
    const classic_notepad::SpellingRevision current = CurrentSpellingRevision();
    if (target.revision.session != current.session || target.revision.content != current.content) {
        errorMessage = L"The document changed after this spelling action was opened.";
        return false;
    }
    if (target.length != target.text.size() || target.length == 0U ||
        target.length > classic_notepad::kMaximumSpellingWindowUnits) {
        errorMessage = L"The spelling action target is malformed.";
        return false;
    }
    if (!ReadCurrentSpellingTextRange(target.start, target.length, currentText, errorMessage)) return false;
    if (currentText != target.text) {
        errorMessage = L"The spelling target changed while its menu was open.";
        return false;
    }
    const std::vector<classic_notepad::TextRange> words =
        classic_notepad::FindSpellCheckWordRanges(currentText);
    if (words.size() != 1U || words.front().start != 0U || words.front().length != currentText.size()) {
        errorMessage = L"The spelling target is no longer a complete word.";
        return false;
    }
    return true;
}

ClassicNotepadApp::AutomationSpellingActionResult ClassicNotepadApp::ApplySpellingAction(
    const std::wstring& action,
    const classic_notepad::SpellingContextTarget& target,
    const std::wstring& suggestion,
    bool injectMappedInsertFailureForTesting)
{
    AutomationSpellingActionResult outcome;
    outcome.status = SpellOperationStatus::Failure;
    outcome.hresult = E_FAIL;
    if (!spellCheckAvailable_) {
        outcome.status = SpellOperationStatus::Unavailable;
        outcome.hresult = S_FALSE;
        outcome.message = L"Spell checking is unavailable.";
        return outcome;
    }

    std::wstring currentText;
    if (!IsSpellingTargetCurrent(target, currentText, outcome.message)) {
        outcome.hresult = E_ABORT;
        return outcome;
    }

    if (action == L"ignoreOnce") {
        const classic_notepad::SpellingRevision current = CurrentSpellingRevision();
        const auto found = std::find_if(ignoredSpellingOccurrences_.begin(), ignoredSpellingOccurrences_.end(),
            [&target, &currentText, &current](
                const classic_notepad::windows::IgnoredSpellingOccurrence& ignored) {
                return ignored.revision.session == current.session &&
                    ignored.revision.content == current.content && ignored.start == target.start &&
                    ignored.length == target.length && ignored.text == currentText;
            });
        if (found == ignoredSpellingOccurrences_.end()) {
            ignoredSpellingOccurrences_.push_back({current, target.start, target.length, currentText});
        }
        RunSpellCheckNow();
        outcome.status = SpellOperationStatus::Success;
        outcome.hresult = S_OK;
        outcome.message.clear();
        return outcome;
    }

    if (action == L"addToDictionary") {
        const SpellActionResult added = spellChecker_.Add(currentText);
        outcome.status = added.status;
        outcome.hresult = added.hresult;
        outcome.changed = added.status == SpellOperationStatus::Success && added.value;
        if (added.status == SpellOperationStatus::Unavailable) {
            outcome.message = L"The spelling backend is unavailable; the word was not added.";
        } else if (added.status == SpellOperationStatus::Failure) {
            outcome.message = L"The word could not be added to the spelling dictionary. HRESULT " +
                FormatSpellHRESULT(added.hresult) + L".";
        } else {
            RefreshSpellCheck(true);
        }
        return outcome;
    }

    if (action != L"replaceSuggestion") {
        outcome.hresult = E_INVALIDARG;
        outcome.message = L"Unknown spelling action.";
        return outcome;
    }
    if (!IsValidSpellReplacement(suggestion)) {
        outcome.hresult = E_INVALIDARG;
        outcome.message = L"The suggestion is empty or contains invalid Unicode or control characters.";
        return outcome;
    }

    const SpellSuggestResult availableSuggestions = spellChecker_.Suggest(currentText, 5U);
    if (availableSuggestions.status != SpellOperationStatus::Success) {
        outcome.status = availableSuggestions.status;
        outcome.hresult = availableSuggestions.hresult;
        outcome.message = availableSuggestions.status == SpellOperationStatus::Unavailable
            ? L"The spelling backend became unavailable; the suggestion was not applied."
            : L"The suggestion could not be validated with the spelling backend. HRESULT " +
                FormatSpellHRESULT(availableSuggestions.hresult) + L".";
        return outcome;
    }
    if (std::find(availableSuggestions.value.begin(), availableSuggestions.value.end(), suggestion) ==
        availableSuggestions.value.end()) {
        outcome.hresult = E_INVALIDARG;
        outcome.message = L"The selected suggestion is no longer available.";
        return outcome;
    }
    if (!IsSpellingTargetCurrent(target, currentText, outcome.message)) {
        outcome.hresult = E_ABORT;
        return outcome;
    }

    if (largeFileView_ != nullptr) {
        std::wstring error;
        if (!largeFileView_->SetSelection(target.start, target.start + target.length, error)) {
            outcome.message = error.empty() ? L"The spelling target could not be selected." : error;
            outcome.hresult = E_FAIL;
            return outcome;
        }
        if (injectMappedInsertFailureForTesting) {
            largeFileView_->InjectNextInsertFailureForTesting(
                L"Injected mapped insertion failure for spelling action verification.");
        }
        if (!largeFileView_->InsertText(suggestion, error)) {
            outcome.message = error.empty() ? L"The suggestion could not be inserted." : error;
            outcome.hresult = E_FAIL;
            outcome.verification = L"unchanged";
            return outcome;
        }
        document_.SetModified(true);
        UpdateTitle();
        UpdateStatusBar();
        outcome.changed = true;
        std::wstring insertedText;
        std::wstring readError;
        if (!ReadCurrentSpellingTextRange(target.start, suggestion.size(), insertedText, readError)) {
            outcome.status = SpellOperationStatus::Failure;
            outcome.hresult = E_FAIL;
            outcome.verification = L"unavailable";
            outcome.message = L"The mapped document changed, but the spelling replacement could not be verified. Inspect the text before using Undo.";
            if (!readError.empty()) outcome.message += L" " + readError;
            RefreshSpellCheck(false);
            return outcome;
        }
        if (insertedText != suggestion) {
            outcome.status = SpellOperationStatus::Failure;
            outcome.hresult = E_FAIL;
            outcome.partial = true;
            outcome.verification = L"mismatch";
            outcome.message = L"The mapped document changed, but the inserted spelling text did not match the suggestion. Inspect the text before using Undo.";
            RefreshSpellCheck(false);
            return outcome;
        }
        outcome.status = SpellOperationStatus::Success;
        outcome.hresult = S_OK;
        outcome.verification = L"verified";
        outcome.message.clear();
        RefreshSpellCheck(false);
        return outcome;
    } else {
        const int oldLength = GetWindowTextLengthW(editor_);
        const DWORD nativeLimit = static_cast<DWORD>(SendMessageW(editor_, EM_GETLIMITTEXT, 0, 0));
        if (oldLength < 0 || target.start + target.length > static_cast<std::uint64_t>(oldLength) ||
            target.start + target.length > static_cast<std::uint64_t>((std::numeric_limits<LONG>::max)())) {
            outcome.hresult = E_BOUNDS;
            outcome.message = L"The native spelling target is outside the editor text range.";
            return outcome;
        }
        const std::uint64_t finalLength = static_cast<std::uint64_t>(oldLength) - target.length + suggestion.size();
        if (finalLength > nativeLimit || finalLength > static_cast<std::uint64_t>((std::numeric_limits<LONG>::max)())) {
            outcome.hresult = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
            outcome.message = L"The suggestion exceeds the native editor text limit.";
            return outcome;
        }
        const DWORD start = static_cast<DWORD>(target.start);
        const DWORD end = static_cast<DWORD>(target.start + target.length);
        SendMessageW(editor_, EM_SETSEL, static_cast<WPARAM>(start), static_cast<LPARAM>(end));

        const bool injectUnchangedNativeFailure = automationMode_ && automationPasteFaultStage_ == L"native-errspace";
        const bool injectPartialNativeFailure = automationMode_ &&
            (automationPasteFaultStage_ == L"native-maxtext" ||
                automationPasteFaultStage_ == L"native-errspace-changed");
        const bool injectVerificationFailure = automationMode_ &&
            automationPasteFaultStage_ == L"native-spell-verification";
        editorPasteChangeNotification_ = false;
        editorPasteNativeFailureNotification_ = 0U;
        if (injectUnchangedNativeFailure) {
            ScopedBoolRestore replacementActive(editorPasteReplacementActive_, true);
            SendMessageW(mainWindow_, WM_COMMAND, MAKEWPARAM(0, EN_ERRSPACE), reinterpret_cast<LPARAM>(editor_));
        } else {
            if (injectPartialNativeFailure) {
                const std::uint64_t injectedLimit = finalLength == 0U ? 0U : finalLength - 1U;
                SendMessageW(editor_, EM_SETLIMITTEXT, static_cast<WPARAM>(injectedLimit), 0);
            }
            {
                ScopedBoolRestore replacementActive(editorPasteReplacementActive_, true);
                SendMessageW(editor_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(suggestion.c_str()));
                if (automationMode_ && automationPasteFaultStage_ == L"native-errspace-changed") {
                    SendMessageW(mainWindow_, WM_COMMAND,
                        MAKEWPARAM(0, EN_ERRSPACE), reinterpret_cast<LPARAM>(editor_));
                }
            }
            if (injectPartialNativeFailure) {
                SendMessageW(editor_, EM_SETLIMITTEXT, static_cast<WPARAM>(nativeLimit), 0);
            }
        }

        outcome.nativeNotification = editorPasteNativeFailureNotification_;
        const int actualLength = GetWindowTextLengthW(editor_);
        DWORD actualSelectionStart = 0U;
        DWORD actualSelectionEnd = 0U;
        GetSelectionRange(actualSelectionStart, actualSelectionEnd);
        const std::uint64_t expectedCaret = target.start + suggestion.size();

        std::wstring actualTargetText;
        std::wstring insertedText;
        std::wstring readError;
        const bool allowVerification = !injectVerificationFailure;
        const bool originalRangeReadable = allowVerification && ReadCurrentSpellingTextRange(
            target.start, currentText.size(), actualTargetText, readError);
        const bool insertedRangeReadable = allowVerification &&
            target.start <= static_cast<std::uint64_t>((std::max)(0, actualLength)) &&
            suggestion.size() <= static_cast<std::uint64_t>((std::max)(0, actualLength)) - target.start &&
            ReadCurrentSpellingTextRange(target.start, suggestion.size(), insertedText, readError);

        const bool changeNotification = editorPasteChangeNotification_;
        const bool lengthChanged = actualLength >= 0 && actualLength != oldLength;
        const bool targetChanged = originalRangeReadable && actualTargetText != currentText;
        outcome.changed = changeNotification || lengthChanged || targetChanged;

        const bool exactText = insertedRangeReadable && insertedText == suggestion;
        const bool exactPostconditions = actualLength >= 0 &&
            static_cast<std::uint64_t>(actualLength) == finalLength &&
            actualSelectionStart == expectedCaret && actualSelectionEnd == expectedCaret && exactText;
        bool partialPrefixVerified = false;
        if (allowVerification && actualSelectionStart == actualSelectionEnd &&
            actualSelectionStart > start && actualSelectionStart < expectedCaret) {
            const std::size_t acceptedUnits = static_cast<std::size_t>(actualSelectionStart - start);
            std::wstring acceptedText;
            std::wstring acceptedReadError;
            if (acceptedUnits <= suggestion.size() &&
                ReadCurrentSpellingTextRange(target.start, acceptedUnits, acceptedText, acceptedReadError) &&
                acceptedText == suggestion.substr(0U, acceptedUnits)) {
                partialPrefixVerified = true;
            }
        }

        if (outcome.changed) {
            UpdateEditorWidthIndexForMutation();
            HandleEditorChanged();
        }

        const bool verificationUnavailable = !allowVerification ||
            (!originalRangeReadable && !insertedRangeReadable);
        if (verificationUnavailable) {
            outcome.status = SpellOperationStatus::Failure;
            outcome.hresult = E_FAIL;
            outcome.verification = L"unavailable";
            outcome.message = outcome.changed
                ? L"The native editor may have changed the document, but the spelling replacement could not be verified. Inspect the text; Undo availability is unknown."
                : L"The native editor replacement could not be verified, so the document state is unknown.";
            if (!readError.empty()) outcome.message += L" " + readError;
            if (outcome.changed) RefreshSpellCheck(false);
            return outcome;
        }

        if (exactPostconditions) {
            if (outcome.nativeNotification == 0U) {
                outcome.status = SpellOperationStatus::Success;
                outcome.hresult = S_OK;
                outcome.verification = L"verified";
                outcome.message.clear();
            } else {
                outcome.status = SpellOperationStatus::Failure;
                outcome.hresult = outcome.nativeNotification == EN_ERRSPACE
                    ? HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY)
                    : HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
                outcome.verification = L"verified-with-notification";
                outcome.message = outcome.nativeNotification == EN_ERRSPACE
                    ? L"The native editor reported EN_ERRSPACE even though the complete replacement text, length, and caret were verified."
                    : L"The native editor reported EN_MAXTEXT even though the complete replacement text, length, and caret were verified.";
            }
            RefreshSpellCheck(false);
            return outcome;
        }

        const bool unchangedVerified = !outcome.changed && actualLength == oldLength &&
            originalRangeReadable && actualTargetText == currentText;
        if (unchangedVerified) {
            outcome.status = SpellOperationStatus::Failure;
            outcome.hresult = outcome.nativeNotification == 0U ? E_FAIL :
                (outcome.nativeNotification == EN_ERRSPACE
                    ? HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY)
                    : HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
            outcome.verification = L"unchanged";
            if (outcome.nativeNotification == EN_ERRSPACE) {
                outcome.message = L"The native editor reported EN_ERRSPACE and left the document text unchanged. The prior Undo state is preserved.";
            } else if (outcome.nativeNotification == EN_MAXTEXT) {
                outcome.message = L"The native editor reported EN_MAXTEXT and left the document text unchanged. The prior Undo state is preserved.";
            } else {
                outcome.message = L"The native editor did not apply the suggestion; the document text is unchanged.";
            }
            return outcome;
        }

        outcome.status = SpellOperationStatus::Failure;
        outcome.hresult = outcome.nativeNotification == EN_ERRSPACE
            ? HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY)
            : (outcome.nativeNotification == EN_MAXTEXT
                ? HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW)
                : E_FAIL);
        outcome.partial = partialPrefixVerified;
        outcome.verification = partialPrefixVerified ? L"partial" : L"mismatch";
        if (partialPrefixVerified) {
            outcome.message = L"The native editor reported a rejection after inserting only part of the suggestion. The document changed; Undo availability was not verified.";
        } else if (outcome.nativeNotification == EN_ERRSPACE) {
            outcome.message = L"The native editor reported EN_ERRSPACE, but the replacement postconditions did not match. Inspect the text; Undo availability is unknown.";
        } else if (outcome.nativeNotification == EN_MAXTEXT) {
            outcome.message = L"The native editor reported EN_MAXTEXT, but the replacement postconditions did not match. Inspect the text; Undo availability is unknown.";
        } else {
            outcome.message = L"The native editor changed text, but the expected replacement text, length, or caret position did not match. Inspect the text before using Undo.";
        }
        if (!readError.empty()) outcome.message += L" " + readError;
        if (outcome.changed) RefreshSpellCheck(false);
        return outcome;
    }
}

ClassicNotepadApp::AutomationSpellingActionResult ClassicNotepadApp::AutomationApplySpellingAction(
    const std::wstring& action,
    const classic_notepad::SpellingContextTarget& target,
    const std::wstring& suggestion,
    bool injectMappedInsertFailureForTesting)
{
    if (!automationMode_) {
        AutomationSpellingActionResult outcome;
        outcome.status = SpellOperationStatus::Failure;
        outcome.hresult = E_ACCESSDENIED;
        outcome.message = L"Spelling action diagnostics are available only in automation mode.";
        return outcome;
    }
    if (injectMappedInsertFailureForTesting && largeFileView_ == nullptr) {
        AutomationSpellingActionResult outcome;
        outcome.status = SpellOperationStatus::Failure;
        outcome.hresult = E_INVALIDARG;
        outcome.message = L"The mapped insertion failure probe requires the mapped editor.";
        return outcome;
    }
    return ApplySpellingAction(action, target, suggestion, injectMappedInsertFailureForTesting);
}

bool ClassicNotepadApp::AutomationConfigureSpellingTestBackend(
    const SpellTestBackendConfiguration& configuration)
{
    if (!automationMode_) return false;
    spellChecker_.ConfigureTestBackend(configuration);
    spellCheckAvailable_ = spellChecker_.IsAvailable();
    ClearIgnoredSpellingOccurrences();
    RefreshSpellCheck(true);
    return true;
}

bool ClassicNotepadApp::AutomationClearSpellingTestBackend()
{
    if (!automationMode_) return false;
    spellChecker_.ClearTestBackend();
    spellCheckAvailable_ = spellChecker_.IsAvailable();
    ClearIgnoredSpellingOccurrences();
    RefreshSpellCheck(true);
    return true;
}

std::uint64_t ClassicNotepadApp::AutomationSpellingTestBackendCallCount(const wchar_t* operation) const
{
    return automationMode_ ? spellChecker_.TestBackendCallCount(operation) : 0U;
}

classic_notepad::SpellingAnnotationBatch ClassicNotepadApp::AutomationGetSpellingAnnotationBatch() const
{
    if (!automationMode_ || !classic_notepad::SpellingBatchIsCurrent(spellingBatch_, CurrentSpellingRevision())) {
        return {};
    }
    return spellingBatch_;
}

bool ClassicNotepadApp::AutomationRefreshSpellCheckNow()
{
    if (!automationMode_) return false;
    RunSpellCheckNow();
    return true;
}

void ClassicNotepadApp::DrawSpellingUnderlines(HWND editorWindow)
{
    if (!spellCheckAvailable_ || editorWindow == nullptr ||
        !classic_notepad::SpellingBatchIsCurrent(spellingBatch_, CurrentSpellingRevision()) ||
        spellingBatch_.annotations.empty()) {
        return;
    }

    HDC deviceContext = GetDC(editorWindow);
    if (deviceContext == nullptr) {
        return;
    }

    HGDIOBJ previousFont = nullptr;
    if (editorFont_ != nullptr) {
        previousFont = SelectObject(deviceContext, editorFont_);
    }

    SetBkMode(deviceContext, TRANSPARENT);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(220, 0, 0));
    HGDIOBJ previousPen = pen != nullptr ? SelectObject(deviceContext, pen) : nullptr;

    RECT clientRect{};
    GetClientRect(editorWindow, &clientRect);
    if (IsRectEmpty(&clientRect)) {
        if (previousPen != nullptr) {
            SelectObject(deviceContext, previousPen);
        }
        if (pen != nullptr) {
            DeleteObject(pen);
        }
        if (previousFont != nullptr) {
            SelectObject(deviceContext, previousFont);
        }
        ReleaseDC(editorWindow, deviceContext);
        return;
    }

    HRGN clipRegion = CreateRectRgn(
        clientRect.left,
        clientRect.top,
        clientRect.right,
        clientRect.bottom);
    if (clipRegion != nullptr) {
        SelectClipRgn(deviceContext, clipRegion);
    }

    TEXTMETRICW textMetrics{};
    GetTextMetricsW(deviceContext, &textMetrics);
    const int lineHeight = std::max(1, static_cast<int>(textMetrics.tmHeight + textMetrics.tmExternalLeading));
    const int firstVisibleLine = static_cast<int>(SendMessageW(editorWindow, EM_GETFIRSTVISIBLELINE, 0, 0));
    const int clientHeight = static_cast<int>(clientRect.bottom - clientRect.top);
    const int visibleLineCount = std::max(1, (clientHeight + lineHeight - 1) / lineHeight + 1);
    const int lastVisibleLine = firstVisibleLine + visibleLineCount;

    for (const classic_notepad::SpellingAnnotation& error : spellingBatch_.annotations) {
        if (error.length == 0U || error.start > (std::numeric_limits<DWORD>::max)() ||
            error.length > (std::numeric_limits<DWORD>::max)() - error.start) {
            continue;
        }

        const DWORD errorStart = static_cast<DWORD>(error.start);
        const DWORD errorEnd = static_cast<DWORD>(error.start + error.length);
        const int startLine = static_cast<int>(SendMessageW(editorWindow, EM_LINEFROMCHAR, errorStart, 0));
        const int endLine = static_cast<int>(SendMessageW(editorWindow, EM_LINEFROMCHAR, errorEnd, 0));
        if (endLine < firstVisibleLine || startLine > lastVisibleLine) {
            continue;
        }

        for (int line = std::max(startLine, firstVisibleLine); line <= std::min(endLine, lastVisibleLine); ++line) {
            const LRESULT lineStartResult = SendMessageW(editorWindow, EM_LINEINDEX, static_cast<WPARAM>(line), 0);
            if (lineStartResult < 0) {
                continue;
            }

            const DWORD lineStart = static_cast<DWORD>(lineStartResult);
            const LRESULT lineLengthResult = SendMessageW(editorWindow, EM_LINELENGTH, lineStart, 0);
            if (lineLengthResult < 0 || static_cast<std::uint64_t>(lineLengthResult) >
                static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)() - lineStart)) continue;
            const DWORD lineEnd = lineStart + static_cast<DWORD>(lineLengthResult);

            const DWORD segmentStart = std::max<DWORD>(lineStart, static_cast<DWORD>(error.start));
            const DWORD segmentEnd = std::min(lineEnd, errorEnd);
            if (segmentEnd <= segmentStart) {
                continue;
            }

            const POINT startPoint = PointFromEditorCharPosition(editorWindow, segmentStart);
            const POINT endPoint = PointFromEditorCharPosition(editorWindow, segmentEnd);
            if (startPoint.x < 0 || startPoint.y < 0 || endPoint.x < 0 || endPoint.y < 0) {
                continue;
            }

            const int baselineOffset = std::max(1, static_cast<int>(textMetrics.tmAscent + 1));
            const int y = startPoint.y + baselineOffset;
            if (y - 2 < clientRect.top || y + 2 >= clientRect.bottom) {
                continue;
            }

            int x = std::max(startPoint.x, clientRect.left);
            const int endX = std::min(std::max(startPoint.x + 1, endPoint.x), clientRect.right);
            bool up = true;
            while (x < endX) {
                const int nextX = std::min(x + 4, endX);
                MoveToEx(deviceContext, x, y + (up ? 1 : -1), nullptr);
                LineTo(deviceContext, nextX, y + (up ? -1 : 1));
                up = !up;
                x = nextX;
            }
        }
    }

    if (clipRegion != nullptr) {
        SelectClipRgn(deviceContext, nullptr);
        DeleteObject(clipRegion);
    }
    if (previousPen != nullptr) {
        SelectObject(deviceContext, previousPen);
    }
    if (pen != nullptr) {
        DeleteObject(pen);
    }
    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }

    ReleaseDC(editorWindow, deviceContext);
}

void ClassicNotepadApp::ShowSpellingUnavailableMessage()
{
    if (spellCheckMessageShown_) {
        return;
    }

    spellCheckMessageShown_ = true;
    ShowMessageDialog(
        L"British English spell checking is not installed for Windows.",
        MB_OK | MB_ICONINFORMATION);
}

bool ClassicNotepadApp::HandleLargeFileContextMenu(
    POINT screenPoint,
    const classic_notepad::SpellingContextTarget& target)
{
    if (largeFileView_ == nullptr || !spellCheckAvailable_ ||
        !classic_notepad::SpellingBatchIsCurrent(spellingBatch_, CurrentSpellingRevision()) ||
        !classic_notepad::SpellingRevisionMatches(target.revision, spellingBatch_.revision) ||
        target.length == 0U || target.length >
            (std::numeric_limits<std::uint64_t>::max)() - target.start) return false;
    const std::uint64_t wordStart = target.start;
    const std::uint64_t wordEnd = target.start + target.length;
    bool misspelled = false;
    for (const classic_notepad::SpellingAnnotation& issue : spellingBatch_.annotations) {
        if (issue.length > (std::numeric_limits<std::uint64_t>::max)() - issue.start) return false;
        const std::uint64_t issueEnd = issue.start + issue.length;
        if (issue.start < wordEnd && issueEnd > wordStart) { misspelled = true; break; }
    }
    if (!misspelled) return false;

    const std::wstring& word = target.text;
    const SpellSuggestResult suggestionResult = spellChecker_.Suggest(word, 5U);
    const std::vector<std::wstring>& suggestions = suggestionResult.value;
    HMENU popup = CreatePopupMenu();
    if (popup == nullptr) return true;
    UINT commandId = kSpellMenuSuggestionBase;
    if (suggestionResult.status == SpellOperationStatus::Success && !suggestions.empty()) {
        for (const std::wstring& suggestion : suggestions) AppendMenuW(popup, MF_STRING, commandId++, suggestion.c_str());
    } else {
        const std::wstring statusText = suggestionResult.status == SpellOperationStatus::Success
            ? L"No spelling suggestions"
            : suggestionResult.status == SpellOperationStatus::Unavailable
                ? L"Spelling suggestions unavailable"
                : L"Could not get spelling suggestions (" + FormatSpellHRESULT(suggestionResult.hresult) + L")";
        AppendMenuW(popup, MF_STRING | MF_GRAYED, kSpellMenuNoSuggestions, statusText.c_str());
    }
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, kSpellMenuIgnoreOnce, L"Ignore Once");
    AppendMenuW(popup, MF_STRING, kSpellMenuAddToDictionary, L"Add to Dictionary");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, ID_EDIT_UNDO, L"Undo");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, ID_EDIT_CUT, L"Cut");
    AppendMenuW(popup, MF_STRING, ID_EDIT_COPY, L"Copy");
    AppendMenuW(popup, MF_STRING, ID_EDIT_PASTE, L"Paste");
    AppendMenuW(popup, MF_STRING, ID_EDIT_DELETE, L"Delete");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, ID_EDIT_SELECT_ALL, L"Select All");
    const UINT selected = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, 0, mainWindow_, nullptr);
    std::wstring action;
    std::wstring replacement;
    if (selected >= kSpellMenuSuggestionBase &&
        selected < kSpellMenuSuggestionBase + static_cast<UINT>(suggestions.size())) {
        action = L"replaceSuggestion";
        replacement = suggestions[selected - kSpellMenuSuggestionBase];
    } else if (selected == kSpellMenuIgnoreOnce) action = L"ignoreOnce";
    else if (selected == kSpellMenuAddToDictionary) action = L"addToDictionary";
    DestroyMenu(popup);
    if (!action.empty()) {
        const AutomationSpellingActionResult result = ApplySpellingAction(action, target, replacement);
        if (result.status != SpellOperationStatus::Success && !automationMode_) ShowError(result.message);
    } else if (selected != 0U) {
        SendMessageW(mainWindow_, WM_COMMAND, MAKEWPARAM(selected, 0), 0);
    }
    return true;
}

bool ClassicNotepadApp::HandleEditorContextMenu(HWND editorWindow, LPARAM lParam)
{
    if (editorWindow == nullptr) {
        return false;
    }

    const bool keyboardInvocation = lParam == -1;
    POINT screenPoint{};
    POINT clientPoint{};
    if (keyboardInvocation) {
        if (!GetCaretPos(&clientPoint)) {
            DWORD selectionStart = 0U;
            DWORD selectionEnd = 0U;
            GetSelectionRange(selectionStart, selectionEnd);
            clientPoint = PointFromEditorCharPosition(editorWindow, selectionEnd);
        }
        screenPoint = clientPoint;
        ClientToScreen(editorWindow, &screenPoint);
    } else {
        screenPoint = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        clientPoint = screenPoint;
        ScreenToClient(editorWindow, &clientPoint);
    }

    classic_notepad::SpellingContextTarget contextTarget;
    bool hasWord = ResolveEditorSpellingContextTarget(
        clientPoint, keyboardInvocation, contextTarget) &&
        contextTarget.start <= (std::numeric_limits<DWORD>::max)() &&
        contextTarget.length <= (std::numeric_limits<DWORD>::max)();
    bool wordMisspelled = false;
    if (hasWord) {
        if (contextTarget.length >
            (std::numeric_limits<std::uint64_t>::max)() - contextTarget.start) {
            hasWord = false;
        } else {
            const std::uint64_t wordEnd = contextTarget.start + contextTarget.length;
            for (const classic_notepad::SpellingAnnotation& error : spellingBatch_.annotations) {
                if (error.length > (std::numeric_limits<std::uint64_t>::max)() - error.start) {
                    hasWord = false;
                    wordMisspelled = false;
                    break;
                }
                const std::uint64_t errorEnd = error.start + error.length;
                if (error.length != 0U && contextTarget.start < errorEnd && error.start < wordEnd) {
                    wordMisspelled = true;
                    break;
                }
            }
        }
    }

    HMENU popup = CreatePopupMenu();
    if (popup == nullptr) {
        return true;
    }
    std::vector<std::unique_ptr<OwnerDrawMenuItem>> contextMenuItems;
    ApplyMenuBackground(popup);

    SpellSuggestResult suggestionResult;
    std::vector<std::wstring> suggestions;
    if (spellCheckAvailable_ && wordMisspelled) {
        suggestionResult = spellChecker_.Suggest(contextTarget.text, 5U);
        suggestions = suggestionResult.value;
        if (suggestionResult.status != SpellOperationStatus::Success || suggestions.empty()) {
            const std::wstring statusText = suggestionResult.status == SpellOperationStatus::Success
                ? L"No spelling suggestions"
                : suggestionResult.status == SpellOperationStatus::Unavailable
                    ? L"Spelling suggestions unavailable"
                    : L"Could not get spelling suggestions (" +
                        FormatSpellHRESULT(suggestionResult.hresult) + L")";
            AppendOwnerDrawMenuItem(
                popup,
                MF_GRAYED,
                kSpellMenuNoSuggestions,
                statusText,
                contextMenuItems);
        } else {
            UINT commandId = kSpellMenuSuggestionBase;
            for (const std::wstring& suggestion : suggestions) {
                AppendOwnerDrawMenuItem(popup, MF_ENABLED, commandId, suggestion, contextMenuItems);
                ++commandId;
            }
        }

        AppendOwnerDrawMenuSeparator(popup, contextMenuItems);
        AppendOwnerDrawMenuItem(popup, MF_ENABLED, kSpellMenuIgnoreOnce, L"Ignore Once", contextMenuItems);
        AppendOwnerDrawMenuItem(popup, MF_ENABLED, kSpellMenuAddToDictionary, L"Add to Dictionary", contextMenuItems);
        AppendOwnerDrawMenuSeparator(popup, contextMenuItems);
    } else if (!spellCheckAvailable_) {
        AppendOwnerDrawMenuItem(
            popup,
            MF_GRAYED,
            kSpellMenuNoSuggestions,
            L"British English spell checking is not installed",
            contextMenuItems);
        AppendOwnerDrawMenuSeparator(popup, contextMenuItems);
    }

    const bool hasSelection = HasSelection();
    const bool hasText = GetWindowTextLengthW(editor_) > 0;
    AppendOwnerDrawMenuItem(
        popup,
        SendMessageW(editor_, EM_CANUNDO, 0, 0) != 0 ? MF_ENABLED : MF_GRAYED,
        ID_EDIT_UNDO,
        L"Undo",
        contextMenuItems);
    AppendOwnerDrawMenuSeparator(popup, contextMenuItems);
    AppendOwnerDrawMenuItem(popup, hasSelection ? MF_ENABLED : MF_GRAYED, ID_EDIT_CUT, L"Cut", contextMenuItems);
    AppendOwnerDrawMenuItem(popup, hasSelection ? MF_ENABLED : MF_GRAYED, ID_EDIT_COPY, L"Copy", contextMenuItems);
    AppendOwnerDrawMenuItem(popup, CanPasteText() ? MF_ENABLED : MF_GRAYED, ID_EDIT_PASTE, L"Paste", contextMenuItems);
    AppendOwnerDrawMenuItem(popup, hasSelection ? MF_ENABLED : MF_GRAYED, ID_EDIT_DELETE, L"Delete", contextMenuItems);
    AppendOwnerDrawMenuSeparator(popup, contextMenuItems);
    AppendOwnerDrawMenuItem(popup, hasText ? MF_ENABLED : MF_GRAYED, ID_EDIT_SELECT_ALL, L"Select All", contextMenuItems);

    const UINT selected = TrackPopupMenu(
        popup,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        0,
        mainWindow_,
        nullptr);

    std::wstring action;
    std::wstring replacement;
    if (selected >= kSpellMenuSuggestionBase &&
        selected < kSpellMenuSuggestionBase + static_cast<UINT>(suggestions.size())) {
        action = L"replaceSuggestion";
        replacement = suggestions[selected - kSpellMenuSuggestionBase];
    } else if (selected == kSpellMenuIgnoreOnce) action = L"ignoreOnce";
    else if (selected == kSpellMenuAddToDictionary) action = L"addToDictionary";
    DestroyMenu(popup);
    if (!action.empty()) {
        const AutomationSpellingActionResult result = ApplySpellingAction(action, contextTarget, replacement);
        if (result.status != SpellOperationStatus::Success && !automationMode_) ShowError(result.message);
    } else if (selected != 0) {
        SendMessageW(mainWindow_, WM_COMMAND, MAKEWPARAM(selected, 0), 0);
    }
    return true;
}

void ClassicNotepadApp::ShowAboutDialog()
{
    AboutDialogState state{};
    state.instance = instance_;
    state.darkMode = darkModeEnabled_;

    if (DialogBoxParamW(
            instance_,
            MAKEINTRESOURCEW(IDD_ABOUT_DIALOG),
            mainWindow_,
            ClassicNotepadApp::AboutDialogProc,
            reinterpret_cast<LPARAM>(&state)) == -1) {
        std::wstring message = L"Classic Notepad ";
        message += CLASSIC_NOTEPAD_VERSION_DISPLAY_W;
        message +=
            L"\n\nFinished native Win32 build.\n"
            L"Single-document editor with classic menus, file open/save, find/replace, Go To, word wrap, font selection, status bar, page setup, print, dark mode, and Windows spell checking.\n\n"
            L"No tabs, cloud features, telemetry, or modern editor extras.";

        ShowMessageDialog(message, MB_OK | MB_ICONINFORMATION);
    }
}

void ClassicNotepadApp::HandleInitialFilePath(const std::wstring& path)
{
    if (IsMissingFilePath(path)) {
        if (ConfirmCreateMissingFile(path)) {
            CreateNewDocumentForPath(path);
        } else {
            UpdateTitle();
            SetFocus(editor_);
        }
        return;
    }

    LoadDocument(path);
}

void ClassicNotepadApp::CreateNewDocumentForPath(const std::wstring& path)
{
    AdvanceSpellingSession();
    ExitLargeFileMode();
    document_.ResetNewFile(path);
    SetEditorText(L"");
    UpdateTitle();
    SetFocus(editor_);
}

bool ClassicNotepadApp::ConfirmCreateMissingFile(const std::wstring& path) const
{
    std::wstring prompt = L"Cannot find the ";
    prompt += path;
    prompt += L" file.\n\nDo you want to create a new file?";

    return ShowMessageDialog(prompt, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) == IDYES;
}

void ClassicNotepadApp::HandleEditorChanged()
{
    if (suppressEditorChange_) {
        return;
    }

    statusCharacterCountDirty_ = true;
    AdvanceSpellingContentRevision();
    UpdateStatusBar();
    UpdateScrollBars();
    RefreshSpellCheck(false);

    if (!document_.IsModified()) {
        document_.SetModified(true);
        UpdateTitle();
    }
}

void ClassicNotepadApp::HandleNew()
{
    RequestDocumentTransition(DocumentTransition::NewDocument);
}

void ClassicNotepadApp::HandleOpen()
{
    RequestDocumentTransition(DocumentTransition::OpenDialog);
}

bool ClassicNotepadApp::CurrentDocumentIsModified() const
{
    if (largeFileView_ != nullptr) {
        return largeFileView_->GetInfo().modified;
    }
    return document_.IsModified() || (editor_ != nullptr &&
        SendMessageW(editor_, EM_GETMODIFY, 0, 0) != 0);
}

void ClassicNotepadApp::RequestDocumentTransition(DocumentTransition transition)
{
    if (transition == DocumentTransition::None || documentTransitionCoordinator_.Pending().IsPending()) {
        return;
    }

    LargeFileViewInfo largeInfo;
    if (largeFileView_ != nullptr) {
        largeInfo = largeFileView_->GetInfo();
        if (largeInfo.operationInProgress) {
            ShowError(L"Wait for the current large-document " + largeInfo.operationName + L" operation to finish before changing documents.");
            return;
        }
    }

    if (!CurrentDocumentIsModified()) {
        PerformDocumentTransition(transition);
        return;
    }

    std::wstring prompt = L"Do you want to save changes to ";
    prompt += document_.DisplayName();
    prompt += L"?";
    const int result = ShowMessageDialog(prompt, MB_YESNOCANCEL | MB_ICONWARNING);
    if (result == IDNO) {
        PerformDocumentTransition(transition);
        return;
    }
    if (result != IDYES || !HandleSave()) {
        return;
    }

    if (largeFileView_ == nullptr) {
        // The ordinary editor writes synchronously, so HandleSave returning
        // true means the bytes are already on disk.
        PerformDocumentTransition(transition);
        return;
    }

    largeInfo = largeFileView_->GetInfo();
    const classic_notepad::TransitionRequestSaveDisposition saveDisposition =
        classic_notepad::ClassifyTransitionRequestSave(MakeTransitionSaveObservation(largeInfo));
    if (saveDisposition == classic_notepad::TransitionRequestSaveDisposition::WaitForSave) {
        if (!documentTransitionCoordinator_.TryQueue(transition, largeInfo.operationToken)) {
            ShowError(L"The save is still running, but its completion could not be associated with this document transition.");
        }
        return;
    }

    // A fast worker may finish before the transition request observes it.
    // Consult its completion result instead of treating the accepted start as
    // a completed save.
    if (saveDisposition == classic_notepad::TransitionRequestSaveDisposition::Saved) {
        PerformDocumentTransition(transition);
        return;
    }
    if (saveDisposition == classic_notepad::TransitionRequestSaveDisposition::SaveFailed) {
        ShowError(largeInfo.operationError.empty() ? L"The large document could not be saved. The current document is still open."
            : largeInfo.operationError);
    }
}

void ClassicNotepadApp::PerformDocumentTransition(DocumentTransition transition)
{
    switch (transition) {
    case DocumentTransition::NewDocument:
        ExitLargeFileMode();
        document_.ResetUntitled();
        SetEditorText(L"");
        UpdateTitle();
        SetFocus(editor_);
        break;
    case DocumentTransition::OpenDialog: {
        const std::wstring path = ShowOpenFileDialog();
        if (!path.empty()) LoadDocument(path);
        break;
    }
    case DocumentTransition::CloseWindow:
        if (mainWindow_ != nullptr) DestroyWindow(mainWindow_);
        break;
    case DocumentTransition::None:
        break;
    }
}

bool ClassicNotepadApp::ResumePendingTransitionAfterLargeSave(const LargeFileViewInfo& info)
{
    const classic_notepad::PendingDocumentTransition pending = documentTransitionCoordinator_.Pending();
    const classic_notepad::PendingTransitionCompletion completion =
        classic_notepad::ClassifyPendingTransitionCompletion(pending, MakeTransitionSaveObservation(info));
    if (completion == classic_notepad::PendingTransitionCompletion::NotReady) {
        return false;
    }

    const DocumentTransition transition = documentTransitionCoordinator_.TakePending().intent;
    if (completion == classic_notepad::PendingTransitionCompletion::PerformTransition) {
        PerformDocumentTransition(transition);
    } else if (completion == classic_notepad::PendingTransitionCompletion::DocumentChangedDuringSave) {
        ShowError(L"The document changed while it was being saved. The newer changes remain open and unsaved.");
    } else {
        ShowError(info.operationError.empty() ? L"The large document could not be saved. The current document is still open."
            : info.operationError);
    }
    return true;
}

bool ClassicNotepadApp::HandleSave()
{
    if (IsLargeFileMode()) {
        if (!document_.HasPath()) return HandleSaveAs();
        std::wstring errorMessage;
        if (!largeFileView_->StartSaveAsync(document_.Path(), document_.Encoding(), document_.SaveLineEnding(), true, errorMessage)) {
            ShowError(errorMessage);
            return false;
        }
        UpdateStatusBar();
        return true;
    }
    if (!document_.HasPath()) {
        return HandleSaveAs();
    }

    std::wstring errorMessage;
    if (!document_.Save(GetEditorText(), errorMessage)) {
        ShowError(errorMessage);
        return false;
    }

    SendMessageW(editor_, EM_SETMODIFY, FALSE, 0);
    UpdateTitle();
    return true;
}

bool ClassicNotepadApp::HandleSaveAs()
{
    if (IsLargeFileMode()) {
        const std::wstring path = ShowSaveFileDialog();
        if (path.empty()) return false;
        std::wstring errorMessage;
        if (!largeFileView_->StartSaveAsync(path, document_.Encoding(), document_.SaveLineEnding(), true, errorMessage)) {
            ShowError(errorMessage);
            return false;
        }
        UpdateStatusBar();
        return true;
    }
    const std::wstring path = ShowSaveFileDialog();
    if (path.empty()) {
        return false;
    }

    std::wstring errorMessage;
    if (!document_.SaveAs(path, GetEditorText(), errorMessage)) {
        ShowError(errorMessage);
        return false;
    }

    SendMessageW(editor_, EM_SETMODIFY, FALSE, 0);
    UpdateTitle();
    return true;
}

void ClassicNotepadApp::HandlePageSetup()
{
    PAGESETUPDLGW pageSetup{};
    pageSetup.lStructSize = sizeof(pageSetup);
    pageSetup.hwndOwner = mainWindow_;
    pageSetup.hDevMode = pageSetupDevMode_;
    pageSetup.hDevNames = pageSetupDevNames_;
    pageSetup.Flags = PSD_INTHOUSANDTHSOFINCHES | PSD_MARGINS;
    pageSetup.rtMargin = pageMarginsThousandths_;

    if (PageSetupDlgW(&pageSetup)) {
        if (pageSetupDevMode_ != nullptr && pageSetupDevMode_ != pageSetup.hDevMode) {
            GlobalFree(pageSetupDevMode_);
        }
        if (pageSetupDevNames_ != nullptr && pageSetupDevNames_ != pageSetup.hDevNames) {
            GlobalFree(pageSetupDevNames_);
        }

        pageSetupDevMode_ = pageSetup.hDevMode;
        pageSetupDevNames_ = pageSetup.hDevNames;
        pageMarginsThousandths_ = pageSetup.rtMargin;
        SetFocus(editor_);
        return;
    }

    const DWORD dialogError = CommDlgExtendedError();
    if (dialogError == 0) {
        SetFocus(editor_);
        return;
    }

    if (dialogError == PDERR_NODEFAULTPRN || dialogError == PDERR_PRINTERNOTFOUND) {
        ShowError(L"No printer is available.");
    } else {
        ShowError(L"The Page Setup dialog could not be shown.");
    }

    SetFocus(editor_);
}

void ClassicNotepadApp::HandlePrint()
{
    if (automationMode_) {
        automationPrintDiagnostics_ = {};
    }
    if (IsLargeFileMode()) {
        ShowLargeFileReadOnlyMessage();
        return;
    }
    PRINTDLGW printDialog{};
    printDialog.lStructSize = sizeof(printDialog);
    printDialog.hwndOwner = mainWindow_;
    printDialog.hDevMode = pageSetupDevMode_;
    printDialog.hDevNames = pageSetupDevNames_;
    printDialog.Flags = PD_ALLPAGES | PD_NOSELECTION | PD_NOPAGENUMS |
        PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE;
    printDialog.nCopies = 1;
    printDialog.nFromPage = 1;
    printDialog.nToPage = 1;
    printDialog.nMinPage = 1;
    printDialog.nMaxPage = 1;

    if (!PrintDlgW(&printDialog)) {
        const DWORD dialogError = CommDlgExtendedError();
        if (dialogError == PDERR_NODEFAULTPRN || dialogError == PDERR_PRINTERNOTFOUND) {
            ShowError(L"No printer is available.");
        } else if (dialogError != 0) {
            ShowError(L"The Print dialog could not be shown.");
        }

        SetFocus(editor_);
        return;
    }

    if (pageSetupDevMode_ != nullptr && pageSetupDevMode_ != printDialog.hDevMode) {
        GlobalFree(pageSetupDevMode_);
    }
    if (pageSetupDevNames_ != nullptr && pageSetupDevNames_ != printDialog.hDevNames) {
        GlobalFree(pageSetupDevNames_);
    }

    pageSetupDevMode_ = printDialog.hDevMode;
    pageSetupDevNames_ = printDialog.hDevNames;

    if (printDialog.hDC == nullptr) {
        ShowError(L"The selected printer could not be opened.");
        SetFocus(editor_);
        return;
    }

    std::wstring errorMessage;
    if (!PrintEditorText(printDialog.hDC, errorMessage)) {
        ShowError(errorMessage);
    } else if (automationMode_) {
        automationPrintDiagnostics_.succeeded = true;
    }

    DeleteDC(printDialog.hDC);
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleUndo()
{
    if (IsLargeFileMode()) {
        std::wstring error;
        if (!largeFileView_->Undo(error) && !error.empty()) ShowError(error);
        return;
    }
    SendMessageW(editor_, EM_UNDO, 0, 0);
    // Some EDIT undo paths do not deliver EN_CHANGE to the parent even though
    // EM_UNDO changed the buffer. Force the incremental character summary to
    // rescan against the current native text length.
    statusCharacterCountDirty_ = true;
    UpdateStatusBar();
    UpdateScrollBars();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleCut()
{
    if (IsLargeFileMode()) {
        std::wstring error;
        if (!largeFileView_->CutSelection(error) && !error.empty()) ShowError(error);
        return;
    }
    SendMessageW(editor_, WM_CUT, 0, 0);
    UpdateStatusBar();
    UpdateScrollBars();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleCopy()
{
    if (IsLargeFileMode()) {
        std::wstring error;
        if (!largeFileView_->CopySelection(error) && !error.empty()) ShowError(error);
        largeFileView_->Focus();
        return;
    }
    SendMessageW(editor_, WM_COPY, 0, 0);
    SetFocus(editor_);
}

void ClassicNotepadApp::HandlePaste()
{
    if (IsLargeFileMode()) {
        ClipboardTextReadResult clipboard = TryReadClipboardText(mainWindow_);
        std::wstring error;
        if (clipboard.outcome == PasteOutcome::Succeeded &&
            !largeFileView_->PasteText(clipboard.text, error) && !error.empty()) ShowError(error);
        return;
    }

    ClipboardTextReadResult clipboard;
    automationPasteClipboardDiagnostics_ = {};
    if (automationPasteTextActive_) {
        try {
            SyntheticClipboardFixture fixture(automationPasteText_, automationPasteFaultStage_);
            clipboard = TryReadClipboardText(mainWindow_, fixture.Api());
            automationPasteClipboardDiagnostics_ = fixture.Diagnostics();
        } catch (const std::bad_alloc&) {
            clipboard.outcome = PasteOutcome::AllocationFailure;
        } catch (const std::length_error&) {
            clipboard.outcome = PasteOutcome::CapacityRejected;
        }
    } else {
        clipboard = TryReadClipboardText(mainWindow_);
    }

    PasteOutcome outcome = clipboard.outcome;
    if (outcome == PasteOutcome::Succeeded && automationPasteFaultStage_ == L"copy") {
        outcome = PasteOutcome::AllocationFailure;
    }
    if (outcome == PasteOutcome::Succeeded && !IsValidClipboardUtf16(clipboard.text)) {
        outcome = PasteOutcome::InvalidData;
    }
    if (outcome == PasteOutcome::Succeeded && clipboard.text.empty()) {
        outcome = PasteOutcome::Empty;
    }

    if (outcome == PasteOutcome::Succeeded) {
        std::size_t normalizedLength = 0U;
        if (!GetNormalizedPasteLength(clipboard.text, normalizedLength)) {
            outcome = PasteOutcome::CapacityRejected;
        } else {
            const int nativeTextLength = GetWindowTextLengthW(editor_);
            DWORD selectionStart = 0;
            DWORD selectionEnd = 0;
            GetSelectionRange(selectionStart, selectionEnd);
            if (selectionEnd < selectionStart) std::swap(selectionStart, selectionEnd);

            const LRESULT nativeLimitValue = SendMessageW(editor_, EM_GETLIMITTEXT, 0, 0);
            const std::size_t nativeLimit = nativeLimitValue <= 0
                ? 0U
                : static_cast<std::size_t>(nativeLimitValue);
            const std::size_t representableLimit = std::min(
                nativeLimit,
                static_cast<std::size_t>(std::numeric_limits<int>::max()));
            bool capacityRejected = nativeTextLength < 0;
            std::size_t expectedLength = 0U;
            std::size_t retainedLength = 0U;
            if (!capacityRejected) {
                const std::size_t oldLength = static_cast<std::size_t>(nativeTextLength);
                if (selectionEnd > oldLength || selectionStart > selectionEnd) {
                    capacityRejected = true;
                } else {
                    retainedLength = oldLength - static_cast<std::size_t>(selectionEnd - selectionStart);
                    if (normalizedLength > std::numeric_limits<std::size_t>::max() - retainedLength) {
                        capacityRejected = true;
                    } else {
                        expectedLength = retainedLength + normalizedLength;
                        capacityRejected = expectedLength > representableLimit;
                    }
                }
            }

            if (capacityRejected || automationPasteFaultStage_ == L"capacity") {
                outcome = PasteOutcome::CapacityRejected;
            } else if (automationPasteFaultStage_ == L"normalization") {
                outcome = PasteOutcome::AllocationFailure;
            } else {
                std::wstring normalizedText;
                try {
                    normalizedText = NormalizeClipboardTextForEditor(clipboard.text, normalizedLength);
                } catch (const std::bad_alloc&) {
                    outcome = PasteOutcome::AllocationFailure;
                } catch (const std::length_error&) {
                    outcome = PasteOutcome::AllocationFailure;
                }

                if (outcome == PasteOutcome::Succeeded && normalizedText.size() != normalizedLength) {
                    outcome = PasteOutcome::NativeFailure;
                }

                if (outcome == PasteOutcome::Succeeded && automationPasteFaultStage_ == L"native-replacement") {
                    outcome = PasteOutcome::AllocationFailure;
                } else if (outcome == PasteOutcome::Succeeded && automationPasteFaultStage_ == L"native-errspace") {
                    editorPasteChangeNotification_ = false;
                    editorPasteNativeFailureNotification_ = 0U;
                    {
                        ScopedBoolRestore replacementActive(editorPasteReplacementActive_, true);
                        ScopedBoolRestore suppressChange(suppressEditorChange_, true);
                        SendMessageW(mainWindow_, WM_COMMAND, MAKEWPARAM(0, EN_ERRSPACE), reinterpret_cast<LPARAM>(editor_));
                    }
                    outcome = PasteOutcome::NativeFailure;
                }

                if (outcome == PasteOutcome::Succeeded) {
                    const bool injectPartialNativeFailure = automationPasteTextActive_ &&
                        (automationPasteFaultStage_ == L"native-maxtext" ||
                            automationPasteFaultStage_ == L"native-errspace-changed");
                    if (injectPartialNativeFailure) {
                        const std::size_t injectedLimit = expectedLength == 0U ? 0U : expectedLength - 1U;
                        SendMessageW(editor_, EM_SETLIMITTEXT, static_cast<WPARAM>(injectedLimit), 0);
                    }
                    editorPasteChangeNotification_ = false;
                    editorPasteNativeFailureNotification_ = 0U;
                    {
                        ScopedBoolRestore replacementActive(editorPasteReplacementActive_, true);
                        ScopedBoolRestore suppressChange(suppressEditorChange_, true);
                        SendMessageW(editor_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(normalizedText.c_str()));
                        if (automationPasteTextActive_ && automationPasteFaultStage_ == L"native-errspace-changed") {
                            SendMessageW(
                                mainWindow_, WM_COMMAND, MAKEWPARAM(0, EN_ERRSPACE), reinterpret_cast<LPARAM>(editor_));
                        }
                    }
                    if (injectPartialNativeFailure) {
                        SendMessageW(editor_, EM_SETLIMITTEXT, static_cast<WPARAM>(nativeLimit), 0);
                    }

                    const int actualLength = GetWindowTextLengthW(editor_);
                    DWORD actualSelectionStart = 0;
                    DWORD actualSelectionEnd = 0;
                    GetSelectionRange(actualSelectionStart, actualSelectionEnd);
                    const std::size_t expectedCaret = static_cast<std::size_t>(selectionStart) + normalizedLength;
                    const bool postStateMatches = actualLength >= 0 &&
                        static_cast<std::size_t>(actualLength) == expectedLength &&
                        actualSelectionStart == expectedCaret && actualSelectionEnd == expectedCaret &&
                        editorPasteChangeNotification_;

                    if (editorPasteNativeFailureNotification_ == EN_MAXTEXT) {
                        outcome = editorPasteChangeNotification_
                            ? PasteOutcome::NativePartial
                            : PasteOutcome::CapacityRejected;
                    } else if (editorPasteNativeFailureNotification_ == EN_ERRSPACE) {
                        outcome = editorPasteChangeNotification_
                            ? PasteOutcome::NativePartial
                            : PasteOutcome::NativeFailure;
                    } else if (!postStateMatches) {
                        outcome = editorPasteChangeNotification_
                            ? PasteOutcome::NativePartial
                            : PasteOutcome::NativeFailure;
                    }

                    if (outcome == PasteOutcome::Succeeded) {
                        SendMessageW(editor_, EM_SCROLLCARET, 0, 0);
                        AdvanceSpellingLayoutRevision();
                        if (automationPasteTextActive_) {
                            automationPastePostCaretFirstVisibleLine_ = std::max(0, static_cast<int>(SendMessageW(
                                editor_, EM_GETFIRSTVISIBLELINE, 0, 0)));
                            const LRESULT topContentPosition = SendMessageW(
                                editor_, EM_LINEINDEX, static_cast<WPARAM>(automationPastePostCaretFirstVisibleLine_), 0);
                            automationPastePostCaretTopContentPosition_ = topContentPosition < 0
                                ? 0
                                : static_cast<int>(topContentPosition);
                            automationPastePostCaretHorizontalOffset_ = GetHorizontalAnchorPixels();
                        }
                        UpdateEditorWidthIndexForMutation();
                        HandleEditorChanged();
                    } else if (editorPasteChangeNotification_) {
                        // An unexpected native rejection can leave a partial insertion. Keep app metadata
                        // in sync with the content the control actually accepted; never try to Undo it.
                        UpdateEditorWidthIndexForMutation();
                        HandleEditorChanged();
                    }
                }
            }
        }
    }

    automationPasteLastOutcome_ = PasteOutcomeName(outcome);
    if (!automationMode_ && outcome == PasteOutcome::CapacityRejected) {
        ShowError(L"The clipboard text exceeds the native editor control's current text limit.");
    } else if (!automationMode_ && outcome == PasteOutcome::AllocationFailure) {
        ShowError(L"The clipboard text could not be pasted because memory allocation failed.");
    } else if (!automationMode_ && outcome == PasteOutcome::NativeFailure) {
        ShowError(L"The editor control did not accept the complete clipboard text.");
    } else if (!automationMode_ && outcome == PasteOutcome::NativePartial) {
        ShowError(L"The editor control accepted only part of the clipboard text. The document now contains that partial paste.");
    }

    SetFocus(editor_);
}

void ClassicNotepadApp::HandleDelete()
{
    if (IsLargeFileMode()) {
        std::wstring error;
        if (!largeFileView_->DeleteSelection(false, error) && !error.empty()) ShowError(error);
        return;
    }
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);
    if (selectionStart == selectionEnd) {
        classic_notepad::TextUnitRange range{};
        if (TryGetNativeCollapsedDeleteRange(editor_, selectionStart, false, range) &&
            range.start != range.end) {
            SendMessageW(editor_, EM_SETSEL, range.start, range.end);
        }
    }

    SendMessageW(editor_, WM_CLEAR, 0, 0);
    UpdateStatusBar();
    UpdateScrollBars();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleFind()
{
    if (IsLargeFileMode()) {
        if (findDialog_ != nullptr) { SetFocus(findDialog_); return; }
        if (replaceDialog_ != nullptr) CloseFindReplaceDialogs();
        findReplace_ = {};
        findReplace_.lStructSize = sizeof(findReplace_);
        findReplace_.hwndOwner = mainWindow_;
        findReplace_.lpstrFindWhat = findBuffer_.data();
        findReplace_.wFindWhatLen = static_cast<WORD>(findBuffer_.size());
        findReplace_.Flags = findFlags_;
        findDialog_ = FindTextW(&findReplace_);
        if (findDialog_ == nullptr) ShowError(L"The Find dialog could not be shown.");
        return;
    }
    if (GetWindowTextLengthW(editor_) <= 0) {
        return;
    }

    if (findDialog_ != nullptr) {
        SetFocus(findDialog_);
        return;
    }

    if (replaceDialog_ != nullptr) {
        CloseFindReplaceDialogs();
    }

    SeedFindTextFromSelection();

    findReplace_ = {};
    findReplace_.lStructSize = sizeof(findReplace_);
    findReplace_.hwndOwner = mainWindow_;
    findReplace_.lpstrFindWhat = findBuffer_.data();
    findReplace_.wFindWhatLen = static_cast<WORD>(findBuffer_.size());
    findReplace_.Flags = findFlags_;

    findDialog_ = FindTextW(&findReplace_);
    if (findDialog_ == nullptr) {
        ShowError(L"The Find dialog could not be shown.");
    }
}

void ClassicNotepadApp::HandleFindNext()
{
    if (IsLargeFileMode()) {
        if (findBuffer_[0] == L'\0') { HandleFind(); return; }
        std::wstring error;
        largeFileView_->StartFindAsync(findBuffer_.data(), (findFlags_ & FR_MATCHCASE) != 0,
            (findFlags_ & FR_WHOLEWORD) != 0, (findFlags_ & FR_DOWN) != 0, error);
        if (!error.empty()) ShowError(error);
        return;
    }
    if (findBuffer_[0] == L'\0') {
        HandleFind();
        return;
    }

    FindNextWithFlags(findFlags_, true);
}

void ClassicNotepadApp::HandleReplace()
{
    if (IsLargeFileMode()) {
        if (replaceDialog_ != nullptr) { SetFocus(replaceDialog_); return; }
        if (findDialog_ != nullptr) CloseFindReplaceDialogs();
        findReplace_ = {};
        findReplace_.lStructSize = sizeof(findReplace_);
        findReplace_.hwndOwner = mainWindow_;
        findReplace_.lpstrFindWhat = findBuffer_.data();
        findReplace_.wFindWhatLen = static_cast<WORD>(findBuffer_.size());
        findReplace_.lpstrReplaceWith = replaceBuffer_.data();
        findReplace_.wReplaceWithLen = static_cast<WORD>(replaceBuffer_.size());
        findReplace_.Flags = findFlags_;
        replaceDialog_ = ReplaceTextW(&findReplace_);
        if (replaceDialog_ == nullptr) ShowError(L"The Replace dialog could not be shown.");
        return;
    }
    if (GetWindowTextLengthW(editor_) <= 0) {
        return;
    }

    if (replaceDialog_ != nullptr) {
        SetFocus(replaceDialog_);
        return;
    }

    if (findDialog_ != nullptr) {
        CloseFindReplaceDialogs();
    }

    SeedFindTextFromSelection();

    findReplace_ = {};
    findReplace_.lStructSize = sizeof(findReplace_);
    findReplace_.hwndOwner = mainWindow_;
    findReplace_.lpstrFindWhat = findBuffer_.data();
    findReplace_.wFindWhatLen = static_cast<WORD>(findBuffer_.size());
    findReplace_.lpstrReplaceWith = replaceBuffer_.data();
    findReplace_.wReplaceWithLen = static_cast<WORD>(replaceBuffer_.size());
    findReplace_.Flags = findFlags_;

    replaceDialog_ = ReplaceTextW(&findReplace_);
    if (replaceDialog_ == nullptr) {
        ShowError(L"The Replace dialog could not be shown.");
    }
}

void ClassicNotepadApp::HandleGoTo()
{
    if (IsLargeFileMode()) {
        const LargeFileViewInfo info = largeFileView_->GetInfo();
        const std::uint64_t target = ShowGoToDialog(
            std::max<std::uint64_t>(1U, info.caretLine + 1U),
            std::max<std::uint64_t>(1U, info.totalLines));
        if (target > 0) { std::wstring error; if (!largeFileView_->GoToLine(target, error)) ShowError(error); }
        return;
    }
    if (!EnsureNativeStatusSummaryComplete()) {
        ShowError(L"Logical line information is unavailable for this document.");
        return;
    }
    const DWORD activeCaretPosition = GetActiveCaretPosition();
    std::uint64_t currentLine = 1U;
    std::uint64_t currentColumn = 1U;
    TryGetNativeLogicalCoordinate(static_cast<int>(activeCaretPosition), currentLine, currentColumn);
    const std::uint64_t maxLine = statusLogicalLineBreakCount_ + 1U;
    const std::uint64_t targetLine = ShowGoToDialog(
        currentLine, maxLine);
    if (targetLine > 0) {
        GoToLine(targetLine);
    }
}

void ClassicNotepadApp::HandleSelectAll()
{
    if (IsLargeFileMode()) {
        largeFileView_->SelectAll();
        largeFileView_->Focus();
        return;
    }
    SendMessageW(editor_, EM_SETSEL, 0, -1);
    UpdateStatusBar();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleTimeDate()
{
    if (IsLargeFileMode()) {
        std::wstring error;
        if (!largeFileView_->InsertText(BuildTimeDateText(), error) && !error.empty()) ShowError(error);
        return;
    }
    ReplaceSelection(BuildTimeDateText());
    UpdateStatusBar();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleToggleWordWrap()
{
    if (IsLargeFileMode()) {
        wordWrap_ = !wordWrap_;
        largeFileView_->SetWordWrap(wordWrap_);
        ResizeEditor();
        UpdateStatusBar();
        UpdateMenuState(ActiveMenu());
        return;
    }
    if (editor_ == nullptr) {
        wordWrap_ = !wordWrap_;
        return;
    }

    const bool hadFocus = GetFocus() == editor_;
    const bool wasModified = document_.IsModified();
    const int firstVisibleLine = static_cast<int>(SendMessageW(editor_, EM_GETFIRSTVISIBLELINE, 0, 0));

    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);
    std::wstring text;
    try {
        text = GetEditorText();
    } catch (const std::bad_alloc&) {
        ShowError(L"Word wrap could not be changed because memory allocation failed.");
        return;
    } catch (const std::length_error&) {
        ShowError(L"Word wrap could not be changed because the document is too large.");
        return;
    }

    const bool targetWordWrap = !wordWrap_;
    ScopedWindow candidateEditor(CreateEditor(false, targetWordWrap));
    if (candidateEditor.Get() == nullptr) {
        ShowError(L"Could not create the replacement editor control.");
        return;
    }
    std::wstring adoptionError;
    bool textAdopted = false;
    {
        ScopedBoolRestore restoreSuppression(suppressEditorChange_, true);
        textAdopted = TrySetEditorText(candidateEditor.Get(), text, adoptionError);
    }
    if (!textAdopted) {
        ShowError(adoptionError);
        return;
    }
    SendMessageW(candidateEditor.Get(), EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(candidateEditor.Get(), EM_SETMODIFY, wasModified ? TRUE : FALSE, 0);
    SendMessageW(candidateEditor.Get(), EM_SETSEL, selectionStart, selectionEnd);
    if (firstVisibleLine > 0) {
        SendMessageW(candidateEditor.Get(), EM_LINESCROLL, 0, firstVisibleLine);
    }

    AdvanceSpellingSession();
    HWND oldEditor = editor_;
    editor_ = candidateEditor.Release();
    nativeSelectionTrackingValid_ = false;
    TrackNativeSelectionAfterMessage(editor_, EM_SETSEL, selectionStart, selectionEnd);
    wordWrap_ = targetWordWrap;
    document_.SetModified(wasModified);
    InvalidateWidestLineCache();
    ShowWindow(editor_, SW_SHOW);
    if (oldEditor != nullptr && IsWindow(oldEditor)) DestroyWindow(oldEditor);

    ResizeEditor();
    UpdateStatusBar();
    UpdateScrollBars();
    RefreshSpellCheck(true);
    UpdateTitle();
    UpdateMenuState(ActiveMenu());
    UpdateMenuChrome();
    if (hadFocus) {
        SetFocus(editor_);
    }
}

void ClassicNotepadApp::FocusActiveEditor()
{
    if (IsLargeFileMode()) {
        largeFileView_->Focus();
    } else if (editor_ != nullptr) {
        SetFocus(editor_);
    }
}

void ClassicNotepadApp::HandleChooseFont()
{
    LOGFONTW logFont{};
    if (editorFont_ != nullptr) {
        GetObjectW(editorFont_, sizeof(logFont), &logFont);
    }

    CHOOSEFONTW chooseFont{};
    chooseFont.lStructSize = sizeof(chooseFont);
    chooseFont.hwndOwner = mainWindow_;
    chooseFont.lpLogFont = &logFont;
    chooseFont.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST;

    if (!ChooseFontW(&chooseFont)) {
        const DWORD dialogError = CommDlgExtendedError();
        if (dialogError != 0) {
            ShowError(L"The Font dialog could not be shown.");
        }
        return;
    }

    HFONT newFont = CreateFontIndirectW(&logFont);
    if (newFont == nullptr) {
        ShowError(L"The selected font could not be created.");
        return;
    }

    AdvanceSpellingLayoutRevision();
    BeginEditorViewportAnchorPreservation();
    HFONT oldFont = editorFont_;
    const bool ownedOldFont = ownsEditorFont_;
    editorFont_ = newFont;
    ownsEditorFont_ = true;
    SendMessageW(editor_, WM_SETFONT, reinterpret_cast<WPARAM>(editorFont_), TRUE);
    if (largeFileView_ != nullptr) {
        largeFileView_->SetTheme(editorFont_, darkModeEnabled_);
    }
    if (ownedOldFont && oldFont != nullptr) {
        DeleteObject(oldFont);
    }

    InvalidateWidestLineCache();
    ResizeEditor();
    UpdateStatusBar();
    UpdateScrollBars();
    InvalidateRect(editor_, nullptr, FALSE);
    EndEditorViewportAnchorPreservation();
    if (largeFileView_ != nullptr) {
        largeFileView_->Focus();
    } else {
        SetFocus(editor_);
    }
}

void ClassicNotepadApp::HandleToggleStatusBar()
{
    BeginEditorViewportAnchorPreservation();
    statusBarVisible_ = !statusBarVisible_;
    if (statusBar_ != nullptr) {
        ShowWindow(statusBar_, statusBarVisible_ ? SW_SHOW : SW_HIDE);
    }

    ResizeEditor();
    UpdateStatusBar();
    UpdateScrollBars();
    UpdateMenuState(ActiveMenu());
    UpdateMenuChrome();
    EndEditorViewportAnchorPreservation();
    if (largeFileView_ != nullptr) {
        largeFileView_->Focus();
    } else {
        SetFocus(editor_);
    }
}

bool ClassicNotepadApp::LoadDocument(const std::wstring& path, std::uint32_t ansiCodePageOverride)
{
    std::uint64_t fileSize = 0;
    std::wstring thresholdError;
    const bool useLargeFileMode = LargeFileView::ShouldUseLargeFile(path, fileSize, thresholdError);
    if (!thresholdError.empty()) {
        ShowError(thresholdError);
        UpdateTitle();
        return false;
    }
    if (useLargeFileMode) {
        std::wstring errorMessage;
        bool loaded = false;
        try {
            loaded = LoadLargeDocument(path, errorMessage);
        } catch (const std::bad_alloc&) {
            SetAllocationFailureMessage(errorMessage);
        } catch (const std::length_error&) {
            SetAllocationFailureMessage(errorMessage);
        }
        if (!loaded) {
            ShowError(errorMessage);
            UpdateTitle();
            return false;
        }
        return true;
    }

    Document::StagedLoad staged;
    std::wstring errorMessage;
    bool prepared = false;
    try {
        prepared = ansiCodePageOverride == 0U
            ? document_.PrepareLoad(path, staged, errorMessage)
            : document_.PrepareLoadWithAnsiCodePage(path, staged, errorMessage, ansiCodePageOverride);
    } catch (const std::bad_alloc&) {
        SetAllocationFailureMessage(errorMessage);
    } catch (const std::length_error&) {
        SetAllocationFailureMessage(errorMessage);
    }
    if (!prepared) {
        ShowError(errorMessage);
        UpdateTitle();
        return false;
    }

    ScopedWindow candidateEditor(CreateEditor(false, wordWrap_));
    if (candidateEditor.Get() == nullptr) {
        ShowError(L"The replacement editor control could not be created.");
        UpdateTitle();
        return false;
    }
    bool textAdopted = false;
    {
        ScopedBoolRestore restoreSuppression(suppressEditorChange_, true);
        textAdopted = TrySetEditorText(candidateEditor.Get(), staged.editorText, errorMessage);
    }
    if (!textAdopted) {
        ShowError(errorMessage);
        UpdateTitle();
        return false;
    }
    SendMessageW(candidateEditor.Get(), EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(candidateEditor.Get(), EM_SETMODIFY, FALSE, 0);
    SendMessageW(candidateEditor.Get(), EM_SETSEL, 0, 0);

    // Every fallible read, decode, allocation, and control adoption step has
    // completed. Now publish the new control and its staged file metadata.
    AdvanceSpellingSession();
    largeFileView_.reset();
    HWND oldEditor = editor_;
    editor_ = candidateEditor.Release();
    nativeSelectionTrackingValid_ = false;
    TrackNativeSelectionAfterMessage(editor_, EM_SETSEL, 0, 0);
    SetStatusCharacterCountFromText(staged.editorText);
    document_.CommitLoad(std::move(staged));
    horizontalScrollPosition_ = 0;
    InvalidateWidestLineCache();
    ShowWindow(editor_, SW_SHOW);
    if (oldEditor != nullptr && IsWindow(oldEditor)) DestroyWindow(oldEditor);
    ResizeEditor();
    UpdateTitle();
    UpdateStatusBar();
    UpdateScrollBars();
    InvalidateRect(editor_, nullptr, TRUE);
    RefreshSpellCheck(true);
    SetFocus(editor_);
    return true;
}

bool ClassicNotepadApp::LoadLargeDocument(const std::wstring& path, std::wstring& errorMessage)
{
    auto view = std::make_unique<LargeFileView>();
    if (!view->Create(
            instance_, mainWindow_, IDC_MAIN_EDITOR, path, editorFont_, darkModeEnabled_, errorMessage, false,
            spellingSessionId_ == (std::numeric_limits<std::uint64_t>::max)()
                ? 1U : spellingSessionId_ + 1U)) {
        return false;
    }
    if (!view->WaitForCompleteIndex(errorMessage)) {
        return false;
    }

    const LargeFileViewInfo info = view->GetInfo();
    if (!info.indexingComplete || !info.encodingValid) {
        errorMessage = L"The large file could not be fully validated and indexed.";
        return false;
    }
    Document::TextEncoding encoding = Document::TextEncoding::Utf8NoBom;
    if (info.encoding == classic_notepad::LargeFileEncoding::Utf16Le) {
        encoding = Document::TextEncoding::Utf16LeBom;
    } else if (info.dataOffset == 3U) {
        encoding = Document::TextEncoding::Utf8Bom;
    }

    const classic_notepad::LineEndingAnalysis lineEndings = classic_notepad::AnalyzeLineEndingCounts(
        info.crlfCount, info.lfCount, info.crCount);
    Document::StagedLargeLoad stagedMetadata;
    document_.PrepareReadOnlyLargeFile(
        path, encoding, lineEndings.style, lineEndings.dominantStyle, stagedMetadata);
    std::function<bool(POINT, const classic_notepad::SpellingContextTarget&)> contextMenuHandler =
        [this](POINT point, const classic_notepad::SpellingContextTarget& target) {
            return HandleLargeFileContextMenu(point, target);
        };
    view->SetContextMenuHandler(std::move(contextMenuHandler));
    view->SetWordWrap(wordWrap_);

    // This test-only failure is placed after path/handler staging and before
    // either the current view or its document metadata are published.
    if (IsInjectedMappedMetadataAllocationFailure(automationMode_)) throw std::bad_alloc();

    // Both commits are nonthrowing. Retain the old view until metadata and the
    // new candidate have been installed, so any earlier failure destroys only
    // the candidate and leaves the active document untouched.
    largeFileView_.swap(view);
    document_.CommitReadOnlyLargeFile(std::move(stagedMetadata));
    AdvanceSpellingSession();
    ShowWindow(largeFileView_->Window(), SW_SHOW);
    ShowWindow(editor_, SW_HIDE);
    // Adoption is complete. Presentation failures must never escape to the
    // preparation catch in LoadDocument and report a failed Open after replacing
    // the active document. The new view and metadata remain authoritative.
    try {
        ResizeEditor();
        InjectMappedPresentationAllocationFailure(automationMode_);
        UpdateStatusBar();
        UpdateTitle();
    } catch (const std::bad_alloc&) {
        ReportAdoptedDocumentPresentationFailure(mainWindow_, automationMode_);
    } catch (const std::length_error&) {
        ReportAdoptedDocumentPresentationFailure(mainWindow_, automationMode_);
    }
    largeFileView_->Focus(MappedCallbackAllocationFailureForTesting(automationMode_));
    return true;
}

void ClassicNotepadApp::ExitLargeFileMode()
{
    if (largeFileView_ == nullptr) {
        return;
    }
    largeFileView_.reset();
    if (editor_ != nullptr) {
        ShowWindow(editor_, SW_SHOW);
    }
    InvalidateWidestLineCache();
    UpdateScrollBars();
}

bool ClassicNotepadApp::IsLargeFileMode() const
{
    return largeFileView_ != nullptr;
}

void ClassicNotepadApp::ShowLargeFileReadOnlyMessage()
{
    ShowError(L"Printing is unavailable for mapped large documents because the print path is bounded to the normal editor control.");
}

bool ClassicNotepadApp::AutomationNewDocument()
{
    ExitLargeFileMode();
    AdvanceSpellingSession();
    document_.ResetUntitled();
    SetEditorText(L"");
    UpdateTitle();
    return true;
}

bool ClassicNotepadApp::AutomationRequestNewTransition(bool saveChanges)
{
    automationMessageDialogResultOverride_ = saveChanges ? IDYES : IDCANCEL;
    RequestDocumentTransition(DocumentTransition::NewDocument);
    automationMessageDialogResultOverride_ = 0;
    return true;
}

bool ClassicNotepadApp::AutomationOpenFile(
    const std::wstring& path,
    std::wstring& errorMessage,
    std::uint32_t ansiCodePageOverride)
{
    if (LoadDocument(path, ansiCodePageOverride)) {
        return true;
    }
    errorMessage = L"The file could not be opened.";
    return false;
}

bool ClassicNotepadApp::AutomationSave(std::wstring& errorMessage)
{
    if (IsLargeFileMode()) {
        if (!largeFileView_->Save(document_.Path(), document_.Encoding(), document_.SaveLineEnding(), true, errorMessage)) return false;
        document_.SetModified(false);
        UpdateTitle();
        UpdateStatusBar();
        return true;
    }
    if (!document_.Save(GetEditorText(), errorMessage)) {
        return false;
    }

    SendMessageW(editor_, EM_SETMODIFY, FALSE, 0);
    UpdateTitle();
    UpdateStatusBar();
    return true;
}

bool ClassicNotepadApp::AutomationSaveAs(const std::wstring& path, std::wstring& errorMessage)
{
    if (IsLargeFileMode()) {
        std::wstring stagedPath = path;
        if (!largeFileView_->Save(path, document_.Encoding(), document_.SaveLineEnding(), true, errorMessage)) return false;
        document_.AdoptLargeFileSaveAs(std::move(stagedPath));
        document_.SetModified(false);
        UpdateTitle();
        UpdateStatusBar();
        return true;
    }
    if (!document_.SaveAs(path, GetEditorText(), errorMessage)) {
        return false;
    }

    SendMessageW(editor_, EM_SETMODIFY, FALSE, 0);
    UpdateTitle();
    UpdateStatusBar();
    return true;
}

void ClassicNotepadApp::AutomationSetText(const std::wstring& text)
{
    if (IsLargeFileMode()) {
        std::wstring error;
        largeFileView_->SetSelection(0, largeFileView_->GetInfo().characterCount, error);
        if (error.empty()) largeFileView_->InsertText(text, error);
        if (error.empty()) document_.SetModified(true);
        UpdateTitle();
        return;
    }
    ReplaceEditorTextFromCommand(text);
}

void ClassicNotepadApp::AutomationInsertText(const std::wstring& text)
{
    if (IsLargeFileMode()) {
        std::wstring error;
        if (largeFileView_->InsertText(text, error)) document_.SetModified(true);
        UpdateTitle();
        UpdateStatusBar();
        return;
    }
    ReplaceSelection(text);
    UpdateStatusBar();
    UpdateScrollBars();
}

std::wstring ClassicNotepadApp::AutomationGetText() const
{
    if (IsLargeFileMode()) {
        std::wstring text;
        std::wstring error;
        const LargeFileViewInfo info = largeFileView_->GetInfo();
        if (info.characterCount <= 1024U * 1024U) largeFileView_->GetTextRange(0, info.characterCount, text, error);
        return text;
    }
    return GetEditorText();
}

std::wstring ClassicNotepadApp::AutomationGetTitle() const
{
    const int textLength = GetWindowTextLengthW(mainWindow_);
    if (textLength <= 0) {
        return {};
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(textLength) + 1U, L'\0');
    const int copiedLength = GetWindowTextW(mainWindow_, buffer.data(), static_cast<int>(buffer.size()));
    return std::wstring(buffer.data(), static_cast<std::size_t>(copiedLength));
}

bool ClassicNotepadApp::AutomationIsModified() const
{
    if (IsLargeFileMode()) return largeFileView_->GetInfo().modified;
    return document_.IsModified();
}

ClassicNotepadApp::AutomationDocumentMetadata ClassicNotepadApp::AutomationGetDocumentMetadata() const
{
    AutomationDocumentMetadata metadata;
    metadata.path = document_.Path();
    metadata.displayName = document_.DisplayName();
    metadata.hasPath = document_.HasPath();
    metadata.encoding = classic_notepad::FormatEncoding(document_.Encoding());
    metadata.lineEnding = classic_notepad::FormatLineEnding(document_.LineEnding());
    metadata.saveLineEnding = classic_notepad::FormatLineEnding(document_.SaveLineEnding());
    return metadata;
}

std::wstring ClassicNotepadApp::AutomationGetStatusText() const
{
    std::wstring text;
    for (std::size_t index = 0; index < statusBarParts_.size(); ++index) {
        if (index > 0U) {
            text += L" | ";
        }

        text += statusBarParts_[index];
    }

    return text;
}

ClassicNotepadApp::AutomationSelection ClassicNotepadApp::AutomationGetSelection() const
{
    AutomationSelection selection;
    if (IsLargeFileMode()) {
        std::uint64_t start = 0, end = 0;
        largeFileView_->GetSelection(start, end);
        selection.start = static_cast<DWORD>(std::min<std::uint64_t>(start, (std::numeric_limits<DWORD>::max)()));
        selection.end = static_cast<DWORD>(std::min<std::uint64_t>(end, (std::numeric_limits<DWORD>::max)()));
        return selection;
    }
    GetSelectionRange(selection.start, selection.end);
    return selection;
}

ClassicNotepadApp::AutomationEditorWidthState ClassicNotepadApp::AutomationGetEditorWidthState() const
{
    AutomationEditorWidthState state;
    if (IsLargeFileMode() || editor_ == nullptr) {
        return state;
    }

    state.generation = editorWidthGeneration_;
    state.editDisplayLineCount = static_cast<std::uint64_t>(std::max<LRESULT>(
        1, SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    state.pendingLines = editorWidthIndexAllocationFailed_
        ? state.editDisplayLineCount
        : static_cast<std::uint64_t>(editorWidthUnknownLines_);
    state.measuredDisplayLines = editorWidthMeasuredLines_;
    state.measuredTextUnits = editorWidthMeasuredTextUnits_;
    state.measurementBatches = editorWidthMeasurementBatches_;
    state.lastBatchDiscoveredDisplayLines = editorWidthLastBatchDiscoveredLines_;
    state.lastBatchPublishedDisplayLines = editorWidthLastBatchPublishedLines_;
    state.lastBatchDiscoveredTextUnits = editorWidthLastBatchDiscoveredTextUnits_;
    state.lastBatchMeasuredTextUnits = editorWidthLastBatchMeasuredTextUnits_;
    state.maximumBatchDiscoveredDisplayLines = editorWidthMaximumBatchDiscoveredLines_;
    state.maximumBatchPublishedDisplayLines = editorWidthMaximumBatchPublishedLines_;
    state.maximumBatchTextUnits = editorWidthMaximumBatchTextUnits_;
    state.localEditMeasuredDisplayLines = editorWidthLocalEditMeasurements_;
    state.localEditMeasuredTextUnits = editorWidthLocalEditTextUnits_;
    state.conservativeLineEstimates = editorWidthConservativeLines_;
    state.maximumEditDisplayLineLength = editorWidthMaximumEditDisplayLineLength_;
    state.measurementPending = editorWidthIndexNeedsRebuild_ || editorWidthIndexAllocationFailed_ ||
        editorWidthUnknownLines_ != 0U;
    state.documentWidth = MeasureWidestEditorLine();
    state.fontMaximumAdvance = GetEditorFontMaximumAdvance();
    state.editTextLimit = static_cast<DWORD>(SendMessageW(editor_, EM_GETLIMITTEXT, 0, 0));
    state.windowDpi = mainWindow_ == nullptr ? 0 : static_cast<int>(GetDpiForWindow(mainWindow_));
    POINT caretPoint{};
    state.caretPositionAvailable = GetCaretPos(&caretPoint) != 0;
    state.caretX = caretPoint.x;
    state.caretY = caretPoint.y;
    DWORD currentSelectionStart = 0;
    DWORD currentSelectionEnd = 0;
    SendMessageW(editor_, EM_GETSEL, reinterpret_cast<WPARAM>(&currentSelectionStart),
        reinterpret_cast<LPARAM>(&currentSelectionEnd));
    state.caretDisplayLine = static_cast<int>(SendMessageW(
        editor_, EM_LINEFROMCHAR, static_cast<WPARAM>(currentSelectionEnd), 0));
    const DWORD charPositionIndex = currentSelectionEnd > 0U ? currentSelectionEnd - 1U : currentSelectionEnd;
    state.charPositionCharacterIndex = static_cast<int>(charPositionIndex);
    const LRESULT charPosition = SendMessageW(
        editor_, EM_POSFROMCHAR, static_cast<WPARAM>(charPositionIndex), 0);
    if (charPosition != -1) {
        state.charPositionAvailable = true;
        state.charPositionX = static_cast<short>(LOWORD(charPosition));
        state.charPositionY = static_cast<short>(HIWORD(charPosition));
    }
    if (state.charPositionAvailable) {
        const LRESULT hitTest = SendMessageW(editor_, EM_CHARFROMPOS, 0,
            MAKELPARAM(static_cast<short>(state.charPositionX), static_cast<short>(state.charPositionY)));
        if (hitTest != -1) {
            state.hitTestAvailable = true;
            state.hitTestCharacterLowWord = LOWORD(hitTest);
            state.hitTestLineHighWord = HIWORD(hitTest);
        }
    }
    state.horizontalScrollBarVisible = horizontalScrollBarVisible_ && !wordWrap_;

    RECT clientRect{};
    GetClientRect(editor_, &clientRect);
    state.viewportWidth = std::max(0, static_cast<int>(clientRect.right - clientRect.left));
    state.viewportHeight = std::max(0, static_cast<int>(clientRect.bottom - clientRect.top));
    const int unit = std::max(1, GetEditorHorizontalScrollUnit());
    const int contentUnits = state.documentWidth / unit + ((state.documentWidth % unit) != 0 ? 1 : 0);
    const int pageUnits = std::max(1, std::max(0, state.viewportWidth - 8) / unit);
    state.documentHorizontalRangeMaximumOffset = std::max(0, contentUnits - 1);
    state.horizontalMaximumOffset = std::max(0, contentUnits - pageUnits);

    SCROLLINFO scrollInfo{};
    scrollInfo.cbSize = sizeof(scrollInfo);
    scrollInfo.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (GetScrollInfo(editor_, SB_HORZ, &scrollInfo)) {
        state.nativeHorizontalScrollMinimum = scrollInfo.nMin;
        state.nativeHorizontalScrollMaximum = scrollInfo.nMax;
        state.nativeHorizontalScrollPage = static_cast<int>(scrollInfo.nPage);
        state.nativeHorizontalOffset = std::max(0, scrollInfo.nPos);
        const long long nativeMaximum = static_cast<long long>(scrollInfo.nMax) - scrollInfo.nMin -
            static_cast<long long>(std::max<UINT>(1U, scrollInfo.nPage)) + 1LL;
        state.nativeHorizontalMaximumOffset = static_cast<int>(std::clamp(
            nativeMaximum,
            0LL,
            static_cast<long long>((std::numeric_limits<int>::max)())));
        state.nativeHorizontalScrollInfoAvailable = scrollInfo.nMax > scrollInfo.nMin ||
            scrollInfo.nPos > 0 || scrollInfo.nPage > 0;
    }
    state.horizontalScrollUnit = unit;
    state.horizontalAnchorPixels = GetHorizontalAnchorPixels();
    state.verticalFirstVisibleLine = std::max(0, static_cast<int>(SendMessageW(
        editor_, EM_GETFIRSTVISIBLELINE, 0, 0)));
    const LRESULT topContentPosition = SendMessageW(
        editor_, EM_LINEINDEX, static_cast<WPARAM>(state.verticalFirstVisibleLine), 0);
    state.verticalTopContentPosition = topContentPosition < 0 ? 0 : static_cast<int>(topContentPosition);
    state.verticalLineHeight = GetEditorLineHeight();
    state.verticalPage = std::max(1, std::max(0,
        static_cast<int>(clientRect.bottom - clientRect.top)) / state.verticalLineHeight);
    state.verticalMaximumOffset = std::max(0,
        static_cast<int>(state.editDisplayLineCount) - state.verticalPage);
    state.verticalScrollBarVisible = verticalScrollBarVisible_;
    state.pastePostCaretFirstVisibleLine = automationPastePostCaretFirstVisibleLine_;
    state.pastePostCaretTopContentPosition = automationPastePostCaretTopContentPosition_;
    state.pastePostCaretHorizontalOffset = automationPastePostCaretHorizontalOffset_;
    if (UseCustomScrollBars()) {
        const ScrollBarMetrics metrics = GetCustomScrollBarMetrics(ScrollBarOrientation::Horizontal);
        state.horizontalOffset = metrics.position;
    } else {
        if (state.nativeHorizontalScrollInfoAvailable) {
            state.horizontalOffset = scrollInfo.nPos;
        }
        state.horizontalOffset = std::max(0, state.horizontalOffset);
    }
    state.customHorizontalOffset = UseCustomScrollBars()
        ? GetCustomScrollBarMetrics(ScrollBarOrientation::Horizontal).position
        : (state.nativeHorizontalScrollInfoAvailable
            ? state.nativeHorizontalOffset / unit
            : std::max(0, horizontalScrollPosition_));
    state.customHorizontalOffsetPixels = state.customHorizontalOffset >
        (std::numeric_limits<int>::max)() / unit
        ? (std::numeric_limits<int>::max)()
        : state.customHorizontalOffset * unit;
    return state;
}

bool ClassicNotepadApp::AutomationSetEditorViewportSize(int width, int height)
{
    if (mainWindow_ == nullptr || editor_ == nullptr || IsLargeFileMode() || width <= 0 || height <= 0 ||
        width > 16384 || height > 16384) {
        return false;
    }
    if (!SetWindowPos(
            mainWindow_, nullptr, 0, 0, width, height,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        return false;
    }
    ResizeEditor();
    return true;
}

bool ClassicNotepadApp::AutomationSendEditorHorizontalScroll(UINT command, int trackPosition)
{
    if (editor_ == nullptr || IsLargeFileMode() || command > SB_ENDSCROLL ||
        trackPosition < 0 || trackPosition > (std::numeric_limits<WORD>::max)()) {
        return false;
    }
    SendMessageW(editor_, WM_HSCROLL, MAKEWPARAM(command, trackPosition), 0);
    return true;
}

bool ClassicNotepadApp::AutomationSendEditorVerticalScroll(UINT command, int trackPosition)
{
    if (editor_ == nullptr || IsLargeFileMode() || command > SB_ENDSCROLL ||
        trackPosition < 0 || trackPosition > (std::numeric_limits<WORD>::max)()) {
        return false;
    }
    SendMessageW(editor_, WM_VSCROLL, MAKEWPARAM(command, trackPosition), 0);
    return true;
}

bool ClassicNotepadApp::AutomationSendEditorLineScroll(int horizontalUnits, int verticalLines)
{
    if (editor_ == nullptr || IsLargeFileMode()) {
        return false;
    }
    SendMessageW(editor_, EM_LINESCROLL, horizontalUnits, verticalLines);
    if (horizontalUnits != 0 || verticalLines != 0) {
        AdvanceSpellingLayoutRevision();
        RefreshSpellCheck(false);
    }
    if (UseCustomScrollBars() && horizontalUnits != 0) {
        const int unit = std::max(1, GetEditorHorizontalScrollUnit());
        int nativeMaximumPixels = -1;
        const int actualPositionPixels = GetNativeEditorHorizontalPosition(&nativeMaximumPixels);
        if (nativeMaximumPixels >= 0) {
            horizontalScrollPosition_ = std::max(0, actualPositionPixels) / unit;
        } else {
            const ScrollBarMetrics metrics = GetCustomScrollBarMetrics(ScrollBarOrientation::Horizontal);
            const int maximum = std::max(metrics.minimum, metrics.maximum - metrics.page + 1);
            horizontalScrollPosition_ = std::clamp(
                horizontalScrollPosition_ + horizontalUnits, metrics.minimum, maximum);
        }
    }
    UpdateStatusBar();
    UpdateScrollBars();
    return true;
}

bool ClassicNotepadApp::AutomationSendEditorKey(UINT virtualKey, bool shift, bool control)
{
    if (editor_ == nullptr || IsLargeFileMode() || virtualKey > 0xFFU) return false;
    BYTE previousState[256]{};
    if (!GetKeyboardState(previousState)) return false;
    BYTE simulatedState[256]{};
    std::copy(std::begin(previousState), std::end(previousState), std::begin(simulatedState));
    auto setPressed = [&](UINT key, bool pressed) {
        if (pressed) simulatedState[key] = static_cast<BYTE>(simulatedState[key] | 0x80U);
        else simulatedState[key] = static_cast<BYTE>(simulatedState[key] & 0x7FU);
    };
    setPressed(VK_SHIFT, shift);
    setPressed(VK_CONTROL, control);
    setPressed(virtualKey, true);
    if (!SetKeyboardState(simulatedState)) return false;
    SendMessageW(editor_, WM_KEYDOWN, virtualKey, 1);
    SendMessageW(editor_, WM_KEYUP, virtualKey, static_cast<LPARAM>(0xC0000001U));
    SetKeyboardState(previousState);
    UpdateStatusBar();
    UpdateScrollBars();
    return true;
}

void ClassicNotepadApp::AutomationPasteTextThroughProductionHandler(const std::wstring& text)
{
    automationPasteTiming_ = {};
    if (editor_ == nullptr || IsLargeFileMode()) {
        return;
    }
    const auto providerSetupStarted = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point providerCleanupStarted{};
    try {
        automationPasteText_ = text;
        editorPasteNativeFailureNotification_ = 0U;
        automationPasteClipboardDiagnostics_ = {};
        automationPasteLastOutcome_ = L"not-run";
        automationPastePostCaretFirstVisibleLine_ = -1;
        automationPastePostCaretTopContentPosition_ = -1;
        automationPastePostCaretHorizontalOffset_ = -1;
        {
            ScopedBoolRestore pasteProvider(automationPasteTextActive_, true);
            automationPasteTiming_.providerSetupMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - providerSetupStarted).count();
            const auto dispatchStarted = std::chrono::steady_clock::now();
            SendMessageW(editor_, WM_PASTE, 0, 0);
            automationPasteTiming_.productionDispatchMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - dispatchStarted).count();
            providerCleanupStarted = std::chrono::steady_clock::now();
        }
    } catch (const std::bad_alloc&) {
        automationPasteLastOutcome_ = PasteOutcomeName(PasteOutcome::AllocationFailure);
    } catch (const std::length_error&) {
        automationPasteLastOutcome_ = PasteOutcomeName(PasteOutcome::AllocationFailure);
    }
    if (providerCleanupStarted == std::chrono::steady_clock::time_point{}) {
        providerCleanupStarted = std::chrono::steady_clock::now();
    }
    automationPasteText_.clear();
    automationPasteTiming_.providerCleanupMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - providerCleanupStarted).count();
}

ClassicNotepadApp::AutomationPasteTiming ClassicNotepadApp::AutomationGetLastPasteTiming() const
{
    return automationPasteTiming_;
}

std::wstring ClassicNotepadApp::AutomationGetLastPasteOutcome() const
{
    return automationPasteLastOutcome_;
}

UINT ClassicNotepadApp::AutomationGetLastPasteNativeNotification() const
{
    return editorPasteNativeFailureNotification_;
}

ClassicNotepadApp::AutomationClipboardDiagnostics ClassicNotepadApp::AutomationGetLastClipboardDiagnostics() const
{
    return automationPasteClipboardDiagnostics_;
}

bool ClassicNotepadApp::AutomationFocusEditor()
{
    if (editor_ == nullptr || IsLargeFileMode()) return false;
    SetFocus(editor_);
    return GetFocus() == editor_;
}

bool ClassicNotepadApp::AutomationClickEditorClientPoint(int x, int y)
{
    if (editor_ == nullptr || IsLargeFileMode() || x < 0 || y < 0 ||
        x > static_cast<int>((std::numeric_limits<short>::max)()) ||
        y > static_cast<int>((std::numeric_limits<short>::max)())) {
        return false;
    }
    RECT client{};
    if (!GetClientRect(editor_, &client) || x >= client.right || y >= client.bottom) {
        return false;
    }
    SetFocus(editor_);
    const LPARAM point = MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
    SendMessageW(editor_, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(editor_, WM_LBUTTONUP, 0, point);
    return true;
}

bool ClassicNotepadApp::AutomationSendEditorDrag(int startX, int startY, int endX, int endY)
{
    if (editor_ == nullptr || IsLargeFileMode() || !IsWindow(editor_)) return false;
    const auto point = [](int x, int y) {
        return MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
    };
    SendMessageW(editor_, WM_LBUTTONDOWN, MK_LBUTTON, point(startX, startY));
    constexpr int steps = 8;
    for (int step = 1; step <= steps; ++step) {
        const int x = startX + (endX - startX) * step / steps;
        const int y = startY + (endY - startY) * step / steps;
        SendMessageW(editor_, WM_MOUSEMOVE, MK_LBUTTON, point(x, y));
    }
    SendMessageW(editor_, WM_LBUTTONUP, 0, point(endX, endY));
    UpdateStatusBar();
    UpdateScrollBars();
    return true;
}

bool ClassicNotepadApp::AutomationSetPasteFaultStage(const std::wstring& stage)
{
    static const std::array<const wchar_t*, 21> supportedStages{
        L"", L"no-text", L"open", L"get", L"size", L"lock", L"conversion", L"invalid",
        L"unicode-padding", L"unicode-odd-size", L"unicode-unterminated", L"ansi", L"clipboard-allocation",
        L"copy", L"normalization", L"capacity", L"native-replacement", L"native-errspace", L"native-maxtext",
        L"native-errspace-changed", L"native-spell-verification" };
    if (!std::any_of(supportedStages.begin(), supportedStages.end(), [&stage](const wchar_t* candidate) {
            return stage == candidate;
        })) {
        return false;
    }
    automationPasteFaultStage_ = stage;
    return true;
}

DWORD ClassicNotepadApp::AutomationSetEditorTextLimit(DWORD limit)
{
    if (editor_ == nullptr || IsLargeFileMode()) return 0U;
    SendMessageW(editor_, EM_SETLIMITTEXT, static_cast<WPARAM>(limit), 0);
    return static_cast<DWORD>(SendMessageW(editor_, EM_GETLIMITTEXT, 0, 0));
}

ClassicNotepadApp::AutomationNativeReplaceProbe ClassicNotepadApp::AutomationProbeNativeReplace(
    const std::wstring& text,
    DWORD selectionStart,
    DWORD selectionEnd,
    const std::wstring& replacement,
    DWORD textLimit)
{
    AutomationNativeReplaceProbe result;
    if (mainWindow_ == nullptr || !automationMode_) return result;

    ScopedWindow probeWindow(CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
        0,
        0,
        1,
        1,
        mainWindow_,
        nullptr,
        instance_,
        nullptr));
    HWND control = probeWindow.Get();
    if (control == nullptr) return result;

    ScopedValueRestore<HWND> activeProbe(automationProbeEditor_, control);
    ScopedValueRestore<UINT> notificationReset(automationProbeNotificationMask_, 0U);
    SendMessageW(control, EM_SETLIMITTEXT, static_cast<WPARAM>(textLimit), 0);
    SetWindowTextW(control, text.c_str());
    result.textLimit = static_cast<DWORD>(SendMessageW(control, EM_GETLIMITTEXT, 0, 0));

    const auto readControlText = [control]() {
        const int length = GetWindowTextLengthW(control);
        if (length <= 0) return std::wstring();
        std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
        const int copied = GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
        value.resize(static_cast<std::size_t>(std::max(0, copied)));
        return value;
    };
    result.textBefore = readControlText();
    SendMessageW(control, EM_SETSEL, selectionStart, selectionEnd);
    SendMessageW(control, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(replacement.c_str()));
    result.textAfter = readControlText();
    result.notificationMask = automationProbeNotificationMask_;
    return result;
}

void ClassicNotepadApp::AutomationSetSelection(DWORD selectionStart, DWORD selectionEnd)
{
    if (IsLargeFileMode()) {
        std::wstring error;
        largeFileView_->SetSelection(selectionStart, selectionEnd, error);
        return;
    }
    SendMessageW(editor_, EM_SETSEL, selectionStart, selectionEnd);
    SendMessageW(editor_, EM_SCROLLCARET, 0, 0);
    AdvanceSpellingLayoutRevision();
    RefreshSpellCheck(false);
    UpdateStatusBar();
    UpdateScrollBars();
}

void ClassicNotepadApp::AutomationSelectAll()
{
    HandleSelectAll();
}

void ClassicNotepadApp::AutomationUndo()
{
    HandleUndo();
}

void ClassicNotepadApp::AutomationCut()
{
    HandleCut();
}

void ClassicNotepadApp::AutomationCopy()
{
    HandleCopy();
}

void ClassicNotepadApp::AutomationPaste()
{
    HandlePaste();
}

void ClassicNotepadApp::AutomationDeleteSelection()
{
    if (IsLargeFileMode()) {
        std::wstring error;
        if (largeFileView_->DeleteSelection(false, error)) document_.SetModified(true);
        UpdateTitle();
        return;
    }
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);
    if (selectionStart == selectionEnd) {
        return;
    }

    SendMessageW(editor_, WM_CLEAR, 0, 0);
    UpdateStatusBar();
    UpdateScrollBars();
}

void ClassicNotepadApp::AutomationDelete()
{
    HandleDelete();
}

void ClassicNotepadApp::AutomationPressInsert()
{
    if (IsLargeFileMode()) {
        largeFileView_->SendKeyInput(VK_INSERT, false, false);
        return;
    }
    SendMessageW(editor_, WM_KEYDOWN, VK_INSERT, 0);
}

void ClassicNotepadApp::AutomationTypeText(const std::wstring& text)
{
    if (IsLargeFileMode()) {
        std::wstring error;
        if (largeFileView_->InsertText(text, error)) document_.SetModified(true);
        UpdateTitle();
        UpdateStatusBar();
        return;
    }
    for (const wchar_t character : text) {
        SendMessageW(editor_, WM_CHAR, static_cast<WPARAM>(character), 0);
    }
    UpdateStatusBar();
    UpdateScrollBars();
}

bool ClassicNotepadApp::AutomationFind(
    const std::wstring& text,
    bool matchCase,
    bool wholeWord,
    bool searchDown)
{
    if (IsLargeFileMode()) {
        if (text.empty() || text.size() >= findBuffer_.size()) return false;
        CopyToFixedBuffer(findBuffer_, text);
        std::wstring selectionError;
        const LargeFileViewInfo info = largeFileView_->GetInfo();
        if (!info.indexingComplete || !info.encodingValid) return false;
        largeFileView_->SetSelection(
            searchDown ? 0U : info.characterCount,
            searchDown ? 0U : info.characterCount,
            selectionError);
        if (!selectionError.empty()) return false;
        std::wstring error;
        const bool found = largeFileView_->Find(text, matchCase, wholeWord, searchDown, error);
        return found;
    }
    if (text.empty() || text.size() >= findBuffer_.size()) {
        return false;
    }

    CopyToFixedBuffer(findBuffer_, text);
    DWORD flags = 0;
    if (searchDown) {
        flags |= FR_DOWN;
        SendMessageW(editor_, EM_SETSEL, 0, 0);
    } else {
        const int textLength = GetWindowTextLengthW(editor_);
        SendMessageW(editor_, EM_SETSEL, textLength, textLength);
    }
    if (matchCase) {
        flags |= FR_MATCHCASE;
    }
    if (wholeWord) {
        flags |= FR_WHOLEWORD;
    }

    return FindNextWithFlags(flags, false);
}

bool ClassicNotepadApp::AutomationFindNext(bool matchCase, bool wholeWord, bool searchDown)
{
    if (IsLargeFileMode()) {
        std::wstring error;
        return findBuffer_[0] != L'\0' && largeFileView_->Find(findBuffer_.data(), matchCase, wholeWord, searchDown, error);
    }
    DWORD flags = 0;
    if (searchDown) {
        flags |= FR_DOWN;
    }
    if (matchCase) {
        flags |= FR_MATCHCASE;
    }
    if (wholeWord) {
        flags |= FR_WHOLEWORD;
    }

    return FindNextWithFlags(flags, false);
}

bool ClassicNotepadApp::AutomationReplace(
    const std::wstring& text,
    const std::wstring& replacement,
    bool matchCase,
    bool wholeWord,
    bool searchDown)
{
    if (IsLargeFileMode()) {
        std::wstring error;
        bool selectedMatch = largeFileView_->SelectionMatches(text, matchCase, wholeWord, error);
        if (!selectedMatch && !error.empty()) return false;
        if (!selectedMatch && !largeFileView_->Find(text, matchCase, wholeWord, searchDown, error)) return false;
        selectedMatch = largeFileView_->SelectionMatches(text, matchCase, wholeWord, error);
        if (!selectedMatch || !error.empty()) return false;
        if (!largeFileView_->InsertText(replacement, error)) return false;
        document_.SetModified(true);
        UpdateTitle();
        UpdateStatusBar();
        largeFileView_->Find(text, matchCase, wholeWord, searchDown, error);
        return true;
    }
    if (text.empty() || text.size() >= findBuffer_.size() || replacement.size() >= replaceBuffer_.size()) {
        return false;
    }

    CopyToFixedBuffer(findBuffer_, text);
    CopyToFixedBuffer(replaceBuffer_, replacement);

    DWORD flags = 0;
    if (searchDown) {
        flags |= FR_DOWN;
    }
    if (matchCase) {
        flags |= FR_MATCHCASE;
    }
    if (wholeWord) {
        flags |= FR_WHOLEWORD;
    }

    if (!SelectionMatchesFindText(flags, nullptr) && !FindNextWithFlags(flags, false)) {
        return false;
    }

    if (!SelectionMatchesFindText(flags, nullptr)) {
        return false;
    }

    ReplaceSelection(replacement);
    UpdateStatusBar();
    UpdateScrollBars();
    FindNextWithFlags(flags, false);
    return true;
}

bool ClassicNotepadApp::AutomationDispatchFindReplace(
    const std::wstring& action,
    const std::wstring& text,
    const std::wstring& replacement,
    bool matchCase,
    bool wholeWord,
    bool searchDown,
    std::wstring& errorMessage)
{
    errorMessage.clear();
    DWORD actionFlag = 0U;
    if (action == L"findNext") actionFlag = FR_FINDNEXT;
    else if (action == L"replace") actionFlag = FR_REPLACE;
    else if (action == L"replaceAll") actionFlag = FR_REPLACEALL;
    else {
        errorMessage = L"The production Find/Replace action is invalid.";
        return false;
    }
    if (text.empty() || text.size() >= findBuffer_.size() || replacement.size() >= replaceBuffer_.size()) {
        errorMessage = L"The production Find/Replace text exceeds the dialog buffer limits.";
        return false;
    }

    CopyToFixedBuffer(findBuffer_, text);
    CopyToFixedBuffer(replaceBuffer_, replacement);
    FINDREPLACEW request{};
    request.lStructSize = sizeof(request);
    request.hwndOwner = mainWindow_;
    request.lpstrFindWhat = findBuffer_.data();
    request.wFindWhatLen = static_cast<WORD>(findBuffer_.size());
    request.lpstrReplaceWith = replaceBuffer_.data();
    request.wReplaceWithLen = static_cast<WORD>(replaceBuffer_.size());
    request.Flags = actionFlag | (searchDown ? FR_DOWN : 0U) |
        (matchCase ? FR_MATCHCASE : 0U) | (wholeWord ? FR_WHOLEWORD : 0U);
    if (mainWindow_ == nullptr || findReplaceMessage_ == 0U) {
        errorMessage = L"The production Find/Replace callback is unavailable.";
        return false;
    }
    SendMessageW(mainWindow_, findReplaceMessage_, 0, reinterpret_cast<LPARAM>(&request));
    return true;
}

std::size_t ClassicNotepadApp::AutomationReplaceAll(
    const std::wstring& text,
    const std::wstring& replacement,
    bool matchCase,
    bool wholeWord)
{
    if (IsLargeFileMode()) {
        std::wstring error;
        const std::uint64_t count = largeFileView_->ReplaceAll(text, replacement, matchCase, wholeWord, error);
        if (count != 0U) { document_.SetModified(true); UpdateTitle(); }
        return static_cast<std::size_t>(std::min<std::uint64_t>(count, (std::numeric_limits<std::size_t>::max)()));
    }
    if (text.empty() || text.size() >= findBuffer_.size() || replacement.size() >= replaceBuffer_.size()) {
        return 0;
    }

    CopyToFixedBuffer(findBuffer_, text);
    CopyToFixedBuffer(replaceBuffer_, replacement);

    const std::wstring sourceText = GetEditorText();
    std::wstring replaced;
    replaced.reserve(sourceText.size());

    std::size_t replacementCount = 0;
    for (std::size_t position = 0; position < sourceText.size();) {
        if (TextMatchesAt(sourceText, position, text, matchCase, wholeWord)) {
            replaced += replacement;
            position += text.size();
            ++replacementCount;
        } else {
            replaced.push_back(sourceText[position]);
            ++position;
        }
    }

    if (replacementCount == 0U) {
        return 0;
    }

    std::wstring replaceError;
    if (!ReplaceNativeWholeDocumentUndoably(replaced, replaceError)) return 0U;
    SendMessageW(editor_, EM_SETSEL, 0, 0);
    SendMessageW(editor_, EM_SCROLLCARET, 0, 0);
    AdvanceSpellingLayoutRevision();
    RefreshSpellCheck(false);
    UpdateStatusBar();
    UpdateScrollBars();
    return replacementCount;
}

bool ClassicNotepadApp::AutomationGoToLine(std::uint64_t lineNumber, std::wstring& errorMessage)
{
    if (IsLargeFileMode()) {
        return largeFileView_->GoToLine(lineNumber, errorMessage);
    }
    if (!EnsureNativeStatusSummaryComplete()) {
        errorMessage = L"Logical line information is unavailable for this document.";
        return false;
    }
    const std::uint64_t maxLine = statusLogicalLineBreakCount_ + 1U;
    if (lineNumber < 1U || lineNumber > maxLine) {
        errorMessage = L"Line number must be between 1 and ";
        errorMessage += std::to_wstring(maxLine);
        errorMessage += L".";
        return false;
    }

    GoToLine(lineNumber);
    return true;
}

std::wstring ClassicNotepadApp::AutomationProbeGoToDialogDefault()
{
    if (!automationMode_ || mainWindow_ == nullptr || IsLargeFileMode()) return {};
    automationGoToDialogInitialValue_.clear();
    ScopedBoolRestore probe(automationGoToDialogProbe_, true);
    SendMessageW(mainWindow_, WM_COMMAND, MAKEWPARAM(ID_EDIT_GO_TO, 0), 0);
    return automationGoToDialogInitialValue_;
}

void ClassicNotepadApp::AutomationInsertTimeDate()
{
    HandleTimeDate();
}

void ClassicNotepadApp::AutomationSetWordWrap(bool enabled)
{
    if (IsLargeFileMode()) {
        wordWrap_ = enabled;
        largeFileView_->SetWordWrap(enabled);
        UpdateMenuState(ActiveMenu());
        return;
    }
    if (wordWrap_ != enabled) {
        HandleToggleWordWrap();
    }
}

bool ClassicNotepadApp::AutomationGetWordWrap() const
{
    return wordWrap_;
}

void ClassicNotepadApp::AutomationInvokeWordWrapMenuCommand()
{
    if (mainWindow_ != nullptr) {
        SendMessageW(mainWindow_, WM_COMMAND, MAKEWPARAM(ID_FORMAT_WORD_WRAP, 0), 0);
    }
}

bool ClassicNotepadApp::AutomationGetWordWrapMenuState(bool& enabled, bool& checked)
{
    HMENU menu = ActiveMenu();
    if (menu == nullptr) {
        return false;
    }
    UpdateMenuState(menu);
    const UINT state = GetMenuState(menu, ID_FORMAT_WORD_WRAP, MF_BYCOMMAND);
    if (state == static_cast<UINT>(-1)) {
        return false;
    }
    enabled = (state & (MF_DISABLED | MF_GRAYED)) == 0;
    checked = (state & MF_CHECKED) != 0;
    return true;
}

bool ClassicNotepadApp::AutomationActiveEditorHasFocus() const
{
    const HWND focused = GetFocus();
    if (IsLargeFileMode()) {
        return focused == largeFileView_->Window();
    }
    return editor_ != nullptr && focused == editor_;
}

bool ClassicNotepadApp::AutomationNativeEditorHasFocus() const
{
    return editor_ != nullptr && GetFocus() == editor_;
}

void ClassicNotepadApp::AutomationSetStatusBarVisible(bool visible)
{
    if (statusBarVisible_ != visible) {
        HandleToggleStatusBar();
    }
}

bool ClassicNotepadApp::AutomationGetStatusBarVisible() const
{
    return statusBarVisible_;
}

bool ClassicNotepadApp::AutomationSetFont(const std::wstring& fontDescription, std::wstring& errorMessage)
{
    std::wstring faceName;
    int pointSize = 0;
    if (!ParseAutomationFontDescription(fontDescription, faceName, pointSize)) {
        errorMessage = L"Font description must use '<face name> <point size>'.";
        return false;
    }

    HDC deviceContext = GetDC(mainWindow_);
    const int dpiY = deviceContext != nullptr ? std::max(1, GetDeviceCaps(deviceContext, LOGPIXELSY)) : 96;
    if (deviceContext != nullptr) {
        ReleaseDC(mainWindow_, deviceContext);
    }

    LOGFONTW logFont{};
    logFont.lfHeight = -MulDiv(pointSize, dpiY, 72);
    logFont.lfWeight = FW_NORMAL;
    logFont.lfCharSet = DEFAULT_CHARSET;
    logFont.lfOutPrecision = OUT_DEFAULT_PRECIS;
    logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    logFont.lfQuality = CLEARTYPE_QUALITY;
    logFont.lfPitchAndFamily = DEFAULT_PITCH;
    wcscpy_s(logFont.lfFaceName, faceName.c_str());

    HFONT newFont = CreateFontIndirectW(&logFont);
    if (newFont == nullptr) {
        errorMessage = L"The selected font could not be created.";
        return false;
    }

    BeginEditorViewportAnchorPreservation();
    HFONT oldFont = editorFont_;
    const bool ownedOldFont = ownsEditorFont_;
    editorFont_ = newFont;
    ownsEditorFont_ = true;
    SendMessageW(editor_, WM_SETFONT, reinterpret_cast<WPARAM>(editorFont_), TRUE);
    if (largeFileView_ != nullptr) {
        largeFileView_->SetTheme(editorFont_, darkModeEnabled_);
    }
    if (ownedOldFont && oldFont != nullptr) {
        DeleteObject(oldFont);
    }

    InvalidateWidestLineCache();
    ResizeEditor();
    UpdateStatusBar();
    UpdateScrollBars();
    InvalidateRect(editor_, nullptr, FALSE);
    EndEditorViewportAnchorPreservation();
    return true;
}

std::wstring ClassicNotepadApp::AutomationGetFont() const
{
    LOGFONTW logFont{};
    if (editorFont_ == nullptr || GetObjectW(editorFont_, sizeof(logFont), &logFont) == 0) {
        return std::wstring(kDefaultEditorFontFace) + L" " + std::to_wstring(kDefaultEditorFontPointSize);
    }

    std::wstring description(logFont.lfFaceName);
    description += L" ";
    description += std::to_wstring(PointSizeFromLogFont(mainWindow_, logFont));
    return description;
}

bool ClassicNotepadApp::AutomationSetPageMarginsThousandths(const RECT& margins, std::wstring& errorMessage)
{
    if (margins.left < 0 || margins.top < 0 || margins.right < 0 || margins.bottom < 0) {
        errorMessage = L"Page margins must be non-negative.";
        return false;
    }

    pageMarginsThousandths_ = margins;
    return true;
}

RECT ClassicNotepadApp::AutomationGetPageMarginsThousandths() const
{
    return pageMarginsThousandths_;
}

bool ClassicNotepadApp::AutomationPrintToTestSink(const std::wstring& path, std::wstring& errorMessage) const
{
    if (IsLargeFileMode()) {
        errorMessage = L"Printing is unavailable for mapped large documents because the print test sink is bounded to the normal editor control.";
        return false;
    }
    const std::wstring sinkText = BuildPrintSinkText(
        L"windows",
        AutomationGetFont(),
        pageMarginsThousandths_,
        GetEditorText());
    return WriteUtf8TextFile(path, sinkText, errorMessage);
}

bool ClassicNotepadApp::AutomationSetPrintFaultStage(const std::wstring& stage)
{
    if (!automationMode_) {
        return false;
    }

    if (stage.empty() || stage == L"none") {
        automationPrintFaultStage_.clear();
        automationPrintDiagnostics_ = {};
        return true;
    }

    if (stage != L"startDoc" && stage != L"startPage" && stage != L"textOut" &&
        stage != L"endPage" && stage != L"endDoc") {
        return false;
    }

    automationPrintFaultStage_ = stage;
    automationPrintDiagnostics_ = {};
    return true;
}

ClassicNotepadApp::AutomationPrintDiagnostics ClassicNotepadApp::AutomationGetPrintDiagnostics() const
{
    return automationMode_ ? automationPrintDiagnostics_ : AutomationPrintDiagnostics{};
}

bool ClassicNotepadApp::AutomationSpellCheckAvailable() const
{
    return spellCheckAvailable_;
}

SpellCheckResult ClassicNotepadApp::AutomationCheckSpelling(const std::wstring& text) const
{
    return spellChecker_.Check(text);
}

SpellSuggestResult ClassicNotepadApp::AutomationSuggestSpelling(
    const std::wstring& word,
    std::size_t limit) const
{
    return spellChecker_.Suggest(word, limit);
}

SpellActionResult ClassicNotepadApp::AutomationIgnoreSpelling(const std::wstring& word)
{
    SpellActionResult result = spellChecker_.Ignore(word);
    if (result.status == SpellOperationStatus::Success) RefreshSpellCheck(true);
    return result;
}

SpellActionResult ClassicNotepadApp::AutomationAddSpelling(const std::wstring&, bool dryRun)
{
    if (!dryRun) return {SpellOperationStatus::Failure, E_ACCESSDENIED, false};
    if (!spellCheckAvailable_) return {SpellOperationStatus::Unavailable, S_FALSE, false};
    return {SpellOperationStatus::Success, S_OK, false};
}

bool ClassicNotepadApp::AutomationDarkModeEnabled() const
{
    return darkModeEnabled_;
}

classic_notepad::AppearanceTheme ClassicNotepadApp::AutomationAppearanceTheme() const
{
    return appearanceTheme_;
}

bool ClassicNotepadApp::AutomationHighContrastThemeActive() const
{
    return IsHighContrastEnabled();
}

void ClassicNotepadApp::AutomationSetAppearanceTheme(classic_notepad::AppearanceTheme theme)
{
    appearanceTheme_ = theme;
    UpdateThemeFromSystem();
}

ClassicNotepadApp::AutomationLargeFileState ClassicNotepadApp::AutomationGetLargeFileState() const
{
    AutomationLargeFileState state;
    if (largeFileView_ == nullptr) {
        state.saveDiagnostics = document_.LastSaveDiagnostics();
        return state;
    }
    const LargeFileViewInfo info = largeFileView_->GetInfo();
    state.active = info.active;
    state.readOnly = info.readOnly;
    state.editable = info.editable;
    state.modified = info.modified;
    state.indexingComplete = info.indexingComplete;
    state.globalScrollReady = info.globalScrollReady;
    state.encodingValid = info.encodingValid;
    state.fileSize = info.fileSize;
    state.indexedBytes = info.indexedBytes;
    state.indexedLines = info.indexedLines;
    state.totalLines = info.totalLines;
    state.firstUsableMilliseconds = info.firstUsableMilliseconds;
    state.firstViewportPaintMilliseconds = info.firstViewportPaintMilliseconds;
    state.fullIndexMilliseconds = info.fullIndexMilliseconds;
    state.characterCount = info.characterCount;
    state.caret = info.caret;
    state.selectionStart = info.selectionStart;
    state.selectionEnd = info.selectionEnd;
    state.topLine = info.topLine;
    state.maximumTopLine = info.maximumTopLine;
    state.verticalScrollRangeMaximum = info.verticalScrollRangeMaximum;
    state.verticalScrollPosition = info.verticalScrollPosition;
    state.verticalScrollPage = info.verticalScrollPage;
    state.visibleLineCount = info.visibleLineCount;
    state.generation = info.generation;
    state.pieceCount = info.pieceCount;
    state.insertedBufferBytes = info.insertedBufferBytes;
    state.saveProgressBytes = info.saveProgressBytes;
    state.saveInProgress = info.saveInProgress;
    state.paintGeneration = info.paintGeneration;
    state.callbackAllocationFailures = info.callbackAllocationFailures;
    state.viewportGeneration = info.viewportGeneration;
    state.wordWrap = info.wordWrap;
    state.viewportVisualRows = info.viewportVisualRows;
    state.wrappedContinuationRows = info.wrappedContinuationRows;
    state.viewportCacheRebuilds = info.viewportCacheRebuilds;
    state.viewportCacheHits = info.viewportCacheHits;
    state.viewportLastBuildDecodedCharacters = info.viewportLastBuildDecodedCharacters;
    state.viewportLastBuildMeasuredCharacters = info.viewportLastBuildMeasuredCharacters;
    state.viewportLastLogicalAnchor = info.viewportLastLogicalAnchor;
    state.viewportLastSegmentStart = info.viewportLastSegmentStart;
    state.viewportLastSegmentEnd = info.viewportLastSegmentEnd;
    state.spellingIssueCount = info.spellingIssueCount;
    state.maximumLineColumns = info.maximumLineColumns;
    state.horizontalOffset = info.horizontalOffset;
    state.horizontalMaximumOffset = info.horizontalMaximumOffset;
    state.viewportWidth = info.viewportWidth;
    state.viewportHeight = info.viewportHeight;
    state.caretRectangle = info.caretRectangle;
    state.operationInProgress = info.operationInProgress;
    state.operationCompleted = info.operationCompleted;
    state.operationSucceeded = info.operationSucceeded;
    state.operationName = info.operationName;
    state.operationError = info.operationError;
    state.operationTargetPath = info.operationTargetPath;
    state.saveDiagnostics = info.saveDiagnostics;
    state.operationGeneration = info.operationGeneration;
    state.operationToken = info.operationToken;
    state.operationProgress = info.operationProgress;
    state.operationResultCount = info.operationResultCount;
    return state;
}

bool ClassicNotepadApp::AutomationGetLargeFileLine(
    std::uint64_t oneBasedLine,
    std::wstring& text,
    std::wstring& errorMessage) const
{
    if (largeFileView_ == nullptr) {
        errorMessage = L"Large-file mode is not active.";
        return false;
    }
    return largeFileView_->GetLine(oneBasedLine, text, errorMessage);
}

bool ClassicNotepadApp::AutomationGetLargeFileVisibleText(
    std::uint64_t& rangeStart,
    std::wstring& text,
    std::wstring& errorMessage)
{
    if (largeFileView_ == nullptr) {
        errorMessage = L"Large-file mode is not active.";
        return false;
    }
    return largeFileView_->GetVisibleText(text, rangeStart, errorMessage);
}

bool ClassicNotepadApp::AutomationGetLargeFileVisibleWindows(
    std::vector<LargeFileVisibleTextWindow>& windows,
    std::wstring& errorMessage)
{
    if (largeFileView_ == nullptr) {
        errorMessage = L"Large-file mode is not active.";
        return false;
    }
    return largeFileView_->GetVisibleTextWindows(windows, errorMessage);
}

bool ClassicNotepadApp::AutomationGetEditorVisibleSpellingWindows(
    std::vector<classic_notepad::SpellingTextWindow>& windows,
    std::wstring& errorMessage) const
{
    errorMessage.clear();
    if (largeFileView_ != nullptr || editor_ == nullptr) {
        errorMessage = L"The current document is not using the ordinary native editor.";
        return false;
    }
    if (!GetVisibleEditorTextWindows(windows)) {
        errorMessage = L"The ordinary editor has no visible spelling text window.";
        return false;
    }
    return true;
}

bool ClassicNotepadApp::AutomationResolveSpellingContextTarget(
    POINT clientPoint,
    bool keyboardInvocation,
    classic_notepad::SpellingContextTarget& target)
{
    target = {};
    if (!spellCheckAvailable_) return false;
    if (largeFileView_ != nullptr) {
        return largeFileView_->ResolveSpellingContextTargetAtPoint(
            clientPoint, keyboardInvocation, target);
    }
    return ResolveEditorSpellingContextTarget(clientPoint, keyboardInvocation, target);
}

bool ClassicNotepadApp::AutomationGetTextPositionPoint(std::uint64_t position, POINT& clientPoint)
{
    if (largeFileView_ != nullptr) {
        return largeFileView_->GetVisibleTextPositionPoint(position, clientPoint);
    }
    if (editor_ == nullptr || position > (std::numeric_limits<DWORD>::max)() ||
        position > static_cast<std::uint64_t>(GetWindowTextLengthW(editor_))) return false;
    clientPoint = PointFromEditorCharPosition(editor_, static_cast<DWORD>(position));
    RECT client{};
    return GetClientRect(editor_, &client) && PtInRect(&client, clientPoint) != 0;
}

bool ClassicNotepadApp::AutomationSendEditorContextMenu(int x, int y, bool keyboardInvocation)
{
    HWND targetWindow = largeFileView_ != nullptr ? largeFileView_->Window() : editor_;
    if (targetWindow == nullptr || !IsWindow(targetWindow) || mainWindow_ == nullptr ||
        SetTimer(mainWindow_, kAutomationContextMenuDismissTimerId, 150U, nullptr) == 0U) {
        return false;
    }
    LPARAM coordinates = -1;
    if (!keyboardInvocation) {
        POINT screenPoint{ x, y };
        if (!ClientToScreen(targetWindow, &screenPoint)) {
            KillTimer(mainWindow_, kAutomationContextMenuDismissTimerId);
            return false;
        }
        coordinates = MAKELPARAM(static_cast<short>(screenPoint.x), static_cast<short>(screenPoint.y));
    }
    SendMessageW(targetWindow, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(targetWindow), coordinates);
    KillTimer(mainWindow_, kAutomationContextMenuDismissTimerId);
    return true;
}

bool ClassicNotepadApp::AutomationSetLargeFileViewportSize(int width, int height)
{
    return largeFileView_ != nullptr && largeFileView_->SetViewportSizeForTesting(width, height);
}

bool ClassicNotepadApp::AutomationGetLargeFileTextRange(
    std::uint64_t start,
    std::uint64_t length,
    std::wstring& text,
    std::wstring& errorMessage) const
{
    if (largeFileView_ == nullptr) {
        errorMessage = L"Large-file mode is not active.";
        return false;
    }
    constexpr std::uint64_t maximum = 1024ULL * 1024ULL;
    if (length > maximum) {
        errorMessage = L"Large-file text ranges are limited to 1 MiB.";
        return false;
    }
    return largeFileView_->GetTextRange(start, length, text, errorMessage);
}

bool ClassicNotepadApp::AutomationSendLargeFileClick(int x, int y, bool shift)
{
    return largeFileView_ != nullptr && largeFileView_->SendClickInput(x, y, shift);
}

bool ClassicNotepadApp::AutomationSendLargeFileDrag(int startX, int startY, int endX, int endY)
{
    return largeFileView_ != nullptr && largeFileView_->SendDragInput(startX, startY, endX, endY);
}

bool ClassicNotepadApp::AutomationSendLargeFileKey(UINT virtualKey, bool shift, bool control)
{
    return largeFileView_ != nullptr && largeFileView_->SendKeyInput(virtualKey, shift, control);
}

bool ClassicNotepadApp::AutomationSendLargeFileVerticalScroll(UINT command, int trackPosition)
{
    return largeFileView_ != nullptr && largeFileView_->SendVerticalScrollInput(command, trackPosition);
}

bool ClassicNotepadApp::AutomationSendLargeFileWheel(int delta)
{
    return largeFileView_ != nullptr && largeFileView_->SendMouseWheelInput(delta);
}

bool ClassicNotepadApp::AutomationSendLargeFileHorizontalScroll(UINT command)
{
    if (largeFileView_ == nullptr || largeFileView_->Window() == nullptr || command > SB_ENDSCROLL) {
        return false;
    }
    SendMessageW(largeFileView_->Window(), WM_HSCROLL, MAKEWPARAM(command, 0), 0);
    return true;
}

bool ClassicNotepadApp::AutomationSendLargeFileText(const std::wstring& text)
{
    return largeFileView_ != nullptr && largeFileView_->SendTextInput(text);
}

bool ClassicNotepadApp::AutomationStartLargeFileFind(
    const std::wstring& text, bool matchCase, bool wholeWord, bool forward, std::wstring& errorMessage)
{
    return largeFileView_ != nullptr && largeFileView_->StartFindAsync(text, matchCase, wholeWord, forward, errorMessage);
}

bool ClassicNotepadApp::AutomationStartLargeFileReplaceAll(
    const std::wstring& text, const std::wstring& replacement,
    bool matchCase, bool wholeWord, std::wstring& errorMessage)
{
    return largeFileView_ != nullptr &&
        largeFileView_->StartReplaceAllAsync(text, replacement, matchCase, wholeWord, errorMessage);
}

void ClassicNotepadApp::AutomationCancelLargeFileOperation()
{
    if (largeFileView_ != nullptr) largeFileView_->CancelOperation();
}

bool ClassicNotepadApp::AutomationStartLargeFileSave(const std::wstring& path, std::wstring& errorMessage)
{
    if (largeFileView_ == nullptr) { errorMessage = L"Large-file mode is not active."; return false; }
    return largeFileView_->StartSaveAsync(path, document_.Encoding(), document_.SaveLineEnding(), true, errorMessage);
}

std::wstring ClassicNotepadApp::GetEditorText() const
{
    if (IsLargeFileMode() || editor_ == nullptr) {
        return {};
    }
    const int textLength = GetWindowTextLengthW(editor_);
    if (textLength <= 0) {
        return {};
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(textLength) + 1U, L'\0');
    const int copiedLength = GetWindowTextW(editor_, buffer.data(), static_cast<int>(buffer.size()));
    return std::wstring(buffer.data(), static_cast<std::size_t>(copiedLength));
}

std::wstring ClassicNotepadApp::GetSelectedText() const
{
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);
    if (selectionStart == selectionEnd) {
        return {};
    }

    if (selectionEnd < selectionStart) {
        std::swap(selectionStart, selectionEnd);
    }

    const std::wstring text = GetEditorText();
    const std::size_t start = std::min<std::size_t>(selectionStart, text.size());
    const std::size_t end = std::min<std::size_t>(selectionEnd, text.size());
    if (end <= start) {
        return {};
    }

    return text.substr(start, end - start);
}

void ClassicNotepadApp::SetEditorText(const std::wstring& text)
{
    horizontalScrollPosition_ = 0;
    InvalidateWidestLineCache();
    SetStatusCharacterCountFromText(text);
    suppressEditorChange_ = true;
    SendMessageW(editor_, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(editor_, text.c_str());
    SendMessageW(editor_, WM_SETREDRAW, TRUE, 0);
    SendMessageW(editor_, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(editor_, EM_SETMODIFY, FALSE, 0);
    suppressEditorChange_ = false;
    InvalidateWidestLineCache();
    UpdateStatusBar();
    UpdateScrollBars();
    InvalidateRect(editor_, nullptr, TRUE);
    AdvanceSpellingContentRevision();
    RefreshSpellCheck(true);
}

void ClassicNotepadApp::ReplaceEditorTextFromCommand(const std::wstring& text)
{
    horizontalScrollPosition_ = 0;
    InvalidateWidestLineCache();
    SetStatusCharacterCountFromText(text);
    suppressEditorChange_ = true;
    SendMessageW(editor_, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(editor_, text.c_str());
    SendMessageW(editor_, WM_SETREDRAW, TRUE, 0);
    SendMessageW(editor_, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(editor_, EM_SETMODIFY, TRUE, 0);
    suppressEditorChange_ = false;
    InvalidateWidestLineCache();

    document_.SetModified(true);
    UpdateTitle();
    UpdateStatusBar();
    UpdateScrollBars();
    InvalidateRect(editor_, nullptr, TRUE);
    AdvanceSpellingContentRevision();
    RefreshSpellCheck(true);
}

void ClassicNotepadApp::ReplaceSelection(const std::wstring& text)
{
    SendMessageW(editor_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
}

bool ClassicNotepadApp::ReplaceNativeWholeDocumentUndoably(
    const std::wstring& text, std::wstring& errorMessage)
{
    errorMessage.clear();
    if (editor_ == nullptr || text.size() > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
        errorMessage = L"The replacement text is too large for the native editor.";
        return false;
    }
    const int oldLength = std::max(0, GetWindowTextLengthW(editor_));
    const LRESULT nativeLimit = SendMessageW(editor_, EM_GETLIMITTEXT, 0, 0);
    if (nativeLimit >= 0 && static_cast<std::uint64_t>(text.size()) > static_cast<std::uint64_t>(nativeLimit)) {
        errorMessage = L"The replacement text exceeds the native editor's current text limit.";
        return false;
    }

    SendMessageW(editor_, EM_SETSEL, 0, oldLength);
    ReplaceSelection(text);
    const int actualLength = GetWindowTextLengthW(editor_);
    if (actualLength < 0 || static_cast<std::size_t>(actualLength) != text.size()) {
        errorMessage = L"The native editor did not accept the complete Replace All result.";
        return false;
    }
    if (GetEditorText() != text) {
        errorMessage = L"The native editor's Replace All result did not match the requested text.";
        return false;
    }
    return true;
}

void ClassicNotepadApp::GetSelectionRange(DWORD& selectionStart, DWORD& selectionEnd) const
{
    selectionStart = 0;
    selectionEnd = 0;
    SendMessageW(
        editor_,
        EM_GETSEL,
        reinterpret_cast<WPARAM>(&selectionStart),
        reinterpret_cast<LPARAM>(&selectionEnd));
}

DWORD ClassicNotepadApp::GetActiveCaretPosition() const
{
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);
    if (selectionStart == selectionEnd) return selectionEnd;
    if (nativeSelectionTrackingValid_ && nativeSelectionEditor_ == editor_ &&
        std::min(nativeSelectionAnchor_, nativeActiveCaretPosition_) == selectionStart &&
        std::max(nativeSelectionAnchor_, nativeActiveCaretPosition_) == selectionEnd) {
        return nativeActiveCaretPosition_;
    }

    POINT caretPoint{};
    if (!GetCaretPos(&caretPoint)) return selectionEnd;

    const auto pointForCharacter = [this](DWORD position, POINT& point) {
        const LRESULT packed = SendMessageW(editor_, EM_POSFROMCHAR, static_cast<WPARAM>(position), 0);
        if (packed == -1) return false;
        point.x = static_cast<short>(LOWORD(packed));
        point.y = static_cast<short>(HIWORD(packed));
        return true;
    };
    const auto samePoint = [&caretPoint](const POINT& point) {
        return point.x == caretPoint.x && point.y == caretPoint.y;
    };

    POINT startPoint{};
    POINT endPoint{};
    const bool hasStartPoint = pointForCharacter(selectionStart, startPoint);
    const bool hasEndPoint = pointForCharacter(selectionEnd, endPoint);
    const bool startMatchesCaret = hasStartPoint && samePoint(startPoint);
    const bool endMatchesCaret = hasEndPoint && samePoint(endPoint);
    if (startMatchesCaret != endMatchesCaret) {
        return startMatchesCaret ? selectionStart : selectionEnd;
    }
    if (hasStartPoint != hasEndPoint) {
        // At EOF EM_POSFROMCHAR may not return a point for selectionEnd. The
        // live caret still distinguishes the visible endpoint from its anchor.
        if (hasStartPoint) return startMatchesCaret ? selectionStart : selectionEnd;
        return endMatchesCaret ? selectionEnd : selectionStart;
    }
    return selectionEnd;
}

void ClassicNotepadApp::TrackNativeSelectionAfterMessage(HWND editor, UINT message, WPARAM wParam, LPARAM lParam)
{
    // Candidate controls must not change the committed editor's orientation.
    if (editor == nullptr || editor != editor_) return;
    if (nativeSelectionEditor_ != editor) nativeSelectionTrackingValid_ = false;
    nativeSelectionEditor_ = editor;
    if (message != WM_SETTEXT && message != EM_SETSEL && message != EM_REPLACESEL &&
        message != EM_UNDO && message != WM_UNDO && message != WM_CUT &&
        message != WM_CLEAR && message != WM_PASTE && message != WM_KEYDOWN &&
        message != WM_CHAR && message != WM_LBUTTONDOWN && message != WM_LBUTTONUP &&
        !(message == WM_MOUSEMOVE && (wParam & MK_LBUTTON) != 0)) return;
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);
    if (selectionEnd < selectionStart) std::swap(selectionStart, selectionEnd);

    const auto rememberCollapsed = [this](DWORD position) {
        nativeSelectionAnchor_ = position;
        nativeActiveCaretPosition_ = position;
        nativeSelectionTrackingValid_ = true;
    };
    const auto rememberFromAnchor = [this, selectionStart, selectionEnd]() {
        if (selectionStart == selectionEnd) {
            nativeSelectionAnchor_ = selectionEnd;
            nativeActiveCaretPosition_ = selectionEnd;
        } else if (nativeSelectionAnchor_ == selectionStart || nativeSelectionAnchor_ == selectionEnd) {
            nativeActiveCaretPosition_ = nativeSelectionAnchor_ == selectionStart
                ? selectionEnd : selectionStart;
        } else {
            nativeSelectionAnchor_ = selectionStart;
            nativeActiveCaretPosition_ = selectionEnd;
        }
        nativeSelectionTrackingValid_ = true;
    };

    if (message == EM_UNDO || message == WM_UNDO || message == EM_REPLACESEL ||
        message == WM_CUT || message == WM_CLEAR || message == WM_PASTE) {
        if (selectionStart != selectionEnd && message != EM_UNDO && message != WM_UNDO) {
            nativeSelectionTrackingValid_ = false;
            return;
        }
        // Native mutation/Undo selects the restored range with its caret at
        // the upper endpoint. Never reuse the pre-mutation anchor.
        nativeSelectionAnchor_ = selectionStart;
        nativeActiveCaretPosition_ = selectionEnd;
        nativeSelectionTrackingValid_ = true;
        return;
    }
    if (message == WM_SETTEXT) {
        rememberCollapsed(selectionEnd);
        return;
    }
    if (message == EM_SETSEL) {
        const DWORD textLength = static_cast<DWORD>(std::max(0, GetWindowTextLengthW(editor_)));
        const auto resolveRequested = [textLength](LPARAM value) {
            if (value < 0) return textLength;
            return static_cast<DWORD>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(value), textLength));
        };
        const DWORD requestedAnchor = static_cast<DWORD>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(wParam), textLength));
        const DWORD requestedActive = resolveRequested(lParam);
        if (selectionStart == selectionEnd) {
            rememberCollapsed(selectionEnd);
        } else if (requestedAnchor >= selectionStart && requestedAnchor <= selectionEnd &&
            requestedActive >= selectionStart && requestedActive <= selectionEnd &&
            requestedAnchor != requestedActive) {
            nativeSelectionAnchor_ = requestedAnchor;
            nativeActiveCaretPosition_ = requestedActive;
            nativeSelectionTrackingValid_ = true;
        } else {
            nativeSelectionAnchor_ = selectionStart;
            nativeActiveCaretPosition_ = selectionEnd;
            nativeSelectionTrackingValid_ = true;
        }
        return;
    }

    if (message == WM_LBUTTONDOWN) {
        if ((wParam & MK_SHIFT) != 0) {
            rememberFromAnchor();
            return;
        }
        rememberCollapsed(selectionEnd);
        return;
    }
    if ((message == WM_MOUSEMOVE && (wParam & MK_LBUTTON) != 0) || message == WM_LBUTTONUP) {
        rememberFromAnchor();
        return;
    }

    if (message != WM_KEYDOWN && message != WM_CHAR) return;
    if (selectionStart == selectionEnd) {
        rememberCollapsed(selectionEnd);
        return;
    }

    const bool extending = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const DWORD trackedStart = std::min(nativeSelectionAnchor_, nativeActiveCaretPosition_);
    const DWORD trackedEnd = std::max(nativeSelectionAnchor_, nativeActiveCaretPosition_);
    if (extending) {
        rememberFromAnchor();
    } else if (!nativeSelectionTrackingValid_ || trackedStart != selectionStart || trackedEnd != selectionEnd) {
        nativeSelectionAnchor_ = selectionStart;
        nativeActiveCaretPosition_ = selectionEnd;
        nativeSelectionTrackingValid_ = true;
    }
}

bool ClassicNotepadApp::HasSelection() const
{
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);
    return selectionStart != selectionEnd;
}

bool ClassicNotepadApp::CanPasteText() const
{
    return IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT);
}

void ClassicNotepadApp::SeedFindTextFromSelection()
{
    const std::wstring selectedText = GetSelectedText();
    if (!selectedText.empty() &&
        selectedText.find_first_of(L"\r\n") == std::wstring::npos &&
        selectedText.size() < findBuffer_.size()) {
        CopyToFixedBuffer(findBuffer_, selectedText);
    }
}

bool ClassicNotepadApp::FindNextWithFlags(DWORD flags, bool showNotFoundMessage, const std::wstring* textSnapshot)
{
    const std::wstring needle(findBuffer_.data());
    if (needle.empty()) {
        return false;
    }

    const std::wstring ownedText = textSnapshot == nullptr ? GetEditorText() : std::wstring();
    const std::wstring& text = textSnapshot != nullptr ? *textSnapshot : ownedText;
    if (text.empty() || needle.size() > text.size()) {
        if (showNotFoundMessage) {
            std::wstring message = L"Cannot find \"";
            message += needle;
            message += L"\".";
            ShowMessageDialog(message, MB_OK | MB_ICONINFORMATION);
        }
        return false;
    }

    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);
    if (selectionEnd < selectionStart) {
        std::swap(selectionStart, selectionEnd);
    }

    const bool searchDown = (flags & FR_DOWN) != 0;
    const bool matchCase = (flags & FR_MATCHCASE) != 0;
    const bool wholeWord = (flags & FR_WHOLEWORD) != 0;
    const std::size_t lastPossible = text.size() - needle.size();

    if (searchDown) {
        const std::size_t startPosition = std::min<std::size_t>(selectionEnd, text.size());
        for (std::size_t position = startPosition; position <= lastPossible; ++position) {
            if (TextMatchesAt(text, position, needle, matchCase, wholeWord)) {
                SendMessageW(editor_, EM_SETSEL, position, position + needle.size());
                SendMessageW(editor_, EM_SCROLLCARET, 0, 0);
                AdvanceSpellingLayoutRevision();
                RefreshSpellCheck(false);
                UpdateStatusBar();
                UpdateScrollBars();
                SetFocus(editor_);
                findFlags_ = flags & kFindOptionFlags;
                return true;
            }
        }
    } else if (selectionStart >= needle.size()) {
        std::size_t position = std::min<std::size_t>(selectionStart - needle.size(), lastPossible);
        for (;;) {
            if (TextMatchesAt(text, position, needle, matchCase, wholeWord)) {
                SendMessageW(editor_, EM_SETSEL, position, position + needle.size());
                SendMessageW(editor_, EM_SCROLLCARET, 0, 0);
                AdvanceSpellingLayoutRevision();
                RefreshSpellCheck(false);
                UpdateStatusBar();
                UpdateScrollBars();
                SetFocus(editor_);
                findFlags_ = flags & kFindOptionFlags;
                return true;
            }

            if (position == 0U) {
                break;
            }

            --position;
        }
    }

    if (showNotFoundMessage) {
        std::wstring message = L"Cannot find \"";
        message += needle;
        message += L"\".";
        ShowMessageDialog(message, MB_OK | MB_ICONINFORMATION);
    }

    return false;
}

bool ClassicNotepadApp::ReplaceCurrentSelectionIfMatch(DWORD flags)
{
    if (!SelectionMatchesFindText(flags, nullptr)) {
        return false;
    }

    ReplaceSelection(std::wstring(replaceBuffer_.data()));
    return true;
}

void ClassicNotepadApp::ReplaceAllMatches(DWORD flags)
{
    const std::wstring needle(findBuffer_.data());
    if (needle.empty()) {
        return;
    }

    const std::wstring replacement(replaceBuffer_.data());
    const std::wstring text = GetEditorText();
    const bool matchCase = (flags & FR_MATCHCASE) != 0;
    const bool wholeWord = (flags & FR_WHOLEWORD) != 0;

    std::wstring replaced;
    replaced.reserve(text.size());

    std::size_t replacementCount = 0;
    for (std::size_t position = 0; position < text.size();) {
        if (TextMatchesAt(text, position, needle, matchCase, wholeWord)) {
            replaced += replacement;
            position += needle.size();
            ++replacementCount;
        } else {
            replaced.push_back(text[position]);
            ++position;
        }
    }

    if (replacementCount == 0U) {
        std::wstring message = L"Cannot find \"";
        message += needle;
        message += L"\".";
        ShowMessageDialog(message, MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring replaceError;
    if (!ReplaceNativeWholeDocumentUndoably(replaced, replaceError)) {
        ShowError(replaceError);
        return;
    }
    SendMessageW(editor_, EM_SETSEL, 0, 0);
    SendMessageW(editor_, EM_SCROLLCARET, 0, 0);
    UpdateStatusBar();
    UpdateScrollBars();
    SetFocus(editor_);
}

bool ClassicNotepadApp::SelectionMatchesFindText(DWORD flags, const std::wstring* textSnapshot) const
{
    const std::wstring needle(findBuffer_.data());
    if (needle.empty()) {
        return false;
    }

    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);
    if (selectionEnd < selectionStart) {
        std::swap(selectionStart, selectionEnd);
    }

    if (selectionEnd <= selectionStart || selectionEnd - selectionStart != needle.size()) {
        return false;
    }

    const std::wstring ownedText = textSnapshot == nullptr ? GetEditorText() : std::wstring();
    const std::wstring& text = textSnapshot != nullptr ? *textSnapshot : ownedText;
    if (selectionEnd > text.size()) {
        return false;
    }

    const bool matchCase = (flags & FR_MATCHCASE) != 0;
    const bool wholeWord = (flags & FR_WHOLEWORD) != 0;
    return TextMatchesAt(text, selectionStart, needle, matchCase, wholeWord);
}

bool ClassicNotepadApp::PrintEditorText(HDC printerDc, std::wstring& errorMessage)
{
    auto setAutomationFailure = [&](const std::wstring& stage, const std::wstring& message) {
        if (automationMode_) {
            automationPrintDiagnostics_.succeeded = false;
            automationPrintDiagnostics_.failureStage = stage;
            automationPrintDiagnostics_.errorMessage = message;
        }
    };
    auto shouldInjectFailure = [&](const std::wstring& stage) {
        if (!automationMode_ || automationPrintFaultStage_ != stage) {
            return false;
        }
        automationPrintFaultStage_.clear();
        automationPrintDiagnostics_.faultInjected = true;
        automationPrintDiagnostics_.injectedStage = stage;
        automationPrintDiagnostics_.failureStage = stage;
        return true;
    };

    if (printerDc == nullptr) {
        errorMessage = L"Printing failed.\n\nThe selected printer could not be opened.";
        setAutomationFailure(L"PrinterDC", errorMessage);
        return false;
    }

    const RECT textRect = BuildPrintableTextRect(printerDc, pageMarginsThousandths_);
    const int printableWidth = textRect.right - textRect.left;
    const int printableHeight = textRect.bottom - textRect.top;
    if (printableWidth <= 0 || printableHeight <= 0) {
        errorMessage = L"Printing failed.\n\nThe printable page area is too small.";
        setAutomationFailure(L"PrintableArea", errorMessage);
        return false;
    }

    HFONT printFont = CreatePrinterFont(printerDc, editorFont_, mainWindow_);
    if (printFont == nullptr) {
        errorMessage = L"Printing failed.\n\nThe selected editor font could not be prepared for printing.";
        setAutomationFailure(L"CreateFont", errorMessage);
        return false;
    }

    HGDIOBJ previousFont = SelectObject(printerDc, printFont);
    if (previousFont == nullptr || previousFont == HGDI_ERROR) {
        DeleteObject(printFont);
        errorMessage = L"Printing failed.\n\nThe printer could not select the editor font.";
        setAutomationFailure(L"SelectFont", errorMessage);
        return false;
    }

    SetBkMode(printerDc, TRANSPARENT);
    SetTextColor(printerDc, RGB(0, 0, 0));

    TEXTMETRICW textMetrics{};
    if (!GetTextMetricsW(printerDc, &textMetrics)) {
        SelectObject(printerDc, previousFont);
        DeleteObject(printFont);
        errorMessage = L"Printing failed.\n\nThe printer could not measure the selected font.";
        setAutomationFailure(L"GetTextMetrics", errorMessage);
        return false;
    }

    const int lineHeight = std::max(1, static_cast<int>(textMetrics.tmHeight + textMetrics.tmExternalLeading));
    if (lineHeight > printableHeight) {
        SelectObject(printerDc, previousFont);
        DeleteObject(printFont);
        errorMessage = L"Printing failed.\n\nThe selected font is too large for the printable page area.";
        setAutomationFailure(L"PrintableArea", errorMessage);
        return false;
    }

    std::wstring documentName = document_.DisplayName();
    if (documentName.empty()) {
        documentName = L"Classic Notepad Document";
    }

    DOCINFOW docInfo{};
    docInfo.cbSize = sizeof(docInfo);
    docInfo.lpszDocName = documentName.c_str();

    if (shouldInjectFailure(L"startDoc")) {
        SelectObject(printerDc, previousFont);
        DeleteObject(printFont);
        errorMessage = L"Printing failed.\n\nThe print job could not be started.";
        setAutomationFailure(L"StartDoc", errorMessage);
        return false;
    }

    if (StartDocW(printerDc, &docInfo) <= 0) {
        SelectObject(printerDc, previousFont);
        DeleteObject(printFont);
        errorMessage = L"Printing failed.\n\nThe print job could not be started.";
        setAutomationFailure(L"StartDoc", errorMessage);
        return false;
    }

    auto failPrintJob = [&](const std::wstring& message, const std::wstring& stage) {
        const int abortResult = AbortDoc(printerDc);
        if (automationMode_) {
            automationPrintDiagnostics_.abortDocCalled = true;
            automationPrintDiagnostics_.abortDocSucceeded = abortResult > 0;
        }
        SelectObject(printerDc, previousFont);
        DeleteObject(printFont);
        errorMessage = message;
        setAutomationFailure(stage, message);
        return false;
    };

    if (shouldInjectFailure(L"startPage") || StartPage(printerDc) <= 0) {
        return failPrintJob(L"Printing failed.\n\nThe first page could not be started.", L"StartPage");
    }

    int y = textRect.top;
    auto startNewPage = [&]() {
        if (shouldInjectFailure(L"endPage")) {
            return false;
        }
        if (EndPage(printerDc) <= 0) {
            setAutomationFailure(L"EndPage", L"Printing failed.\n\nThe current page could not be completed.");
            return false;
        }

        if (shouldInjectFailure(L"startPage")) {
            return false;
        }
        if (StartPage(printerDc) <= 0) {
            setAutomationFailure(L"StartPage", L"Printing failed.\n\nA new page could not be started.");
            return false;
        }

        y = textRect.top;
        return true;
    };

    const std::vector<std::wstring> lines = SplitPrintLines(GetEditorText());
    for (const std::wstring& sourceLine : lines) {
        const std::wstring line = ExpandTabsForPrinting(sourceLine);

        if (line.empty()) {
            if (y + lineHeight > textRect.bottom && !startNewPage()) {
                const std::wstring stage = automationPrintDiagnostics_.failureStage.empty()
                    ? L"StartPage" : automationPrintDiagnostics_.failureStage;
                return failPrintJob(L"Printing failed.\n\nA new page could not be started.", stage);
            }

            y += lineHeight;
            continue;
        }

        std::size_t start = 0;
        while (start < line.size()) {
            if (y + lineHeight > textRect.bottom && !startNewPage()) {
                const std::wstring stage = automationPrintDiagnostics_.failureStage.empty()
                    ? L"StartPage" : automationPrintDiagnostics_.failureStage;
                return failPrintJob(L"Printing failed.\n\nA new page could not be started.", stage);
            }

            std::size_t length = FindFittingTextLength(printerDc, line, start, printableWidth);
            length = FindWrapBreakLength(line, start, length);

            std::size_t printLength = length;
            while (printLength > 0U && line[start + printLength - 1U] == L' ') {
                --printLength;
            }

            if (printLength > 0U) {
                if (shouldInjectFailure(L"textOut")) {
                    return failPrintJob(L"Printing failed.\n\nText could not be sent to the printer.", L"TextOut");
                }
                if (!TextOutW(printerDc, textRect.left, y, line.c_str() + start, static_cast<int>(printLength))) {
                    return failPrintJob(L"Printing failed.\n\nText could not be sent to the printer.", L"TextOut");
                }
            }

            y += lineHeight;
            start += length;
            while (start < line.size() && line[start] == L' ') {
                ++start;
            }
        }
    }

    if (shouldInjectFailure(L"endPage") || EndPage(printerDc) <= 0) {
        return failPrintJob(L"Printing failed.\n\nThe last page could not be completed.", L"EndPage");
    }

    if (shouldInjectFailure(L"endDoc")) {
        return failPrintJob(L"Printing failed.\n\nThe print job could not be completed.", L"EndDoc");
    }

    if (EndDoc(printerDc) <= 0) {
        errorMessage = L"Printing failed.\n\nThe print job could not be completed.";
        return failPrintJob(errorMessage, L"EndDoc");
    }

    SelectObject(printerDc, previousFont);
    DeleteObject(printFont);
    if (automationMode_) {
        automationPrintDiagnostics_.succeeded = true;
    }
    return true;
}

void ClassicNotepadApp::CloseFindReplaceDialogs()
{
    if (findDialog_ != nullptr) {
        DestroyWindow(findDialog_);
        findDialog_ = nullptr;
    }

    if (replaceDialog_ != nullptr) {
        DestroyWindow(replaceDialog_);
        replaceDialog_ = nullptr;
    }
}

void ClassicNotepadApp::DestroyOwnedEditorFont()
{
    if (ownsEditorFont_ && editorFont_ != nullptr) {
        DeleteObject(editorFont_);
    }

    editorFont_ = nullptr;
    ownsEditorFont_ = false;
}

void ClassicNotepadApp::DestroyOwnedMenuFont()
{
    if (ownsMenuFont_ && menuFont_ != nullptr) {
        DeleteObject(menuFont_);
    }

    menuFont_ = nullptr;
    ownsMenuFont_ = false;
}

void ClassicNotepadApp::DestroyPrintDialogHandles()
{
    if (pageSetupDevMode_ != nullptr) {
        GlobalFree(pageSetupDevMode_);
        pageSetupDevMode_ = nullptr;
    }

    if (pageSetupDevNames_ != nullptr) {
        GlobalFree(pageSetupDevNames_);
        pageSetupDevNames_ = nullptr;
    }
}

void ClassicNotepadApp::HandleFindReplaceMessage(LPARAM lParam)
{
    const auto* findReplace = reinterpret_cast<const FINDREPLACEW*>(lParam);
    if (findReplace == nullptr) {
        return;
    }

    if ((findReplace->Flags & FR_DIALOGTERM) != 0) {
        findDialog_ = nullptr;
        replaceDialog_ = nullptr;
        return;
    }

    findFlags_ = findReplace->Flags & kFindOptionFlags;

    if (IsLargeFileMode()) {
        const bool matchCase = (findReplace->Flags & FR_MATCHCASE) != 0;
        const bool wholeWord = (findReplace->Flags & FR_WHOLEWORD) != 0;
        const bool down = (findReplace->Flags & FR_DOWN) != 0;
        std::wstring error;
        if ((findReplace->Flags & FR_FINDNEXT) != 0) {
            largeFileView_->StartFindAsync(findBuffer_.data(), matchCase, wholeWord, down, error);
        } else if ((findReplace->Flags & FR_REPLACE) != 0) {
            const bool selectedMatch = largeFileView_->SelectionMatches(
                findBuffer_.data(), matchCase, wholeWord, error);
            if (error.empty() && selectedMatch) {
                if (largeFileView_->InsertText(replaceBuffer_.data(), error)) {
                    largeFileView_->StartFindAsync(findBuffer_.data(), matchCase, wholeWord, down, error);
                }
            } else if (error.empty()) {
                largeFileView_->StartFindAsync(findBuffer_.data(), matchCase, wholeWord, down, error);
            }
        } else if ((findReplace->Flags & FR_REPLACEALL) != 0) {
            largeFileView_->StartReplaceAllAsync(findBuffer_.data(), replaceBuffer_.data(), matchCase, wholeWord, error);
        }
        if (!error.empty()) ShowError(error);
        return;
    }

    if ((findReplace->Flags & FR_FINDNEXT) != 0) {
        FindNextWithFlags(findReplace->Flags, true);
    } else if ((findReplace->Flags & FR_REPLACE) != 0) {
        const std::wstring textSnapshot = GetEditorText();
        const bool replaced = SelectionMatchesFindText(findReplace->Flags, &textSnapshot);
        if (replaced) {
            ReplaceSelection(std::wstring(replaceBuffer_.data()));
            FindNextWithFlags(findReplace->Flags, false);
        } else {
            FindNextWithFlags(findReplace->Flags, true, &textSnapshot);
        }
    } else if ((findReplace->Flags & FR_REPLACEALL) != 0) {
        ReplaceAllMatches(findReplace->Flags);
    }
}

std::uint64_t ClassicNotepadApp::ShowGoToDialog(std::uint64_t currentLine, std::uint64_t maxLine)
{
    GoToDialogState state{};
    state.currentLine = currentLine;
    state.maxLine = maxLine;
    state.selectedLine = currentLine;
    state.automationInitialValue = automationGoToDialogProbe_
        ? &automationGoToDialogInitialValue_ : nullptr;

    const INT_PTR result = DialogBoxParamW(
        instance_,
        MAKEINTRESOURCEW(IDD_GOTO_DIALOG),
        mainWindow_,
        ClassicNotepadApp::GoToDialogProc,
        reinterpret_cast<LPARAM>(&state));

    return result == IDOK ? state.selectedLine : 0U;
}

void ClassicNotepadApp::GoToLine(std::uint64_t lineNumber)
{
    DWORD characterIndex = 0U;
    if (lineNumber == 0U || !EnsureNativeStatusSummaryComplete() ||
        !GetNativeLogicalLineStart(lineNumber, characterIndex)) return;

    SendMessageW(editor_, EM_SETSEL, static_cast<WPARAM>(characterIndex), static_cast<LPARAM>(characterIndex));
    SendMessageW(editor_, EM_SCROLLCARET, 0, 0);
    AdvanceSpellingLayoutRevision();
    RefreshSpellCheck(false);
    UpdateStatusBar();
    UpdateScrollBars();
    SetFocus(editor_);
}

std::wstring ClassicNotepadApp::BuildTimeDateText() const
{
    std::array<wchar_t, 128> timeBuffer{};
    std::array<wchar_t, 128> dateBuffer{};

    GetTimeFormatW(
        LOCALE_USER_DEFAULT,
        TIME_NOSECONDS,
        nullptr,
        nullptr,
        timeBuffer.data(),
        static_cast<int>(timeBuffer.size()));

    GetDateFormatW(
        LOCALE_USER_DEFAULT,
        DATE_SHORTDATE,
        nullptr,
        nullptr,
        dateBuffer.data(),
        static_cast<int>(dateBuffer.size()));

    std::wstring timeDate(timeBuffer.data());
    if (!timeDate.empty() && dateBuffer[0] != L'\0') {
        timeDate += L' ';
    }

    timeDate += dateBuffer.data();
    return timeDate;
}

std::wstring ClassicNotepadApp::ShowOpenFileDialog()
{
    std::vector<wchar_t> fileName(kFileDialogPathBufferChars, L'\0');
    OPENFILENAMEW openFileName{};
    openFileName.lStructSize = sizeof(openFileName);
    openFileName.hwndOwner = mainWindow_;
    openFileName.lpstrFilter = kFileDialogFilter;
    openFileName.lpstrFile = fileName.data();
    openFileName.nMaxFile = static_cast<DWORD>(fileName.size());
    openFileName.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&openFileName)) {
        return fileName.data();
    }

    const DWORD dialogError = CommDlgExtendedError();
    if (dialogError != 0) {
        ShowError(L"The Open dialog could not be shown.");
    }

    return {};
}

std::wstring ClassicNotepadApp::ShowSaveFileDialog()
{
    std::vector<wchar_t> fileName(kFileDialogPathBufferChars, L'\0');
    if (document_.HasPath()) {
        wcsncpy_s(fileName.data(), fileName.size(), document_.Path().c_str(), _TRUNCATE);
    }

    OPENFILENAMEW saveFileName{};
    saveFileName.lStructSize = sizeof(saveFileName);
    saveFileName.hwndOwner = mainWindow_;
    saveFileName.lpstrFilter = kFileDialogFilter;
    saveFileName.lpstrFile = fileName.data();
    saveFileName.nMaxFile = static_cast<DWORD>(fileName.size());
    saveFileName.lpstrDefExt = L"txt";
    saveFileName.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (GetSaveFileNameW(&saveFileName)) {
        return fileName.data();
    }

    const DWORD dialogError = CommDlgExtendedError();
    if (dialogError != 0) {
        ShowError(L"The Save dialog could not be shown.");
    }

    return {};
}

void ClassicNotepadApp::ShowError(const std::wstring& message)
{
    if (automationMode_) {
        return;
    }

    ShowMessageDialog(message, MB_OK | MB_ICONERROR);
}

int ClassicNotepadApp::ShowMessageDialog(const std::wstring& message, UINT flags) const
{
    if (automationMode_) {
        if (automationMessageDialogResultOverride_ != 0) {
            return automationMessageDialogResultOverride_;
        }
        return IDCANCEL;
    }

    return ShowThemedMessageDialog(mainWindow_, instance_, message, L"Classic Notepad", flags, darkModeEnabled_);
}

LRESULT ClassicNotepadApp::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == findReplaceMessage_) {
        HandleFindReplaceMessage(lParam);
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        mainMenu_ = GetMenu(mainWindow_);
        menuBar_ = CreateMenuBar();
        if (menuBar_ == nullptr) {
            MessageBoxW(mainWindow_, L"Could not create the menu bar.", L"Classic Notepad", MB_OK | MB_ICONERROR);
            return -1;
        }
        editor_ = CreateEditor(true, wordWrap_);
        if (editor_ == nullptr) {
            MessageBoxW(mainWindow_, L"Could not create the editor control.", L"Classic Notepad", MB_OK | MB_ICONERROR);
            return -1;
        }
        verticalScrollBar_ = CreateCustomScrollBar(ScrollBarOrientation::Vertical);
        horizontalScrollBar_ = CreateCustomScrollBar(ScrollBarOrientation::Horizontal);
        scrollCorner_ = CreateScrollCorner();
        if (verticalScrollBar_ == nullptr || horizontalScrollBar_ == nullptr || scrollCorner_ == nullptr) {
            MessageBoxW(mainWindow_, L"Could not create the custom scrollbars.", L"Classic Notepad", MB_OK | MB_ICONERROR);
            return -1;
        }
        statusBar_ = CreateStatusBar();
        if (statusBar_ == nullptr) {
            MessageBoxW(mainWindow_, L"Could not create the status bar.", L"Classic Notepad", MB_OK | MB_ICONERROR);
            return -1;
        }
        ApplyThemeToWindows();
        document_.ResetUntitled();
        spellCheckAvailable_ = comInitialized_ && spellChecker_.Initialize(L"en-GB");
        if (!spellCheckAvailable_ && !automationMode_) {
            ShowSpellingUnavailableMessage();
        }
        ResizeEditor();
        UpdateStatusBar();
        RefreshSpellCheck(true);
        SetFocus(editor_);
        return 0;

    case WM_SIZE:
        ResizeEditor();
        InvalidateRect(editor_, nullptr, FALSE);
        if (largeFileView_ != nullptr) {
            InvalidateRect(largeFileView_->Window(), nullptr, FALSE);
        }
        return 0;

    case WM_SETFOCUS:
        if (largeFileView_ != nullptr) {
            largeFileView_->Focus();
        } else if (editor_ != nullptr) {
            SetFocus(editor_);
        }
        return 0;

    case WM_SYSKEYDOWN:
        if (wParam == VK_MENU && ActivateMenuBarFromKeyboard()) {
            return 0;
        }
        break;

    case WM_SYSCHAR:
        if (ActivateMenuMnemonic(static_cast<wchar_t>(wParam))) {
            return 0;
        }
        break;

    case WM_SYSKEYUP:
        if (wParam == VK_MENU && menuKeyboardActive_) {
            return 0;
        }
        break;

    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        UpdateThemeFromSystem();
        return 0;

    case WM_NCHITTEST: {
        POINT point{
            static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
            static_cast<LONG>(static_cast<short>(HIWORD(lParam)))
        };
        ScreenToClient(mainWindow_, &point);
        const RECT gripRect = GetResizeGripRect();
        if (darkModeEnabled_ && IsResizableFromGrip() && !IsRectEmpty(&gripRect) && PtInRect(&gripRect, point)) {
            return HTBOTTOMRIGHT;
        }
        break;
    }

    case WM_ERASEBKGND:
        if (darkModeEnabled_ && darkStatusBackgroundBrush_ != nullptr) {
            RECT clientRect{};
            GetClientRect(mainWindow_, &clientRect);
            FillRect(reinterpret_cast<HDC>(wParam), &clientRect, darkStatusBackgroundBrush_);
            return 1;
        }
        break;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        const LRESULT controlColorResult = HandleControlColor(
            reinterpret_cast<HDC>(wParam),
            reinterpret_cast<HWND>(lParam));
        if (controlColorResult != 0) {
            return controlColorResult;
        }
        break;
    }

    case WM_DRAWITEM: {
        const LRESULT drawItemResult = HandleDrawItem(wParam, lParam);
        if (drawItemResult != 0) {
            return drawItemResult;
        }
        break;
    }

    case WM_MEASUREITEM: {
        const LRESULT measureItemResult = HandleMeasureItem(wParam, lParam);
        if (measureItemResult != 0) {
            return measureItemResult;
        }
        break;
    }

    case WM_INITMENUPOPUP:
        if (HIWORD(lParam) == 0) {
            UpdateMenuState(ActiveMenu());
        }
        return 0;

    case WM_NOTIFY: {
        const LRESULT notifyResult = HandleNotify(lParam);
        if (notifyResult != 0) {
            return notifyResult;
        }
        break;
    }

    case WM_MOUSEWHEEL:
        if (HandleMouseWheel(wParam)) {
            return 0;
        }
        break;

    case WM_COMMAND:
        if (automationProbeEditor_ != nullptr && reinterpret_cast<HWND>(lParam) == automationProbeEditor_) {
            const UINT notification = HIWORD(wParam);
            if (notification == EN_CHANGE) automationProbeNotificationMask_ |= 1U;
            else if (notification == EN_ERRSPACE) automationProbeNotificationMask_ |= 2U;
            else if (notification == EN_MAXTEXT) automationProbeNotificationMask_ |= 4U;
            return 0;
        }
        if (reinterpret_cast<HWND>(lParam) == editor_ && editorPasteReplacementActive_) {
            const UINT notification = HIWORD(wParam);
            if (notification == EN_CHANGE) {
                editorPasteChangeNotification_ = true;
                return 0;
            }
            else if (notification == EN_ERRSPACE || notification == EN_MAXTEXT) {
                editorPasteNativeFailureNotification_ = notification;
                return 0;
            }
        }
        if (reinterpret_cast<HWND>(lParam) == editor_ && HIWORD(wParam) == EN_CHANGE) {
            UpdateEditorWidthIndexForMutation();
            HandleEditorChanged();
            return 0;
        }

        switch (LOWORD(wParam)) {
        case ID_FILE_NEW:
            HandleNew();
            return 0;

        case ID_FILE_OPEN:
            HandleOpen();
            return 0;

        case ID_FILE_SAVE:
            HandleSave();
            return 0;

        case ID_FILE_SAVE_AS:
            HandleSaveAs();
            return 0;

        case ID_FILE_PAGE_SETUP:
            HandlePageSetup();
            return 0;

        case ID_FILE_PRINT:
            HandlePrint();
            return 0;

        case ID_FILE_EXIT:
            SendMessageW(mainWindow_, WM_CLOSE, 0, 0);
            return 0;

        case ID_EDIT_UNDO:
            HandleUndo();
            return 0;

        case ID_EDIT_CUT:
            HandleCut();
            return 0;

        case ID_EDIT_COPY:
            HandleCopy();
            return 0;

        case ID_EDIT_PASTE:
            HandlePaste();
            return 0;

        case ID_EDIT_DELETE:
            HandleDelete();
            return 0;

        case ID_EDIT_FIND:
            HandleFind();
            return 0;

        case ID_EDIT_FIND_NEXT:
            HandleFindNext();
            return 0;

        case ID_EDIT_REPLACE:
            HandleReplace();
            return 0;

        case ID_EDIT_GO_TO:
            HandleGoTo();
            return 0;

        case ID_EDIT_SELECT_ALL:
            HandleSelectAll();
            return 0;

        case ID_EDIT_TIME_DATE:
            HandleTimeDate();
            return 0;

        case ID_FORMAT_WORD_WRAP:
            HandleToggleWordWrap();
            return 0;

        case ID_FORMAT_FONT:
            HandleChooseFont();
            return 0;

        case ID_VIEW_STATUS_BAR:
            HandleToggleStatusBar();
            return 0;

        case ID_HELP_ABOUT:
            ShowAboutDialog();
            return 0;

        default:
            break;
        }
        break;

    case WM_CLOSE:
        RequestDocumentTransition(DocumentTransition::CloseWindow);
        return 0;

    case WM_DESTROY:
        largeFileView_.reset();
        CloseFindReplaceDialogs();
        if (spellCheckTimerId_ != 0) {
            KillTimer(mainWindow_, spellCheckTimerId_);
            spellCheckTimerId_ = 0;
        }
        KillTimer(mainWindow_, kEditorWidthIndexTimerId);
        KillTimer(mainWindow_, kStatusCharacterCountTimerId);
        if (mainMenu_ != nullptr && GetMenu(mainWindow_) != mainMenu_) {
            DestroyMenu(mainMenu_);
            mainMenu_ = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    case WM_TIMER:
        if (wParam == kAutomationContextMenuDismissTimerId) {
            KillTimer(mainWindow_, kAutomationContextMenuDismissTimerId);
            EndMenu();
            return 0;
        }
        if (wParam == kSpellCheckTimerId) {
            RunSpellCheckNow();
            return 0;
        }
        if (wParam == kEditorWidthIndexTimerId) {
            MeasureEditorWidthBatch();
            return 0;
        }
        if (wParam == kStatusCharacterCountTimerId) {
            MeasureStatusCharacterCountBatch();
            return 0;
        }
        break;

    case WM_APP + 101:
        if (largeFileView_ != nullptr) {
            LargeFileViewInfo info = largeFileView_->GetInfo();
            document_.SetModified(info.modified);
            const std::uint64_t completionToken = static_cast<std::uint64_t>(wParam);
            if (documentTransitionCoordinator_.TryAcceptOperationNotification(
                    completionToken, info.operationToken, info.operationCompleted)) {
                if (info.operationSucceeded && info.operationName == L"Save" &&
                    !info.operationTargetPath.empty() && document_.Path() != info.operationTargetPath) {
                    document_.AdoptLargeFileSaveAs(std::move(info.operationTargetPath));
                    document_.SetModified(info.modified);
                }
                largeFileView_->AcknowledgeOperationCompletion(completionToken);
                const LargeFileViewInfo acknowledgedInfo = largeFileView_->GetInfo();
                document_.SetModified(acknowledgedInfo.modified);
                if (ResumePendingTransitionAfterLargeSave(acknowledgedInfo)) {
                    if (largeFileView_ != nullptr) {
                        UpdateStatusBar();
                        UpdateTitle();
                        RefreshSpellCheck(false);
                    }
                    return 0;
                }
            }
            UpdateStatusBar();
            UpdateTitle();
            RefreshSpellCheck(false);
        }
        return 0;

    default:
        break;
    }

    return DefWindowProcW(mainWindow_, message, wParam, lParam);
}

INT_PTR CALLBACK ClassicNotepadApp::GoToDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<GoToDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));

    switch (message) {
    case WM_INITDIALOG:
        state = reinterpret_cast<GoToDialogState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        if (state != nullptr) {
            SetDlgItemTextW(dialog, IDC_GOTO_LINE_EDIT, std::to_wstring(state->currentLine).c_str());
            SendDlgItemMessageW(dialog, IDC_GOTO_LINE_EDIT, EM_SETLIMITTEXT, 20, 0);
            SendDlgItemMessageW(dialog, IDC_GOTO_LINE_EDIT, EM_SETSEL, 0, -1);
            if (state->automationInitialValue != nullptr) {
                std::array<wchar_t, 32> initialValue{};
                GetDlgItemTextW(dialog, IDC_GOTO_LINE_EDIT, initialValue.data(),
                    static_cast<int>(initialValue.size()));
                *state->automationInitialValue = initialValue.data();
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
        }
        SetFocus(GetDlgItem(dialog, IDC_GOTO_LINE_EDIT));
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            if (state != nullptr) {
                std::array<wchar_t, 32> text{};
                GetDlgItemTextW(dialog, IDC_GOTO_LINE_EDIT, text.data(), static_cast<int>(text.size()));
                wchar_t* end = nullptr;
                errno = 0;
                const unsigned long long parsed = std::wcstoull(text.data(), &end, 10);
                while (end != nullptr && std::iswspace(*end)) ++end;
                const bool translated = end != text.data() && end != nullptr && *end == L'\0' && errno != ERANGE;
                const std::uint64_t lineNumber = static_cast<std::uint64_t>(parsed);
                if (!translated || lineNumber == 0U || lineNumber > state->maxLine) {
                    std::wstring messageText = L"Line number must be between 1 and ";
                    messageText += std::to_wstring(state->maxLine);
                    messageText += L".";
                    MessageBoxW(dialog, messageText.c_str(), L"Classic Notepad", MB_OK | MB_ICONWARNING);
                    SendDlgItemMessageW(dialog, IDC_GOTO_LINE_EDIT, EM_SETSEL, 0, -1);
                    SetFocus(GetDlgItem(dialog, IDC_GOTO_LINE_EDIT));
                    return TRUE;
                }

                state->selectedLine = lineNumber;
            }
            EndDialog(dialog, IDOK);
            return TRUE;

        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;

        default:
            break;
        }
        break;

    default:
        break;
    }

    return FALSE;
}

INT_PTR CALLBACK ClassicNotepadApp::AboutDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<AboutDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));

    switch (message) {
    case WM_INITDIALOG:
        state = reinterpret_cast<AboutDialogState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetDlgItemTextW(dialog, IDC_ABOUT_VERSION, CLASSIC_NOTEPAD_VERSION_DISPLAY_W);
        if (state != nullptr) {
            if (state->darkMode) {
                ApplyDarkTitleBar(dialog, true);
                state->backgroundBrush = CreateSolidBrush(MessageDialogBackgroundColor(true));

                HWND okButton = GetDlgItem(dialog, IDOK);
                if (okButton != nullptr) {
                    const LONG_PTR style = GetWindowLongPtrW(okButton, GWL_STYLE);
                    SetWindowLongPtrW(okButton, GWL_STYLE, (style & ~BS_TYPEMASK) | BS_OWNERDRAW);
                    SetWindowPos(
                        okButton,
                        nullptr,
                        0,
                        0,
                        0,
                        0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
                }
            }

            state->largeIcon = static_cast<HICON>(LoadImageW(
                state->instance,
                MAKEINTRESOURCEW(IDI_APPICON),
                IMAGE_ICON,
                kAboutIconSizePixels,
                kAboutIconSizePixels,
                LR_DEFAULTCOLOR));
            state->ownsLargeIcon = state->largeIcon != nullptr;
            if (state->largeIcon == nullptr) {
                state->largeIcon = LoadIconW(nullptr, IDI_APPLICATION);
            }
        }
        return TRUE;

    case WM_CTLCOLORDLG:
        if (state != nullptr && state->darkMode && state->backgroundBrush != nullptr) {
            return reinterpret_cast<INT_PTR>(state->backgroundBrush);
        }
        break;

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        if (state != nullptr && state->darkMode && state->backgroundBrush != nullptr) {
            HDC deviceContext = reinterpret_cast<HDC>(wParam);
            SetTextColor(deviceContext, MessageDialogTextColor(true));
            SetBkMode(deviceContext, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(state->backgroundBrush);
        }
        break;

    case WM_DRAWITEM:
        if (wParam == IDC_ABOUT_ICON && state != nullptr && state->largeIcon != nullptr) {
            auto* drawItem = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            const int controlWidth = std::max(0, static_cast<int>(drawItem->rcItem.right - drawItem->rcItem.left));
            const int controlHeight = std::max(0, static_cast<int>(drawItem->rcItem.bottom - drawItem->rcItem.top));
            const int iconSize = std::min(kAboutIconSizePixels, std::min(controlWidth, controlHeight));
            const int x = drawItem->rcItem.left + ((controlWidth - iconSize) / 2);
            const int y = drawItem->rcItem.top + ((controlHeight - iconSize) / 2);

            HBRUSH backgroundBrush = state->darkMode && state->backgroundBrush != nullptr
                ? state->backgroundBrush
                : reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
            FillRect(drawItem->hDC, &drawItem->rcItem, backgroundBrush);
            DrawIconEx(drawItem->hDC, x, y, state->largeIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            return TRUE;
        }

        if (wParam == IDOK && state != nullptr) {
            auto* drawItem = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (drawItem != nullptr && drawItem->hDC != nullptr) {
                std::array<wchar_t, 64> buttonText{};
                GetWindowTextW(drawItem->hwndItem, buttonText.data(), static_cast<int>(buttonText.size()));
                HFONT buttonFont = reinterpret_cast<HFONT>(SendMessageW(drawItem->hwndItem, WM_GETFONT, 0, 0));
                DrawThemedPushButton(
                    drawItem->hDC,
                    drawItem->rcItem,
                    buttonText.data(),
                    buttonFont,
                    state->darkMode,
                    true,
                    drawItem->itemState,
                    dialog);
                return TRUE;
            }
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
        case IDCANCEL:
            EndDialog(dialog, LOWORD(wParam));
            return TRUE;

        default:
            break;
        }
        break;

    case WM_DESTROY:
        if (state != nullptr && state->ownsLargeIcon && state->largeIcon != nullptr) {
            DestroyIcon(state->largeIcon);
            state->largeIcon = nullptr;
            state->ownsLargeIcon = false;
        }
        if (state != nullptr && state->backgroundBrush != nullptr) {
            DeleteObject(state->backgroundBrush);
            state->backgroundBrush = nullptr;
        }
        return TRUE;

    default:
        break;
    }

    return FALSE;
}

LRESULT CALLBACK ClassicNotepadApp::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    ClassicNotepadApp* app = reinterpret_cast<ClassicNotepadApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<ClassicNotepadApp*>(createStruct->lpCreateParams);
        app->mainWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    if (app != nullptr) {
        return app->HandleMessage(message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK ClassicNotepadApp::MenuBarWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    ClassicNotepadApp* app = reinterpret_cast<ClassicNotepadApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<ClassicNotepadApp*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    if (app == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC deviceContext = BeginPaint(window, &paint);
        app->PaintMenuBar(deviceContext);
        EndPaint(window, &paint);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT point{
            static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
            static_cast<LONG>(static_cast<short>(HIWORD(lParam)))
        };
        app->SetHotMenuIndex(app->MenuIndexFromPoint(point));
        if (!app->menuTrackingMouse_) {
            TRACKMOUSEEVENT trackMouse{};
            trackMouse.cbSize = sizeof(trackMouse);
            trackMouse.dwFlags = TME_LEAVE;
            trackMouse.hwndTrack = window;
            app->menuTrackingMouse_ = TrackMouseEvent(&trackMouse) != 0;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        app->menuTrackingMouse_ = false;
        if (app->activeMenuIndex_ < 0 && !app->menuKeyboardActive_) {
            app->SetHotMenuIndex(-1);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        SetFocus(window);
        POINT point{
            static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
            static_cast<LONG>(static_cast<short>(HIWORD(lParam)))
        };
        const int index = app->MenuIndexFromPoint(point);
        if (index >= 0) {
            app->ShowMenuPopup(index, false);
        }
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const int itemCount = app->GetTopLevelMenuCount();
        if (itemCount <= 0) {
            return 0;
        }

        int hotIndex = app->hotMenuIndex_ >= 0 ? app->hotMenuIndex_ : 0;
        switch (wParam) {
        case VK_LEFT:
            hotIndex = (hotIndex + itemCount - 1) % itemCount;
            app->menuKeyboardActive_ = true;
            app->SetHotMenuIndex(hotIndex);
            return 0;

        case VK_RIGHT:
            hotIndex = (hotIndex + 1) % itemCount;
            app->menuKeyboardActive_ = true;
            app->SetHotMenuIndex(hotIndex);
            return 0;

        case VK_HOME:
            app->menuKeyboardActive_ = true;
            app->SetHotMenuIndex(0);
            return 0;

        case VK_END:
            app->menuKeyboardActive_ = true;
            app->SetHotMenuIndex(itemCount - 1);
            return 0;

        case VK_DOWN:
        case VK_RETURN:
        case VK_SPACE:
            app->ShowMenuPopup(hotIndex, true);
            return 0;

        case VK_ESCAPE:
            app->ClearMenuMode();
            app->FocusActiveEditor();
            return 0;

        default:
            break;
        }
        break;
    }

    case WM_SYSCHAR:
        if (app->ActivateMenuMnemonic(static_cast<wchar_t>(wParam))) {
            return 0;
        }
        break;

    case WM_SYSKEYUP:
        if (wParam == VK_MENU && app->menuKeyboardActive_) {
            return 0;
        }
        break;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_KILLFOCUS:
        if (app->activeMenuIndex_ < 0) {
            app->ClearMenuMode();
        }
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK ClassicNotepadApp::ScrollBarWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    ClassicNotepadApp* app = reinterpret_cast<ClassicNotepadApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<ClassicNotepadApp*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    if (app == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return TRUE;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC deviceContext = BeginPaint(window, &paint);

        RECT clientRect{};
        GetClientRect(window, &clientRect);
        const int width = std::max(0, static_cast<int>(clientRect.right - clientRect.left));
        const int height = std::max(0, static_cast<int>(clientRect.bottom - clientRect.top));

        HDC bufferedContext = nullptr;
        HBITMAP bufferBitmap = nullptr;
        HGDIOBJ previousBitmap = nullptr;
        if (width > 0 && height > 0) {
            bufferedContext = CreateCompatibleDC(deviceContext);
            if (bufferedContext != nullptr) {
                bufferBitmap = CreateCompatibleBitmap(deviceContext, width, height);
                if (bufferBitmap != nullptr) {
                    previousBitmap = SelectObject(bufferedContext, bufferBitmap);
                    app->PaintCustomScrollBar(window, bufferedContext);
                    BitBlt(deviceContext, 0, 0, width, height, bufferedContext, 0, 0, SRCCOPY);
                }
            }
        }

        if (bufferBitmap == nullptr) {
            app->PaintCustomScrollBar(window, deviceContext);
        }

        if (previousBitmap != nullptr) {
            SelectObject(bufferedContext, previousBitmap);
        }
        if (bufferBitmap != nullptr) {
            DeleteObject(bufferBitmap);
        }
        if (bufferedContext != nullptr) {
            DeleteDC(bufferedContext);
        }

        EndPaint(window, &paint);
        return 0;
    }

    case WM_LBUTTONDOWN:
        app->BeginCustomScrollBarInteraction(window, lParam);
        return 0;

    case WM_MOUSEMOVE:
        app->UpdateCustomScrollBarDrag(window, lParam);
        return 0;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        app->EndCustomScrollBarDrag(window);
        return 0;

    case WM_MOUSEWHEEL:
        if (app->HandleMouseWheel(wParam)) {
            return 0;
        }
        if (app->editor_ != nullptr) {
            SendMessageW(app->editor_, WM_MOUSEWHEEL, wParam, lParam);
            app->UpdateStatusBar();
            app->UpdateScrollBars();
            InvalidateRect(app->editor_, nullptr, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK ClassicNotepadApp::EditorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    ClassicNotepadApp* app = reinterpret_cast<ClassicNotepadApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (app != nullptr && message == WM_CONTEXTMENU) {
        if (app->HandleEditorContextMenu(window, lParam)) {
            return 0;
        }
    }

    if (app != nullptr && message == WM_PASTE) {
        app->BeginEditorMutationTracking(message, wParam);
        app->HandlePaste();
        app->EndEditorMutationTracking();
        return 0;
    }

    if (app != nullptr && message == WM_VSCROLL && app->UseCustomScrollBars()) {
        const ScrollBarMetrics metrics = app->GetCustomScrollBarMetrics(ScrollBarOrientation::Vertical);
        const int maximum = std::max(metrics.minimum, metrics.maximum - metrics.page + 1);
        int target = metrics.position;
        switch (LOWORD(wParam)) {
        case SB_LINEUP:
            --target;
            break;
        case SB_LINEDOWN:
            ++target;
            break;
        case SB_PAGEUP:
            target -= metrics.page;
            break;
        case SB_PAGEDOWN:
            target += metrics.page;
            break;
        case SB_TOP:
            target = metrics.minimum;
            break;
        case SB_BOTTOM:
            target = maximum;
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            target = static_cast<int>(HIWORD(wParam));
            break;
        default:
            break;
        }
        app->ScrollCustomScrollBarToPosition(
            ScrollBarOrientation::Vertical,
            std::clamp(target, metrics.minimum, maximum));
        return 0;
    }

    if (app != nullptr && message == WM_HSCROLL && app->UseCustomScrollBars()) {
        const ScrollBarMetrics metrics = app->GetCustomScrollBarMetrics(ScrollBarOrientation::Horizontal);
        const int maximum = std::max(metrics.minimum, metrics.maximum - metrics.page + 1);
        int target = metrics.position;
        switch (LOWORD(wParam)) {
        case SB_LINELEFT:
            --target;
            break;
        case SB_LINERIGHT:
            ++target;
            break;
        case SB_PAGELEFT:
            target -= metrics.page;
            break;
        case SB_PAGERIGHT:
            target += metrics.page;
            break;
        case SB_LEFT:
            target = metrics.minimum;
            break;
        case SB_RIGHT:
            target = maximum;
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            target = static_cast<int>(HIWORD(wParam));
            break;
        default:
            break;
        }
        app->ScrollCustomScrollBarToPosition(
            ScrollBarOrientation::Horizontal,
            std::clamp(target, metrics.minimum, maximum));
        return 0;
    }

    if (app != nullptr) {
        if (message == WM_SYSKEYDOWN && wParam == VK_MENU && app->ActivateMenuBarFromKeyboard()) {
            return 0;
        }

        if (message == WM_SYSCHAR && app->ActivateMenuMnemonic(static_cast<wchar_t>(wParam))) {
            return 0;
        }

        if (message == WM_SYSKEYUP && wParam == VK_MENU && app->menuKeyboardActive_) {
            return 0;
        }

        if (message == WM_KEYDOWN) app->overtypeAwaitingLowSurrogate_ = false;

        if (message == WM_KEYDOWN && wParam == VK_INSERT && IsPlainInsertKeyPress()) {
            app->overwriteMode_ = !app->overwriteMode_;
            app->UpdateStatusBar();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (message == WM_CHAR) {
            const wchar_t typedUnit = static_cast<wchar_t>(wParam);
            const bool pairedLowSurrogate = app->overtypeAwaitingLowSurrogate_ &&
                classic_notepad::IsLowSurrogateUnit(typedUnit);
            if (!pairedLowSurrogate) app->overtypeAwaitingLowSurrogate_ = false;

            if (app->overwriteMode_ && IsOvertypeCharacter(wParam)) {
                if (!pairedLowSurrogate) {
                    DWORD selectionStart = 0;
                    DWORD selectionEnd = 0;
                    app->GetSelectionRange(selectionStart, selectionEnd);
                    if (selectionStart == selectionEnd) {
                        classic_notepad::TextUnitRange range{};
                        if (TryGetNativeOvertypeRange(window, selectionStart, range) &&
                            range.start != range.end) {
                            SendMessageW(window, EM_SETSEL, range.start, range.end);
                        }
                    }
                }
                app->overtypeAwaitingLowSurrogate_ =
                    classic_notepad::IsHighSurrogateUnit(typedUnit);
            }

        }

        const bool collapsedDelete =
            (message == WM_CHAR && wParam == 8U) ||
            (message == WM_KEYDOWN && wParam == VK_DELETE);
        if (collapsedDelete) {
            DWORD selectionStart = 0;
            DWORD selectionEnd = 0;
            app->GetSelectionRange(selectionStart, selectionEnd);
            if (selectionStart == selectionEnd) {
                classic_notepad::TextUnitRange range{};
                const bool backspace = message == WM_CHAR;
                if (TryGetNativeCollapsedDeleteRange(window, selectionStart, backspace, range) &&
                    (range.end - range.start > 1U || range.start != selectionStart ||
                        range.end != selectionEnd)) {
                    SendMessageW(window, EM_SETSEL, range.start, range.end);
                }
            }
        }

        if (message == WM_MOUSEWHEEL && app->HandleMouseWheel(wParam)) {
            return 0;
        }
    }

    WNDPROC originalProc = app != nullptr ? app->originalEditorProc_ : nullptr;
    const bool hadActiveMutation = app != nullptr && app->editorWidthIndexMutationActive_;
    if (app != nullptr) {
        app->BeginEditorMutationTracking(message, wParam);
    }
    const bool trackedMutation = app != nullptr && !hadActiveMutation && app->editorWidthIndexMutationActive_;
    if (app != nullptr && window == app->editor_ && message == WM_LBUTTONDOWN &&
        (wParam & MK_SHIFT) != 0) {
        // EDIT extends Shift-click from the existing selection anchor. Capture
        // it before native mouse processing changes the sorted selection range.
        DWORD start = 0;
        DWORD end = 0;
        app->GetSelectionRange(start, end);
        const DWORD active = app->GetActiveCaretPosition();
        app->nativeSelectionEditor_ = window;
        app->nativeSelectionAnchor_ = active == start ? end : start;
        app->nativeActiveCaretPosition_ = active;
        app->nativeSelectionTrackingValid_ = true;
    }
    const bool hadValidSelectionTracking = app != nullptr && app->nativeSelectionTrackingValid_;
    if (app != nullptr && window == app->editor_ &&
        (message == WM_SETTEXT || message == EM_REPLACESEL || message == EM_UNDO ||
            message == WM_UNDO || message == WM_CUT || message == WM_CLEAR || message == WM_PASTE)) {
        app->nativeSelectionTrackingValid_ = false;
    }
    const LRESULT result = originalProc != nullptr
        ? CallWindowProcW(originalProc, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);

    if (app != nullptr) {
        if (window == app->editor_ && (message == EM_UNDO || message == WM_UNDO) && result == 0) {
            // A failed/no-op Undo keeps the existing selection orientation.
            app->nativeSelectionTrackingValid_ = hadValidSelectionTracking;
        } else {
            app->TrackNativeSelectionAfterMessage(window, message, wParam, lParam);
        }
    }

    if (trackedMutation) {
        app->EndEditorMutationTracking();
    }

    if (app == nullptr) {
        return result;
    }

    switch (message) {
    case WM_PAINT:
        app->DrawSpellingUnderlines(window);
        break;

    case WM_CHAR:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_HSCROLL:
    case WM_VSCROLL:
    case WM_SETFOCUS:
    case WM_SIZE:
        app->UpdateStatusBar();
        if (message == WM_CHAR ||
            message == WM_KEYDOWN ||
            message == WM_MOUSEWHEEL ||
            message == WM_HSCROLL ||
            message == WM_VSCROLL ||
            message == WM_SIZE) {
            app->UpdateScrollBars();
        }
        if (message == WM_KEYDOWN ||
            message == WM_MOUSEWHEEL ||
            message == WM_HSCROLL ||
            message == WM_VSCROLL ||
            message == WM_SIZE) {
            app->AdvanceSpellingLayoutRevision();
            app->RefreshSpellCheck(false);
        }
        InvalidateRect(
            window,
            nullptr,
            (message == WM_MOUSEWHEEL || message == WM_HSCROLL || message == WM_VSCROLL) ? TRUE : FALSE);
        break;

    case WM_MOUSEMOVE:
        if ((wParam & MK_LBUTTON) != 0) {
            app->UpdateStatusBar();
        }
        break;

    default:
        break;
    }

    return result;
}

LRESULT CALLBACK ClassicNotepadApp::StatusBarSubclassProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR referenceData)
{
    auto* app = reinterpret_cast<ClassicNotepadApp*>(referenceData);

    switch (message) {
    case WM_ERASEBKGND:
        if (app != nullptr && app->darkModeEnabled_ && app->darkStatusBackgroundBrush_ != nullptr) {
            RECT clientRect{};
            GetClientRect(window, &clientRect);
            FillRect(reinterpret_cast<HDC>(wParam), &clientRect, app->darkStatusBackgroundBrush_);
            return TRUE;
        }
        break;

    case WM_PAINT: {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (app != nullptr && app->darkModeEnabled_) {
            HDC deviceContext = GetDC(window);
            if (deviceContext != nullptr) {
                app->DrawDarkStatusBarChrome(deviceContext);
                ReleaseDC(window, deviceContext);
            }
        }
        return result;
    }

    case WM_LBUTTONDOWN:
        if (app != nullptr && app->StartResizeFromStatusGrip(window, lParam)) {
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (app != nullptr) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            if (app->IsPointInStatusBarResizeGrip(point)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
                return TRUE;
            }
        }
        break;

    case WM_NCDESTROY:
        RemoveWindowSubclass(window, ClassicNotepadApp::StatusBarSubclassProc, subclassId);
        break;

    default:
        break;
    }

    return DefSubclassProc(window, message, wParam, lParam);
}
