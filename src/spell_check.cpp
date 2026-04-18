#include "spell_check.h"

#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

namespace {

std::wstring CopyCoTaskMemString(wchar_t* value)
{
    if (value == nullptr) {
        return {};
    }

    std::wstring result(value);
    CoTaskMemFree(value);
    return result;
}

} // namespace

SpellCheckService::~SpellCheckService()
{
    Reset();
}

void SpellCheckService::Reset()
{
    available_ = false;

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
    if (FAILED(hr) || factory_ == nullptr) {
        return false;
    }

    BOOL supported = FALSE;
    hr = factory_->IsSupported(languageTag_.c_str(), &supported);
    if (FAILED(hr) || !supported) {
        return false;
    }

    hr = factory_->CreateSpellChecker(languageTag_.c_str(), &checker_);
    if (FAILED(hr) || checker_ == nullptr) {
        return false;
    }

    available_ = true;
    return true;
}

bool SpellCheckService::IsAvailable() const
{
    return available_ && checker_ != nullptr;
}

std::vector<SpellingErrorRange> SpellCheckService::Check(const std::wstring& text) const
{
    std::vector<SpellingErrorRange> errors;
    if (!IsAvailable() || text.empty()) {
        return errors;
    }

    Microsoft::WRL::ComPtr<IEnumSpellingError> errorEnumerator;
    HRESULT hr = checker_->Check(text.c_str(), &errorEnumerator);
    if (FAILED(hr) || errorEnumerator == nullptr) {
        return errors;
    }

    for (;;) {
        Microsoft::WRL::ComPtr<ISpellingError> spellingError;
        hr = errorEnumerator->Next(&spellingError);
        if (hr != S_OK || spellingError == nullptr) {
            break;
        }

        SpellingErrorRange range{};
        spellingError->get_StartIndex(&range.start);
        spellingError->get_Length(&range.length);
        spellingError->get_CorrectiveAction(&range.action);

        wchar_t* replacement = nullptr;
        if (SUCCEEDED(spellingError->get_Replacement(&replacement))) {
            range.replacement = CopyCoTaskMemString(replacement);
        }

        errors.push_back(std::move(range));
    }

    return errors;
}

std::vector<std::wstring> SpellCheckService::Suggest(const std::wstring& word, std::size_t limit) const
{
    std::vector<std::wstring> suggestions;
    if (!IsAvailable() || word.empty() || limit == 0U) {
        return suggestions;
    }

    Microsoft::WRL::ComPtr<IEnumString> suggestionEnumerator;
    HRESULT hr = checker_->Suggest(word.c_str(), &suggestionEnumerator);
    if (FAILED(hr) || suggestionEnumerator == nullptr) {
        return suggestions;
    }

    while (suggestions.size() < limit) {
        wchar_t* suggestion = nullptr;
        ULONG fetched = 0;
        hr = suggestionEnumerator->Next(1, &suggestion, &fetched);
        if (hr != S_OK || fetched == 0 || suggestion == nullptr) {
            break;
        }

        suggestions.push_back(CopyCoTaskMemString(suggestion));
    }

    return suggestions;
}

void SpellCheckService::Ignore(const std::wstring& word)
{
    if (!IsAvailable() || word.empty()) {
        return;
    }

    checker_->Ignore(word.c_str());
}

void SpellCheckService::Add(const std::wstring& word)
{
    if (!IsAvailable() || word.empty()) {
        return;
    }

    checker_->Add(word.c_str());
}
