#include <gtk/gtk.h>
#include <libspelling.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string NormalizeLanguageCode(std::string code)
{
    std::replace(code.begin(), code.end(), '-', '_');
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return code;
}

std::vector<std::string> ListLanguages(SpellingProvider* provider)
{
    std::vector<std::string> languages;
    GPtrArray* languageInfos = provider == nullptr ? nullptr : spelling_provider_list_languages(provider);
    if (languageInfos == nullptr) {
        return languages;
    }

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

std::string ChooseBritishEnglish(SpellingProvider* provider, const std::vector<std::string>& languages)
{
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

bool IsAccepted(SpellingChecker* checker, const char* word)
{
    return spelling_checker_check_word(checker, word, -1) != FALSE;
}

void PrintLanguageList(const std::vector<std::string>& languages)
{
    std::cout << "libspelling languages:";
    if (languages.empty()) {
        std::cout << " <none>";
    }

    for (const std::string& language : languages) {
        std::cout << ' ' << language;
    }
    std::cout << '\n';
}

} // namespace

int main()
{
    if (gtk_init_check() == FALSE) {
        std::cout << "GTK display initialization unavailable; continuing libspelling probe.\n";
    }

    spelling_init();
    SpellingProvider* provider = spelling_provider_get_default();
    if (provider == nullptr) {
        std::cout << "SpellCapability=MissingBackend\n";
        return 0;
    }

    const std::vector<std::string> languages = ListLanguages(provider);
    PrintLanguageList(languages);

    const std::string language = ChooseBritishEnglish(provider, languages);
    if (language.empty()) {
        std::cout << "SpellCapability=MissingDictionary\n";
        return 0;
    }

    SpellingChecker* checker = spelling_checker_new(provider, language.c_str());
    if (checker == nullptr) {
        std::cout << "SpellCapability=MissingDictionary\n";
        return 0;
    }

    spelling_checker_set_language(checker, language.c_str());
    std::cout << "SpellCapability=Available\n";
    std::cout << "SelectedLanguage=" << language << '\n';

    const bool tehAccepted = IsAccepted(checker, "teh");
    const bool colourAccepted = IsAccepted(checker, "colour");
    const bool centreAccepted = IsAccepted(checker, "centre");
    const bool recieveAccepted = IsAccepted(checker, "recieve");
    const bool colorAccepted = IsAccepted(checker, "color");
    const bool centerAccepted = IsAccepted(checker, "center");

    std::cout << "teh=" << (tehAccepted ? "accepted" : "misspelled") << '\n';
    std::cout << "colour=" << (colourAccepted ? "accepted" : "misspelled") << '\n';
    std::cout << "centre=" << (centreAccepted ? "accepted" : "misspelled") << '\n';
    std::cout << "recieve=" << (recieveAccepted ? "accepted" : "misspelled") << '\n';
    std::cout << "color=" << (colorAccepted ? "accepted" : "misspelled") << '\n';
    std::cout << "center=" << (centerAccepted ? "accepted" : "misspelled") << '\n';

    const bool gbExamplesPass = !tehAccepted && !recieveAccepted && colourAccepted && centreAccepted;
    g_object_unref(checker);

    if (!gbExamplesPass) {
        std::cerr << "GB spelling probe failed expected examples.\n";
        return 1;
    }

    return 0;
}
