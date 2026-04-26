#pragma once

#include <string>

namespace classic_notepad::linux_ui {

class GtkNotepadApp;

class GtkAutomationController {
public:
    explicit GtkAutomationController(GtkNotepadApp& app);

    int Run();

private:
    GtkNotepadApp& app_;
    std::wstring testClipboard_;
};

} // namespace classic_notepad::linux_ui
