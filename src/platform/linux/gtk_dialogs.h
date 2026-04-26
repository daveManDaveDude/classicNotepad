#pragma once

#include <gtk/gtk.h>

#include <string>

namespace classic_notepad::linux_ui {

enum class DirtyPromptResult {
    Save,
    Discard,
    Cancel
};

std::wstring ShowOpenFileDialog(GtkWindow* parent);
std::wstring ShowSaveFileDialog(GtkWindow* parent, const std::wstring& suggestedName);
DirtyPromptResult ShowDirtyPrompt(GtkWindow* parent, const std::wstring& displayName);
bool ConfirmCreateMissingFile(GtkWindow* parent, const std::wstring& path);
void ShowErrorDialog(GtkWindow* parent, const std::wstring& message);

} // namespace classic_notepad::linux_ui
