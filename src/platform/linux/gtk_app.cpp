#include "gtk_app.h"

#include "gtk_actions.h"
#include "gtk_automation.h"
#include "gtk_dialogs.h"
#include "text_metadata.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

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
    if (buffer_ == nullptr) {
        return;
    }

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
    document_.SetModified(true);
    UpdateTitle();
    UpdateStatus();
}

std::wstring GtkNotepadApp::GetText() const
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

    return classic_notepad::NormalizeLineEndingsForEditor(wideText);
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
    if (gtk_text_buffer_get_selection_bounds(buffer_, &start, &end)) {
        selection.start = static_cast<std::size_t>(gtk_text_iter_get_offset(&start));
        selection.end = static_cast<std::size_t>(gtk_text_iter_get_offset(&end));
        return selection;
    }

    gtk_text_buffer_get_iter_at_mark(buffer_, &start, gtk_text_buffer_get_insert(buffer_));
    selection.start = static_cast<std::size_t>(gtk_text_iter_get_offset(&start));
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
    gtk_text_buffer_get_iter_at_offset(buffer_, &start, static_cast<int>(ClampOffset(selectionStart)));
    gtk_text_buffer_get_iter_at_offset(buffer_, &end, static_cast<int>(ClampOffset(selectionEnd)));
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

void GtkNotepadApp::DeleteSelection()
{
    if (buffer_ == nullptr) {
        return;
    }

    GtkTextIter start;
    GtkTextIter end;
    if (gtk_text_buffer_get_selection_bounds(buffer_, &start, &end)) {
        gtk_text_buffer_delete(buffer_, &start, &end);
        document_.SetModified(true);
        UpdateTitle();
        UpdateStatus();
    }
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
}

bool GtkNotepadApp::GetStatusBarVisible() const
{
    return statusBarVisible_;
}

void GtkNotepadApp::OnBufferChanged()
{
    if (suppressChange_) {
        return;
    }

    document_.SetModified(true);
    UpdateTitle();
    UpdateStatus();
}

void GtkNotepadApp::OnCursorMoved()
{
    UpdateStatus();
}

void GtkNotepadApp::OnWindowDestroyed()
{
    window_ = nullptr;
    textView_ = nullptr;
    buffer_ = nullptr;
    statusBar_ = nullptr;
    statusLabel_ = nullptr;

    if (application_ != nullptr) {
        g_application_quit(G_APPLICATION(application_));
    }
}

void GtkNotepadApp::BuildWindow(GtkApplication* application)
{
    window_ = gtk_application_window_new(application);
    gtk_window_set_default_size(GTK_WINDOW(window_), 800, 500);
    g_signal_connect(window_, "close-request", G_CALLBACK(OnCloseRequest), this);
    g_signal_connect(window_, "destroy", G_CALLBACK(OnWindowDestroy), this);

    InstallFileActions(*this);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window_), root);

    GtkWidget* menuBar = CreateFileMenuBar();
    gtk_box_append(GTK_BOX(root), menuBar);

    GtkWidget* scrolledWindow = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolledWindow, TRUE);
    gtk_widget_set_hexpand(scrolledWindow, TRUE);
    gtk_box_append(GTK_BOX(root), scrolledWindow);

    textView_ = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(textView_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textView_), wordWrap_ ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolledWindow), textView_);

    buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textView_));
    g_signal_connect(buffer_, "changed", G_CALLBACK(OnBufferChangedSignal), this);
    g_signal_connect(buffer_, "notify::cursor-position", G_CALLBACK(OnCursorMovedSignal), this);

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
    gtk_text_buffer_set_text(buffer_, utf8Text.c_str(), static_cast<int>(utf8Text.size()));
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
    suppressChange_ = false;

    document_.SetModified(markModified);
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
