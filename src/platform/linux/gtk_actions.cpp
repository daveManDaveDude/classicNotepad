#include "gtk_actions.h"

#include "gtk_app.h"

#include <string>

namespace classic_notepad::linux_ui {
namespace {

gboolean RestoreClassicArrowCursorAfterAction(gpointer userData);
void QueueCursorRestore(GtkNotepadApp* app);

template <typename Action>
void RunMenuAction(gpointer userData, Action action)
{
    auto* app = static_cast<GtkNotepadApp*>(userData);
    action(*app);
    QueueCursorRestore(app);
}

void OnNew(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleNew(); });
}

void OnOpen(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleOpen(); });
}

void OnSave(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleSave(); });
}

void OnSaveAs(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleSaveAs(); });
}

void OnPageSetup(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandlePageSetup(); });
}

void OnPrint(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandlePrint(); });
}

void OnExit(GSimpleAction*, GVariant*, gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->HandleExit();
}

void OnUndo(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleUndo(); });
}

void OnCut(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleCut(); });
}

void OnCopy(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleCopy(); });
}

void OnPaste(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandlePaste(); });
}

void OnDelete(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleDelete(); });
}

void OnFind(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleFind(); });
}

void OnFindNext(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleFindNext(); });
}

void OnReplace(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleReplace(); });
}

void OnGoTo(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleGoTo(); });
}

void OnSelectAll(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleSelectAll(); });
}

void OnTimeDate(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleTimeDate(); });
}

void OnSpellingReplaceIndex(GSimpleAction* action, GVariant*, gpointer userData)
{
    const char* name = g_action_get_name(G_ACTION(action));
    if (name == nullptr) {
        return;
    }

    constexpr const char* kPrefix = "spelling-replace-";
    const std::string actionName(name);
    if (actionName.rfind(kPrefix, 0) != 0 || actionName.size() <= std::char_traits<char>::length(kPrefix)) {
        return;
    }

    const char digit = actionName[std::char_traits<char>::length(kPrefix)];
    if (digit < '0' || digit > '4') {
        return;
    }

    auto* app = static_cast<GtkNotepadApp*>(userData);
    app->HandleReplaceSpellingSuggestion(static_cast<std::size_t>(digit - '0'));
    QueueCursorRestore(app);
}

void OnSpellingIgnore(GSimpleAction*, GVariant*, gpointer userData)
{
    auto* app = static_cast<GtkNotepadApp*>(userData);
    app->HandleIgnoreContextSpelling();
    QueueCursorRestore(app);
}

void OnSpellingAdd(GSimpleAction*, GVariant*, gpointer userData)
{
    auto* app = static_cast<GtkNotepadApp*>(userData);
    app->HandleAddContextSpelling();
    QueueCursorRestore(app);
}

gboolean DismissOpenMenusAfterAction(gpointer userData);
gboolean ApplyAppearanceThemeAfterMenuDismissal(gpointer userData);
void QueueMenuDismissal(GtkNotepadApp* app);
void QueueAppearanceThemeChange(GtkNotepadApp* app, classic_notepad::AppearanceTheme theme);

struct PendingAppearanceThemeChange {
    GtkNotepadApp* app = nullptr;
    classic_notepad::AppearanceTheme theme = classic_notepad::AppearanceTheme::System;
};

void OnWordWrapChangeState(GSimpleAction*, GVariant* value, gpointer userData)
{
    if (value == nullptr) {
        return;
    }

    auto* app = static_cast<GtkNotepadApp*>(userData);
    app->SetWordWrap(g_variant_get_boolean(value) != FALSE);
    QueueMenuDismissal(app);
}

void OnFont(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleChooseFont(); });
}

void OnStatusBarChangeState(GSimpleAction*, GVariant* value, gpointer userData)
{
    if (value == nullptr) {
        return;
    }

    auto* app = static_cast<GtkNotepadApp*>(userData);
    app->SetStatusBarVisible(g_variant_get_boolean(value) != FALSE);
    QueueMenuDismissal(app);
}

gboolean DismissOpenMenusAfterAction(gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->DismissOpenMenusAndResetModels();
    return G_SOURCE_REMOVE;
}

gboolean ApplyAppearanceThemeAfterMenuDismissal(gpointer userData)
{
    auto* pending = static_cast<PendingAppearanceThemeChange*>(userData);
    pending->app->DismissOpenMenusAndResetModels();
    pending->app->SetAppearanceTheme(pending->theme);
    pending->app->RestoreClassicArrowCursor();
    delete pending;
    return G_SOURCE_REMOVE;
}

gboolean RestoreClassicArrowCursorAfterAction(gpointer userData)
{
    static_cast<GtkNotepadApp*>(userData)->RestoreClassicArrowCursor();
    return G_SOURCE_REMOVE;
}

void QueueMenuDismissal(GtkNotepadApp* app)
{
    g_idle_add(DismissOpenMenusAfterAction, app);
}

void QueueCursorRestore(GtkNotepadApp* app)
{
    g_idle_add(RestoreClassicArrowCursorAfterAction, app);
}

void QueueAppearanceThemeChange(GtkNotepadApp* app, classic_notepad::AppearanceTheme theme)
{
    app->DismissOpenMenusAndResetModels();
    g_idle_add(ApplyAppearanceThemeAfterMenuDismissal, new PendingAppearanceThemeChange {app, theme});
}

void OnAppearanceSystem(GSimpleAction*, GVariant*, gpointer userData)
{
    auto* app = static_cast<GtkNotepadApp*>(userData);
    QueueAppearanceThemeChange(app, classic_notepad::AppearanceTheme::System);
}

void OnAppearanceLight(GSimpleAction*, GVariant*, gpointer userData)
{
    auto* app = static_cast<GtkNotepadApp*>(userData);
    QueueAppearanceThemeChange(app, classic_notepad::AppearanceTheme::Light);
}

void OnAppearanceDark(GSimpleAction*, GVariant*, gpointer userData)
{
    auto* app = static_cast<GtkNotepadApp*>(userData);
    QueueAppearanceThemeChange(app, classic_notepad::AppearanceTheme::Dark);
}

void OnAbout(GSimpleAction*, GVariant*, gpointer userData)
{
    RunMenuAction(userData, [](GtkNotepadApp& app) { app.HandleAbout(); });
}

const GActionEntry kActions[] = {
    {"new", OnNew, nullptr, nullptr, nullptr},
    {"open", OnOpen, nullptr, nullptr, nullptr},
    {"save", OnSave, nullptr, nullptr, nullptr},
    {"save-as", OnSaveAs, nullptr, nullptr, nullptr},
    {"page-setup", OnPageSetup, nullptr, nullptr, nullptr},
    {"print", OnPrint, nullptr, nullptr, nullptr},
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
    {"spelling-replace-0", OnSpellingReplaceIndex, nullptr, nullptr, nullptr},
    {"spelling-replace-1", OnSpellingReplaceIndex, nullptr, nullptr, nullptr},
    {"spelling-replace-2", OnSpellingReplaceIndex, nullptr, nullptr, nullptr},
    {"spelling-replace-3", OnSpellingReplaceIndex, nullptr, nullptr, nullptr},
    {"spelling-replace-4", OnSpellingReplaceIndex, nullptr, nullptr, nullptr},
    {"spelling-ignore", OnSpellingIgnore, nullptr, nullptr, nullptr},
    {"spelling-add", OnSpellingAdd, nullptr, nullptr, nullptr},
    {"word-wrap", nullptr, nullptr, "false", OnWordWrapChangeState},
    {"font", OnFont, nullptr, nullptr, nullptr},
    {"status-bar", nullptr, nullptr, "true", OnStatusBarChangeState},
    {"appearance-system", OnAppearanceSystem, nullptr, nullptr, nullptr},
    {"appearance-light", OnAppearanceLight, nullptr, nullptr, nullptr},
    {"appearance-dark", OnAppearanceDark, nullptr, nullptr, nullptr},
    {"about", OnAbout, nullptr, nullptr, nullptr},
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
    SetAccels(app.Application(), "win.print", "<Primary>p");
    SetAccels(app.Application(), "win.exit", "<Primary>q", "<Alt>F4");
    SetAccels(app.Application(), "win.undo", "<Primary>z");
    SetAccels(app.Application(), "win.cut", "<Primary>x");
    SetAccels(app.Application(), "win.copy", "<Primary>c");
    SetAccels(app.Application(), "win.paste", "<Primary>v");
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
    GMenu* filePrintSection = g_menu_new();
    GMenu* fileExitSection = g_menu_new();
    GMenu* editMenu = g_menu_new();
    GMenu* editUndoSection = g_menu_new();
    GMenu* editClipboardSection = g_menu_new();
    GMenu* editFindSection = g_menu_new();
    GMenu* editOtherSection = g_menu_new();
    GMenu* formatMenu = g_menu_new();
    GMenu* viewMenu = g_menu_new();
    GMenu* appearanceMenu = g_menu_new();
    GMenu* helpMenu = g_menu_new();

    g_menu_append(filePrimarySection, "New", "win.new");
    g_menu_append(filePrimarySection, "Open...", "win.open");
    g_menu_append(filePrimarySection, "Save", "win.save");
    g_menu_append(filePrimarySection, "Save As...", "win.save-as");
    g_menu_append_section(fileMenu, nullptr, G_MENU_MODEL(filePrimarySection));

    g_menu_append(filePrintSection, "Page Setup...", "win.page-setup");
    g_menu_append(filePrintSection, "Print...", "win.print");
    g_menu_append_section(fileMenu, nullptr, G_MENU_MODEL(filePrintSection));

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
    g_menu_append(appearanceMenu, "System", "win.appearance-system");
    g_menu_append(appearanceMenu, "Light", "win.appearance-light");
    g_menu_append(appearanceMenu, "Dark", "win.appearance-dark");
    g_menu_append_submenu(viewMenu, "Appearance", G_MENU_MODEL(appearanceMenu));
    g_menu_append(helpMenu, "About Classic Notepad", "win.about");

    g_menu_append_submenu(bar, "File", G_MENU_MODEL(fileMenu));
    g_menu_append_submenu(bar, "Edit", G_MENU_MODEL(editMenu));
    g_menu_append_submenu(bar, "Format", G_MENU_MODEL(formatMenu));
    g_menu_append_submenu(bar, "View", G_MENU_MODEL(viewMenu));
    g_menu_append_submenu(bar, "Help", G_MENU_MODEL(helpMenu));

    GtkWidget* menuBar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(bar));
    g_object_unref(helpMenu);
    g_object_unref(appearanceMenu);
    g_object_unref(viewMenu);
    g_object_unref(formatMenu);
    g_object_unref(editOtherSection);
    g_object_unref(editFindSection);
    g_object_unref(editClipboardSection);
    g_object_unref(editUndoSection);
    g_object_unref(editMenu);
    g_object_unref(fileExitSection);
    g_object_unref(filePrintSection);
    g_object_unref(filePrimarySection);
    g_object_unref(fileMenu);
    g_object_unref(bar);
    return menuBar;
}

} // namespace classic_notepad::linux_ui
