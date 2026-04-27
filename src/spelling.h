#pragma once

#include <cstddef>

namespace classic_notepad {

enum class SpellCapability {
    Available,
    MissingBackend,
    MissingDictionary,
    DisabledByBuild
};

struct SpellIssue {
    std::size_t startUtf16 = 0;
    std::size_t lengthUtf16 = 0;
};

const wchar_t* SpellCapabilityLabel(SpellCapability capability);

} // namespace classic_notepad
