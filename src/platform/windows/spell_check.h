#pragma once

#include "win32_platform.h"

#include <windows.h>
#include <spellcheck.h>

#include <cstdint>
#include <string>
#include <optional>
#include <limits>
#include <vector>

struct SpellingErrorRange {
    DWORD start = 0;
    DWORD length = 0;
    std::wstring replacement;
    CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
};

enum class SpellOperationStatus : std::uint8_t {
    Success,
    Unavailable,
    Failure,
};

template <typename TValue>
struct SpellOperationResult {
    SpellOperationStatus status = SpellOperationStatus::Unavailable;
    HRESULT hresult = S_FALSE;
    TValue value{};
};

struct SpellTestBackendConfiguration {
    bool available = true;
    std::vector<std::wstring> misspelledWords;
    std::vector<std::wstring> suggestions;
    SpellOperationStatus checkStatus = SpellOperationStatus::Success;
    SpellOperationStatus suggestStatus = SpellOperationStatus::Success;
    SpellOperationStatus ignoreStatus = SpellOperationStatus::Success;
    SpellOperationStatus addStatus = SpellOperationStatus::Success;
    HRESULT checkHresult = S_OK;
    HRESULT suggestHresult = S_OK;
    HRESULT ignoreHresult = S_OK;
    HRESULT addHresult = S_OK;
    std::size_t failCheckAfter = (std::numeric_limits<std::size_t>::max)();
    std::size_t failSuggestAfter = (std::numeric_limits<std::size_t>::max)();
};

using SpellCheckResult = SpellOperationResult<std::vector<SpellingErrorRange>>;
using SpellSuggestResult = SpellOperationResult<std::vector<std::wstring>>;
using SpellActionResult = SpellOperationResult<bool>;

class SpellCheckService {
public:
    ~SpellCheckService();
    bool Initialize(const wchar_t* languageTag = L"en-GB");
    bool IsAvailable() const;
    SpellCheckResult Check(const std::wstring& text) const;
    SpellSuggestResult Suggest(const std::wstring& word, std::size_t limit) const;
    SpellActionResult Ignore(const std::wstring& word);
    SpellActionResult Add(const std::wstring& word);
    void ConfigureTestBackend(SpellTestBackendConfiguration configuration);
    void ClearTestBackend();
    std::uint64_t TestBackendCallCount(const wchar_t* operation) const;
    void Reset();

private:
    std::wstring languageTag_;
    bool available_ = false;
    std::optional<SpellTestBackendConfiguration> testBackend_;
    mutable std::uint64_t checkCalls_ = 0;
    mutable std::uint64_t suggestCalls_ = 0;
    std::uint64_t ignoreCalls_ = 0;
    std::uint64_t addCalls_ = 0;
    std::vector<std::wstring> testIgnoredWords_;
    std::vector<std::wstring> testAddedWords_;
    ISpellCheckerFactory* factory_ = nullptr;
    ISpellChecker* checker_ = nullptr;
};
