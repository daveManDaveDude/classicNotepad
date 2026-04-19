#include "app.h"

#include "app_version.h"
#include "resource.h"
#include "spell_text_utils.h"

#include <cderr.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kMainWindowClass[] = L"ClassicNotepadMainWindow";
constexpr wchar_t kMenuBarClass[] = L"ClassicNotepadMenuBar";
constexpr wchar_t kScrollBarClass[] = L"ClassicNotepadScrollBar";
constexpr wchar_t kAppTitle[] = L"Untitled - Classic Notepad";
constexpr wchar_t kFileDialogFilter[] = L"Text Documents (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
constexpr wchar_t kFindReplaceMessageName[] = L"commdlg_FindReplace";
constexpr DWORD kFindOptionFlags = FR_DOWN | FR_MATCHCASE | FR_WHOLEWORD;
constexpr int kTabWidthSpaces = 8;
constexpr UINT_PTR kSpellCheckTimerId = 1;
constexpr UINT kSpellCheckDelayMs = 300;
constexpr UINT kSpellMenuSuggestionBase = 50000;
constexpr UINT kSpellMenuSuggestionMax = 50004;
constexpr UINT kSpellMenuNoSuggestions = 50005;
constexpr UINT kSpellMenuIgnoreOnce = 50006;
constexpr UINT kSpellMenuAddToDictionary = 50007;
constexpr int kSpellCheckVisibleLineMargin = 3;
constexpr std::size_t kMaxVisibleSpellCheckChars = 96U * 1024U;
constexpr int kFullDocumentWidthScanCharLimit = 256 * 1024;
constexpr int kExactLineWidthMeasureCharLimit = 4096;
constexpr int kMaxEditorLineFetchChars = 0xFFFE;
constexpr int kExactStatusCharacterCountLimit = 256 * 1024;
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
constexpr int kStatusBarHorizontalPadding = 12;
constexpr int kStatusBarSeparatorInset = 7;
constexpr int kStatusBarMinHeight = 32;

struct GoToDialogState {
    int currentLine = 1;
    int maxLine = 1;
    int selectedLine = 1;
};

struct AboutDialogState {
    HINSTANCE instance = nullptr;
    HICON largeIcon = nullptr;
    bool ownsLargeIcon = false;
};

struct FontFamilySearch {
    const wchar_t* faceName = nullptr;
    bool found = false;
};

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

bool IsWordCharacter(wchar_t character)
{
    return std::iswalnum(static_cast<wint_t>(character)) != 0 || character == L'_';
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

bool HasWordBoundaryAt(const std::wstring& text, std::size_t position)
{
    if (position == 0U || position >= text.size()) {
        return true;
    }

    return !IsWordCharacter(text[position - 1U]) || !IsWordCharacter(text[position]);
}

bool TextMatchesAt(
    const std::wstring& text,
    std::size_t position,
    const std::wstring& needle,
    bool matchCase,
    bool wholeWord)
{
    if (needle.empty() || position + needle.size() > text.size()) {
        return false;
    }

    const int length = static_cast<int>(needle.size());
    const int comparison = CompareStringOrdinal(
        text.c_str() + position,
        length,
        needle.c_str(),
        length,
        matchCase ? FALSE : TRUE);

    if (comparison != CSTR_EQUAL) {
        return false;
    }

    if (!wholeWord) {
        return true;
    }

    return HasWordBoundaryAt(text, position) && HasWordBoundaryAt(text, position + needle.size());
}

void CopyToFixedBuffer(std::array<wchar_t, 512>& buffer, const std::wstring& text)
{
    buffer.fill(L'\0');
    wcsncpy_s(buffer.data(), buffer.size(), text.c_str(), _TRUNCATE);
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

bool ShouldUseDarkMode()
{
    if (IsHighContrastEnabled()) {
        return false;
    }

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

std::wstring FormatEncoding(Document::TextEncoding encoding)
{
    switch (encoding) {
    case Document::TextEncoding::Utf8NoBom:
        return L"UTF-8";
    case Document::TextEncoding::Utf8Bom:
        return L"UTF-8 with BOM";
    case Document::TextEncoding::Utf16LeBom:
        return L"UTF-16 LE";
    case Document::TextEncoding::Ansi:
        return L"ANSI";
    default:
        return L"UTF-8";
    }
}

std::wstring FormatLineEnding(Document::LineEndingStyle lineEnding)
{
    switch (lineEnding) {
    case Document::LineEndingStyle::Crlf:
        return L"Windows (CRLF)";
    case Document::LineEndingStyle::Lf:
        return L"Unix (LF)";
    case Document::LineEndingStyle::Cr:
        return L"Macintosh (CR)";
    case Document::LineEndingStyle::Mixed:
        return L"Mixed";
    default:
        return L"Windows (CRLF)";
    }
}

std::wstring FormatNumberWithSeparators(std::size_t value)
{
    const std::wstring digits = std::to_wstring(value);
    std::wstring formatted;
    formatted.reserve(digits.size() + ((digits.size() - 1U) / 3U));

    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index > 0U && ((digits.size() - index) % 3U) == 0U) {
            formatted += L',';
        }

        formatted += digits[index];
    }

    return formatted;
}

std::wstring FormatCharacterCount(std::size_t count)
{
    std::wstring text = FormatNumberWithSeparators(count);
    text += count == 1U ? L" character" : L" characters";
    return text;
}

std::size_t CountStatusCharacters(const std::wstring& text)
{
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r' && index + 1U < text.size() && text[index + 1U] == L'\n') {
            ++index;
        } else if (
            text[index] >= 0xD800 &&
            text[index] <= 0xDBFF &&
            index + 1U < text.size() &&
            text[index + 1U] >= 0xDC00 &&
            text[index + 1U] <= 0xDFFF) {
            ++index;
        }

        ++count;
    }

    return count;
}

std::wstring StripMenuMnemonics(const std::wstring& text)
{
    std::wstring stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'&') {
            if (index + 1U < text.size() && text[index + 1U] == L'&') {
                stripped.push_back(L'&');
                ++index;
            }
            continue;
        }

        stripped.push_back(text[index]);
    }

    return stripped;
}

wchar_t FindMenuMnemonic(const std::wstring& text)
{
    for (std::size_t index = 0; index + 1U < text.size(); ++index) {
        if (text[index] == L'&' && text[index + 1U] != L'&') {
            return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(text[index + 1U])));
        }

        if (text[index] == L'&') {
            ++index;
        }
    }

    return L'\0';
}

void SplitMenuItemText(const std::wstring& text, std::wstring& label, std::wstring& accelerator)
{
    const std::size_t separator = text.find(L'\t');
    if (separator == std::wstring::npos) {
        label = StripMenuMnemonics(text);
        accelerator.clear();
        return;
    }

    label = StripMenuMnemonics(text.substr(0, separator));
    accelerator = StripMenuMnemonics(text.substr(separator + 1U));
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
    darkModeEnabled_ = ShouldUseDarkMode();
    RecreateThemeBrushes();
}

ClassicNotepadApp::~ClassicNotepadApp()
{
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

HWND ClassicNotepadApp::CreateEditor()
{
    const bool useCustomScrollBars = UseCustomScrollBars();
    const DWORD editorStyle =
        WS_CHILD | WS_VISIBLE |
        (useCustomScrollBars ? 0 : WS_VSCROLL) |
        (wordWrap_ || useCustomScrollBars ? 0 : WS_HSCROLL) |
        ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
        (wordWrap_ ? 0 : ES_AUTOHSCROLL) |
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

    verticalScrollBarVisible_ = true;
    horizontalScrollBarVisible_ = !wordWrap_;
    nativeVerticalScrollBarVisible_ = !useCustomScrollBars;
    nativeHorizontalScrollBarVisible_ = !useCustomScrollBars && !wordWrap_;
    return editor;
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
        const std::wstring text = StripMenuMnemonics(GetTopLevelMenuText(itemIndex));
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
        const std::wstring text = StripMenuMnemonics(GetTopLevelMenuText(index));
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
    if (editor_ != nullptr) {
        SetFocus(editor_);
    }
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
        if (FindMenuMnemonic(GetTopLevelMenuText(index)) == target) {
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
            itemInfo.wID == ID_VIEW_STATUS_BAR) {
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
    const bool newDarkMode = ShouldUseDarkMode();
    if (darkModeEnabled_ != newDarkMode) {
        darkModeEnabled_ = newDarkMode;
        RecreateThemeBrushes();
    }

    ApplyThemeToWindows();
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
        SplitMenuItemText(menuItem->text, label, accelerator);

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
    SplitMenuItemText(menuItem->text, label, accelerator);
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
    if (!darkModeEnabled_ || header == nullptr || header->hwndFrom != statusBar_ || header->code != NM_CUSTOMDRAW) {
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
    if (editor_ == nullptr) {
        return;
    }

    UpdateScrollBars();
    RefreshSpellCheck(false);
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
}

void ClassicNotepadApp::UpdateScrollBars()
{
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
    cachedWidestLineWidth_ = 0;
    widestLineCacheDirty_ = true;
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

int ClassicNotepadApp::GetLastVisibleEditorLine(int lineMargin) const
{
    if (editor_ == nullptr) {
        return 0;
    }

    const int lineCount = std::max<int>(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    const int firstVisibleLine = std::clamp(
        static_cast<int>(SendMessageW(editor_, EM_GETFIRSTVISIBLELINE, 0, 0)),
        0,
        std::max(0, lineCount - 1));
    const int startLine = std::max(0, firstVisibleLine - std::max(0, lineMargin));
    return std::min(lineCount - 1, startLine + GetVisibleEditorLineCount(lineMargin) - 1);
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

bool ClassicNotepadApp::GetVisibleEditorText(std::wstring& text, DWORD& rangeStart) const
{
    text.clear();
    rangeStart = 0;

    if (editor_ == nullptr || GetWindowTextLengthW(editor_) <= 0) {
        return false;
    }

    const int lineCount = std::max<int>(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    const int firstVisibleLine = std::clamp(
        static_cast<int>(SendMessageW(editor_, EM_GETFIRSTVISIBLELINE, 0, 0)),
        0,
        std::max(0, lineCount - 1));
    const int startLine = std::max(0, firstVisibleLine - kSpellCheckVisibleLineMargin);
    const int endLine = GetLastVisibleEditorLine(kSpellCheckVisibleLineMargin);

    const LRESULT startResult = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(startLine), 0);
    if (startResult < 0) {
        return false;
    }

    rangeStart = static_cast<DWORD>(startResult);
    DWORD expectedPosition = rangeStart;
    text.reserve(std::min<std::size_t>(kMaxVisibleSpellCheckChars, 4096U));

    for (int line = startLine; line <= endLine && text.size() < kMaxVisibleSpellCheckChars; ++line) {
        const LRESULT lineStartResult = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(line), 0);
        if (lineStartResult < 0) {
            continue;
        }

        const DWORD lineStart = static_cast<DWORD>(lineStartResult);
        if (lineStart > expectedPosition) {
            const DWORD gapLength = lineStart - expectedPosition;
            const std::size_t remaining = kMaxVisibleSpellCheckChars - text.size();
            const std::size_t copiedGap = std::min<std::size_t>(gapLength, remaining);
            text.append(copiedGap, L'\n');
            expectedPosition += static_cast<DWORD>(copiedGap);
            if (copiedGap < gapLength) {
                break;
            }
        } else if (lineStart < expectedPosition) {
            continue;
        }

        const int lineLength = static_cast<int>(SendMessageW(editor_, EM_LINELENGTH, static_cast<WPARAM>(lineStart), 0));
        const std::size_t remaining = kMaxVisibleSpellCheckChars - text.size();
        const int copyLength = static_cast<int>(std::min<std::size_t>(
            std::max(0, lineLength),
            std::min<std::size_t>(remaining, static_cast<std::size_t>(kMaxEditorLineFetchChars))));
        if (copyLength <= 0) {
            continue;
        }

        const std::wstring lineText = GetEditorLineText(line, copyLength);
        text += lineText;
        expectedPosition += static_cast<DWORD>(lineText.size());
        if (lineText.size() < static_cast<std::size_t>(lineLength)) {
            break;
        }
    }

    return !text.empty();
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

    const int textLength = GetWindowTextLengthW(editor_);
    if (textLength <= 0) {
        cachedWidestLineWidth_ = 0;
        widestLineCacheDirty_ = false;
        return 0;
    }

    if (textLength > kFullDocumentWidthScanCharLimit) {
        return MeasureVisibleEditorLineWidth();
    }

    if (!widestLineCacheDirty_) {
        return cachedWidestLineWidth_;
    }

    cachedWidestLineWidth_ = MeasureFullEditorLineWidth();
    widestLineCacheDirty_ = false;
    return cachedWidestLineWidth_;
}

int ClassicNotepadApp::MeasureFullEditorLineWidth() const
{
    if (editor_ == nullptr) {
        return 0;
    }

    HDC deviceContext = GetDC(editor_);
    if (deviceContext == nullptr) {
        return 0;
    }

    HGDIOBJ previousFont = nullptr;
    if (editorFont_ != nullptr) {
        previousFont = SelectObject(deviceContext, editorFont_);
    }

    int widestLine = 0;
    const std::wstring text = GetEditorText();
    std::wstring expandedLine;
    std::size_t lineStart = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        const bool atEnd = index == text.size();
        const bool atLineBreak = !atEnd && (text[index] == L'\r' || text[index] == L'\n');
        if (!atEnd && !atLineBreak) {
            continue;
        }

        ExpandTabsForMeasurementRange(text, lineStart, index - lineStart, expandedLine);
        SIZE textSize{};
        if (!expandedLine.empty() &&
            GetTextExtentPoint32W(
                deviceContext,
                expandedLine.c_str(),
                static_cast<int>(std::min<std::size_t>(
                    expandedLine.size(),
                    static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                &textSize)) {
            widestLine = std::max(widestLine, static_cast<int>(textSize.cx));
        }

        if (!atEnd && text[index] == L'\r' && index + 1U < text.size() && text[index + 1U] == L'\n') {
            ++index;
        }
        lineStart = index + 1U;
    }

    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }
    ReleaseDC(editor_, deviceContext);

    return widestLine;
}

int ClassicNotepadApp::MeasureVisibleEditorLineWidth() const
{
    if (editor_ == nullptr) {
        return 0;
    }

    HDC deviceContext = GetDC(editor_);
    if (deviceContext == nullptr) {
        return 0;
    }

    HGDIOBJ previousFont = nullptr;
    if (editorFont_ != nullptr) {
        previousFont = SelectObject(deviceContext, editorFont_);
    }

    TEXTMETRICW textMetrics{};
    const bool measuredMetrics = GetTextMetricsW(deviceContext, &textMetrics) != 0;
    const int averageCharacterWidth = measuredMetrics
        ? std::max(1, static_cast<int>(textMetrics.tmAveCharWidth))
        : 1;

    int widestLine = 0;
    const int lineCount = std::max<int>(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    const int firstVisibleLine = std::clamp(
        static_cast<int>(SendMessageW(editor_, EM_GETFIRSTVISIBLELINE, 0, 0)),
        0,
        std::max(0, lineCount - 1));
    const int startLine = std::max(0, firstVisibleLine - kSpellCheckVisibleLineMargin);
    const int endLine = GetLastVisibleEditorLine(kSpellCheckVisibleLineMargin);

    for (int line = startLine; line <= endLine; ++line) {
        const LRESULT lineStartResult = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(line), 0);
        if (lineStartResult < 0) {
            continue;
        }

        const int lineLength = static_cast<int>(SendMessageW(
            editor_,
            EM_LINELENGTH,
            static_cast<WPARAM>(lineStartResult),
            0));
        if (lineLength <= 0) {
            continue;
        }

        if (lineLength > kExactLineWidthMeasureCharLimit) {
            widestLine = std::max(widestLine, SaturatingTextWidthEstimate(lineLength, averageCharacterWidth));
            continue;
        }

        const std::wstring lineText = ExpandTabsForPrinting(GetEditorLineText(line, lineLength));
        SIZE textSize{};
        if (!lineText.empty() &&
            GetTextExtentPoint32W(
                deviceContext,
                lineText.c_str(),
                static_cast<int>(std::min<std::size_t>(
                    lineText.size(),
                    static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                &textSize)) {
            widestLine = std::max(widestLine, static_cast<int>(textSize.cx));
        }
    }

    if (previousFont != nullptr) {
        SelectObject(deviceContext, previousFont);
    }
    ReleaseDC(editor_, deviceContext);

    return widestLine;
}

bool ClassicNotepadApp::NeedsVerticalScrollBar(int availableHeight) const
{
    if (editor_ == nullptr || availableHeight <= 0) {
        return false;
    }

    const int lineCount = std::max<int>(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    return lineCount * GetEditorLineHeight() > availableHeight;
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

    if (horizontalChanged && !horizontalVisible) {
        horizontalScrollPosition_ = 0;
        SendMessageW(editor_, WM_HSCROLL, SB_LEFT, 0);
    } else if (useCustomScrollBars && horizontalVisible) {
        const ScrollBarMetrics horizontalMetrics = GetCustomScrollBarMetrics(ScrollBarOrientation::Horizontal);
        horizontalScrollPosition_ = horizontalMetrics.position;
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
    const int rangeUnits = std::max(1, (widestLine + unit - 1) / unit);
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
    }

    UpdateStatusBar();
    UpdateScrollBars();
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
    title += L" - Classic Notepad";
    SetWindowTextW(mainWindow_, title.c_str());
}

void ClassicNotepadApp::UpdateMenuState(HMENU menu)
{
    if (menu == nullptr || editor_ == nullptr) {
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

    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);

    const DWORD caretPosition = selectionEnd;
    LRESULT zeroBasedLine = SendMessageW(editor_, EM_LINEFROMCHAR, static_cast<WPARAM>(caretPosition), 0);
    if (zeroBasedLine < 0) {
        zeroBasedLine = 0;
    }

    LRESULT lineStart = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(zeroBasedLine), 0);
    if (lineStart < 0) {
        lineStart = 0;
    }

    const DWORD lineStartPosition = lineStart > 0 ? static_cast<DWORD>(lineStart) : 0;
    const int line = static_cast<int>(zeroBasedLine) + 1;
    const int column = caretPosition >= lineStartPosition
        ? static_cast<int>(caretPosition - lineStartPosition) + 1
        : 1;
    statusBarParts_[0] = L"Ln ";
    statusBarParts_[0] += FormatNumberWithSeparators(static_cast<std::size_t>(std::max(1, line)));
    statusBarParts_[0] += L", Col ";
    statusBarParts_[0] += FormatNumberWithSeparators(static_cast<std::size_t>(std::max(1, column)));
    statusBarParts_[1] = FormatCharacterCount(GetStatusCharacterCount());
    statusBarParts_[2] = FormatLineEnding(document_.LineEnding());
    statusBarParts_[3] = FormatEncoding(document_.Encoding());

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
    int right = 0;
    const int horizontalPadding = ScalePixelsForWindow(statusBar_, kStatusBarHorizontalPadding);
    for (int index = 0; index < kStatusBarPartCount; ++index) {
        if (index + 1 == kStatusBarPartCount) {
            partRights[static_cast<std::size_t>(index)] = -1;
            break;
        }

        const SIZE textSize = MeasureText(deviceContext, statusBarParts_[static_cast<std::size_t>(index)]);
        right += textSize.cx + (horizontalPadding * 2);
        partRights[static_cast<std::size_t>(index)] = right;
    }

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
    statusCharacterCount_ = CountStatusCharacters(text);
    statusCharacterTextLength_ = static_cast<int>(std::min<std::size_t>(
        text.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    statusCharacterCountDirty_ = false;
}

std::size_t ClassicNotepadApp::GetStatusCharacterCount()
{
    if (editor_ == nullptr) {
        statusCharacterCount_ = 0;
        statusCharacterTextLength_ = 0;
        statusCharacterCountDirty_ = false;
        return statusCharacterCount_;
    }

    const int textLength = GetWindowTextLengthW(editor_);
    if (!statusCharacterCountDirty_ && textLength == statusCharacterTextLength_) {
        return statusCharacterCount_;
    }

    statusCharacterTextLength_ = std::max(0, textLength);
    if (textLength > kExactStatusCharacterCountLimit) {
        statusCharacterCount_ = static_cast<std::size_t>(textLength);
    } else {
        statusCharacterCount_ = CountStatusCharacters(GetEditorText());
    }

    statusCharacterCountDirty_ = false;
    return statusCharacterCount_;
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
        spellingErrors_.clear();
        if (editor_ != nullptr) {
            InvalidateRect(editor_, nullptr, FALSE);
        }
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

    if (!spellCheckAvailable_) {
        spellingErrors_.clear();
        if (editor_ != nullptr) {
            InvalidateRect(editor_, nullptr, FALSE);
        }
        return;
    }

    std::wstring visibleText;
    DWORD visibleStart = 0;
    if (!GetVisibleEditorText(visibleText, visibleStart)) {
        spellingErrors_.clear();
        if (editor_ != nullptr) {
            InvalidateRect(editor_, nullptr, FALSE);
        }
        return;
    }

    spellingErrors_ = spellChecker_.Check(visibleText);
    for (SpellingErrorRange& error : spellingErrors_) {
        if (error.start > std::numeric_limits<DWORD>::max() - visibleStart) {
            error.start = std::numeric_limits<DWORD>::max();
        } else {
            error.start += visibleStart;
        }
    }
    if (editor_ != nullptr) {
        InvalidateRect(editor_, nullptr, FALSE);
    }
}

void ClassicNotepadApp::DrawSpellingUnderlines(HWND editorWindow)
{
    if (!spellCheckAvailable_ || spellingErrors_.empty() || editorWindow == nullptr) {
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

    TEXTMETRICW textMetrics{};
    GetTextMetricsW(deviceContext, &textMetrics);
    const int lineHeight = std::max(1, static_cast<int>(textMetrics.tmHeight + textMetrics.tmExternalLeading));
    const int firstVisibleLine = static_cast<int>(SendMessageW(editorWindow, EM_GETFIRSTVISIBLELINE, 0, 0));
    const int clientHeight = static_cast<int>(clientRect.bottom - clientRect.top);
    const int visibleLineCount = std::max(1, (clientHeight + lineHeight - 1) / lineHeight + 1);
    const int lastVisibleLine = firstVisibleLine + visibleLineCount;

    for (const SpellingErrorRange& error : spellingErrors_) {
        if (error.length == 0) {
            continue;
        }

        const DWORD errorEnd = error.start + error.length;
        const int startLine = static_cast<int>(SendMessageW(editorWindow, EM_LINEFROMCHAR, error.start, 0));
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
            const WORD lineLength = static_cast<WORD>(SendMessageW(editorWindow, EM_LINELENGTH, lineStart, 0));
            const DWORD lineEnd = lineStart + lineLength;

            const DWORD segmentStart = std::max(lineStart, error.start);
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
            int x = startPoint.x;
            const int endX = std::max(startPoint.x + 1, endPoint.x);
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
    MessageBoxW(
        mainWindow_,
        L"British English spell checking is not installed for Windows.",
        L"Classic Notepad",
        MB_OK | MB_ICONINFORMATION);
}

bool ClassicNotepadApp::HandleEditorContextMenu(HWND editorWindow, LPARAM lParam)
{
    if (editorWindow == nullptr) {
        return false;
    }

    POINT screenPoint{};
    if (lParam == -1) {
        DWORD start = 0;
        DWORD end = 0;
        GetSelectionRange(start, end);
        const POINT caret = PointFromEditorCharPosition(editorWindow, end);
        screenPoint = caret;
        ClientToScreen(editorWindow, &screenPoint);
    } else {
        screenPoint.x = static_cast<short>(LOWORD(lParam));
        screenPoint.y = static_cast<short>(HIWORD(lParam));
    }

    POINT clientPoint = screenPoint;
    ScreenToClient(editorWindow, &clientPoint);
    DWORD charIndex = 0;
    bool hasCharacterIndex = false;
    if (lParam == -1) {
        DWORD selectionStart = 0;
        DWORD selectionEnd = 0;
        GetSelectionRange(selectionStart, selectionEnd);
        charIndex = selectionEnd;
        hasCharacterIndex = true;
    } else {
        const LRESULT charResult = SendMessageW(
            editorWindow,
            EM_CHARFROMPOS,
            0,
            MAKELPARAM(clientPoint.x, clientPoint.y));
        charIndex = LOWORD(charResult);
        hasCharacterIndex = true;
    }

    std::wstring visibleText;
    DWORD visibleTextStart = 0;
    const bool hasVisibleText = GetVisibleEditorText(visibleText, visibleTextStart);
    classic_notepad::TextRange wordRange{};
    std::size_t absoluteWordStart = 0;
    const bool hasWord = hasCharacterIndex &&
        hasVisibleText &&
        charIndex >= visibleTextStart &&
        static_cast<std::size_t>(charIndex - visibleTextStart) < visibleText.size() &&
        classic_notepad::ExpandWordRangeAt(visibleText, charIndex - visibleTextStart, wordRange);
    if (hasWord) {
        absoluteWordStart = static_cast<std::size_t>(visibleTextStart) + wordRange.start;
    }

    bool wordMisspelled = false;
    if (hasWord) {
        for (const SpellingErrorRange& error : spellingErrors_) {
            if (classic_notepad::RangesOverlap(
                    absoluteWordStart,
                    wordRange.length,
                    error.start,
                    error.length)) {
                wordMisspelled = true;
                break;
            }
        }
    }

    HMENU popup = CreatePopupMenu();
    if (popup == nullptr) {
        return true;
    }
    std::vector<std::unique_ptr<OwnerDrawMenuItem>> contextMenuItems;
    ApplyMenuBackground(popup);

    contextMenuWord_.clear();
    contextMenuWordStart_ = 0;
    contextMenuWordLength_ = 0;

    if (spellCheckAvailable_ && wordMisspelled) {
        contextMenuWord_ = visibleText.substr(wordRange.start, wordRange.length);
        contextMenuWordStart_ = static_cast<DWORD>(absoluteWordStart);
        contextMenuWordLength_ = static_cast<DWORD>(wordRange.length);

        const std::vector<std::wstring> suggestions = spellChecker_.Suggest(contextMenuWord_, 5);
        if (suggestions.empty()) {
            AppendOwnerDrawMenuItem(
                popup,
                MF_GRAYED,
                kSpellMenuNoSuggestions,
                L"No spelling suggestions",
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

    if (selected >= kSpellMenuSuggestionBase && selected <= kSpellMenuSuggestionMax) {
        MENUITEMINFOW itemInfo{};
        itemInfo.cbSize = sizeof(itemInfo);
        itemInfo.fMask = MIIM_DATA;
        if (GetMenuItemInfoW(popup, selected, FALSE, &itemInfo)) {
            const auto* suggestionItem = reinterpret_cast<const OwnerDrawMenuItem*>(itemInfo.dwItemData);
            if (suggestionItem != nullptr) {
                SendMessageW(editor_, EM_SETSEL, contextMenuWordStart_, contextMenuWordStart_ + contextMenuWordLength_);
                SendMessageW(editor_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(suggestionItem->text.c_str()));
            }
        }
    } else if (selected == kSpellMenuIgnoreOnce) {
        spellChecker_.Ignore(contextMenuWord_);
        RunSpellCheckNow();
    } else if (selected == kSpellMenuAddToDictionary) {
        spellChecker_.Add(contextMenuWord_);
        RunSpellCheckNow();
    } else if (selected != 0) {
        SendMessageW(mainWindow_, WM_COMMAND, MAKEWPARAM(selected, 0), 0);
    }

    DestroyMenu(popup);
    return true;
}

void ClassicNotepadApp::ShowAboutDialog()
{
    AboutDialogState state{};
    state.instance = instance_;

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

        MessageBoxW(mainWindow_, message.c_str(), L"About Classic Notepad", MB_OK | MB_ICONINFORMATION);
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

    return MessageBoxW(
        mainWindow_,
        prompt.c_str(),
        L"Classic Notepad",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) == IDYES;
}

void ClassicNotepadApp::HandleEditorChanged()
{
    if (suppressEditorChange_) {
        return;
    }

    statusCharacterCountDirty_ = true;
    InvalidateWidestLineCache();
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
    if (!ConfirmSaveChanges()) {
        return;
    }

    document_.ResetUntitled();
    SetEditorText(L"");
    UpdateTitle();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleOpen()
{
    if (!ConfirmSaveChanges()) {
        return;
    }

    const std::wstring path = ShowOpenFileDialog();
    if (path.empty()) {
        return;
    }

    LoadDocument(path);
}

bool ClassicNotepadApp::HandleSave()
{
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
    }

    DeleteDC(printDialog.hDC);
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleUndo()
{
    SendMessageW(editor_, EM_UNDO, 0, 0);
    UpdateStatusBar();
    UpdateScrollBars();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleCut()
{
    SendMessageW(editor_, WM_CUT, 0, 0);
    UpdateStatusBar();
    UpdateScrollBars();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleCopy()
{
    SendMessageW(editor_, WM_COPY, 0, 0);
    SetFocus(editor_);
}

void ClassicNotepadApp::HandlePaste()
{
    SendMessageW(editor_, WM_PASTE, 0, 0);
    UpdateStatusBar();
    UpdateScrollBars();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleDelete()
{
    SendMessageW(editor_, WM_CLEAR, 0, 0);
    UpdateStatusBar();
    UpdateScrollBars();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleFind()
{
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
    if (findBuffer_[0] == L'\0') {
        HandleFind();
        return;
    }

    FindNextWithFlags(findFlags_, true);
}

void ClassicNotepadApp::HandleReplace()
{
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
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    GetSelectionRange(selectionStart, selectionEnd);

    const int currentLine = static_cast<int>(SendMessageW(editor_, EM_LINEFROMCHAR, selectionStart, 0)) + 1;
    const int maxLine = std::max<int>(1, static_cast<int>(SendMessageW(editor_, EM_GETLINECOUNT, 0, 0)));
    const int targetLine = ShowGoToDialog(currentLine, maxLine);
    if (targetLine > 0) {
        GoToLine(targetLine);
    }
}

void ClassicNotepadApp::HandleSelectAll()
{
    SendMessageW(editor_, EM_SETSEL, 0, -1);
    UpdateStatusBar();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleTimeDate()
{
    ReplaceSelection(BuildTimeDateText());
    UpdateStatusBar();
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleToggleWordWrap()
{
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
    const std::wstring text = GetEditorText();

    HWND oldEditor = editor_;
    editor_ = nullptr;
    wordWrap_ = !wordWrap_;
    DestroyWindow(oldEditor);

    editor_ = CreateEditor();
    if (editor_ == nullptr) {
        MessageBoxW(mainWindow_, L"Could not recreate the editor control.", L"Classic Notepad", MB_OK | MB_ICONERROR);
        DestroyWindow(mainWindow_);
        return;
    }

    SetEditorText(text);
    SendMessageW(editor_, EM_SETMODIFY, wasModified ? TRUE : FALSE, 0);
    document_.SetModified(wasModified);
    SendMessageW(editor_, EM_SETSEL, selectionStart, selectionEnd);
    if (firstVisibleLine > 0) {
        SendMessageW(editor_, EM_LINESCROLL, 0, firstVisibleLine);
    }

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

    HFONT oldFont = editorFont_;
    const bool ownedOldFont = ownsEditorFont_;
    editorFont_ = newFont;
    ownsEditorFont_ = true;
    SendMessageW(editor_, WM_SETFONT, reinterpret_cast<WPARAM>(editorFont_), TRUE);
    if (ownedOldFont && oldFont != nullptr) {
        DeleteObject(oldFont);
    }

    InvalidateWidestLineCache();
    ResizeEditor();
    UpdateStatusBar();
    UpdateScrollBars();
    InvalidateRect(editor_, nullptr, FALSE);
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleToggleStatusBar()
{
    statusBarVisible_ = !statusBarVisible_;
    if (statusBar_ != nullptr) {
        ShowWindow(statusBar_, statusBarVisible_ ? SW_SHOW : SW_HIDE);
    }

    ResizeEditor();
    UpdateStatusBar();
    UpdateScrollBars();
    UpdateMenuState(ActiveMenu());
    UpdateMenuChrome();
    SetFocus(editor_);
}

bool ClassicNotepadApp::LoadDocument(const std::wstring& path)
{
    std::wstring editorText;
    std::wstring errorMessage;

    if (!document_.Load(path, editorText, errorMessage)) {
        ShowError(errorMessage);
        UpdateTitle();
        return false;
    }

    SetEditorText(editorText);
    UpdateTitle();
    SetFocus(editor_);
    return true;
}

bool ClassicNotepadApp::ConfirmSaveChanges()
{
    if (!document_.IsModified()) {
        return true;
    }

    std::wstring prompt = L"Do you want to save changes to ";
    prompt += document_.DisplayName();
    prompt += L"?";

    const int result = MessageBoxW(
        mainWindow_,
        prompt.c_str(),
        L"Classic Notepad",
        MB_YESNOCANCEL | MB_ICONWARNING);

    if (result == IDYES) {
        return HandleSave();
    }

    return result == IDNO;
}

std::wstring ClassicNotepadApp::GetEditorText() const
{
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
    UpdateStatusBar();
    UpdateScrollBars();
    InvalidateRect(editor_, nullptr, TRUE);
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

    document_.SetModified(true);
    UpdateTitle();
    UpdateStatusBar();
    UpdateScrollBars();
    InvalidateRect(editor_, nullptr, TRUE);
    RefreshSpellCheck(true);
}

void ClassicNotepadApp::ReplaceSelection(const std::wstring& text)
{
    SendMessageW(editor_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
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
            MessageBoxW(mainWindow_, message.c_str(), L"Classic Notepad", MB_OK | MB_ICONINFORMATION);
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
                UpdateStatusBar();
                UpdateScrollBars();
                SetFocus(editor_);
                findFlags_ = flags & kFindOptionFlags;
                return true;
            }
        }
    } else if (selectionStart > 0U) {
        std::size_t position = std::min<std::size_t>(selectionStart - 1U, lastPossible);
        for (;;) {
            if (TextMatchesAt(text, position, needle, matchCase, wholeWord)) {
                SendMessageW(editor_, EM_SETSEL, position, position + needle.size());
                SendMessageW(editor_, EM_SCROLLCARET, 0, 0);
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
        MessageBoxW(mainWindow_, message.c_str(), L"Classic Notepad", MB_OK | MB_ICONINFORMATION);
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
        MessageBoxW(mainWindow_, message.c_str(), L"Classic Notepad", MB_OK | MB_ICONINFORMATION);
        return;
    }

    ReplaceEditorTextFromCommand(replaced);
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

bool ClassicNotepadApp::PrintEditorText(HDC printerDc, std::wstring& errorMessage) const
{
    if (printerDc == nullptr) {
        errorMessage = L"Printing failed.\n\nThe selected printer could not be opened.";
        return false;
    }

    const RECT textRect = BuildPrintableTextRect(printerDc, pageMarginsThousandths_);
    const int printableWidth = textRect.right - textRect.left;
    const int printableHeight = textRect.bottom - textRect.top;
    if (printableWidth <= 0 || printableHeight <= 0) {
        errorMessage = L"Printing failed.\n\nThe printable page area is too small.";
        return false;
    }

    HFONT printFont = CreatePrinterFont(printerDc, editorFont_, mainWindow_);
    if (printFont == nullptr) {
        errorMessage = L"Printing failed.\n\nThe selected editor font could not be prepared for printing.";
        return false;
    }

    HGDIOBJ previousFont = SelectObject(printerDc, printFont);
    if (previousFont == nullptr || previousFont == HGDI_ERROR) {
        DeleteObject(printFont);
        errorMessage = L"Printing failed.\n\nThe printer could not select the editor font.";
        return false;
    }

    SetBkMode(printerDc, TRANSPARENT);
    SetTextColor(printerDc, RGB(0, 0, 0));

    TEXTMETRICW textMetrics{};
    if (!GetTextMetricsW(printerDc, &textMetrics)) {
        SelectObject(printerDc, previousFont);
        DeleteObject(printFont);
        errorMessage = L"Printing failed.\n\nThe printer could not measure the selected font.";
        return false;
    }

    const int lineHeight = std::max(1, static_cast<int>(textMetrics.tmHeight + textMetrics.tmExternalLeading));
    if (lineHeight > printableHeight) {
        SelectObject(printerDc, previousFont);
        DeleteObject(printFont);
        errorMessage = L"Printing failed.\n\nThe selected font is too large for the printable page area.";
        return false;
    }

    std::wstring documentName = document_.DisplayName();
    if (documentName.empty()) {
        documentName = L"Classic Notepad Document";
    }

    DOCINFOW docInfo{};
    docInfo.cbSize = sizeof(docInfo);
    docInfo.lpszDocName = documentName.c_str();

    if (StartDocW(printerDc, &docInfo) <= 0) {
        SelectObject(printerDc, previousFont);
        DeleteObject(printFont);
        errorMessage = L"Printing failed.\n\nThe print job could not be started.";
        return false;
    }

    auto failPrintJob = [&](const std::wstring& message) {
        AbortDoc(printerDc);
        SelectObject(printerDc, previousFont);
        DeleteObject(printFont);
        errorMessage = message;
        return false;
    };

    if (StartPage(printerDc) <= 0) {
        return failPrintJob(L"Printing failed.\n\nThe first page could not be started.");
    }

    int y = textRect.top;
    auto startNewPage = [&]() {
        if (EndPage(printerDc) <= 0) {
            return false;
        }

        if (StartPage(printerDc) <= 0) {
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
                return failPrintJob(L"Printing failed.\n\nA new page could not be started.");
            }

            y += lineHeight;
            continue;
        }

        std::size_t start = 0;
        while (start < line.size()) {
            if (y + lineHeight > textRect.bottom && !startNewPage()) {
                return failPrintJob(L"Printing failed.\n\nA new page could not be started.");
            }

            std::size_t length = FindFittingTextLength(printerDc, line, start, printableWidth);
            length = FindWrapBreakLength(line, start, length);

            std::size_t printLength = length;
            while (printLength > 0U && line[start + printLength - 1U] == L' ') {
                --printLength;
            }

            if (printLength > 0U &&
                !TextOutW(printerDc, textRect.left, y, line.c_str() + start, static_cast<int>(printLength))) {
                return failPrintJob(L"Printing failed.\n\nText could not be sent to the printer.");
            }

            y += lineHeight;
            start += length;
            while (start < line.size() && line[start] == L' ') {
                ++start;
            }
        }
    }

    if (EndPage(printerDc) <= 0) {
        return failPrintJob(L"Printing failed.\n\nThe last page could not be completed.");
    }

    if (EndDoc(printerDc) <= 0) {
        SelectObject(printerDc, previousFont);
        DeleteObject(printFont);
        errorMessage = L"Printing failed.\n\nThe print job could not be completed.";
        return false;
    }

    SelectObject(printerDc, previousFont);
    DeleteObject(printFont);
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

int ClassicNotepadApp::ShowGoToDialog(int currentLine, int maxLine)
{
    GoToDialogState state{};
    state.currentLine = currentLine;
    state.maxLine = maxLine;
    state.selectedLine = currentLine;

    const INT_PTR result = DialogBoxParamW(
        instance_,
        MAKEINTRESOURCEW(IDD_GOTO_DIALOG),
        mainWindow_,
        ClassicNotepadApp::GoToDialogProc,
        reinterpret_cast<LPARAM>(&state));

    return result == IDOK ? state.selectedLine : 0;
}

void ClassicNotepadApp::GoToLine(int lineNumber)
{
    const int zeroBasedLine = std::max(0, lineNumber - 1);
    LRESULT characterIndex = SendMessageW(editor_, EM_LINEINDEX, static_cast<WPARAM>(zeroBasedLine), 0);
    if (characterIndex < 0) {
        characterIndex = GetWindowTextLengthW(editor_);
    }

    SendMessageW(editor_, EM_SETSEL, static_cast<WPARAM>(characterIndex), static_cast<LPARAM>(characterIndex));
    SendMessageW(editor_, EM_SCROLLCARET, 0, 0);
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
    std::array<wchar_t, 32768> fileName{};
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
    std::array<wchar_t, 32768> fileName{};
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
    MessageBoxW(mainWindow_, message.c_str(), L"Classic Notepad", MB_OK | MB_ICONERROR);
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
        editor_ = CreateEditor();
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
        if (!spellCheckAvailable_) {
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
        return 0;

    case WM_SETFOCUS:
        if (editor_ != nullptr) {
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
        if (reinterpret_cast<HWND>(lParam) == editor_ && HIWORD(wParam) == EN_CHANGE) {
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
        if (ConfirmSaveChanges()) {
            DestroyWindow(mainWindow_);
        }
        return 0;

    case WM_DESTROY:
        CloseFindReplaceDialogs();
        if (spellCheckTimerId_ != 0) {
            KillTimer(mainWindow_, spellCheckTimerId_);
            spellCheckTimerId_ = 0;
        }
        if (mainMenu_ != nullptr && GetMenu(mainWindow_) != mainMenu_) {
            DestroyMenu(mainMenu_);
            mainMenu_ = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    case WM_TIMER:
        if (wParam == kSpellCheckTimerId) {
            RunSpellCheckNow();
            return 0;
        }
        break;

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
            SetDlgItemInt(dialog, IDC_GOTO_LINE_EDIT, static_cast<UINT>(state->currentLine), FALSE);
            SendDlgItemMessageW(dialog, IDC_GOTO_LINE_EDIT, EM_SETLIMITTEXT, 9, 0);
            SendDlgItemMessageW(dialog, IDC_GOTO_LINE_EDIT, EM_SETSEL, 0, -1);
        }
        SetFocus(GetDlgItem(dialog, IDC_GOTO_LINE_EDIT));
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            if (state != nullptr) {
                BOOL translated = FALSE;
                const UINT lineNumber = GetDlgItemInt(dialog, IDC_GOTO_LINE_EDIT, &translated, FALSE);
                if (!translated || lineNumber == 0U || lineNumber > static_cast<UINT>(state->maxLine)) {
                    std::wstring messageText = L"Line number must be between 1 and ";
                    messageText += std::to_wstring(state->maxLine);
                    messageText += L".";
                    MessageBoxW(dialog, messageText.c_str(), L"Classic Notepad", MB_OK | MB_ICONWARNING);
                    SendDlgItemMessageW(dialog, IDC_GOTO_LINE_EDIT, EM_SETSEL, 0, -1);
                    SetFocus(GetDlgItem(dialog, IDC_GOTO_LINE_EDIT));
                    return TRUE;
                }

                state->selectedLine = static_cast<int>(lineNumber);
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
            state->largeIcon = static_cast<HICON>(LoadImageW(
                state->instance,
                MAKEINTRESOURCEW(IDI_ABOUTICON),
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

    case WM_DRAWITEM:
        if (wParam == IDC_ABOUT_ICON && state != nullptr && state->largeIcon != nullptr) {
            auto* drawItem = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            const int controlWidth = std::max(0, static_cast<int>(drawItem->rcItem.right - drawItem->rcItem.left));
            const int controlHeight = std::max(0, static_cast<int>(drawItem->rcItem.bottom - drawItem->rcItem.top));
            const int iconSize = std::min(kAboutIconSizePixels, std::min(controlWidth, controlHeight));
            const int x = drawItem->rcItem.left + ((controlWidth - iconSize) / 2);
            const int y = drawItem->rcItem.top + ((controlHeight - iconSize) / 2);

            FillRect(drawItem->hDC, &drawItem->rcItem, reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1));
            DrawIconEx(drawItem->hDC, x, y, state->largeIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            return TRUE;
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
            if (app->editor_ != nullptr) {
                SetFocus(app->editor_);
            }
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

        if (message == WM_MOUSEWHEEL && app->HandleMouseWheel(wParam)) {
            return 0;
        }
    }

    WNDPROC originalProc = app != nullptr ? app->originalEditorProc_ : nullptr;
    const LRESULT result = originalProc != nullptr
        ? CallWindowProcW(originalProc, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);

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
            app->RefreshSpellCheck(false);
        }
        InvalidateRect(window, nullptr, FALSE);
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
