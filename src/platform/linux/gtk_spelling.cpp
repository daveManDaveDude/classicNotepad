#include "gtk_spelling.h"

#include "encoding.h"
#include "spell_text_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace classic_notepad::linux_ui {
namespace {

std::string Utf8FromWideForSpelling(const std::wstring& text)
{
    std::vector<std::uint8_t> bytes;
    std::wstring error;
    if (!classic_notepad::EncodeTextBytes(text, classic_notepad::TextEncoding::Utf8NoBom, bytes, error)) {
        return {};
    }

    return std::string(bytes.begin(), bytes.end());
}

std::wstring WideFromUtf8ForSpelling(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return {};
    }

    const std::string bytesText(text);
    const std::vector<std::uint8_t> bytes(bytesText.begin(), bytesText.end());
    classic_notepad::DecodeTextResult decoded;
    std::wstring error;
    if (!classic_notepad::DecodeTextBytes(bytes, decoded, error)) {
        return {};
    }

    return decoded.text;
}

std::string NormalizeLanguageCode(std::string code)
{
    std::replace(code.begin(), code.end(), '-', '_');
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return code;
}

std::vector<std::string> LoadAvailableLanguages(SpellingProvider* provider)
{
    std::vector<std::string> languages;
    if (provider == nullptr) {
        return languages;
    }

    GPtrArray* languageInfos = spelling_provider_list_languages(provider);
    if (languageInfos == nullptr) {
        return languages;
    }

    languages.reserve(languageInfos->len);
    for (guint index = 0; index < languageInfos->len; ++index) {
        auto* info = static_cast<SpellingLanguageInfo*>(g_ptr_array_index(languageInfos, index));
        const char* code = info == nullptr ? nullptr : spelling_language_info_get_code(info);
        if (code != nullptr && code[0] != '\0') {
            languages.emplace_back(code);
        }
    }

    g_ptr_array_unref(languageInfos);
    std::sort(languages.begin(), languages.end());
    languages.erase(std::unique(languages.begin(), languages.end()), languages.end());
    return languages;
}

std::string ChooseBritishEnglishLanguage(SpellingProvider* provider, const std::vector<std::string>& languages)
{
    if (provider == nullptr) {
        return {};
    }

    constexpr const char* kCandidates[] = {
        "en_GB",
        "en-GB",
        "en_GB.UTF-8"
    };

    for (const char* candidate : kCandidates) {
        if (spelling_provider_supports_language(provider, candidate) != FALSE) {
            return candidate;
        }
    }

    for (const std::string& language : languages) {
        const std::string normalized = NormalizeLanguageCode(language);
        if (normalized == "en_gb" || normalized.rfind("en_gb.", 0) == 0) {
            return language;
        }
    }

    return {};
}

std::size_t Utf16LengthOfCodePoint(wchar_t character)
{
    if constexpr (sizeof(wchar_t) == 2U) {
        (void)character;
        return 1U;
    } else {
        const auto codePoint = static_cast<std::uint32_t>(character);
        return codePoint > 0xFFFFU ? 2U : 1U;
    }
}

std::size_t Utf16OffsetForWideRange(const std::wstring& text, std::size_t start, std::size_t length)
{
    if (start >= text.size() || length == 0U) {
        return 0;
    }

    const std::size_t end = length > text.size() - start ? text.size() : start + length;
    std::size_t offset = 0;
    for (std::size_t index = 0; index < end; ++index) {
        if (index >= start) {
            offset += Utf16LengthOfCodePoint(text[index]);
        }
    }
    return offset;
}

std::size_t Utf16OffsetForWideIndex(const std::wstring& text, std::size_t wideIndex)
{
    std::size_t offset = 0;
    for (std::size_t index = 0; index < std::min(wideIndex, text.size()); ++index) {
        offset += Utf16LengthOfCodePoint(text[index]);
    }
    return offset;
}

bool CheckWord(SpellingChecker* checker, const std::wstring& word)
{
    if (checker == nullptr || word.empty()) {
        return true;
    }

    const std::string utf8Word = Utf8FromWideForSpelling(word);
    if (utf8Word.empty()) {
        return true;
    }

    return spelling_checker_check_word(
        checker,
        utf8Word.c_str(),
        static_cast<gssize>(utf8Word.size())) != FALSE;
}

void ConfigurePlainSourceBuffer(GtkSourceBuffer* buffer)
{
    if (buffer == nullptr) {
        return;
    }

    gtk_source_buffer_set_language(buffer, nullptr);
    gtk_source_buffer_set_highlight_syntax(buffer, FALSE);
    gtk_source_buffer_set_highlight_matching_brackets(buffer, FALSE);
}

void ConfigurePlainSourceView(GtkSourceView* view)
{
    if (view == nullptr) {
        return;
    }

    gtk_source_view_set_show_line_numbers(view, FALSE);
    gtk_source_view_set_show_line_marks(view, FALSE);
    gtk_source_view_set_show_right_margin(view, FALSE);
    gtk_source_view_set_highlight_current_line(view, FALSE);
    gtk_source_view_set_auto_indent(view, FALSE);
    gtk_source_view_set_indent_on_tab(view, FALSE);
    gtk_source_view_set_enable_snippets(view, FALSE);
    gtk_source_view_set_background_pattern(view, GTK_SOURCE_BACKGROUND_PATTERN_TYPE_NONE);
}

} // namespace

GtkSpellingService::GtkSpellingService()
{
    spelling_init();
    provider_ = spelling_provider_get_default();
    if (provider_ == nullptr) {
        capability_ = classic_notepad::SpellCapability::MissingBackend;
        return;
    }

    availableLanguages_ = LoadAvailableLanguages(provider_);
    languageCode_ = ChooseBritishEnglishLanguage(provider_, availableLanguages_);
    if (languageCode_.empty()) {
        capability_ = classic_notepad::SpellCapability::MissingDictionary;
        return;
    }

    checker_ = spelling_checker_new(provider_, languageCode_.c_str());
    if (checker_ == nullptr) {
        capability_ = classic_notepad::SpellCapability::MissingDictionary;
        return;
    }

    spelling_checker_set_language(checker_, languageCode_.c_str());
    capability_ = classic_notepad::SpellCapability::Available;
}

GtkSpellingService::~GtkSpellingService()
{
    if (adapter_ != nullptr) {
        g_object_unref(adapter_);
        adapter_ = nullptr;
    }

    if (checker_ != nullptr) {
        g_object_unref(checker_);
        checker_ = nullptr;
    }
}

classic_notepad::SpellCapability GtkSpellingService::Capability() const
{
    return capability_;
}

const std::string& GtkSpellingService::LanguageCode() const
{
    return languageCode_;
}

const std::vector<std::string>& GtkSpellingService::AvailableLanguages() const
{
    return availableLanguages_;
}

GtkWidget* GtkSpellingService::CreatePlainTextView() const
{
    GtkSourceBuffer* sourceBuffer = gtk_source_buffer_new(nullptr);
    ConfigurePlainSourceBuffer(sourceBuffer);
    GtkWidget* view = gtk_source_view_new_with_buffer(sourceBuffer);
    g_object_unref(sourceBuffer);

    ConfigurePlainSourceView(GTK_SOURCE_VIEW(view));
    return view;
}

void GtkSpellingService::Attach(GtkTextBuffer* buffer)
{
    if (adapter_ != nullptr) {
        g_object_unref(adapter_);
        adapter_ = nullptr;
    }

    if (capability_ != classic_notepad::SpellCapability::Available ||
        checker_ == nullptr ||
        buffer == nullptr ||
        !GTK_SOURCE_IS_BUFFER(buffer)) {
        return;
    }

    adapter_ = spelling_text_buffer_adapter_new(GTK_SOURCE_BUFFER(buffer), checker_);
    spelling_text_buffer_adapter_set_language(adapter_, languageCode_.c_str());
    spelling_text_buffer_adapter_set_enabled(adapter_, TRUE);
    spelling_text_buffer_adapter_invalidate_all(adapter_);
}

GMenuModel* GtkSpellingService::ContextMenuModel() const
{
    if (adapter_ == nullptr) {
        return nullptr;
    }

    return spelling_text_buffer_adapter_get_menu_model(adapter_);
}

void GtkSpellingService::InvalidateAll()
{
    if (adapter_ != nullptr) {
        spelling_text_buffer_adapter_invalidate_all(adapter_);
    }
}

std::vector<classic_notepad::SpellIssue> GtkSpellingService::CheckText(const std::wstring& text) const
{
    std::vector<classic_notepad::SpellIssue> issues;
    if (capability_ != classic_notepad::SpellCapability::Available || checker_ == nullptr || text.empty()) {
        return issues;
    }

    for (const classic_notepad::TextRange& range : classic_notepad::FindSpellCheckWordRanges(text)) {
        const std::wstring word = text.substr(range.start, range.length);
        if (CheckWord(checker_, word)) {
            continue;
        }

        issues.push_back({
            Utf16OffsetForWideIndex(text, range.start),
            Utf16OffsetForWideRange(text, range.start, range.length)
        });
    }

    return issues;
}

std::vector<std::wstring> GtkSpellingService::Suggest(const std::wstring& word, std::size_t limit) const
{
    std::vector<std::wstring> suggestions;
    if (capability_ != classic_notepad::SpellCapability::Available || checker_ == nullptr || word.empty() || limit == 0U) {
        return suggestions;
    }

    const std::string utf8Word = Utf8FromWideForSpelling(word);
    if (utf8Word.empty()) {
        return suggestions;
    }

    char** corrections = spelling_checker_list_corrections(checker_, utf8Word.c_str());
    if (corrections == nullptr) {
        return suggestions;
    }

    for (std::size_t index = 0; corrections[index] != nullptr && suggestions.size() < limit; ++index) {
        suggestions.push_back(WideFromUtf8ForSpelling(corrections[index]));
    }

    g_strfreev(corrections);
    return suggestions;
}

bool GtkSpellingService::IgnoreOnce(const std::wstring& word)
{
    if (capability_ != classic_notepad::SpellCapability::Available || checker_ == nullptr || word.empty()) {
        return false;
    }

    const std::string utf8Word = Utf8FromWideForSpelling(word);
    if (utf8Word.empty()) {
        return false;
    }

    spelling_checker_ignore_word(checker_, utf8Word.c_str());
    InvalidateAll();
    return true;
}

bool GtkSpellingService::AddToDictionary(const std::wstring& word, bool dryRun)
{
    if (capability_ != classic_notepad::SpellCapability::Available || checker_ == nullptr || word.empty()) {
        return false;
    }

    const std::string utf8Word = Utf8FromWideForSpelling(word);
    if (utf8Word.empty()) {
        return false;
    }

    if (!dryRun) {
        spelling_checker_add_word(checker_, utf8Word.c_str());
        InvalidateAll();
    }
    return true;
}

} // namespace classic_notepad::linux_ui
