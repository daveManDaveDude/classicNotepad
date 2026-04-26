#include "ansi_encoding.h"

#include <array>
#include <cstdint>

namespace classic_notepad {
namespace {

constexpr wchar_t kUndefined = L'\0';

constexpr std::array<wchar_t, 32> kWindows1252Controls{
    L'\u20AC', kUndefined, L'\u201A', L'\u0192', L'\u201E', L'\u2026', L'\u2020', L'\u2021',
    L'\u02C6', L'\u2030', L'\u0160', L'\u2039', L'\u0152', kUndefined, L'\u017D', kUndefined,
    kUndefined, L'\u2018', L'\u2019', L'\u201C', L'\u201D', L'\u2022', L'\u2013', L'\u2014',
    L'\u02DC', L'\u2122', L'\u0161', L'\u203A', L'\u0153', kUndefined, L'\u017E', L'\u0178'
};

} // namespace

bool DecodeAnsiBytes(const std::uint8_t* data, std::size_t size, std::wstring& text)
{
    text.clear();
    text.reserve(size);

    for (std::size_t index = 0; index < size; ++index) {
        const std::uint8_t byte = data[index];
        if (byte >= 0x80U && byte <= 0x9FU) {
            const wchar_t mapped = kWindows1252Controls[byte - 0x80U];
            text.push_back(mapped == kUndefined ? static_cast<wchar_t>(byte) : mapped);
        } else {
            text.push_back(static_cast<wchar_t>(byte));
        }
    }

    return true;
}

bool EncodeAnsiText(const std::wstring& text, std::vector<std::uint8_t>& bytes, std::wstring& errorMessage)
{
    bytes.clear();
    bytes.reserve(text.size());

    for (const wchar_t character : text) {
        if (character <= 0x7FU || (character >= 0xA0U && character <= 0xFFU)) {
            bytes.push_back(static_cast<std::uint8_t>(character));
            continue;
        }

        bool encoded = false;
        for (std::size_t index = 0; index < kWindows1252Controls.size(); ++index) {
            if (kWindows1252Controls[index] == character) {
                bytes.push_back(static_cast<std::uint8_t>(0x80U + index));
                encoded = true;
                break;
            }
        }

        if (!encoded) {
            errorMessage =
                L"The document contains characters that cannot be saved in the portable ANSI fallback encoding.";
            return false;
        }
    }

    return true;
}

} // namespace classic_notepad
