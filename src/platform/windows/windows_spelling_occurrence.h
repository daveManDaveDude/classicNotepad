#pragma once

#include "../../spell_text_utils.h"

#include <cstdint>
#include <string>
#include <vector>

namespace classic_notepad::windows {

struct IgnoredSpellingOccurrence {
    SpellingRevision revision;
    std::uint64_t start = 0;
    std::uint64_t length = 0;
    std::wstring text;
};

bool IsIgnoredSpellingOccurrence(
    const std::vector<IgnoredSpellingOccurrence>& ignoredOccurrences,
    const SpellingAnnotation& annotation,
    const SpellingRevision& revision,
    const std::vector<SpellingTextWindow>& windows);

} // namespace classic_notepad::windows
