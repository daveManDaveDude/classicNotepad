#include "document.h"
#include "encoding.h"
#include "line_endings.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failureCount = 0;

void Expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failureCount;
    }
}

void ExpectText(const std::wstring& actual, const std::wstring& expected, const char* message)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failureCount;
    }
}

void ExpectBytes(
    const std::vector<std::uint8_t>& actual,
    std::initializer_list<std::uint8_t> expected,
    const char* message)
{
    if (actual != std::vector<std::uint8_t>(expected)) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failureCount;
    }
}

void WriteBinary(const std::filesystem::path& path, std::initializer_list<std::uint8_t> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const std::vector<std::uint8_t> data(bytes);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<std::uint8_t> ReadBinary(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void TestEncodingDetection()
{
    std::wstring error;
    classic_notepad::DecodeTextResult decoded;

    Expect(
        classic_notepad::DecodeTextBytes({ 0xEFU, 0xBBU, 0xBFU, 'c', 'a', 'f', 0xC3U, 0xA9U }, decoded, error),
        "UTF-8 BOM file decodes");
    Expect(decoded.encoding == classic_notepad::TextEncoding::Utf8Bom, "UTF-8 BOM is detected");
    ExpectText(decoded.text, L"caf\u00E9", "UTF-8 BOM text is decoded");

    Expect(
        classic_notepad::DecodeTextBytes({ 'c', 'a', 'f', 0xC3U, 0xA9U }, decoded, error),
        "UTF-8 without BOM file decodes");
    Expect(decoded.encoding == classic_notepad::TextEncoding::Utf8NoBom, "UTF-8 without BOM is detected");
    ExpectText(decoded.text, L"caf\u00E9", "UTF-8 without BOM text is decoded");

    Expect(
        classic_notepad::DecodeTextBytes({ 0xFFU, 0xFEU, 'A', 0x00U, 0xACU, 0x20U }, decoded, error),
        "UTF-16 LE BOM file decodes");
    Expect(decoded.encoding == classic_notepad::TextEncoding::Utf16LeBom, "UTF-16 LE BOM is detected");
    ExpectText(decoded.text, L"A\u20AC", "UTF-16 LE text is decoded");

    Expect(
        classic_notepad::DecodeTextBytes({ 0xC3U, 0x28U }, decoded, error),
        "Invalid UTF-8 falls back to ANSI");
    Expect(decoded.encoding == classic_notepad::TextEncoding::Ansi, "ANSI fallback is selected for invalid UTF-8");
}

void TestEncodingOutput()
{
    std::wstring error;
    std::vector<std::uint8_t> encoded;

    Expect(
        classic_notepad::EncodeTextBytes(L"caf\u00E9", classic_notepad::TextEncoding::Utf8Bom, encoded, error),
        "UTF-8 BOM text encodes");
    ExpectBytes(encoded, { 0xEFU, 0xBBU, 0xBFU, 'c', 'a', 'f', 0xC3U, 0xA9U }, "UTF-8 BOM is preserved on encode");

    Expect(
        classic_notepad::EncodeTextBytes(L"caf\u00E9", classic_notepad::TextEncoding::Utf8NoBom, encoded, error),
        "UTF-8 text encodes");
    ExpectBytes(encoded, { 'c', 'a', 'f', 0xC3U, 0xA9U }, "UTF-8 without BOM is preserved on encode");

    Expect(
        classic_notepad::EncodeTextBytes(L"A\u20AC", classic_notepad::TextEncoding::Utf16LeBom, encoded, error),
        "UTF-16 LE text encodes");
    ExpectBytes(encoded, { 0xFFU, 0xFEU, 'A', 0x00U, 0xACU, 0x20U }, "UTF-16 LE BOM is preserved on encode");
}

void TestLineEndingAnalysis()
{
    using classic_notepad::LineEndingStyle;

    auto analysis = classic_notepad::AnalyzeLineEndings(L"a\r\nb\r\n");
    Expect(analysis.style == LineEndingStyle::Crlf, "CRLF style is detected");
    Expect(analysis.dominantStyle == LineEndingStyle::Crlf, "CRLF dominant style is detected");

    analysis = classic_notepad::AnalyzeLineEndings(L"a\nb\n");
    Expect(analysis.style == LineEndingStyle::Lf, "LF style is detected");
    Expect(analysis.dominantStyle == LineEndingStyle::Lf, "LF dominant style is detected");

    analysis = classic_notepad::AnalyzeLineEndings(L"a\rb\r");
    Expect(analysis.style == LineEndingStyle::Cr, "CR style is detected");
    Expect(analysis.dominantStyle == LineEndingStyle::Cr, "CR dominant style is detected");

    analysis = classic_notepad::AnalyzeLineEndings(L"a\r\nb\nc\r");
    Expect(analysis.style == LineEndingStyle::Mixed, "Mixed line endings are tracked");
    Expect(analysis.dominantStyle == LineEndingStyle::Crlf, "Mixed tie defaults to CRLF as dominant");
}

void TestLineEndingConversion()
{
    using classic_notepad::LineEndingStyle;

    ExpectText(
        classic_notepad::NormalizeLineEndingsForEditor(L"a\nb\rc\r\n"),
        L"a\r\nb\r\nc\r\n",
        "Editor text is normalized to CRLF");

    ExpectText(
        classic_notepad::ConvertLineEndingsFromEditor(L"a\r\nb\r\n", LineEndingStyle::Crlf),
        L"a\r\nb\r\n",
        "CRLF save preserves CRLF endings");

    ExpectText(
        classic_notepad::ConvertLineEndingsFromEditor(L"a\r\nb\r\n", LineEndingStyle::Lf),
        L"a\nb\n",
        "LF save preserves LF endings");

    ExpectText(
        classic_notepad::ConvertLineEndingsFromEditor(L"a\r\nb\r\n", LineEndingStyle::Cr),
        L"a\rb\r",
        "CR save preserves CR endings");
}

void TestDocumentRoundTrips()
{
    const std::filesystem::path testRoot =
        std::filesystem::temp_directory_path() / L"classic-notepad-phase3-tests";
    std::filesystem::create_directories(testRoot);

    {
        const std::filesystem::path path = testRoot / L"lf-utf8.txt";
        WriteBinary(path, { 'a', '\n', 'b', '\n' });

        Document document;
        std::wstring editorText;
        std::wstring error;

        Expect(document.Load(path.wstring(), editorText, error), "Document loads UTF-8 LF file");
        ExpectText(editorText, L"a\r\nb\r\n", "LF file is normalized for the editor");

        editorText += L"c\r\n";
        Expect(document.Save(editorText, error), "Document saves UTF-8 LF file");
        ExpectBytes(ReadBinary(path), { 'a', '\n', 'b', '\n', 'c', '\n' }, "UTF-8 LF file remains LF after save");
    }

    {
        const std::filesystem::path path = testRoot / L"crlf-utf8.txt";
        WriteBinary(path, { 'a', '\r', '\n', 'b', '\r', '\n' });

        Document document;
        std::wstring editorText;
        std::wstring error;

        Expect(document.Load(path.wstring(), editorText, error), "Document loads UTF-8 CRLF file");
        editorText += L"c\r\n";
        Expect(document.Save(editorText, error), "Document saves UTF-8 CRLF file");
        ExpectBytes(
            ReadBinary(path),
            { 'a', '\r', '\n', 'b', '\r', '\n', 'c', '\r', '\n' },
            "UTF-8 CRLF file remains CRLF after save");
    }

    {
        const std::filesystem::path path = testRoot / L"utf16-lf.txt";
        WriteBinary(path, { 0xFFU, 0xFEU, 'A', 0x00U, '\n', 0x00U, 0xACU, 0x20U });

        Document document;
        std::wstring editorText;
        std::wstring error;

        Expect(document.Load(path.wstring(), editorText, error), "Document loads UTF-16 LE LF file");
        ExpectText(editorText, L"A\r\n\u20AC", "UTF-16 LF file is normalized for the editor");

        editorText += L"\r\nZ";
        Expect(document.Save(editorText, error), "Document saves UTF-16 LE LF file");
        ExpectBytes(
            ReadBinary(path),
            { 0xFFU, 0xFEU, 'A', 0x00U, '\n', 0x00U, 0xACU, 0x20U, '\n', 0x00U, 'Z', 0x00U },
            "UTF-16 LE file remains UTF-16 LE and LF after save");
    }

    std::filesystem::remove_all(testRoot);
}

} // namespace

int main()
{
    TestEncodingDetection();
    TestEncodingOutput();
    TestLineEndingAnalysis();
    TestLineEndingConversion();
    TestDocumentRoundTrips();

    if (g_failureCount != 0) {
        std::cerr << g_failureCount << " test failure(s).\n";
        return 1;
    }

    std::cout << "All text conversion tests passed.\n";
    return 0;
}
