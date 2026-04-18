#include "app.h"

#include <shellapi.h>
#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    std::wstring initialFilePath;
    int argumentCount = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments != nullptr) {
        if (argumentCount > 1 && arguments[1] != nullptr) {
            initialFilePath = arguments[1];
        }
        LocalFree(arguments);
    }

    ClassicNotepadApp app(instance);
    return app.Run(showCommand, initialFilePath);
}
