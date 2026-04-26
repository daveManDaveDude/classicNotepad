#include "gtk_actions.h"

#include "gtk_app.h"

namespace classic_notepad::linux_ui {
namespace {

void OnNew(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleNew();
}

void OnOpen(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleOpen();
}

void OnSave(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleSave();
}

void OnSaveAs(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleSaveAs();
}

void OnExit(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleExit();
}

void OnUndo(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleUndo();
}

void OnCut(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleCut();
}

void OnCopy(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleCopy();
}

void OnPaste(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandlePaste();
}

void OnDelete(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleDelete();
}

void OnFind(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleFind();
}

void OnFindNext(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleFindNext();
}

void OnReplace(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleReplace();
}

void OnGoTo(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleGoTo();
}

void OnSelectAll(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleSelectAll();
}

void OnTimeDate(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleTimeDate();
}

void OnWordWrap(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleToggleWordWrap();
}

void OnFont(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleChooseFont();
}

void OnStatusBar(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleToggleStatusBar();
}

const GActionEntry kActions[] = {
    {"new", OnNew, nullptr, nullptr, nullptr},
    {"open", OnOpen, nullptr, nullptr, nullptr},
    {"save", OnSave, nullptr, nullptr, nullptr},
    {"save-as", OnSaveAs, nullptr, nullptr, nullptr},
    {"exit", OnExit, nullptr, nullptr, nullptr},
    {"undo", OnUndo, nullptr, nullptr, nullptr},
    {"cut", OnCut, nullptr, nullptr, nullptr},
    {"copy", OnCopy, nullptr, nullptr, nullptr},
    {"paste", OnPaste, nullptr, nullptr, nullptr},
    {"delete", OnDelete, nullptr, nullptr, nullptr},
    {"find", OnFind, nullptr, nullptr, nullptr},
    {"find-next", OnFindNext, nullptr, nullptr, nullptr},
    {"replace", OnReplace, nullptr, nullptr, nullptr},
    {"go-to", OnGoTo, nullptr, nullptr, nullptr},
    {"select-all", OnSelectAll, nullptr, nullptr, nullptr},
    {"time-date", OnTimeDate, nullptr, nullptr, nullptr},
    {"word-wrap", OnWordWrap, nullptr, nullptr, nullptr},
    {"font", OnFont, nullptr, nullptr, nullptr},
    {"status-bar", OnStatusBar, nullptr, nullptr, nullptr},
};

void SetAccels(GtkApplication* application, const char* action, const char* first, const char* second = nullptr)
{
    const char* accels[] = {first, second, nullptr};
    gtk_application_set_accels_for_action(application, action, accels);
}

} // namespace

void InstallAppActions(GtkNotepadApp& app)
{
    g_action_map_add_action_entries(
        G_ACTION_MAP(app.Window()),
        kActions,
        G_N_ELEMENTS(kActions),
        &app);

    SetAccels(app.Application(), "win.new", "<Primary>n");
    SetAccels(app.Application(), "win.open", "<Primary>o");
    SetAccels(app.Application(), "win.save", "<Primary>s");
    SetAccels(app.Application(), "win.save-as", "<Primary><Shift>s");
    SetAccels(app.Application(), "win.exit", "<Primary>q", "<Alt>F4");
    SetAccels(app.Application(), "win.undo", "<Primary>z");
    SetAccels(app.Application(), "win.cut", "<Primary>x");
    SetAccels(app.Application(), "win.copy", "<Primary>c");
    SetAccels(app.Application(), "win.paste", "<Primary>v");
    SetAccels(app.Application(), "win.delete", "Delete");
    SetAccels(app.Application(), "win.find", "<Primary>f");
    SetAccels(app.Application(), "win.find-next", "F3");
    SetAccels(app.Application(), "win.replace", "<Primary>h");
    SetAccels(app.Application(), "win.go-to", "<Primary>g");
    SetAccels(app.Application(), "win.select-all", "<Primary>a");
    SetAccels(app.Application(), "win.time-date", "F5");
}

GtkWidget* CreateMenuBar()
{
    GMenu* bar = g_menu_new();
    GMenu* fileMenu = g_menu_new();
    GMenu* filePrimarySection = g_menu_new();
    GMenu* fileExitSection = g_menu_new();
    GMenu* editMenu = g_menu_new();
    GMenu* editUndoSection = g_menu_new();
    GMenu* editClipboardSection = g_menu_new();
    GMenu* editFindSection = g_menu_new();
    GMenu* editOtherSection = g_menu_new();
    GMenu* formatMenu = g_menu_new();
    GMenu* viewMenu = g_menu_new();

    g_menu_append(filePrimarySection, "New", "win.new");
    g_menu_append(filePrimarySection, "Open...", "win.open");
    g_menu_append(filePrimarySection, "Save", "win.save");
    g_menu_append(filePrimarySection, "Save As...", "win.save-as");
    g_menu_append_section(fileMenu, nullptr, G_MENU_MODEL(filePrimarySection));

    g_menu_append(fileExitSection, "Exit", "win.exit");
    g_menu_append_section(fileMenu, nullptr, G_MENU_MODEL(fileExitSection));

    g_menu_append(editUndoSection, "Undo", "win.undo");
    g_menu_append_section(editMenu, nullptr, G_MENU_MODEL(editUndoSection));

    g_menu_append(editClipboardSection, "Cut", "win.cut");
    g_menu_append(editClipboardSection, "Copy", "win.copy");
    g_menu_append(editClipboardSection, "Paste", "win.paste");
    g_menu_append(editClipboardSection, "Delete", "win.delete");
    g_menu_append_section(editMenu, nullptr, G_MENU_MODEL(editClipboardSection));

    g_menu_append(editFindSection, "Find...", "win.find");
    g_menu_append(editFindSection, "Find Next", "win.find-next");
    g_menu_append(editFindSection, "Replace...", "win.replace");
    g_menu_append(editFindSection, "Go To...", "win.go-to");
    g_menu_append_section(editMenu, nullptr, G_MENU_MODEL(editFindSection));

    g_menu_append(editOtherSection, "Select All", "win.select-all");
    g_menu_append(editOtherSection, "Time/Date", "win.time-date");
    g_menu_append_section(editMenu, nullptr, G_MENU_MODEL(editOtherSection));

    g_menu_append(formatMenu, "Word Wrap", "win.word-wrap");
    g_menu_append(formatMenu, "Font...", "win.font");

    g_menu_append(viewMenu, "Status Bar", "win.status-bar");

    g_menu_append_submenu(bar, "File", G_MENU_MODEL(fileMenu));
    g_menu_append_submenu(bar, "Edit", G_MENU_MODEL(editMenu));
    g_menu_append_submenu(bar, "Format", G_MENU_MODEL(formatMenu));
    g_menu_append_submenu(bar, "View", G_MENU_MODEL(viewMenu));

    GtkWidget* menuBar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(bar));
    g_object_unref(viewMenu);
    g_object_unref(formatMenu);
    g_object_unref(editOtherSection);
    g_object_unref(editFindSection);
    g_object_unref(editClipboardSection);
    g_object_unref(editUndoSection);
    g_object_unref(editMenu);
    g_object_unref(fileExitSection);
    g_object_unref(filePrimarySection);
    g_object_unref(fileMenu);
    g_object_unref(bar);
    return menuBar;
}

} // namespace classic_notepad::linux_ui
