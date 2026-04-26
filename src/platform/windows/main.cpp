#include "app.h"

#include <shellapi.h>
#include <windows.h>

int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE,
    _In_ PWSTR,
    _In_ int showCommand)
{
    std::wstring initialFilePath;
    bool automationMode = false;
    bool automationVisible = false;
    int argumentCount = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments != nullptr) {
        for (int index = 1; index < argumentCount; ++index) {
            if (arguments[index] == nullptr) {
                continue;
            }

            const std::wstring argument = arguments[index];
            if (argument == L"--automation") {
                automationMode = true;
                continue;
            }

            if (argument == L"--automation-visible") {
                automationMode = true;
                automationVisible = true;
                continue;
            }

            if (initialFilePath.empty()) {
                initialFilePath = argument;
            }
        }
        LocalFree(arguments);
    }

    ClassicNotepadApp app(instance);
    if (automationMode) {
        return app.RunAutomation(showCommand, initialFilePath, automationVisible);
    }

    return app.Run(showCommand, initialFilePath);
}
