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

const GActionEntry kFileActions[] = {
    {"new", OnNew, nullptr, nullptr, nullptr},
    {"open", OnOpen, nullptr, nullptr, nullptr},
    {"save", OnSave, nullptr, nullptr, nullptr},
    {"save-as", OnSaveAs, nullptr, nullptr, nullptr},
    {"exit", OnExit, nullptr, nullptr, nullptr},
};

void SetAccels(GtkApplication* application, const char* action, const char* first, const char* second = nullptr)
{
    const char* accels[] = {first, second, nullptr};
    gtk_application_set_accels_for_action(application, action, accels);
}

} // namespace

void InstallFileActions(GtkNotepadApp& app)
{
    g_action_map_add_action_entries(
        G_ACTION_MAP(app.Window()),
        kFileActions,
        G_N_ELEMENTS(kFileActions),
        &app);

    SetAccels(app.Application(), "win.new", "<Primary>n");
    SetAccels(app.Application(), "win.open", "<Primary>o");
    SetAccels(app.Application(), "win.save", "<Primary>s");
    SetAccels(app.Application(), "win.save-as", "<Primary><Shift>s");
    SetAccels(app.Application(), "win.exit", "<Primary>q", "<Alt>F4");
}

GtkWidget* CreateFileMenuBar()
{
    GMenu* bar = g_menu_new();
    GMenu* fileMenu = g_menu_new();
    GMenu* filePrimarySection = g_menu_new();
    GMenu* fileExitSection = g_menu_new();

    g_menu_append(filePrimarySection, "New", "win.new");
    g_menu_append(filePrimarySection, "Open...", "win.open");
    g_menu_append(filePrimarySection, "Save", "win.save");
    g_menu_append(filePrimarySection, "Save As...", "win.save-as");
    g_menu_append_section(fileMenu, nullptr, G_MENU_MODEL(filePrimarySection));

    g_menu_append(fileExitSection, "Exit", "win.exit");
    g_menu_append_section(fileMenu, nullptr, G_MENU_MODEL(fileExitSection));

    g_menu_append_submenu(bar, "File", G_MENU_MODEL(fileMenu));

    GtkWidget* menuBar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(bar));
    g_object_unref(fileExitSection);
    g_object_unref(filePrimarySection);
    g_object_unref(fileMenu);
    g_object_unref(bar);
    return menuBar;
}

} // namespace classic_notepad::linux_ui
