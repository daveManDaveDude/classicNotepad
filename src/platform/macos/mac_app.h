#pragma once

#include <string>

namespace classic_notepad::macos {

class ClassicNotepadMacApp {
public:
    int Run(int argc, char* argv[]);

private:
    std::wstring initialPath_;
};

} // namespace classic_notepad::macos
