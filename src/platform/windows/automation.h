#pragma once

class ClassicNotepadApp;

class WindowsAutomationController {
public:
    explicit WindowsAutomationController(ClassicNotepadApp& app);

    int Run();

private:
    ClassicNotepadApp& app_;
};
