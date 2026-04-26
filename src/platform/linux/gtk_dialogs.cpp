#include "gtk_dialogs.h"

#include "gtk_app.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace classic_notepad::linux_ui {
namespace {

struct MessageLoopState {
    GMainLoop* loop = nullptr;
    int response = GTK_RESPONSE_CANCEL;
    bool responded = false;
};

struct FileDialogState {
    GMainLoop* loop = nullptr;
    std::wstring path;
    bool save = false;
};

struct FindDialogState {
    GMainLoop* loop = nullptr;
    FindDialogResult result;
    GtkWidget* findEntry = nullptr;
    GtkWidget* matchCaseCheck = nullptr;
    GtkWidget* wholeWordCheck = nullptr;
    GtkWidget* searchDownCheck = nullptr;
};

struct ReplaceDialogState {
    GMainLoop* loop = nullptr;
    ReplaceDialogResult result;
    GtkWidget* findEntry = nullptr;
    GtkWidget* replaceEntry = nullptr;
    GtkWidget* matchCaseCheck = nullptr;
    GtkWidget* wholeWordCheck = nullptr;
    GtkWidget* searchDownCheck = nullptr;
};

struct GoToDialogState {
    GMainLoop* loop = nullptr;
    GtkWidget* entry = nullptr;
    GtkWidget* errorLabel = nullptr;
    int maxLine = 1;
    int selectedLine = 1;
    bool accepted = false;
};

struct FontDialogState {
    GMainLoop* loop = nullptr;
    std::wstring description;
};

void ButtonClicked(GtkButton* button, gpointer userData)
{
    auto* state = static_cast<MessageLoopState*>(userData);
    state->response = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "classic-notepad-response"));
    state->responded = true;

    GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(button));
    if (root != nullptr && GTK_IS_WINDOW(root)) {
        gtk_window_destroy(GTK_WINDOW(root));
    }
}

void DialogDestroyed(GtkWidget*, gpointer userData)
{
    auto* state = static_cast<MessageLoopState*>(userData);
    if (!state->responded) {
        state->response = GTK_RESPONSE_CANCEL;
    }

    if (state->loop != nullptr && g_main_loop_is_running(state->loop)) {
        g_main_loop_quit(state->loop);
    }
}

GtkWidget* AddButton(GtkWidget* row, const char* label, int response, MessageLoopState& state)
{
    GtkWidget* button = gtk_button_new_with_label(label);
    g_object_set_data(G_OBJECT(button), "classic-notepad-response", GINT_TO_POINTER(response));
    g_signal_connect(button, "clicked", G_CALLBACK(ButtonClicked), &state);
    gtk_box_append(GTK_BOX(row), button);
    return button;
}

GtkWidget* AddDialogButton(GtkWidget* row, const char* label)
{
    GtkWidget* button = gtk_button_new_with_label(label);
    gtk_box_append(GTK_BOX(row), button);
    return button;
}

GtkWidget* AddLabeledEntry(GtkWidget* content, const char* labelText, const std::wstring& initialText)
{
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(content), row);

    GtkWidget* label = gtk_label_new(labelText);
    gtk_widget_set_size_request(label, 96, -1);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_box_append(GTK_BOX(row), label);

    GtkWidget* entry = gtk_entry_new();
    gtk_widget_set_hexpand(entry, TRUE);
    const std::string utf8InitialText = Utf8FromWide(initialText);
    gtk_editable_set_text(GTK_EDITABLE(entry), utf8InitialText.c_str());
    gtk_box_append(GTK_BOX(row), entry);
    return entry;
}

void ConfigureDialogWindow(GtkWidget* dialog, GtkWindow* parent, const char* title, int width)
{
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), width, -1);
    if (parent != nullptr) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    }
}

GtkWidget* CreateDialogContent(GtkWidget* dialog)
{
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), content);
    return content;
}

void ReadFindOptions(FindDialogState& state)
{
    state.result.accepted = true;
    state.result.text = WideFromUtf8(gtk_editable_get_text(GTK_EDITABLE(state.findEntry)));
    state.result.matchCase = gtk_check_button_get_active(GTK_CHECK_BUTTON(state.matchCaseCheck)) != FALSE;
    state.result.wholeWord = gtk_check_button_get_active(GTK_CHECK_BUTTON(state.wholeWordCheck)) != FALSE;
    state.result.searchDown = gtk_check_button_get_active(GTK_CHECK_BUTTON(state.searchDownCheck)) != FALSE;
}

void FindButtonClicked(GtkButton* button, gpointer userData)
{
    auto* state = static_cast<FindDialogState*>(userData);
    ReadFindOptions(*state);

    GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(button));
    if (root != nullptr && GTK_IS_WINDOW(root)) {
        gtk_window_destroy(GTK_WINDOW(root));
    }
}

void FindDialogDestroyed(GtkWidget*, gpointer userData)
{
    auto* state = static_cast<FindDialogState*>(userData);
    if (state->loop != nullptr && g_main_loop_is_running(state->loop)) {
        g_main_loop_quit(state->loop);
    }
}

void ReadReplaceOptions(ReplaceDialogState& state, ReplaceDialogAction action)
{
    state.result.action = action;
    state.result.text = WideFromUtf8(gtk_editable_get_text(GTK_EDITABLE(state.findEntry)));
    state.result.replacement = WideFromUtf8(gtk_editable_get_text(GTK_EDITABLE(state.replaceEntry)));
    state.result.matchCase = gtk_check_button_get_active(GTK_CHECK_BUTTON(state.matchCaseCheck)) != FALSE;
    state.result.wholeWord = gtk_check_button_get_active(GTK_CHECK_BUTTON(state.wholeWordCheck)) != FALSE;
    state.result.searchDown = gtk_check_button_get_active(GTK_CHECK_BUTTON(state.searchDownCheck)) != FALSE;
}

void ReplaceButtonClicked(GtkButton* button, gpointer userData)
{
    auto* state = static_cast<ReplaceDialogState*>(userData);
    const auto action = static_cast<ReplaceDialogAction>(
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "classic-notepad-replace-action")));
    ReadReplaceOptions(*state, action);

    GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(button));
    if (root != nullptr && GTK_IS_WINDOW(root)) {
        gtk_window_destroy(GTK_WINDOW(root));
    }
}

void ReplaceDialogDestroyed(GtkWidget*, gpointer userData)
{
    auto* state = static_cast<ReplaceDialogState*>(userData);
    if (state->loop != nullptr && g_main_loop_is_running(state->loop)) {
        g_main_loop_quit(state->loop);
    }
}

void CancelOwningDialog(GtkButton* button, gpointer)
{
    GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(button));
    if (root != nullptr && GTK_IS_WINDOW(root)) {
        gtk_window_destroy(GTK_WINDOW(root));
    }
}

void GoToOkClicked(GtkButton*, gpointer userData)
{
    auto* state = static_cast<GoToDialogState*>(userData);
    const std::wstring text = WideFromUtf8(gtk_editable_get_text(GTK_EDITABLE(state->entry)));
    wchar_t* end = nullptr;
    const long lineNumber = std::wcstol(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end != L'\0' || lineNumber < 1 || lineNumber > state->maxLine) {
        std::wstring message = L"Line number must be between 1 and ";
        message += std::to_wstring(state->maxLine);
        message += L".";
        const std::string utf8Message = Utf8FromWide(message);
        gtk_label_set_text(GTK_LABEL(state->errorLabel), utf8Message.c_str());
        gtk_editable_select_region(GTK_EDITABLE(state->entry), 0, -1);
        gtk_widget_grab_focus(state->entry);
        return;
    }

    state->selectedLine = static_cast<int>(lineNumber);
    state->accepted = true;

    GtkRoot* root = gtk_widget_get_root(state->entry);
    if (root != nullptr && GTK_IS_WINDOW(root)) {
        gtk_window_destroy(GTK_WINDOW(root));
    }
}

void GoToDialogDestroyed(GtkWidget*, gpointer userData)
{
    auto* state = static_cast<GoToDialogState*>(userData);
    if (state->loop != nullptr && g_main_loop_is_running(state->loop)) {
        g_main_loop_quit(state->loop);
    }
}

#if GTK_CHECK_VERSION(4, 10, 0)
void OnFontDialogReady(GObject* sourceObject, GAsyncResult* result, gpointer userData)
{
    auto* state = static_cast<FontDialogState*>(userData);
    GError* error = nullptr;
    PangoFontDescription* description = gtk_font_dialog_choose_font_finish(
        GTK_FONT_DIALOG(sourceObject),
        result,
        &error);
    if (description != nullptr) {
        char* text = pango_font_description_to_string(description);
        if (text != nullptr) {
            state->description = WideFromUtf8(text);
            g_free(text);
        }
        pango_font_description_free(description);
    }

    if (error != nullptr) {
        g_error_free(error);
    }

    if (state->loop != nullptr && g_main_loop_is_running(state->loop)) {
        g_main_loop_quit(state->loop);
    }
}
#endif

int RunMessageDialog(
    GtkWindow* parent,
    const char* title,
    const std::wstring& message,
    const char* firstLabel,
    int firstResponse,
    const char* secondLabel,
    int secondResponse,
    const char* thirdLabel,
    int thirdResponse)
{
    MessageLoopState state;
    state.loop = g_main_loop_new(nullptr, FALSE);

    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 140);
    if (parent != nullptr) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    }
    g_signal_connect(dialog, "destroy", G_CALLBACK(DialogDestroyed), &state);

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), content);

    const std::string utf8Message = Utf8FromWide(message);
    GtkWidget* label = gtk_label_new(utf8Message.c_str());
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_box_append(GTK_BOX(content), label);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(content), row);

    if (firstLabel != nullptr) {
        AddButton(row, firstLabel, firstResponse, state);
    }
    if (secondLabel != nullptr) {
        AddButton(row, secondLabel, secondResponse, state);
    }
    if (thirdLabel != nullptr) {
        AddButton(row, thirdLabel, thirdResponse, state);
    }

    gtk_window_present(GTK_WINDOW(dialog));
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    return state.response;
}

void SetSelectedPath(FileDialogState& state, GFile* file)
{
    if (file == nullptr) {
        return;
    }

    char* path = g_file_get_path(file);
    if (path != nullptr) {
        state.path = WideFromUtf8(path);
        g_free(path);
    }
}

#if GTK_CHECK_VERSION(4, 10, 0)
void OnFileDialogReady(GObject* sourceObject, GAsyncResult* result, gpointer userData)
{
    auto* state = static_cast<FileDialogState*>(userData);
    GError* error = nullptr;
    GtkFileDialog* dialog = GTK_FILE_DIALOG(sourceObject);
    GFile* file = state->save
        ? gtk_file_dialog_save_finish(dialog, result, &error)
        : gtk_file_dialog_open_finish(dialog, result, &error);

    SetSelectedPath(*state, file);

    if (file != nullptr) {
        g_object_unref(file);
    }

    if (error != nullptr) {
        g_error_free(error);
    }

    if (state->loop != nullptr && g_main_loop_is_running(state->loop)) {
        g_main_loop_quit(state->loop);
    }
}

std::wstring RunFileDialog(
    GtkWindow* parent,
    bool save,
    const char* title,
    const char* acceptLabel,
    const std::wstring& suggestedName)
{
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title);
    gtk_file_dialog_set_accept_label(dialog, acceptLabel);
    gtk_file_dialog_set_modal(dialog, TRUE);

    if (save && !suggestedName.empty()) {
        const std::string utf8Name = Utf8FromWide(suggestedName);
        gtk_file_dialog_set_initial_name(dialog, utf8Name.c_str());
    }

    FileDialogState state;
    state.loop = g_main_loop_new(nullptr, FALSE);
    state.save = save;

    if (save) {
        gtk_file_dialog_save(dialog, parent, nullptr, OnFileDialogReady, &state);
    } else {
        gtk_file_dialog_open(dialog, parent, nullptr, OnFileDialogReady, &state);
    }

    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    g_object_unref(dialog);
    return state.path;
}
#else
void OnFileChooserNativeResponse(GtkNativeDialog* dialog, int response, gpointer userData)
{
    auto* state = static_cast<FileDialogState*>(userData);
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile* file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        if (file != nullptr) {
            SetSelectedPath(*state, file);
            g_object_unref(file);
        }
    }

    if (state->loop != nullptr && g_main_loop_is_running(state->loop)) {
        g_main_loop_quit(state->loop);
    }
}

std::wstring RunFileDialog(
    GtkWindow* parent,
    bool save,
    const char* title,
    const char* acceptLabel,
    const std::wstring& suggestedName)
{
    const GtkFileChooserAction action = save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN;
    GtkFileChooserNative* dialog = gtk_file_chooser_native_new(
        title,
        parent,
        action,
        acceptLabel,
        "_Cancel");
    gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(dialog), TRUE);

    if (!suggestedName.empty() && action == GTK_FILE_CHOOSER_ACTION_SAVE) {
        const std::string utf8Name = Utf8FromWide(suggestedName);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), utf8Name.c_str());
    }

    FileDialogState state;
    state.loop = g_main_loop_new(nullptr, FALSE);
    state.save = save;
    g_signal_connect(dialog, "response", G_CALLBACK(OnFileChooserNativeResponse), &state);

    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    g_object_unref(dialog);
    return state.path;
}
#endif

} // namespace

std::wstring ShowOpenFileDialog(GtkWindow* parent)
{
    return RunFileDialog(parent, false, "Open", "_Open", {});
}

std::wstring ShowSaveFileDialog(GtkWindow* parent, const std::wstring& suggestedName)
{
    return RunFileDialog(parent, true, "Save As", "_Save", suggestedName);
}

DirtyPromptResult ShowDirtyPrompt(GtkWindow* parent, const std::wstring& displayName)
{
    std::wstring prompt = L"Do you want to save changes to ";
    prompt += displayName;
    prompt += L"?";

    const int response = RunMessageDialog(
        parent,
        "Classic Notepad",
        prompt,
        "Save",
        GTK_RESPONSE_YES,
        "Don't Save",
        GTK_RESPONSE_NO,
        "Cancel",
        GTK_RESPONSE_CANCEL);

    if (response == GTK_RESPONSE_YES) {
        return DirtyPromptResult::Save;
    }

    if (response == GTK_RESPONSE_NO) {
        return DirtyPromptResult::Discard;
    }

    return DirtyPromptResult::Cancel;
}

bool ConfirmCreateMissingFile(GtkWindow* parent, const std::wstring& path)
{
    std::wstring prompt = L"Cannot find the ";
    prompt += path;
    prompt += L" file.\n\nDo you want to create a new file?";

    return RunMessageDialog(
        parent,
        "Classic Notepad",
        prompt,
        "Yes",
        GTK_RESPONSE_YES,
        "No",
        GTK_RESPONSE_NO,
        nullptr,
        GTK_RESPONSE_CANCEL) == GTK_RESPONSE_YES;
}

FindDialogResult ShowFindDialog(
    GtkWindow* parent,
    const std::wstring& initialText,
    bool matchCase,
    bool wholeWord,
    bool searchDown)
{
    FindDialogState state;
    state.loop = g_main_loop_new(nullptr, FALSE);
    state.result.matchCase = matchCase;
    state.result.wholeWord = wholeWord;
    state.result.searchDown = searchDown;

    GtkWidget* dialog = gtk_window_new();
    ConfigureDialogWindow(dialog, parent, "Find", 420);
    g_signal_connect(dialog, "destroy", G_CALLBACK(FindDialogDestroyed), &state);

    GtkWidget* content = CreateDialogContent(dialog);
    state.findEntry = AddLabeledEntry(content, "Find what:", initialText);

    state.matchCaseCheck = gtk_check_button_new_with_label("Match case");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state.matchCaseCheck), matchCase);
    gtk_box_append(GTK_BOX(content), state.matchCaseCheck);

    state.wholeWordCheck = gtk_check_button_new_with_label("Whole word");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state.wholeWordCheck), wholeWord);
    gtk_box_append(GTK_BOX(content), state.wholeWordCheck);

    state.searchDownCheck = gtk_check_button_new_with_label("Search down");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state.searchDownCheck), searchDown);
    gtk_box_append(GTK_BOX(content), state.searchDownCheck);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(content), row);

    GtkWidget* findButton = AddDialogButton(row, "Find Next");
    g_signal_connect(findButton, "clicked", G_CALLBACK(FindButtonClicked), &state);
    GtkWidget* cancelButton = AddDialogButton(row, "Cancel");
    g_signal_connect(cancelButton, "clicked", G_CALLBACK(CancelOwningDialog), nullptr);

    gtk_widget_grab_focus(state.findEntry);
    gtk_window_present(GTK_WINDOW(dialog));
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    return state.result;
}

ReplaceDialogResult ShowReplaceDialog(
    GtkWindow* parent,
    const std::wstring& initialText,
    const std::wstring& initialReplacement,
    bool matchCase,
    bool wholeWord,
    bool searchDown)
{
    ReplaceDialogState state;
    state.loop = g_main_loop_new(nullptr, FALSE);
    state.result.matchCase = matchCase;
    state.result.wholeWord = wholeWord;
    state.result.searchDown = searchDown;

    GtkWidget* dialog = gtk_window_new();
    ConfigureDialogWindow(dialog, parent, "Replace", 460);
    g_signal_connect(dialog, "destroy", G_CALLBACK(ReplaceDialogDestroyed), &state);

    GtkWidget* content = CreateDialogContent(dialog);
    state.findEntry = AddLabeledEntry(content, "Find what:", initialText);
    state.replaceEntry = AddLabeledEntry(content, "Replace with:", initialReplacement);

    state.matchCaseCheck = gtk_check_button_new_with_label("Match case");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state.matchCaseCheck), matchCase);
    gtk_box_append(GTK_BOX(content), state.matchCaseCheck);

    state.wholeWordCheck = gtk_check_button_new_with_label("Whole word");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state.wholeWordCheck), wholeWord);
    gtk_box_append(GTK_BOX(content), state.wholeWordCheck);

    state.searchDownCheck = gtk_check_button_new_with_label("Search down");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state.searchDownCheck), searchDown);
    gtk_box_append(GTK_BOX(content), state.searchDownCheck);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(content), row);

    GtkWidget* replaceButton = AddDialogButton(row, "Replace");
    g_object_set_data(
        G_OBJECT(replaceButton),
        "classic-notepad-replace-action",
        GINT_TO_POINTER(static_cast<int>(ReplaceDialogAction::Replace)));
    g_signal_connect(replaceButton, "clicked", G_CALLBACK(ReplaceButtonClicked), &state);

    GtkWidget* replaceAllButton = AddDialogButton(row, "Replace All");
    g_object_set_data(
        G_OBJECT(replaceAllButton),
        "classic-notepad-replace-action",
        GINT_TO_POINTER(static_cast<int>(ReplaceDialogAction::ReplaceAll)));
    g_signal_connect(replaceAllButton, "clicked", G_CALLBACK(ReplaceButtonClicked), &state);

    GtkWidget* cancelButton = AddDialogButton(row, "Cancel");
    g_signal_connect(cancelButton, "clicked", G_CALLBACK(CancelOwningDialog), nullptr);

    gtk_widget_grab_focus(state.findEntry);
    gtk_window_present(GTK_WINDOW(dialog));
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    return state.result;
}

bool ShowGoToDialog(GtkWindow* parent, int currentLine, int maxLine, int& selectedLine)
{
    GoToDialogState state;
    state.loop = g_main_loop_new(nullptr, FALSE);
    state.maxLine = std::max(1, maxLine);
    state.selectedLine = std::max(1, std::min(currentLine, state.maxLine));

    GtkWidget* dialog = gtk_window_new();
    ConfigureDialogWindow(dialog, parent, "Go To Line", 320);
    g_signal_connect(dialog, "destroy", G_CALLBACK(GoToDialogDestroyed), &state);

    GtkWidget* content = CreateDialogContent(dialog);
    state.entry = AddLabeledEntry(content, "Line number:", std::to_wstring(state.selectedLine));
    state.errorLabel = gtk_label_new("");
    gtk_widget_add_css_class(state.errorLabel, "error");
    gtk_label_set_xalign(GTK_LABEL(state.errorLabel), 0.0F);
    gtk_box_append(GTK_BOX(content), state.errorLabel);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(content), row);

    GtkWidget* okButton = AddDialogButton(row, "OK");
    g_signal_connect(okButton, "clicked", G_CALLBACK(GoToOkClicked), &state);
    GtkWidget* cancelButton = AddDialogButton(row, "Cancel");
    g_signal_connect(cancelButton, "clicked", G_CALLBACK(CancelOwningDialog), nullptr);

    gtk_editable_select_region(GTK_EDITABLE(state.entry), 0, -1);
    gtk_widget_grab_focus(state.entry);
    gtk_window_present(GTK_WINDOW(dialog));
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);

    if (state.accepted) {
        selectedLine = state.selectedLine;
    }
    return state.accepted;
}

std::wstring ShowFontDialog(GtkWindow* parent, const std::wstring& currentFont)
{
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkFontDialog* dialog = gtk_font_dialog_new();
    gtk_font_dialog_set_title(dialog, "Font");
    gtk_font_dialog_set_modal(dialog, TRUE);

    const std::string utf8CurrentFont = Utf8FromWide(currentFont);
    PangoFontDescription* initialDescription = utf8CurrentFont.empty()
        ? nullptr
        : pango_font_description_from_string(utf8CurrentFont.c_str());

    FontDialogState state;
    state.loop = g_main_loop_new(nullptr, FALSE);
    gtk_font_dialog_choose_font(dialog, parent, initialDescription, nullptr, OnFontDialogReady, &state);

    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    if (initialDescription != nullptr) {
        pango_font_description_free(initialDescription);
    }
    g_object_unref(dialog);
    return state.description;
#else
    (void)parent;
    (void)currentFont;
    return {};
#endif
}

void ShowErrorDialog(GtkWindow* parent, const std::wstring& message)
{
    RunMessageDialog(
        parent,
        "Classic Notepad",
        message,
        "Close",
        GTK_RESPONSE_CLOSE,
        nullptr,
        GTK_RESPONSE_CANCEL,
        nullptr,
        GTK_RESPONSE_CANCEL);
}

} // namespace classic_notepad::linux_ui
