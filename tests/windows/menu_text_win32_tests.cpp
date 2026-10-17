#include "../../src/platform/windows/windows_menu_text.h"

#include <cwctype>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void ExpectText(const std::wstring& actual, const std::wstring& expected, const char* message)
{
    Expect(actual == expected, message);
}

void RunStripMenuMnemonicChecks()
{
    using classic_notepad::windows::StripMenuMnemonics;
    ExpectText(StripMenuMnemonics(L""), L"", "Empty menu text stays empty");
    ExpectText(StripMenuMnemonics(L"File"), L"File", "Text without mnemonic markers is unchanged");
    ExpectText(StripMenuMnemonics(L"&File"), L"File", "A single mnemonic marker is removed");
    ExpectText(StripMenuMnemonics(L"Save && Exit"), L"Save & Exit",
        "An escaped ampersand is retained once");
    ExpectText(StripMenuMnemonics(L"&&"), L"&", "A pair of markers produces one ampersand");
    ExpectText(StripMenuMnemonics(L"&&&A"), L"&A",
        "Escaped ampersand followed by mnemonic marker preserves the label");
    ExpectText(StripMenuMnemonics(L"&&&&"), L"&&",
        "Two escaped ampersand pairs produce two ampersands");
    ExpectText(StripMenuMnemonics(L"&&&&X"), L"&&X",
        "Escaped ampersand pairs remain escaped when followed by ordinary text");
    ExpectText(StripMenuMnemonics(L"Tail&"), L"Tail",
        "A trailing mnemonic marker is removed");

    const std::wstring unicodeText = L"\U0001F600&漢字";
    ExpectText(StripMenuMnemonics(unicodeText), L"\U0001F600漢字",
        "Supplementary and CJK text is preserved while stripping the marker");

    const std::wstring withNull(L"A\0&B", 4U);
    const std::wstring strippedNull(L"A\0B", 3U);
    ExpectText(StripMenuMnemonics(withNull), strippedNull,
        "Embedded NUL is preserved and later mnemonic markers are processed");
}

void RunFindMenuMnemonicChecks()
{
    using classic_notepad::windows::FindMenuMnemonic;
    Expect(FindMenuMnemonic(L"") == L'\0', "Empty text has no mnemonic");
    Expect(FindMenuMnemonic(L"File") == L'\0', "Text without a mnemonic has no result");
    Expect(FindMenuMnemonic(L"&Open") == L'o', "ASCII mnemonic is lowercased");
    Expect(FindMenuMnemonic(L"Open && Close") == L'\0',
        "Escaped ampersands are skipped when searching for a mnemonic");
    Expect(FindMenuMnemonic(L"&&&Open") == L'o',
        "The first unescaped marker is selected after escaped pairs");
    Expect(FindMenuMnemonic(L"Tail&") == L'\0', "A trailing marker does not name a mnemonic");
    Expect(FindMenuMnemonic(L"&&&&") == L'\0', "Only escaped marker pairs have no mnemonic");
    Expect(FindMenuMnemonic(L"&&&&X") == L'\0',
        "Escaped ampersand pairs followed by text still have no mnemonic");
    Expect(FindMenuMnemonic(L"&A &B") == L'a',
        "The first of multiple real mnemonic markers is returned");
    Expect(FindMenuMnemonic(L"&漢") == static_cast<wchar_t>(std::towlower(static_cast<wint_t>(L'漢'))),
        "CJK mnemonic uses the existing towlower behavior");
    Expect(FindMenuMnemonic(L"&É") == static_cast<wchar_t>(std::towlower(static_cast<wint_t>(L'É'))),
        "BMP accented mnemonic uses the existing towlower behavior");

    const std::wstring emoji = L"\U0001F600";
    Expect(emoji.size() >= 2U, "Windows stores the supplementary test character as UTF-16 units");
    Expect(FindMenuMnemonic(L"&\U0001F600") ==
            static_cast<wchar_t>(std::towlower(static_cast<wint_t>(emoji.front()))),
        "Supplementary mnemonic follows the existing first-wchar towlower behavior");

    const std::wstring afterNull(L"X\0&B", 4U);
    Expect(FindMenuMnemonic(afterNull) == L'b',
        "Mnemonic search continues across an embedded NUL in the length-delimited string");
    const std::wstring markerBeforeNull(L"&\0X", 3U);
    Expect(FindMenuMnemonic(markerBeforeNull) == L'\0',
        "A marker before an embedded NUL returns the existing NUL character result");
}

void RunSplitMenuItemTextChecks()
{
    using classic_notepad::windows::SplitMenuItemText;
    std::wstring label;
    std::wstring accelerator = L"stale";
    SplitMenuItemText(L"&File", label, accelerator);
    ExpectText(label, L"File", "No-tab menu text is stripped into the label");
    ExpectText(accelerator, L"", "No-tab menu text clears the accelerator output");

    SplitMenuItemText(L"&&Open\tCtrl+&&O", label, accelerator);
    ExpectText(label, L"&Open", "Mnemonic stripping applies to the menu label");
    ExpectText(accelerator, L"Ctrl+&O", "Mnemonic stripping also applies to the accelerator");

    SplitMenuItemText(L"Name\tFirst\tSecond", label, accelerator);
    ExpectText(label, L"Name", "The first tab separates the label");
    ExpectText(accelerator, L"First\tSecond", "Later tabs remain in the accelerator text");

    SplitMenuItemText(L"\t", label, accelerator);
    ExpectText(label, L"", "An empty label before the tab stays empty");
    ExpectText(accelerator, L"", "An empty accelerator after the tab stays empty");

    SplitMenuItemText(L"\t&Ctrl+X", label, accelerator);
    ExpectText(label, L"", "A leading tab produces an empty label");
    ExpectText(accelerator, L"Ctrl+X", "A leading tab still strips the accelerator mnemonic");
    accelerator = L"preseeded";
    SplitMenuItemText(L"&File\t", label, accelerator);
    ExpectText(label, L"File", "A trailing tab retains the stripped label");
    ExpectText(accelerator, L"", "A trailing tab clears a preseeded accelerator");

    const std::wstring explicitSurrogatePair{
        static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00)};
    std::wstring unicodeMenu = L"&漢";
    unicodeMenu += explicitSurrogatePair;
    unicodeMenu += L"\t&Ctrl+";
    unicodeMenu += explicitSurrogatePair;
    unicodeMenu += L"字";
    std::wstring expectedUnicodeLabel = L"漢";
    expectedUnicodeLabel += explicitSurrogatePair;
    std::wstring expectedUnicodeAccelerator = L"Ctrl+";
    expectedUnicodeAccelerator += explicitSurrogatePair;
    expectedUnicodeAccelerator += L"字";
    SplitMenuItemText(unicodeMenu, label, accelerator);
    ExpectText(label, expectedUnicodeLabel,
        "Split preserves explicit UTF-16 surrogate units and CJK in the label");
    ExpectText(accelerator, expectedUnicodeAccelerator,
        "Split preserves explicit UTF-16 surrogate units and CJK in the accelerator");

    SplitMenuItemText(L"&File && Edit\t&Ctrl+&&X", label, accelerator);
    ExpectText(label, L"File & Edit",
        "Split removes real label mnemonics and retains escaped ampersands");
    ExpectText(accelerator, L"Ctrl+&X",
        "Split removes real accelerator mnemonics and retains escaped ampersands");

    const std::wstring inputWithNull(L"A\0&B\tCtrl+&&X", 13U);
    const std::wstring expectedLabel(L"A\0B", 3U);
    SplitMenuItemText(inputWithNull, label, accelerator);
    ExpectText(label, expectedLabel, "Split preserves embedded NUL in the label");
    ExpectText(accelerator, L"Ctrl+&X", "Split processes escaped ampersands after embedded NUL");

    label = L"preseeded label";
    accelerator = L"preseeded accelerator";
    SplitMenuItemText(L"", label, accelerator);
    ExpectText(label, L"", "Splitting empty text clears the preseeded label");
    ExpectText(accelerator, L"", "Splitting empty text clears the preseeded accelerator");

    std::wstring aliasedOutput = L"old";
    SplitMenuItemText(L"Label\tShortcut", aliasedOutput, aliasedOutput);
    ExpectText(aliasedOutput, L"Shortcut", "Tab split retains the original label-then-accelerator assignment order");
    aliasedOutput = L"old";
    SplitMenuItemText(L"Label", aliasedOutput, aliasedOutput);
    ExpectText(aliasedOutput, L"", "No-tab split retains the original label-then-clear assignment order");
}

} // namespace

int main()
{
    RunStripMenuMnemonicChecks();
    RunFindMenuMnemonicChecks();
    RunSplitMenuItemTextChecks();
    if (failures != 0) {
        std::cerr << failures << " Windows menu-text check(s) failed.\n";
        return 1;
    }
    std::cout << "WindowsMenuTextTests passed.\n";
    return 0;
}
