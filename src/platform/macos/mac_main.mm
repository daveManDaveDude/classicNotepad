#include "mac_app.h"

int main(int argc, char* argv[])
{
    classic_notepad::macos::ClassicNotepadMacApp app;
    return app.Run(argc, argv);
}
