#pragma once

#include <cstddef>
#include <string_view>

namespace classic_notepad {

// Validates the UTF-16 surrogate-unit pairing used by the editable document.
// Other code units, including controls and embedded NUL, retain the existing
// LargeDocument replacement policy.
inline bool IsValidInsertedText(std::wstring_view text) noexcept
{
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (text[index] >= 0xD800 && text[index] <= 0xDBFF) {
            if (index + 1U >= text.size() ||
                text[index + 1U] < 0xDC00 || text[index + 1U] > 0xDFFF) {
                return false;
            }
            ++index;
        } else if (text[index] >= 0xDC00 && text[index] <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

} // namespace classic_notepad
