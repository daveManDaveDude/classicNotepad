#pragma once

#include "spelling.h"

#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <libspelling.h>

#include <cstddef>
#include <string>
#include <vector>

namespace classic_notepad::linux_ui {

class GtkSpellingService {
public:
    GtkSpellingService();
    ~GtkSpellingService();

    GtkSpellingService(const GtkSpellingService&) = delete;
    GtkSpellingService& operator=(const GtkSpellingService&) = delete;

    classic_notepad::SpellCapability Capability() const;
    const std::string& LanguageCode() const;
    const std::vector<std::string>& AvailableLanguages() const;

    GtkWidget* CreatePlainTextView() const;
    void Attach(GtkTextBuffer* buffer);
    GMenuModel* ContextMenuModel() const;
    void InvalidateAll();

    std::vector<classic_notepad::SpellIssue> CheckText(const std::wstring& text) const;
    std::vector<std::wstring> Suggest(const std::wstring& word, std::size_t limit) const;
    bool IgnoreOnce(const std::wstring& word);
    bool AddToDictionary(const std::wstring& word, bool dryRun);

private:
    SpellingProvider* provider_ = nullptr;
    SpellingChecker* checker_ = nullptr;
    SpellingTextBufferAdapter* adapter_ = nullptr;
    classic_notepad::SpellCapability capability_ = classic_notepad::SpellCapability::MissingBackend;
    std::string languageCode_;
    std::vector<std::string> availableLanguages_;
};

} // namespace classic_notepad::linux_ui
