#include "spell_check.h"

#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <utility>

namespace {

std::wstring CopyCoTaskMemString(wchar_t* value)
{
    if (value == nullptr) return {};
    std::wstring result(value);
    CoTaskMemFree(value);
    return result;
}

bool EqualWord(const std::wstring& left, const std::wstring& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) return false;
    }
    return true;
}

template <typename TValue>
SpellOperationResult<TValue> MakeResult(
    SpellOperationStatus status,
    HRESULT hresult,
    TValue value = {})
{
    return {status, hresult, std::move(value)};
}

SpellOperationStatus StatusFromHRESULT(HRESULT hresult)
{
    return SUCCEEDED(hresult) ? SpellOperationStatus::Success : SpellOperationStatus::Failure;
}

HRESULT FailedHRESULT(HRESULT hresult)
{
    return FAILED(hresult) ? hresult : E_FAIL;
}

HRESULT HRESULTForTestStatus(SpellOperationStatus status, HRESULT configured)
{
    if (status == SpellOperationStatus::Unavailable) return S_FALSE;
    if (status == SpellOperationStatus::Failure) return FailedHRESULT(configured);
    return configured;
}

} // namespace

SpellCheckService::~SpellCheckService()
{
    Reset();
}

void SpellCheckService::Reset()
{
    available_ = false;
    testBackend_.reset();
    testIgnoredWords_.clear();
    testAddedWords_.clear();
    checkCalls_ = 0U;
    suggestCalls_ = 0U;
    ignoreCalls_ = 0U;
    addCalls_ = 0U;

    if (checker_ != nullptr) {
        checker_->Release();
        checker_ = nullptr;
    }

    if (factory_ != nullptr) {
        factory_->Release();
        factory_ = nullptr;
    }
}

bool SpellCheckService::Initialize(const wchar_t* languageTag)
{
    Reset();
    languageTag_ = languageTag == nullptr ? L"" : languageTag;

    HRESULT hr = CoCreateInstance(
        __uuidof(SpellCheckerFactory),
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory_));
    if (FAILED(hr) || factory_ == nullptr) return false;

    BOOL supported = FALSE;
    hr = factory_->IsSupported(languageTag_.c_str(), &supported);
    if (FAILED(hr) || !supported) return false;

    hr = factory_->CreateSpellChecker(languageTag_.c_str(), &checker_);
    if (FAILED(hr) || checker_ == nullptr) return false;

    available_ = true;
    return true;
}

bool SpellCheckService::IsAvailable() const
{
    if (testBackend_.has_value()) return testBackend_->available;
    return available_ && checker_ != nullptr;
}

SpellCheckResult SpellCheckService::Check(const std::wstring& text) const
{
    ++checkCalls_;
    if (testBackend_.has_value()) {
        const SpellTestBackendConfiguration& test = *testBackend_;
        if (!test.available) return MakeResult<std::vector<SpellingErrorRange>>(SpellOperationStatus::Unavailable, S_FALSE);
        if (test.checkStatus != SpellOperationStatus::Success) {
            return MakeResult<std::vector<SpellingErrorRange>>(test.checkStatus,
                HRESULTForTestStatus(test.checkStatus, test.checkHresult));
        }

        std::vector<SpellingErrorRange> errors;
        for (std::size_t position = 0; position < text.size();) {
            std::size_t matchedLength = 0U;
            for (const std::wstring& word : test.misspelledWords) {
                if (word.empty() || word.size() > text.size() - position ||
                    !EqualWord(text.substr(position, word.size()), word)) continue;
                const bool leftBoundary = position == 0U || !std::iswalnum(text[position - 1U]);
                const std::size_t end = position + word.size();
                const bool rightBoundary = end == text.size() || !std::iswalnum(text[end]);
                if (leftBoundary && rightBoundary) {
                    matchedLength = word.size();
                    break;
                }
            }
            if (matchedLength == 0U) {
                ++position;
                continue;
            }
            SpellingErrorRange range{};
            range.start = static_cast<DWORD>(position);
            range.length = static_cast<DWORD>(matchedLength);
            errors.push_back(range);
            position += matchedLength;
        }
        if (test.failCheckAfter < errors.size()) {
            return MakeResult<std::vector<SpellingErrorRange>>(SpellOperationStatus::Failure, E_FAIL);
        }

        const auto ignoredOrAdded = [this, &text](const SpellingErrorRange& range) {
            if (range.start > text.size() || range.length > text.size() - range.start) return false;
            const std::wstring word = text.substr(range.start, range.length);
            const auto contains = [&word](const std::vector<std::wstring>& words) {
                return std::any_of(words.begin(), words.end(), [&word](const std::wstring& candidate) {
                    return EqualWord(candidate, word);
                });
            };
            return contains(testIgnoredWords_) || contains(testAddedWords_);
        };
        errors.erase(std::remove_if(errors.begin(), errors.end(), ignoredOrAdded), errors.end());
        return MakeResult(SpellOperationStatus::Success, test.checkHresult, std::move(errors));
    }

    if (!IsAvailable()) return MakeResult<std::vector<SpellingErrorRange>>(SpellOperationStatus::Unavailable, S_FALSE);
    if (text.empty()) return MakeResult(SpellOperationStatus::Success, S_OK, std::vector<SpellingErrorRange>{});

    Microsoft::WRL::ComPtr<IEnumSpellingError> errorEnumerator;
    HRESULT hr = checker_->Check(text.c_str(), &errorEnumerator);
    if (FAILED(hr) || errorEnumerator == nullptr) {
        return MakeResult<std::vector<SpellingErrorRange>>(SpellOperationStatus::Failure, FailedHRESULT(hr));
    }

    std::vector<SpellingErrorRange> errors;
    for (;;) {
        Microsoft::WRL::ComPtr<ISpellingError> spellingError;
        hr = errorEnumerator->Next(&spellingError);
        if (hr == S_FALSE) break;
        if (FAILED(hr)) return MakeResult<std::vector<SpellingErrorRange>>(SpellOperationStatus::Failure, hr);
        if (hr != S_OK || spellingError == nullptr) {
            return MakeResult<std::vector<SpellingErrorRange>>(SpellOperationStatus::Failure, E_UNEXPECTED);
        }

        SpellingErrorRange range{};
        if (FAILED(hr = spellingError->get_StartIndex(&range.start)) ||
            FAILED(hr = spellingError->get_Length(&range.length)) ||
            FAILED(hr = spellingError->get_CorrectiveAction(&range.action))) {
            return MakeResult<std::vector<SpellingErrorRange>>(SpellOperationStatus::Failure, hr);
        }

        wchar_t* replacement = nullptr;
        hr = spellingError->get_Replacement(&replacement);
        if (FAILED(hr)) {
            if (replacement != nullptr) CoTaskMemFree(replacement);
            return MakeResult<std::vector<SpellingErrorRange>>(SpellOperationStatus::Failure, hr);
        }
        range.replacement = CopyCoTaskMemString(replacement);
        errors.push_back(std::move(range));
    }
    return MakeResult(SpellOperationStatus::Success, S_OK, std::move(errors));
}

SpellSuggestResult SpellCheckService::Suggest(const std::wstring& word, std::size_t limit) const
{
    ++suggestCalls_;
    if (testBackend_.has_value()) {
        const SpellTestBackendConfiguration& test = *testBackend_;
        if (!test.available) return MakeResult<std::vector<std::wstring>>(SpellOperationStatus::Unavailable, S_FALSE);
        if (test.suggestStatus != SpellOperationStatus::Success) {
            return MakeResult<std::vector<std::wstring>>(test.suggestStatus,
                HRESULTForTestStatus(test.suggestStatus, test.suggestHresult));
        }
        std::vector<std::wstring> suggestions = test.suggestions;
        if (test.failSuggestAfter < suggestions.size()) {
            return MakeResult<std::vector<std::wstring>>(SpellOperationStatus::Failure, E_FAIL);
        }
        if (suggestions.size() > limit) suggestions.resize(limit);
        return MakeResult(SpellOperationStatus::Success, test.suggestHresult, std::move(suggestions));
    }

    if (!IsAvailable()) return MakeResult<std::vector<std::wstring>>(SpellOperationStatus::Unavailable, S_FALSE);
    if (word.empty() || limit == 0U) return MakeResult(SpellOperationStatus::Success, S_OK, std::vector<std::wstring>{});

    Microsoft::WRL::ComPtr<IEnumString> suggestionEnumerator;
    HRESULT hr = checker_->Suggest(word.c_str(), &suggestionEnumerator);
    if (FAILED(hr) || suggestionEnumerator == nullptr) {
        return MakeResult<std::vector<std::wstring>>(SpellOperationStatus::Failure, FailedHRESULT(hr));
    }

    std::vector<std::wstring> suggestions;
    while (suggestions.size() < limit) {
        wchar_t* suggestion = nullptr;
        ULONG fetched = 0;
        hr = suggestionEnumerator->Next(1, &suggestion, &fetched);
        if (hr == S_FALSE) {
            if (suggestion != nullptr) CoTaskMemFree(suggestion);
            break;
        }
        if (FAILED(hr)) {
            if (suggestion != nullptr) CoTaskMemFree(suggestion);
            return MakeResult<std::vector<std::wstring>>(SpellOperationStatus::Failure, hr);
        }
        if (hr != S_OK || fetched == 0U || suggestion == nullptr) {
            if (suggestion != nullptr) CoTaskMemFree(suggestion);
            return MakeResult<std::vector<std::wstring>>(SpellOperationStatus::Failure, E_UNEXPECTED);
        }
        suggestions.push_back(CopyCoTaskMemString(suggestion));
    }
    return MakeResult(SpellOperationStatus::Success, S_OK, std::move(suggestions));
}

SpellActionResult SpellCheckService::Ignore(const std::wstring& word)
{
    ++ignoreCalls_;
    if (word.empty()) return MakeResult(SpellOperationStatus::Failure, E_INVALIDARG, false);
    if (testBackend_.has_value()) {
        const SpellTestBackendConfiguration& test = *testBackend_;
        if (!test.available) return MakeResult(SpellOperationStatus::Unavailable, S_FALSE, false);
        if (test.ignoreStatus != SpellOperationStatus::Success) {
            return MakeResult(test.ignoreStatus,
                HRESULTForTestStatus(test.ignoreStatus, test.ignoreHresult), false);
        }
        if (std::none_of(testIgnoredWords_.begin(), testIgnoredWords_.end(), [&word](const std::wstring& ignored) {
                return EqualWord(ignored, word);
            })) testIgnoredWords_.push_back(word);
        return MakeResult(SpellOperationStatus::Success, test.ignoreHresult, true);
    }
    if (!IsAvailable()) return MakeResult(SpellOperationStatus::Unavailable, S_FALSE, false);
    const HRESULT hr = checker_->Ignore(word.c_str());
    return MakeResult(StatusFromHRESULT(hr), hr, SUCCEEDED(hr));
}

SpellActionResult SpellCheckService::Add(const std::wstring& word)
{
    ++addCalls_;
    if (word.empty()) return MakeResult(SpellOperationStatus::Failure, E_INVALIDARG, false);
    if (testBackend_.has_value()) {
        const SpellTestBackendConfiguration& test = *testBackend_;
        if (!test.available) return MakeResult(SpellOperationStatus::Unavailable, S_FALSE, false);
        if (test.addStatus != SpellOperationStatus::Success) {
            return MakeResult(test.addStatus,
                HRESULTForTestStatus(test.addStatus, test.addHresult), false);
        }
        if (std::none_of(testAddedWords_.begin(), testAddedWords_.end(), [&word](const std::wstring& added) {
                return EqualWord(added, word);
            })) testAddedWords_.push_back(word);
        return MakeResult(SpellOperationStatus::Success, test.addHresult, true);
    }
    if (!IsAvailable()) return MakeResult(SpellOperationStatus::Unavailable, S_FALSE, false);
    const HRESULT hr = checker_->Add(word.c_str());
    return MakeResult(StatusFromHRESULT(hr), hr, SUCCEEDED(hr));
}

void SpellCheckService::ConfigureTestBackend(SpellTestBackendConfiguration configuration)
{
    testBackend_ = std::move(configuration);
    testIgnoredWords_.clear();
    testAddedWords_.clear();
    checkCalls_ = 0U;
    suggestCalls_ = 0U;
    ignoreCalls_ = 0U;
    addCalls_ = 0U;
}

void SpellCheckService::ClearTestBackend()
{
    testBackend_.reset();
    testIgnoredWords_.clear();
    testAddedWords_.clear();
    checkCalls_ = 0U;
    suggestCalls_ = 0U;
    ignoreCalls_ = 0U;
    addCalls_ = 0U;
}

std::uint64_t SpellCheckService::TestBackendCallCount(const wchar_t* operation) const
{
    if (operation == nullptr) return 0U;
    if (std::wcscmp(operation, L"check") == 0) return checkCalls_;
    if (std::wcscmp(operation, L"suggest") == 0) return suggestCalls_;
    if (std::wcscmp(operation, L"ignore") == 0) return ignoreCalls_;
    if (std::wcscmp(operation, L"add") == 0) return addCalls_;
    return 0U;
}
