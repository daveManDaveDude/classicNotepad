#pragma once

#include <windows.h>
#include <spellcheck.h>

#include <string>
#include <vector>

struct SpellingErrorRange {
    DWORD start = 0;
    DWORD length = 0;
    std::wstring replacement;
    CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
};

class SpellCheckService {
public:
    ~SpellCheckService();
    bool Initialize(const wchar_t* languageTag = L"en-GB");
    bool IsAvailable() const;
    std::vector<SpellingErrorRange> Check(const std::wstring& text) const;
    std::vector<std::wstring> Suggest(const std::wstring& word, std::size_t limit) const;
    void Ignore(const std::wstring& word);
    void Add(const std::wstring& word);
    void Reset();

private:
    std::wstring languageTag_;
    bool available_ = false;
    ISpellCheckerFactory* factory_ = nullptr;
    ISpellChecker* checker_ = nullptr;
};
