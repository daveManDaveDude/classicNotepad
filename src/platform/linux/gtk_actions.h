#pragma once

#include <gtk/gtk.h>

namespace classic_notepad::linux_ui {

class GtkNotepadApp;

void InstallFileActions(GtkNotepadApp& app);
GtkWidget* CreateFileMenuBar();

} // namespace classic_notepad::linux_ui
