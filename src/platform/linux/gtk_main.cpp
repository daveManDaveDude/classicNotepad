#include "document.h"
#include "text_metadata.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct AppState {
    Document document;
    GtkWidget* window = nullptr;
    GtkTextBuffer* buffer = nullptr;
    GtkWidget* statusLabel = nullptr;
    std::wstring initialPath;
    bool suppressChange = false;
};

std::wstring WideFromUtf8(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return {};
    }

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text);
    classic_notepad::DecodeTextResult decoded;
    std::wstring error;
    if (!classic_notepad::DecodeTextBytes(
            std::vector<std::uint8_t>(bytes, bytes + std::strlen(text)),
            decoded,
            error)) {
        return {};
    }

    return decoded.text;
}

std::string Utf8FromWide(const std::wstring& text)
{
    std::vector<std::uint8_t> bytes;
    std::wstring error;
    if (!classic_notepad::EncodeTextBytes(text, classic_notepad::TextEncoding::Utf8NoBom, bytes, error)) {
        return {};
    }

    return std::string(bytes.begin(), bytes.end());
}

std::wstring GetBufferText(GtkTextBuffer* buffer)
{
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);

    char* text = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
    std::wstring wideText = WideFromUtf8(text);
    g_free(text);

    return classic_notepad::NormalizeLineEndingsForEditor(wideText);
}

void SetWindowTitle(AppState& state)
{
    std::wstring title;
    if (state.document.IsModified()) {
        title += L"*";
    }

    title += state.document.DisplayName();
    title += L" - Classic Notepad";

    const std::string utf8Title = Utf8FromWide(title);
    gtk_window_set_title(GTK_WINDOW(state.window), utf8Title.c_str());
}

void UpdateStatus(AppState& state)
{
    if (state.buffer == nullptr || state.statusLabel == nullptr) {
        return;
    }

    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(state.buffer, &cursor, gtk_text_buffer_get_insert(state.buffer));

    const int line = gtk_text_iter_get_line(&cursor) + 1;
    const int column = gtk_text_iter_get_line_offset(&cursor) + 1;
    const std::wstring text = GetBufferText(state.buffer);

    std::wstring status = L"Ln ";
    status += classic_notepad::FormatNumberWithSeparators(static_cast<std::size_t>(std::max(1, line)));
    status += L", Col ";
    status += classic_notepad::FormatNumberWithSeparators(static_cast<std::size_t>(std::max(1, column)));
    status += L" | ";
    status += classic_notepad::FormatCharacterCount(classic_notepad::CountStatusCharacters(text));
    status += L" | ";
    status += classic_notepad::FormatLineEnding(state.document.LineEnding());
    status += L" | ";
    status += classic_notepad::FormatEncoding(state.document.Encoding());

    const std::string utf8Status = Utf8FromWide(status);
    gtk_label_set_text(GTK_LABEL(state.statusLabel), utf8Status.c_str());
}

void MarkChanged(GtkTextBuffer*, AppState* state)
{
    if (state->suppressChange) {
        return;
    }

    state->document.SetModified(true);
    SetWindowTitle(*state);
    UpdateStatus(*state);
}

void UpdateStatusOnCursorMove(GObject*, GParamSpec*, AppState* state)
{
    UpdateStatus(*state);
}

void ShowError(GtkWindow* parent, const std::wstring& message)
{
    const std::string utf8Message = Utf8FromWide(message);

    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Classic Notepad");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 120);

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), content);

    GtkWidget* label = gtk_label_new(utf8Message.c_str());
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_box_append(GTK_BOX(content), label);

    GtkWidget* button = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(button, GTK_ALIGN_END);
    g_signal_connect_swapped(button, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_box_append(GTK_BOX(content), button);

    gtk_window_present(GTK_WINDOW(dialog));
}

void LoadInitialFile(AppState& state)
{
    if (state.initialPath.empty()) {
        state.document.ResetUntitled();
        SetWindowTitle(state);
        UpdateStatus(state);
        return;
    }

    std::wstring editorText;
    std::wstring error;
    if (!state.document.Load(state.initialPath, editorText, error)) {
        state.document.ResetUntitled();
        SetWindowTitle(state);
        UpdateStatus(state);
        ShowError(GTK_WINDOW(state.window), error);
        return;
    }

    const std::string utf8Text = Utf8FromWide(editorText);
    state.suppressChange = true;
    gtk_text_buffer_set_text(state.buffer, utf8Text.c_str(), static_cast<int>(utf8Text.size()));
    state.suppressChange = false;

    state.document.SetModified(false);
    SetWindowTitle(state);
    UpdateStatus(state);
}

void Activate(GtkApplication* application, gpointer userData)
{
    auto* state = static_cast<AppState*>(userData);

    state->window = gtk_application_window_new(application);
    gtk_window_set_default_size(GTK_WINDOW(state->window), 800, 500);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(state->window), root);

    GtkWidget* scrolledWindow = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolledWindow, TRUE);
    gtk_widget_set_hexpand(scrolledWindow, TRUE);
    gtk_box_append(GTK_BOX(root), scrolledWindow);

    GtkWidget* textView = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(textView), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textView), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolledWindow), textView);

    state->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textView));
    g_signal_connect(state->buffer, "changed", G_CALLBACK(MarkChanged), state);
    g_signal_connect(state->buffer, "notify::cursor-position", G_CALLBACK(UpdateStatusOnCursorMove), state);

    state->statusLabel = gtk_label_new("");
    gtk_widget_set_halign(state->statusLabel, GTK_ALIGN_START);
    gtk_widget_set_margin_start(state->statusLabel, 8);
    gtk_widget_set_margin_end(state->statusLabel, 8);
    gtk_widget_set_margin_top(state->statusLabel, 4);
    gtk_widget_set_margin_bottom(state->statusLabel, 4);
    gtk_box_append(GTK_BOX(root), state->statusLabel);

    LoadInitialFile(*state);
    gtk_window_present(GTK_WINDOW(state->window));
}

} // namespace

int main(int argc, char** argv)
{
    AppState state;
    if (argc > 1) {
        state.initialPath = WideFromUtf8(argv[1]);
    }

    GtkApplication* application = gtk_application_new("dev.classicnotepad.gtk", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(Activate), &state);

    const int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
