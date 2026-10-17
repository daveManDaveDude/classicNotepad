#include "windows_spelling_occurrence.h"

namespace classic_notepad::windows {

bool IsIgnoredSpellingOccurrence(
    const std::vector<IgnoredSpellingOccurrence>& ignoredOccurrences,
    const SpellingAnnotation& annotation,
    const SpellingRevision& revision,
    const std::vector<SpellingTextWindow>& windows)
{
    for (const IgnoredSpellingOccurrence& ignored : ignoredOccurrences) {
        if (ignored.revision.session != revision.session || ignored.revision.content != revision.content ||
            ignored.start != annotation.start || ignored.length != annotation.length) continue;
        for (const SpellingTextWindow& window : windows) {
            if (annotation.start < window.absoluteStart) continue;
            const std::uint64_t relative64 = annotation.start - window.absoluteStart;
            if (relative64 > window.text.size()) continue;
            const std::size_t relative = static_cast<std::size_t>(relative64);
            if (annotation.length > window.text.size() - relative) continue;
            if (window.text.compare(relative, static_cast<std::size_t>(annotation.length), ignored.text) == 0) {
                return true;
            }
        }
    }
    return false;
}

} // namespace classic_notepad::windows
