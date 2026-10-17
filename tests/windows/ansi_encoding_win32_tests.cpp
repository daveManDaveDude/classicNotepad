#include "ansi_encoding.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void TestUtf8CodePageFallback()
{
    using classic_notepad::DecodeAnsiBytesForCodePage;
    using classic_notepad::EncodeAnsiTextForCodePage;

    constexpr std::uint32_t kUtf8CodePage = 65001U;
    const std::vector<std::uint8_t> malformed{ 0xC3U, 0x28U };
    std::wstring decoded = L"stale output";
    Expect(!DecodeAnsiBytesForCodePage(malformed.data(), malformed.size(), kUtf8CodePage, decoded),
        "UTF-8 ACP fallback rejects a malformed sequence instead of replacing it");
    Expect(decoded.empty(), "Rejected UTF-8 ACP input leaves no partial decoded output");

    const std::vector<std::uint8_t> valid{ 0xC3U, 0xA9U };
    Expect(DecodeAnsiBytesForCodePage(valid.data(), valid.size(), kUtf8CodePage, decoded) && decoded == L"\u00E9",
        "UTF-8 ACP fallback decodes a valid multibyte scalar");

    std::vector<std::uint8_t> encoded;
    std::wstring error;
    Expect(EncodeAnsiTextForCodePage(L"\u00E9", kUtf8CodePage, encoded, error) && encoded == valid,
        "UTF-8 ACP encoding round-trips exactly through its explicit code-page seam");
}

void TestShiftJisCodePageFallback()
{
    using classic_notepad::DecodeAnsiBytesForCodePage;
    using classic_notepad::EncodeAnsiTextForCodePage;

    constexpr std::uint32_t kShiftJisCodePage = 932U;
    const std::vector<std::uint8_t> valid{ 0x82U, 0xA0U };
    std::wstring decoded;
    Expect(DecodeAnsiBytesForCodePage(valid.data(), valid.size(), kShiftJisCodePage, decoded) &&
            decoded == L"\u3042",
        "Shift-JIS fallback accepts and exactly decodes a valid multibyte character");

    std::vector<std::uint8_t> encoded;
    std::wstring error;
    Expect(EncodeAnsiTextForCodePage(L"\u3042", kShiftJisCodePage, encoded, error) && encoded == valid,
        "Shift-JIS encoding round-trips exact multibyte bytes");

    for (const std::vector<std::uint8_t>& malformed : {
             std::vector<std::uint8_t>{ 0x82U },
             std::vector<std::uint8_t>{ 0x82U, 0x20U } }) {
        decoded = L"stale output";
        Expect(!DecodeAnsiBytesForCodePage(malformed.data(), malformed.size(), kShiftJisCodePage, decoded),
            "Shift-JIS fallback rejects a truncated or malformed multibyte sequence");
        Expect(decoded.empty(), "Rejected Shift-JIS input leaves no partial decoded output");
    }
}

} // namespace

int main()
{
    TestUtf8CodePageFallback();
    TestShiftJisCodePageFallback();
    if (failures == 0) {
        std::cout << "All Windows ANSI encoding tests passed.\n";
        return 0;
    }
    std::cerr << failures << " Windows ANSI encoding test(s) failed.\n";
    return 1;
}
