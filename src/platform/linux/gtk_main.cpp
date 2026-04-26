#include "gtk_app.h"

#include <string>
#include <utility>

using classic_notepad::linux_ui::GtkNotepadApp;
using classic_notepad::linux_ui::WideFromUtf8;

int main(int argc, char** argv)
{
    std::wstring initialPath;
    bool automationMode = false;
    bool automationVisible = false;

    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            continue;
        }

        const std::string argument = argv[index];
        if (argument == "--automation") {
            automationMode = true;
            continue;
        }

        if (argument == "--automation-visible") {
            automationMode = true;
            automationVisible = true;
            continue;
        }

        if (initialPath.empty()) {
            initialPath = WideFromUtf8(argument);
        }
    }

    GtkNotepadApp app(std::move(initialPath));
    if (automationMode) {
        return app.RunAutomation(automationVisible);
    }

    char* gtkArgv[] = {argc > 0 ? argv[0] : const_cast<char*>("ClassicNotepadGtk"), nullptr};
    int gtkArgc = 1;
    return app.Run(gtkArgc, gtkArgv);
}
