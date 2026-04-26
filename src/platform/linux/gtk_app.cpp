#include "gtk_app.h"

#include "app_version.h"
#include "gtk_actions.h"
#include "gtk_automation.h"
#include "gtk_dialogs.h"
#include "text_metadata.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cwctype>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

#include <pango/pangocairo.h>

namespace classic_notepad::linux_ui {
namespace {

void OnActivate(GtkApplication* application, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->Activate(application);
}

void OnBufferChangedSignal(GtkTextBuffer*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->OnBufferChanged();
}

void OnCursorMovedSignal(GObject*, GParamSpec*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->OnCursorMoved();
}

gboolean OnCloseRequest(GtkWindow*, gpointer userData)
{
    return static_cast<GtkNotepadApp*>(userData)->ConfirmDiscard() ? FALSE : TRUE;
}

void OnWindowDestroy(GtkWidget*, gpointer userData)
{
    auto* app = static_cast<GtkNotepadApp*>(userData);
    app->OnWindowDestroyed();
}

std::vector<std::uint8_t> BytesFromString(const std::string& text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::size_t ClampOffset(std::size_t value)
{
    return std::min<std::size_t>(value, static_cast<std::size_t>(G_MAXINT));
}

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

bool CharactersEqual(wchar_t left, wchar_t right, bool matchCase)
{
    if (matchCase) {
        return left == right;
    }

    return std::towlower(static_cast<wint_t>(left)) == std::towlower(static_cast<wint_t>(right));
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

    for (std::size_t index = 0; index < needle.size(); ++index) {
        if (!CharactersEqual(text[position + index], needle[index], matchCase)) {
            return false;
        }
    }

    if (!wholeWord) {
        return true;
    }

    return HasWordBoundaryAt(text, position) && HasWordBoundaryAt(text, position + needle.size());
}

bool IsLineBreakAt(const std::wstring& rawText, std::size_t position, std::size_t& rawLength)
{
    if (position >= rawText.size()) {
        rawLength = 0;
        return false;
    }

    if (rawText[position] == L'\r') {
        rawLength = position + 1U < rawText.size() && rawText[position + 1U] == L'\n' ? 2U : 1U;
        return true;
    }

    if (rawText[position] == L'\n') {
        rawLength = 1U;
        return true;
    }

    rawLength = 0;
    return false;
}

std::size_t EditorOffsetFromBufferOffset(const std::wstring& rawText, std::size_t bufferOffset)
{
    std::size_t editorOffset = 0;
    std::size_t rawOffset = 0;
    while (rawOffset < rawText.size() && rawOffset < bufferOffset) {
        std::size_t rawLineBreakLength = 0;
        if (IsLineBreakAt(rawText, rawOffset, rawLineBreakLength)) {
            editorOffset += 2U;
            rawOffset += rawLineBreakLength;
        } else {
            ++editorOffset;
            ++rawOffset;
        }
    }

    return editorOffset;
}

std::size_t BufferOffsetFromEditorOffset(const std::wstring& rawText, std::size_t editorOffset)
{
    std::size_t rawOffset = 0;
    std::size_t currentEditorOffset = 0;
    while (rawOffset < rawText.size()) {
        std::size_t rawLineBreakLength = 0;
        const std::size_t editorLength = IsLineBreakAt(rawText, rawOffset, rawLineBreakLength) ? 2U : 1U;
        if (editorOffset < currentEditorOffset + editorLength) {
            return rawOffset;
        }

        currentEditorOffset += editorLength;
        rawOffset += rawLineBreakLength == 0U ? 1U : rawLineBreakLength;
        if (editorOffset == currentEditorOffset) {
            return rawOffset;
        }
    }

    return rawText.size();
}

std::string EscapeCssString(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (char character : text) {
        if (character == '\\' || character == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::string CssSizeFromPangoDescription(PangoFontDescription* description)
{
    const int pangoSize = pango_font_description_get_size(description);
    if (pangoSize <= 0) {
        return "11pt";
    }

    const double points = static_cast<double>(pangoSize) / static_cast<double>(PANGO_SCALE);
    std::ostringstream output;
    output << std::fixed << std::setprecision(points == static_cast<int>(points) ? 0 : 1) << points << "pt";
    return output.str();
}

struct ClipboardReadState {
    GMainLoop* loop = nullptr;
    std::wstring text;
};

void OnClipboardTextReady(GObject* sourceObject, GAsyncResult* result, gpointer userData)
{
    auto* state = static_cast<ClipboardReadState*>(userData);
    GError* error = nullptr;
    char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(sourceObject), result, &error);
    if (text != nullptr) {
        state->text = WideFromUtf8(text);
        g_free(text);
    }

    if (error != nullptr) {
        g_error_free(error);
    }

    if (state->loop != nullptr && g_main_loop_is_running(state->loop)) {
        g_main_loop_quit(state->loop);
    }
}

std::wstring ReadClipboardText(GtkWidget* widget)
{
    if (widget == nullptr) {
        return {};
    }

    GdkClipboard* clipboard = gtk_widget_get_clipboard(widget);
    if (clipboard == nullptr) {
        return {};
    }

    ClipboardReadState state;
    state.loop = g_main_loop_new(nullptr, FALSE);
    gdk_clipboard_read_text_async(clipboard, nullptr, OnClipboardTextReady, &state);
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    return classic_notepad::NormalizeLineEndingsForEditor(state.text);
}

std::wstring TextForNativePrinting(const std::wstring& text)
{
    std::wstring normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            if (index + 1U < text.size() && text[index + 1U] == L'\n') {
                ++index;
            }
            normalized.push_back(L'\n');
            continue;
        }

        normalized.push_back(text[index]);
    }

    return normalized;
}

std::wstring BuildPrintSinkText(
    const wchar_t* platform,
    const std::wstring& fontDescription,
    const GtkNotepadApp::AutomationPageMargins& margins,
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
    const std::string utf8Path = Utf8FromWide(path);
    if (utf8Path.empty()) {
        errorMessage = L"Print sink path cannot be empty.";
        return false;
    }

    const std::string utf8Text = Utf8FromWide(text);
    GError* error = nullptr;
    const gboolean saved = g_file_set_contents(
        utf8Path.c_str(),
        utf8Text.data(),
        static_cast<gssize>(utf8Text.size()),
        &error);
    if (saved != FALSE) {
        return true;
    }

    errorMessage = L"The print sink could not be written.";
    if (error != nullptr) {
        errorMessage += L"\n\n";
        errorMessage += WideFromUtf8(error->message);
        g_error_free(error);
    }
    return false;
}

PangoLayout* CreatePrintLayout(GtkPrintContext* context, const GtkNotepadApp& app)
{
    PangoLayout* layout = gtk_print_context_create_pango_layout(context);
    const std::wstring printText = TextForNativePrinting(app.GetText());
    const std::string utf8Text = Utf8FromWide(printText);
    pango_layout_set_text(layout, utf8Text.c_str(), static_cast<int>(utf8Text.size()));
    pango_layout_set_width(
        layout,
        static_cast<int>(std::max(1.0, gtk_print_context_get_width(context)) * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

    const std::string utf8Font = Utf8FromWide(app.GetFont());
    PangoFontDescription* description = pango_font_description_from_string(
        utf8Font.empty() ? "Monospace 11" : utf8Font.c_str());
    if (description != nullptr) {
        pango_layout_set_font_description(layout, description);
        pango_font_description_free(description);
    }

    return layout;
}

void OnBeginPrint(GtkPrintOperation* operation, GtkPrintContext* context, gpointer userData)
{
    const auto* app = static_cast<const GtkNotepadApp*>(userData);
    PangoLayout* layout = CreatePrintLayout(context, *app);

    int layoutHeight = 0;
    pango_layout_get_size(layout, nullptr, &layoutHeight);
    const double height = static_cast<double>(layoutHeight) / static_cast<double>(PANGO_SCALE);
    const double pageHeight = std::max(1.0, gtk_print_context_get_height(context));
    const int pages = std::max(1, static_cast<int>(std::ceil(height / pageHeight)));
    gtk_print_operation_set_n_pages(operation, pages);

    g_object_unref(layout);
}

void OnDrawPrintPage(GtkPrintOperation*, GtkPrintContext* context, int pageNumber, gpointer userData)
{
    const auto* app = static_cast<const GtkNotepadApp*>(userData);
    PangoLayout* layout = CreatePrintLayout(context, *app);
    cairo_t* cairo = gtk_print_context_get_cairo_context(context);
    const double width = gtk_print_context_get_width(context);
    const double pageHeight = std::max(1.0, gtk_print_context_get_height(context));

    cairo_save(cairo);
    cairo_rectangle(cairo, 0, 0, width, pageHeight);
    cairo_clip(cairo);
    cairo_translate(cairo, 0, -static_cast<double>(pageNumber) * pageHeight);
    pango_cairo_show_layout(cairo, layout);
    cairo_restore(cairo);

    g_object_unref(layout);
}

} // namespace

std::wstring WideFromUtf8(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return {};
    }

    return WideFromUtf8(std::string(text));
}

std::wstring WideFromUtf8(const std::string& text)
{
    if (text.empty()) {
        return {};
    }

    classic_notepad::DecodeTextResult decoded;
    std::wstring error;
    if (!classic_notepad::DecodeTextBytes(BytesFromString(text), decoded, error)) {
        return {};
    }

    return decoded.text;
}

std::string Utf8FromWide(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }

    std::vector<std::uint8_t> bytes;
    std::wstring error;
    if (!classic_notepad::EncodeTextBytes(text, classic_notepad::TextEncoding::Utf8NoBom, bytes, error)) {
        return {};
    }

    return std::string(bytes.begin(), bytes.end());
}

GtkNotepadApp::GtkNotepadApp(std::wstring initialPath)
    : initialPath_(std::move(initialPath))
{
}

GtkNotepadApp::~GtkNotepadApp()
{
    if (window_ != nullptr) {
        GtkWidget* window = window_;
        window_ = nullptr;
        gtk_window_destroy(GTK_WINDOW(window));
    }

    if (fontProvider_ != nullptr) {
        g_object_unref(fontProvider_);
        fontProvider_ = nullptr;
    }

    if (pageSetup_ != nullptr) {
        g_object_unref(pageSetup_);
        pageSetup_ = nullptr;
    }

    if (printSettings_ != nullptr) {
        g_object_unref(printSettings_);
        printSettings_ = nullptr;
    }

    if (application_ != nullptr) {
        g_object_unref(application_);
        application_ = nullptr;
    }
}

int GtkNotepadApp::Run(int argc, char** argv)
{
    application_ = gtk_application_new("dev.classicnotepad.gtk", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application_, "activate", G_CALLBACK(OnActivate), this);

    const int status = g_application_run(G_APPLICATION(application_), argc, argv);
    return status;
}

int GtkNotepadApp::RunAutomation(bool visible)
{
    automationMode_ = true;
    automationVisible_ = visible;
    application_ = gtk_application_new("dev.classicnotepad.gtk.automation", G_APPLICATION_DEFAULT_FLAGS);

    GError* error = nullptr;
    if (!g_application_register(G_APPLICATION(application_), nullptr, &error)) {
        if (error != nullptr) {
            g_printerr("Classic Notepad automation failed to initialize GTK: %s\n", error->message);
            g_error_free(error);
        }
        return -1;
    }

    BuildWindow(application_);
    HandleInitialFilePath(true);
    if (automationVisible_) {
        gtk_window_present(GTK_WINDOW(window_));
    }
    PumpEvents();

    GtkAutomationController controller(*this);
    const int result = controller.Run();

    if (window_ != nullptr) {
        GtkWidget* window = window_;
        window_ = nullptr;
        gtk_window_destroy(GTK_WINDOW(window));
    }

    return result;
}

void GtkNotepadApp::Activate(GtkApplication* application)
{
    if (window_ != nullptr) {
        gtk_window_present(GTK_WINDOW(window_));
        return;
    }

    BuildWindow(application);
    HandleInitialFilePath(false);
    gtk_window_present(GTK_WINDOW(window_));
}

void GtkNotepadApp::PumpEvents()
{
    while (g_main_context_pending(nullptr)) {
        g_main_context_iteration(nullptr, FALSE);
    }
}

GtkApplication* GtkNotepadApp::Application() const
{
    return application_;
}

GtkWindow* GtkNotepadApp::Window() const
{
    return window_ == nullptr ? nullptr : GTK_WINDOW(window_);
}

GtkTextBuffer* GtkNotepadApp::Buffer() const
{
    return buffer_;
}

GtkWidget* GtkNotepadApp::TextView() const
{
    return textView_;
}

bool GtkNotepadApp::IsAutomationMode() const
{
    return automationMode_;
}

void GtkNotepadApp::HandleNew()
{
    if (!ConfirmDiscard()) {
        return;
    }

    NewDocument();
}

void GtkNotepadApp::HandleOpen()
{
    if (!ConfirmDiscard()) {
        return;
    }

    const std::wstring path = ShowOpenFileDialog(Window());
    if (path.empty()) {
        return;
    }

    std::wstring error;
    if (!OpenFile(path, error)) {
        ShowError(error);
    }
}

bool GtkNotepadApp::HandleSave()
{
    if (!document_.HasPath()) {
        return HandleSaveAs();
    }

    std::wstring error;
    if (!Save(error)) {
        ShowError(error);
        return false;
    }

    return true;
}

bool GtkNotepadApp::HandleSaveAs()
{
    const std::wstring path = ShowSaveFileDialog(Window(), SuggestedSaveName());
    if (path.empty()) {
        return false;
    }

    std::wstring error;
    if (!SaveAs(path, error)) {
        ShowError(error);
        return false;
    }

    return true;
}

void GtkNotepadApp::HandlePageSetup()
{
    if (automationMode_) {
        return;
    }

    GtkPageSetup* selected = gtk_print_run_page_setup_dialog(
        Window(),
        EnsurePageSetup(),
        EnsurePrintSettings());
    if (selected == nullptr) {
        return;
    }

    if (pageSetup_ != nullptr) {
        g_object_unref(pageSetup_);
    }
    pageSetup_ = selected;
    StorePageSetup(pageSetup_);
}

void GtkNotepadApp::HandlePrint()
{
    if (automationMode_) {
        return;
    }

    GtkPrintOperation* operation = gtk_print_operation_new();
    gtk_print_operation_set_default_page_setup(operation, EnsurePageSetup());
    gtk_print_operation_set_print_settings(operation, EnsurePrintSettings());
    gtk_print_operation_set_job_name(operation, "Classic Notepad");
    g_signal_connect(operation, "begin-print", G_CALLBACK(OnBeginPrint), this);
    g_signal_connect(operation, "draw-page", G_CALLBACK(OnDrawPrintPage), this);

    GError* error = nullptr;
    const GtkPrintOperationResult result = gtk_print_operation_run(
        operation,
        GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
        Window(),
        &error);

    if (error != nullptr) {
        ShowError(WideFromUtf8(error->message));
        g_error_free(error);
    } else if (result == GTK_PRINT_OPERATION_RESULT_APPLY) {
        GtkPrintSettings* settings = gtk_print_operation_get_print_settings(operation);
        if (settings != nullptr) {
            g_object_ref(settings);
            if (printSettings_ != nullptr) {
                g_object_unref(printSettings_);
            }
            printSettings_ = settings;
        }
    }

    g_object_unref(operation);
}

void GtkNotepadApp::HandleExit()
{
    if (!ConfirmDiscard()) {
        return;
    }

    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void GtkNotepadApp::HandleUndo()
{
    Undo();
}

void GtkNotepadApp::HandleCut()
{
    Cut();
}

void GtkNotepadApp::HandleCopy()
{
    Copy();
}

void GtkNotepadApp::HandlePaste()
{
    Paste();
}

void GtkNotepadApp::HandleDelete()
{
    DeleteSelection();
}

void GtkNotepadApp::HandleFind()
{
    FindDialogResult result = ShowFindDialog(Window(), findText_, findMatchCase_, findWholeWord_, findSearchDown_);
    if (!result.accepted || result.text.empty()) {
        return;
    }

    if (!Find(result.text, result.matchCase, result.wholeWord, result.searchDown)) {
        std::wstring message = L"Cannot find \"";
        message += result.text;
        message += L"\".";
        ShowError(message);
    }
}

void GtkNotepadApp::HandleFindNext()
{
    if (findText_.empty()) {
        HandleFind();
        return;
    }

    if (!FindNext(findMatchCase_, findWholeWord_, findSearchDown_)) {
        std::wstring message = L"Cannot find \"";
        message += findText_;
        message += L"\".";
        ShowError(message);
    }
}

void GtkNotepadApp::HandleReplace()
{
    ReplaceDialogResult result = ShowReplaceDialog(
        Window(),
        findText_,
        replaceText_,
        findMatchCase_,
        findWholeWord_,
        findSearchDown_);

    if (result.action == ReplaceDialogAction::Cancel || result.text.empty()) {
        return;
    }

    findText_ = result.text;
    replaceText_ = result.replacement;
    findMatchCase_ = result.matchCase;
    findWholeWord_ = result.wholeWord;
    findSearchDown_ = result.searchDown;

    if (result.action == ReplaceDialogAction::Replace) {
        if (!Replace(result.text, result.replacement, result.matchCase, result.wholeWord, result.searchDown)) {
            std::wstring message = L"Cannot find \"";
            message += result.text;
            message += L"\".";
            ShowError(message);
        }
        return;
    }

    const std::size_t count = ReplaceAll(result.text, result.replacement, result.matchCase, result.wholeWord);
    if (count == 0U) {
        std::wstring message = L"Cannot find \"";
        message += result.text;
        message += L"\".";
        ShowError(message);
    }
}

void GtkNotepadApp::HandleGoTo()
{
    int selectedLine = CurrentLine();
    if (ShowGoToDialog(Window(), selectedLine, MaxLine(), selectedLine)) {
        std::wstring error;
        if (!GoToLine(selectedLine, error)) {
            ShowError(error);
        }
    }
}

void GtkNotepadApp::HandleSelectAll()
{
    SelectAll();
}

void GtkNotepadApp::HandleTimeDate()
{
    InsertTimeDate();
}

void GtkNotepadApp::HandleToggleWordWrap()
{
    SetWordWrap(!wordWrap_);
}

void GtkNotepadApp::HandleChooseFont()
{
    const std::wstring selectedFont = ShowFontDialog(Window(), fontDescription_);
    if (selectedFont.empty()) {
        return;
    }

    std::wstring error;
    if (!SetFont(selectedFont, error)) {
        ShowError(error);
    }
}

void GtkNotepadApp::HandleToggleStatusBar()
{
    SetStatusBarVisible(!statusBarVisible_);
}

void GtkNotepadApp::HandleAbout()
{
    if (automationMode_) {
        return;
    }

    ShowAboutDialog(Window(), CLASSIC_NOTEPAD_VERSION_DISPLAY_W);
}

bool GtkNotepadApp::NewDocument()
{
    document_.ResetUntitled();
    SetBufferText(L"", false);
    UpdateTitle();
    UpdateStatus();
    return true;
}

bool GtkNotepadApp::OpenFile(const std::wstring& path, std::wstring& errorMessage)
{
    std::wstring editorText;
    if (!document_.Load(path, editorText, errorMessage)) {
        UpdateTitle();
        UpdateStatus();
        return false;
    }

    SetBufferText(editorText, false);
    UpdateTitle();
    UpdateStatus();
    return true;
}

bool GtkNotepadApp::Save(std::wstring& errorMessage)
{
    if (!document_.Save(GetText(), errorMessage)) {
        return false;
    }

    UpdateTitle();
    UpdateStatus();
    return true;
}

bool GtkNotepadApp::SaveAs(const std::wstring& path, std::wstring& errorMessage)
{
    if (!document_.SaveAs(path, GetText(), errorMessage)) {
        return false;
    }

    UpdateTitle();
    UpdateStatus();
    return true;
}

bool GtkNotepadApp::ConfirmDiscard()
{
    if (automationMode_ || !document_.IsModified()) {
        return true;
    }

    const DirtyPromptResult result = ShowDirtyPrompt(Window(), document_.DisplayName());
    if (result == DirtyPromptResult::Save) {
        return HandleSave();
    }

    return result == DirtyPromptResult::Discard;
}

void GtkNotepadApp::CreateNewDocumentForPath(const std::wstring& path)
{
    document_.ResetNewFile(path);
    SetBufferText(L"", false);
    UpdateTitle();
    UpdateStatus();
}

void GtkNotepadApp::HandleInitialFilePath(bool createMissingWithoutPrompt)
{
    if (initialPath_.empty()) {
        SetInitialTitleAndStatus();
        return;
    }

    if (MissingFilePath(initialPath_)) {
        if (createMissingWithoutPrompt || ConfirmCreateMissingFile(Window(), initialPath_)) {
            CreateNewDocumentForPath(initialPath_);
        } else {
            document_.ResetUntitled();
            SetInitialTitleAndStatus();
        }
        return;
    }

    std::wstring error;
    if (!OpenFile(initialPath_, error)) {
        document_.ResetUntitled();
        SetInitialTitleAndStatus();
        ShowError(error);
    }
}

void GtkNotepadApp::UpdateTitle()
{
    title_.clear();
    if (document_.IsModified()) {
        title_ += L"*";
    }

    title_ += document_.DisplayName();
    title_ += L" - Classic Notepad";

    if (window_ != nullptr) {
        const std::string utf8Title = Utf8FromWide(title_);
        gtk_window_set_title(GTK_WINDOW(window_), utf8Title.c_str());
    }
}

void GtkNotepadApp::UpdateStatus()
{
    int line = 1;
    int column = 1;

    if (buffer_ != nullptr) {
        GtkTextIter cursor;
        gtk_text_buffer_get_iter_at_mark(buffer_, &cursor, gtk_text_buffer_get_insert(buffer_));
        line = gtk_text_iter_get_line(&cursor) + 1;
        column = gtk_text_iter_get_line_offset(&cursor) + 1;
    }

    const std::wstring text = GetText();

    statusText_ = L"Ln ";
    statusText_ += classic_notepad::FormatNumberWithSeparators(static_cast<std::size_t>(std::max(1, line)));
    statusText_ += L", Col ";
    statusText_ += classic_notepad::FormatNumberWithSeparators(static_cast<std::size_t>(std::max(1, column)));
    statusText_ += L" | ";
    statusText_ += classic_notepad::FormatCharacterCount(classic_notepad::CountStatusCharacters(text));
    statusText_ += L" | ";
    statusText_ += classic_notepad::FormatLineEnding(document_.LineEnding());
    statusText_ += L" | ";
    statusText_ += classic_notepad::FormatEncoding(document_.Encoding());

    if (statusLabel_ != nullptr) {
        const std::string utf8Status = Utf8FromWide(statusText_);
        gtk_label_set_text(GTK_LABEL(statusLabel_), utf8Status.c_str());
    }
}

void GtkNotepadApp::ShowError(const std::wstring& message)
{
    if (automationMode_) {
        return;
    }

    ShowErrorDialog(Window(), message);
}

void GtkNotepadApp::SetText(const std::wstring& text)
{
    SetBufferText(text, true);
    UpdateTitle();
    UpdateStatus();
}

void GtkNotepadApp::InsertText(const std::wstring& text)
{
    ReplaceSelection(text);
}

std::wstring GtkNotepadApp::GetSelectedText() const
{
    const AutomationSelection selection = GetSelection();
    if (selection.end <= selection.start) {
        return {};
    }

    const std::wstring text = GetText();
    const std::size_t start = std::min(selection.start, text.size());
    const std::size_t end = std::min(selection.end, text.size());
    return start < end ? text.substr(start, end - start) : std::wstring();
}

void GtkNotepadApp::ReplaceSelection(const std::wstring& text)
{
    if (buffer_ == nullptr) {
        return;
    }

    gtk_text_buffer_begin_user_action(buffer_);
    GtkTextIter start;
    GtkTextIter end;
    if (gtk_text_buffer_get_selection_bounds(buffer_, &start, &end)) {
        gtk_text_buffer_delete(buffer_, &start, &end);
        gtk_text_buffer_get_iter_at_mark(buffer_, &start, gtk_text_buffer_get_insert(buffer_));
    } else {
        gtk_text_buffer_get_iter_at_mark(buffer_, &start, gtk_text_buffer_get_insert(buffer_));
    }

    const std::string utf8Text = Utf8FromWide(text);
    gtk_text_buffer_insert(buffer_, &start, utf8Text.c_str(), static_cast<int>(utf8Text.size()));
    gtk_text_buffer_end_user_action(buffer_);
    document_.SetModified(true);
    UpdateTitle();
    UpdateStatus();
}

std::wstring GtkNotepadApp::GetText() const
{
    return classic_notepad::NormalizeLineEndingsForEditor(GetRawText());
}

std::wstring GtkNotepadApp::GetRawText() const
{
    if (buffer_ == nullptr) {
        return {};
    }

    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_get_end_iter(buffer_, &end);

    char* text = gtk_text_buffer_get_text(buffer_, &start, &end, TRUE);
    const std::wstring wideText = WideFromUtf8(text);
    g_free(text);

    return wideText;
}

std::wstring GtkNotepadApp::GetTitle() const
{
    return title_;
}

bool GtkNotepadApp::IsModified() const
{
    return document_.IsModified();
}

GtkNotepadApp::AutomationDocumentMetadata GtkNotepadApp::GetDocumentMetadata() const
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

std::wstring GtkNotepadApp::GetStatusText() const
{
    return statusText_;
}

GtkNotepadApp::AutomationSelection GtkNotepadApp::GetSelection() const
{
    AutomationSelection selection;
    if (buffer_ == nullptr) {
        return selection;
    }

    GtkTextIter start;
    GtkTextIter end;
    const std::wstring rawText = GetRawText();
    if (gtk_text_buffer_get_selection_bounds(buffer_, &start, &end)) {
        selection.start = EditorOffsetFromBufferOffset(rawText, static_cast<std::size_t>(gtk_text_iter_get_offset(&start)));
        selection.end = EditorOffsetFromBufferOffset(rawText, static_cast<std::size_t>(gtk_text_iter_get_offset(&end)));
        return selection;
    }

    gtk_text_buffer_get_iter_at_mark(buffer_, &start, gtk_text_buffer_get_insert(buffer_));
    selection.start = EditorOffsetFromBufferOffset(rawText, static_cast<std::size_t>(gtk_text_iter_get_offset(&start)));
    selection.end = selection.start;
    return selection;
}

void GtkNotepadApp::SetSelection(std::size_t selectionStart, std::size_t selectionEnd)
{
    if (buffer_ == nullptr) {
        return;
    }

    GtkTextIter start;
    GtkTextIter end;
    const std::wstring rawText = GetRawText();
    const std::size_t bufferStart = BufferOffsetFromEditorOffset(rawText, selectionStart);
    const std::size_t bufferEnd = BufferOffsetFromEditorOffset(rawText, selectionEnd);
    gtk_text_buffer_get_iter_at_offset(buffer_, &start, static_cast<int>(ClampOffset(bufferStart)));
    gtk_text_buffer_get_iter_at_offset(buffer_, &end, static_cast<int>(ClampOffset(bufferEnd)));
    gtk_text_buffer_select_range(buffer_, &start, &end);
    UpdateStatus();
}

void GtkNotepadApp::SelectAll()
{
    if (buffer_ == nullptr) {
        return;
    }

    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_get_end_iter(buffer_, &end);
    gtk_text_buffer_select_range(buffer_, &start, &end);
    UpdateStatus();
}

void GtkNotepadApp::Undo()
{
    if (buffer_ == nullptr) {
        return;
    }

    gtk_text_buffer_undo(buffer_);
    UpdateTitle();
    UpdateStatus();
}

void GtkNotepadApp::Copy()
{
    if (textView_ == nullptr) {
        return;
    }

    const std::wstring selectedText = GetSelectedText();
    if (selectedText.empty()) {
        return;
    }

    const std::string utf8Text = Utf8FromWide(selectedText);
    GdkClipboard* clipboard = gtk_widget_get_clipboard(textView_);
    if (clipboard != nullptr) {
        gdk_clipboard_set_text(clipboard, utf8Text.c_str());
    }
}

void GtkNotepadApp::Cut()
{
    Copy();
    DeleteSelection();
}

void GtkNotepadApp::Paste()
{
    const std::wstring text = ReadClipboardText(textView_);
    if (!text.empty()) {
        ReplaceSelection(text);
    }
}

void GtkNotepadApp::DeleteSelection()
{
    if (buffer_ == nullptr) {
        return;
    }

    GtkTextIter start;
    GtkTextIter end;
    if (gtk_text_buffer_get_selection_bounds(buffer_, &start, &end)) {
        gtk_text_buffer_begin_user_action(buffer_);
        gtk_text_buffer_delete(buffer_, &start, &end);
        gtk_text_buffer_end_user_action(buffer_);
        document_.SetModified(true);
        UpdateTitle();
        UpdateStatus();
    }
}

bool GtkNotepadApp::Find(const std::wstring& text, bool matchCase, bool wholeWord, bool searchDown)
{
    if (text.empty()) {
        return false;
    }

    findText_ = text;
    findMatchCase_ = matchCase;
    findWholeWord_ = wholeWord;
    findSearchDown_ = searchDown;
    if (searchDown) {
        SetSelection(0, 0);
    } else {
        const std::size_t textLength = GetText().size();
        SetSelection(textLength, textLength);
    }

    return FindNext(matchCase, wholeWord, searchDown);
}

bool GtkNotepadApp::FindNext(bool matchCase, bool wholeWord, bool searchDown)
{
    if (findText_.empty()) {
        return false;
    }

    const std::wstring text = GetText();
    if (text.empty() || findText_.size() > text.size()) {
        return false;
    }

    const AutomationSelection selection = GetSelection();
    const std::size_t selectionStart = std::min(selection.start, selection.end);
    const std::size_t selectionEnd = std::max(selection.start, selection.end);
    const std::size_t lastPossible = text.size() - findText_.size();

    if (searchDown) {
        const std::size_t startPosition = std::min(selectionEnd, text.size());
        for (std::size_t position = startPosition; position <= lastPossible; ++position) {
            if (TextMatchesAt(text, position, findText_, matchCase, wholeWord)) {
                SetSelection(position, position + findText_.size());
                findMatchCase_ = matchCase;
                findWholeWord_ = wholeWord;
                findSearchDown_ = searchDown;
                return true;
            }
        }
    } else if (selectionStart > 0U) {
        std::size_t position = std::min<std::size_t>(selectionStart - 1U, lastPossible);
        for (;;) {
            if (TextMatchesAt(text, position, findText_, matchCase, wholeWord)) {
                SetSelection(position, position + findText_.size());
                findMatchCase_ = matchCase;
                findWholeWord_ = wholeWord;
                findSearchDown_ = searchDown;
                return true;
            }

            if (position == 0U) {
                break;
            }
            --position;
        }
    }

    return false;
}

bool GtkNotepadApp::Replace(
    const std::wstring& text,
    const std::wstring& replacement,
    bool matchCase,
    bool wholeWord,
    bool searchDown)
{
    if (text.empty()) {
        return false;
    }

    findText_ = text;
    replaceText_ = replacement;
    findMatchCase_ = matchCase;
    findWholeWord_ = wholeWord;
    findSearchDown_ = searchDown;

    const std::wstring currentText = GetText();
    const AutomationSelection selection = GetSelection();
    const std::size_t selectionStart = std::min(selection.start, selection.end);
    const std::size_t selectionEnd = std::max(selection.start, selection.end);
    const bool selectionMatches =
        selectionEnd > selectionStart &&
        selectionEnd - selectionStart == text.size() &&
        TextMatchesAt(currentText, selectionStart, text, matchCase, wholeWord);
    if (!selectionMatches &&
        !FindNext(matchCase, wholeWord, searchDown)) {
        return false;
    }

    const AutomationSelection foundSelection = GetSelection();
    const std::size_t foundStart = std::min(foundSelection.start, foundSelection.end);
    const std::size_t foundEnd = std::max(foundSelection.start, foundSelection.end);
    if (foundEnd <= foundStart ||
        foundEnd - foundStart != text.size() ||
        !TextMatchesAt(GetText(), foundStart, text, matchCase, wholeWord)) {
        return false;
    }

    ReplaceSelection(replacement);
    return true;
}

std::size_t GtkNotepadApp::ReplaceAll(
    const std::wstring& text,
    const std::wstring& replacement,
    bool matchCase,
    bool wholeWord)
{
    if (text.empty()) {
        return 0;
    }

    findText_ = text;
    replaceText_ = replacement;
    findMatchCase_ = matchCase;
    findWholeWord_ = wholeWord;

    const std::wstring sourceText = GetText();
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

    SetText(replaced);
    SetSelection(0, 0);
    return replacementCount;
}

bool GtkNotepadApp::GoToLine(int lineNumber, std::wstring& errorMessage)
{
    const int maxLine = MaxLine();
    if (lineNumber < 1 || lineNumber > maxLine) {
        errorMessage = L"Line number must be between 1 and ";
        errorMessage += std::to_wstring(maxLine);
        errorMessage += L".";
        return false;
    }

    const std::wstring text = GetText();
    int currentLine = 1;
    std::size_t offset = 0;
    while (currentLine < lineNumber && offset < text.size()) {
        if (text[offset] == L'\n') {
            ++currentLine;
        }
        ++offset;
    }

    SetSelection(offset, offset);
    return true;
}

void GtkNotepadApp::InsertTimeDate()
{
    ReplaceSelection(BuildTimeDateText());
}

void GtkNotepadApp::SetWordWrap(bool enabled)
{
    wordWrap_ = enabled;
    if (textView_ != nullptr) {
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textView_), enabled ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    }
}

bool GtkNotepadApp::GetWordWrap() const
{
    return wordWrap_;
}

void GtkNotepadApp::SetStatusBarVisible(bool visible)
{
    statusBarVisible_ = visible;
    if (statusBar_ != nullptr) {
        gtk_widget_set_visible(statusBar_, visible);
    }

    if (window_ != nullptr) {
        GAction* action = g_action_map_lookup_action(G_ACTION_MAP(window_), "status-bar");
        if (G_IS_SIMPLE_ACTION(action)) {
            g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(statusBarVisible_));
        }
    }

    if (menuBar_ != nullptr && GTK_IS_POPOVER_MENU_BAR(menuBar_)) {
        GMenuModel* model = gtk_popover_menu_bar_get_menu_model(GTK_POPOVER_MENU_BAR(menuBar_));
        if (model != nullptr) {
            g_object_ref(model);
            gtk_popover_menu_bar_set_menu_model(GTK_POPOVER_MENU_BAR(menuBar_), model);
            g_object_unref(model);
        }
    }
}

bool GtkNotepadApp::GetStatusBarVisible() const
{
    return statusBarVisible_;
}

bool GtkNotepadApp::SetFont(const std::wstring& fontDescription, std::wstring& errorMessage)
{
    const std::string utf8Description = Utf8FromWide(fontDescription);
    if (utf8Description.empty()) {
        errorMessage = L"Font description cannot be empty.";
        return false;
    }

    PangoFontDescription* description = pango_font_description_from_string(utf8Description.c_str());
    if (description == nullptr || pango_font_description_get_family(description) == nullptr) {
        if (description != nullptr) {
            pango_font_description_free(description);
        }
        errorMessage = L"Font description is invalid.";
        return false;
    }

    char* normalized = pango_font_description_to_string(description);
    fontDescription_ = normalized != nullptr ? WideFromUtf8(normalized) : fontDescription;
    if (normalized != nullptr) {
        g_free(normalized);
    }
    pango_font_description_free(description);

    ApplyFont();
    UpdateStatus();
    return true;
}

std::wstring GtkNotepadApp::GetFont() const
{
    return fontDescription_;
}

bool GtkNotepadApp::SetPageMargins(const AutomationPageMargins& margins, std::wstring& errorMessage)
{
    if (margins.left < 0 || margins.top < 0 || margins.right < 0 || margins.bottom < 0) {
        errorMessage = L"Page margins must be non-negative.";
        return false;
    }

    pageMargins_ = margins;
    GtkPageSetup* pageSetup = EnsurePageSetup();
    gtk_page_setup_set_left_margin(pageSetup, static_cast<double>(pageMargins_.left) / 1000.0, GTK_UNIT_INCH);
    gtk_page_setup_set_top_margin(pageSetup, static_cast<double>(pageMargins_.top) / 1000.0, GTK_UNIT_INCH);
    gtk_page_setup_set_right_margin(pageSetup, static_cast<double>(pageMargins_.right) / 1000.0, GTK_UNIT_INCH);
    gtk_page_setup_set_bottom_margin(pageSetup, static_cast<double>(pageMargins_.bottom) / 1000.0, GTK_UNIT_INCH);
    return true;
}

GtkNotepadApp::AutomationPageMargins GtkNotepadApp::GetPageMargins() const
{
    return pageMargins_;
}

bool GtkNotepadApp::PrintToTestSink(const std::wstring& path, std::wstring& errorMessage) const
{
    const std::wstring sinkText = BuildPrintSinkText(L"linux", fontDescription_, pageMargins_, GetText());
    return WriteUtf8TextFile(path, sinkText, errorMessage);
}

classic_notepad::SpellCapability GtkNotepadApp::SpellCheckCapability() const
{
#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    return spelling_ == nullptr ? classic_notepad::SpellCapability::MissingBackend : spelling_->Capability();
#else
    return classic_notepad::SpellCapability::DisabledByBuild;
#endif
}

std::wstring GtkNotepadApp::SpellCheckLanguage() const
{
#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    return spelling_ == nullptr ? std::wstring() : WideFromUtf8(spelling_->LanguageCode());
#else
    return {};
#endif
}

bool GtkNotepadApp::SpellCheckAvailable() const
{
    return SpellCheckCapability() == classic_notepad::SpellCapability::Available;
}

std::vector<GtkNotepadApp::AutomationSpellingIssue> GtkNotepadApp::CheckSpelling(const std::wstring& text) const
{
    std::vector<AutomationSpellingIssue> issues;
#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    if (spelling_ == nullptr || !SpellCheckAvailable()) {
        return issues;
    }

    for (const classic_notepad::SpellIssue& issue : spelling_->CheckText(text)) {
        issues.push_back({ issue.startUtf16, issue.lengthUtf16, {}, L"spell" });
    }
#else
    (void)text;
#endif
    return issues;
}

std::vector<std::wstring> GtkNotepadApp::SuggestSpelling(const std::wstring& word, std::size_t limit) const
{
#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    if (spelling_ != nullptr && SpellCheckAvailable()) {
        return spelling_->Suggest(word, limit);
    }
#else
    (void)word;
    (void)limit;
#endif
    return {};
}

bool GtkNotepadApp::IgnoreSpelling(const std::wstring& word, std::wstring& errorMessage)
{
#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    if (spelling_ != nullptr && spelling_->IgnoreOnce(word)) {
        return true;
    }
#else
    (void)word;
#endif
    errorMessage = L"Spell checking is unavailable.";
    return false;
}

bool GtkNotepadApp::AddSpelling(const std::wstring& word, bool dryRun, std::wstring& errorMessage)
{
#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    if (spelling_ != nullptr && spelling_->AddToDictionary(word, dryRun)) {
        return true;
    }
#else
    (void)word;
    (void)dryRun;
#endif
    errorMessage = L"Spell checking is unavailable.";
    return false;
}

void GtkNotepadApp::OnBufferChanged()
{
    if (suppressChange_) {
        return;
    }

    document_.SetModified(true);
#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    if (spelling_ != nullptr) {
        spelling_->InvalidateAll();
    }
#endif
    UpdateTitle();
    UpdateStatus();
}

void GtkNotepadApp::OnCursorMoved()
{
    UpdateStatus();
}

void GtkNotepadApp::OnWindowDestroyed()
{
#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    spelling_.reset();
#endif
    window_ = nullptr;
    textView_ = nullptr;
    buffer_ = nullptr;
    statusBar_ = nullptr;
    statusLabel_ = nullptr;

    if (application_ != nullptr) {
        g_application_quit(G_APPLICATION(application_));
    }
}

std::wstring GtkNotepadApp::BuildTimeDateText() const
{
    std::time_t now = std::time(nullptr);
    std::tm localTime {};
#if defined(_WIN32)
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif

    char buffer[128] {};
    if (std::strftime(buffer, sizeof(buffer), "%H:%M %x", &localTime) == 0U) {
        return L"";
    }

    return WideFromUtf8(buffer);
}

int GtkNotepadApp::CurrentLine() const
{
    if (buffer_ == nullptr) {
        return 1;
    }

    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(buffer_, &cursor, gtk_text_buffer_get_insert(buffer_));
    return gtk_text_iter_get_line(&cursor) + 1;
}

int GtkNotepadApp::MaxLine() const
{
    const std::wstring text = GetText();
    int lineCount = 1;
    for (wchar_t character : text) {
        if (character == L'\n') {
            ++lineCount;
        }
    }

    return std::max(1, lineCount);
}

GtkPageSetup* GtkNotepadApp::EnsurePageSetup()
{
    if (pageSetup_ == nullptr) {
        pageSetup_ = gtk_page_setup_new();
    }

    gtk_page_setup_set_left_margin(pageSetup_, static_cast<double>(pageMargins_.left) / 1000.0, GTK_UNIT_INCH);
    gtk_page_setup_set_top_margin(pageSetup_, static_cast<double>(pageMargins_.top) / 1000.0, GTK_UNIT_INCH);
    gtk_page_setup_set_right_margin(pageSetup_, static_cast<double>(pageMargins_.right) / 1000.0, GTK_UNIT_INCH);
    gtk_page_setup_set_bottom_margin(pageSetup_, static_cast<double>(pageMargins_.bottom) / 1000.0, GTK_UNIT_INCH);
    return pageSetup_;
}

GtkPrintSettings* GtkNotepadApp::EnsurePrintSettings()
{
    if (printSettings_ == nullptr) {
        printSettings_ = gtk_print_settings_new();
    }

    return printSettings_;
}

void GtkNotepadApp::StorePageSetup(GtkPageSetup* pageSetup)
{
    if (pageSetup == nullptr) {
        return;
    }

    pageMargins_.left = static_cast<int>(std::lround(gtk_page_setup_get_left_margin(pageSetup, GTK_UNIT_INCH) * 1000.0));
    pageMargins_.top = static_cast<int>(std::lround(gtk_page_setup_get_top_margin(pageSetup, GTK_UNIT_INCH) * 1000.0));
    pageMargins_.right = static_cast<int>(std::lround(gtk_page_setup_get_right_margin(pageSetup, GTK_UNIT_INCH) * 1000.0));
    pageMargins_.bottom = static_cast<int>(std::lround(gtk_page_setup_get_bottom_margin(pageSetup, GTK_UNIT_INCH) * 1000.0));
}

void GtkNotepadApp::ApplyFont()
{
    if (textView_ == nullptr) {
        return;
    }

    const std::string utf8Description = Utf8FromWide(fontDescription_);
    PangoFontDescription* description = pango_font_description_from_string(utf8Description.c_str());
    if (description == nullptr) {
        return;
    }

    const char* family = pango_font_description_get_family(description);
    if (family == nullptr || family[0] == '\0') {
        pango_font_description_free(description);
        return;
    }

    std::string css = ".classic-notepad-editor { font-family: \"";
    css += EscapeCssString(family);
    css += "\"; font-size: ";
    css += CssSizeFromPangoDescription(description);
    css += "; }";
    pango_font_description_free(description);

    if (fontProvider_ == nullptr) {
        fontProvider_ = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            gtk_widget_get_display(textView_),
            GTK_STYLE_PROVIDER(fontProvider_),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(fontProvider_, css.c_str());
#else
    gtk_css_provider_load_from_data(fontProvider_, css.c_str(), static_cast<gssize>(css.size()));
#endif
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(textView_), FALSE);
}

void GtkNotepadApp::InstallContextMenu()
{
    if (textView_ == nullptr) {
        return;
    }

    GMenu* menu = g_menu_new();
    GMenuModel* spellingMenuModel = nullptr;
#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    if (spelling_ != nullptr) {
        spellingMenuModel = spelling_->ContextMenuModel();
    }
#endif
    if (spellingMenuModel != nullptr) {
        g_menu_append_section(menu, nullptr, spellingMenuModel);
    }

    GMenu* undoSection = g_menu_new();
    GMenu* clipboardSection = g_menu_new();
    GMenu* selectSection = g_menu_new();

    g_menu_append(undoSection, "Undo", "win.undo");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(undoSection));

    g_menu_append(clipboardSection, "Cut", "win.cut");
    g_menu_append(clipboardSection, "Copy", "win.copy");
    g_menu_append(clipboardSection, "Paste", "win.paste");
    g_menu_append(clipboardSection, "Delete", "win.delete");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(clipboardSection));

    g_menu_append(selectSection, "Select All", "win.select-all");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(selectSection));

    gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(textView_), G_MENU_MODEL(menu));
    g_object_unref(selectSection);
    g_object_unref(clipboardSection);
    g_object_unref(undoSection);
    g_object_unref(menu);
}

void GtkNotepadApp::BuildWindow(GtkApplication* application)
{
    window_ = gtk_application_window_new(application);
    gtk_window_set_default_size(GTK_WINDOW(window_), 800, 500);
    g_signal_connect(window_, "close-request", G_CALLBACK(OnCloseRequest), this);
    g_signal_connect(window_, "destroy", G_CALLBACK(OnWindowDestroy), this);

    InstallAppActions(*this);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window_), root);

    menuBar_ = CreateMenuBar();
    gtk_box_append(GTK_BOX(root), menuBar_);

    GtkWidget* scrolledWindow = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolledWindow, TRUE);
    gtk_widget_set_hexpand(scrolledWindow, TRUE);
    gtk_box_append(GTK_BOX(root), scrolledWindow);

#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    spelling_ = std::make_unique<GtkSpellingService>();
    textView_ = spelling_->CreatePlainTextView();
#else
    textView_ = gtk_text_view_new();
#endif
    gtk_widget_add_css_class(textView_, "classic-notepad-editor");
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(textView_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textView_), wordWrap_ ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolledWindow), textView_);

    buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textView_));
    gtk_text_buffer_set_enable_undo(buffer_, TRUE);
    gtk_text_buffer_set_max_undo_levels(buffer_, 100);
    g_signal_connect(buffer_, "changed", G_CALLBACK(OnBufferChangedSignal), this);
    g_signal_connect(buffer_, "notify::cursor-position", G_CALLBACK(OnCursorMovedSignal), this);
#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    if (spelling_ != nullptr) {
        spelling_->Attach(buffer_);
    }
#endif
    InstallContextMenu();
    ApplyFont();

    statusBar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_visible(statusBar_, statusBarVisible_);
    gtk_box_append(GTK_BOX(root), statusBar_);

    statusLabel_ = gtk_label_new("");
    gtk_widget_set_halign(statusLabel_, GTK_ALIGN_START);
    gtk_widget_set_margin_start(statusLabel_, 8);
    gtk_widget_set_margin_end(statusLabel_, 8);
    gtk_widget_set_margin_top(statusLabel_, 4);
    gtk_widget_set_margin_bottom(statusLabel_, 4);
    gtk_box_append(GTK_BOX(statusBar_), statusLabel_);
}

void GtkNotepadApp::SetBufferText(const std::wstring& text, bool markModified)
{
    if (buffer_ == nullptr) {
        return;
    }

    const std::string utf8Text = Utf8FromWide(text);
    suppressChange_ = true;
    gtk_text_buffer_begin_irreversible_action(buffer_);
    gtk_text_buffer_set_text(buffer_, utf8Text.c_str(), static_cast<int>(utf8Text.size()));
    gtk_text_buffer_end_irreversible_action(buffer_);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
    suppressChange_ = false;

    document_.SetModified(markModified);
#if CLASSIC_NOTEPAD_HAS_LIBSPELLING
    if (spelling_ != nullptr) {
        spelling_->InvalidateAll();
    }
#endif
}

void GtkNotepadApp::SetInitialTitleAndStatus()
{
    document_.ResetUntitled();
    SetBufferText(L"", false);
    UpdateTitle();
    UpdateStatus();
}

bool GtkNotepadApp::MissingFilePath(const std::wstring& path) const
{
    const std::string utf8Path = Utf8FromWide(path);
    return !utf8Path.empty() && !g_file_test(utf8Path.c_str(), G_FILE_TEST_EXISTS);
}

std::wstring GtkNotepadApp::SuggestedSaveName() const
{
    return document_.HasPath() ? document_.DisplayName() : L"Untitled.txt";
}

} // namespace classic_notepad::linux_ui
