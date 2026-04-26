#pragma once

#include <gtk/gtk.h>

#include <string>

namespace classic_notepad::linux_ui {

enum class DirtyPromptResult {
    Save,
    Discard,
    Cancel
};

struct FindDialogResult {
    bool accepted = false;
    std::wstring text;
    bool matchCase = false;
    bool wholeWord = false;
    bool searchDown = true;
};

enum class ReplaceDialogAction {
    Cancel,
    Replace,
    ReplaceAll
};

struct ReplaceDialogResult {
    ReplaceDialogAction action = ReplaceDialogAction::Cancel;
    std::wstring text;
    std::wstring replacement;
    bool matchCase = false;
    bool wholeWord = false;
    bool searchDown = true;
};

std::wstring ShowOpenFileDialog(GtkWindow* parent);
std::wstring ShowSaveFileDialog(GtkWindow* parent, const std::wstring& suggestedName);
DirtyPromptResult ShowDirtyPrompt(GtkWindow* parent, const std::wstring& displayName);
bool ConfirmCreateMissingFile(GtkWindow* parent, const std::wstring& path);
FindDialogResult ShowFindDialog(
    GtkWindow* parent,
    const std::wstring& initialText,
    bool matchCase,
    bool wholeWord,
    bool searchDown);
ReplaceDialogResult ShowReplaceDialog(
    GtkWindow* parent,
    const std::wstring& initialText,
    const std::wstring& initialReplacement,
    bool matchCase,
    bool wholeWord,
    bool searchDown);
bool ShowGoToDialog(GtkWindow* parent, int currentLine, int maxLine, int& selectedLine);
std::wstring ShowFontDialog(GtkWindow* parent, const std::wstring& currentFont);
void ShowErrorDialog(GtkWindow* parent, const std::wstring& message);

} // namespace classic_notepad::linux_ui
