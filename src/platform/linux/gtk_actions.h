#pragma once

#include <gtk/gtk.h>

namespace classic_notepad::linux_ui {

class GtkNotepadApp;

void InstallAppActions(GtkNotepadApp& app);
GtkWidget* CreateMenuBar();

} // namespace classic_notepad::linux_ui
