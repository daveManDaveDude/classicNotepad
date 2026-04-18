#include "app.h"

#include "resource.h"

#include <cderr.h>
#include <commdlg.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kMainWindowClass[] = L"ClassicNotepadMainWindow";
constexpr wchar_t kAppTitle[] = L"Untitled - Classic Notepad";
constexpr wchar_t kFileDialogFilter[] = L"Text Documents (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
constexpr wchar_t kFindReplaceMessageName[] = L"commdlg_FindReplace";
constexpr DWORD kFindOptionFlags = FR_DOWN | FR_MATCHCASE | FR_WHOLEWORD;
constexpr int kTabWidthSpaces = 8;

struct GoToDialogState {
    int currentLine = 1;
    int maxLine = 1;
    int selectedLine = 1;
};

bool IsWordCharacter(wchar_t character)
{
    return std::iswalnum(static_cast<wint_t>(character)) != 0 || character == L'_';
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
}

ClassicNotepadApp::~ClassicNotepadApp()
{
    DestroyOwnedEditorFont();
    DestroyPrintDialogHandles();
}

int ClassicNotepadApp::Run(int showCommand, const std::wstring& initialFilePath)
{
    if (!RegisterMainWindowClass() || !CreateMainWindow(showCommand)) {
        return -1;
    }

    accelerator_ = LoadAcceleratorsW(instance_, MAKEINTRESOURCEW(IDR_ACCELERATORS));

    if (!initialFilePath.empty()) {
        LoadDocument(initialFilePath);
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
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    windowClass.lpszClassName = kMainWindowClass;
    windowClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);

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

    ShowWindow(mainWindow_, showCommand);
    UpdateWindow(mainWindow_);
    return true;
}

HWND ClassicNotepadApp::CreateEditor()
{
    const DWORD editorStyle =
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        (wordWrap_ ? 0 : WS_HSCROLL) |
        ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
        (wordWrap_ ? 0 : ES_AUTOHSCROLL) |
        ES_NOHIDESEL | ES_WANTRETURN;

    HWND editor = CreateWindowExW(
        WS_EX_CLIENTEDGE,
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
        editorFont_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        ownsEditorFont_ = false;
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
        WS_CHILD | (statusBarVisible_ ? WS_VISIBLE : 0) | SBARS_SIZEGRIP,
        0,
        0,
        0,
        0,
        mainWindow_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VIEW_STATUS_BAR)),
        instance_,
        nullptr);

    return statusBar;
}

void ClassicNotepadApp::ResizeEditor()
{
    if (editor_ == nullptr) {
        return;
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
    const int height = std::max(0, static_cast<int>(clientArea.bottom - clientArea.top) - statusHeight);
    MoveWindow(editor_, 0, 0, width, height, TRUE);
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
    std::wstring text = L"Ln ";
    text += std::to_wstring(line);
    text += L", Col ";
    text += std::to_wstring(column);

    SendMessageW(statusBar_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void ClassicNotepadApp::ShowAboutDialog()
{
    constexpr wchar_t message[] =
        L"Classic Notepad\n\n"
        L"Phase 6 native Win32 build.\n"
        L"Single editor window, classic menu bar, file open/save, classic edit commands, word wrap, font selection, status bar, page setup, print, and no modern extras.";

    MessageBoxW(mainWindow_, message, L"About Classic Notepad", MB_OK | MB_ICONINFORMATION);
}

void ClassicNotepadApp::HandleEditorChanged()
{
    if (suppressEditorChange_) {
        return;
    }

    UpdateStatusBar();

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
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleCut()
{
    SendMessageW(editor_, WM_CUT, 0, 0);
    UpdateStatusBar();
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
    SetFocus(editor_);
}

void ClassicNotepadApp::HandleDelete()
{
    SendMessageW(editor_, WM_CLEAR, 0, 0);
    UpdateStatusBar();
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
    UpdateTitle();
    UpdateMenuState(GetMenu(mainWindow_));
    DrawMenuBar(mainWindow_);
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

    ResizeEditor();
    UpdateStatusBar();
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
    UpdateMenuState(GetMenu(mainWindow_));
    DrawMenuBar(mainWindow_);
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
    suppressEditorChange_ = true;
    SetWindowTextW(editor_, text.c_str());
    SendMessageW(editor_, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(editor_, EM_SETMODIFY, FALSE, 0);
    suppressEditorChange_ = false;
    UpdateStatusBar();
}

void ClassicNotepadApp::ReplaceEditorTextFromCommand(const std::wstring& text)
{
    suppressEditorChange_ = true;
    SetWindowTextW(editor_, text.c_str());
    SendMessageW(editor_, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(editor_, EM_SETMODIFY, TRUE, 0);
    suppressEditorChange_ = false;

    document_.SetModified(true);
    UpdateTitle();
    UpdateStatusBar();
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

bool ClassicNotepadApp::FindNextWithFlags(DWORD flags, bool showNotFoundMessage)
{
    const std::wstring needle(findBuffer_.data());
    if (needle.empty()) {
        return false;
    }

    const std::wstring text = GetEditorText();
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
    if (!SelectionMatchesFindText(flags)) {
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
    SetFocus(editor_);
}

bool ClassicNotepadApp::SelectionMatchesFindText(DWORD flags) const
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

    const std::wstring text = GetEditorText();
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
    if (previousFont == nullptr) {
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
        const bool replaced = ReplaceCurrentSelectionIfMatch(findReplace->Flags);
        FindNextWithFlags(findReplace->Flags, !replaced);
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
        editor_ = CreateEditor();
        if (editor_ == nullptr) {
            MessageBoxW(mainWindow_, L"Could not create the editor control.", L"Classic Notepad", MB_OK | MB_ICONERROR);
            return -1;
        }
        statusBar_ = CreateStatusBar();
        if (statusBar_ == nullptr) {
            MessageBoxW(mainWindow_, L"Could not create the status bar.", L"Classic Notepad", MB_OK | MB_ICONERROR);
            return -1;
        }
        document_.ResetUntitled();
        ResizeEditor();
        UpdateStatusBar();
        SetFocus(editor_);
        return 0;

    case WM_SIZE:
        ResizeEditor();
        return 0;

    case WM_SETFOCUS:
        if (editor_ != nullptr) {
            SetFocus(editor_);
        }
        return 0;

    case WM_INITMENUPOPUP:
        if (HIWORD(lParam) == 0) {
            UpdateMenuState(GetMenu(mainWindow_));
        }
        return 0;

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
        PostQuitMessage(0);
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

LRESULT CALLBACK ClassicNotepadApp::EditorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    ClassicNotepadApp* app = reinterpret_cast<ClassicNotepadApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    WNDPROC originalProc = app != nullptr ? app->originalEditorProc_ : nullptr;
    const LRESULT result = originalProc != nullptr
        ? CallWindowProcW(originalProc, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);

    if (app == nullptr) {
        return result;
    }

    switch (message) {
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
        app->UpdateStatusBar();
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
