#include "gtk_dialogs.h"

#include "gtk_app.h"

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
