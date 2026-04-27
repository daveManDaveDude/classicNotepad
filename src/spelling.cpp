#include "spelling.h"

namespace classic_notepad {

const wchar_t* SpellCapabilityLabel(SpellCapability capability)
{
    switch (capability) {
    case SpellCapability::Available:
        return L"Available";
    case SpellCapability::MissingBackend:
        return L"MissingBackend";
    case SpellCapability::MissingDictionary:
        return L"MissingDictionary";
    case SpellCapability::DisabledByBuild:
        return L"DisabledByBuild";
    default:
        return L"Unknown";
    }
}

} // namespace classic_notepad
