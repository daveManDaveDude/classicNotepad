#include "document.h"
#include "editor_text_semantics.h"
#include "appearance.h"
#include "encoding.h"
#include "line_endings.h"
#include "large_file_index.h"
#include "large_document.h"
#include "large_file_scroll.h"
#include "spelling.h"
#include "spell_text_utils.h"
#include "text_metadata.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failureCount = 0;

classic_notepad::LargeFileWidthSummary PlainWidthSummary(std::uint64_t units)
{
    classic_notepad::LargeFileWidthSummary summary;
    summary.suffixColumns = units;
    summary.suffixResidue = static_cast<std::uint8_t>(units % 8U);
    summary.maximumLineColumns = units;
    for (std::size_t residue = 0; residue < summary.prefixColumns.size(); ++residue) {
        summary.prefixColumns[residue] = units;
        summary.prefixResidues[residue] = static_cast<std::uint8_t>((residue + units % 8U) % 8U);
    }
    return summary;
}

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
        classic_notepad::DecodeTextBytes(
            { 0xFFU, 0xFEU, 0x3DU, 0xD8U, 0x00U, 0xDEU }, decoded, error),
        "UTF-16 LE supplementary scalar decodes");
    Expect(decoded.text == std::wstring(L"\U0001F600"), "UTF-16 LE supplementary scalar remains intact");
    Expect(
        !classic_notepad::DecodeTextBytes({ 0xFFU, 0xFEU, 0x00U, 0xD8U }, decoded, error) && !error.empty(),
        "UTF-16 LE rejects an unmatched high surrogate");
    Expect(
        !classic_notepad::DecodeTextBytes({ 0xFFU, 0xFEU, 0x00U, 0xDCU }, decoded, error) && !error.empty(),
        "UTF-16 LE rejects an unmatched low surrogate");
    Expect(
        !classic_notepad::DecodeTextBytes({ 0xFFU, 0xFEU, 'A' }, decoded, error) && !error.empty(),
        "UTF-16 LE rejects a truncated code unit");
    Expect(
        !classic_notepad::DecodeTextBytes({ 0xEFU, 0xBBU, 0xBFU, 0xF0U, 0x9FU, 0x98U }, decoded, error) &&
            !error.empty(),
        "UTF-8 BOM input rejects a truncated supplementary scalar");

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

    Expect(
        classic_notepad::EncodeTextBytes(
            std::wstring(L"\U0001F600"), classic_notepad::TextEncoding::Utf8NoBom, encoded, error),
        "Supplementary scalar encodes as UTF-8");
    ExpectBytes(encoded, { 0xF0U, 0x9FU, 0x98U, 0x80U }, "Supplementary scalar encodes without changing text");

    const std::wstring malformed = std::wstring(1U, static_cast<wchar_t>(0xD800U));
    Expect(
        !classic_notepad::EncodeTextBytes(malformed, classic_notepad::TextEncoding::Utf8NoBom, encoded, error) &&
            !error.empty(),
        "UTF-8 output rejects an unmatched surrogate");

    error.clear();
    Expect(
        classic_notepad::EncodeTextBytes(L"A", classic_notepad::TextEncoding::Ansi, encoded, error),
        "ANSI output accepts a losslessly representable character");
    ExpectBytes(encoded, { 'A' }, "ANSI output retains the exact ASCII byte");
    error.clear();
    if (classic_notepad::EncodeTextBytes(L"\u20AC", classic_notepad::TextEncoding::Ansi, encoded, error)) {
        classic_notepad::DecodeTextResult roundTrip;
        std::wstring roundTripError;
        Expect(classic_notepad::DecodeTextBytes(encoded, roundTrip, roundTripError) &&
                roundTrip.text == L"\u20AC",
            "ANSI output round-trips any character supported by the active system code page");
    } else {
        Expect(!error.empty(), "ANSI conversion reports when a character is unavailable in the active code page");
    }
    error.clear();
    const bool emojiEncoded = classic_notepad::EncodeTextBytes(
        L"\U0001F600", classic_notepad::TextEncoding::Ansi, encoded, error);
    if (emojiEncoded) {
        classic_notepad::DecodeTextResult roundTrip;
        std::wstring roundTripError;
        Expect(classic_notepad::DecodeTextBytes(encoded, roundTrip, roundTripError) &&
                roundTrip.text == L"\U0001F600",
            "ANSI output round-trips supplementary text when the active code page can represent it");
    } else {
        Expect(!error.empty(), "ANSI conversion reports characters unavailable in the active code page");
    }
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

    analysis = classic_notepad::AnalyzeLineEndingCounts(0U, 4U, 4U);
    Expect(analysis.style == LineEndingStyle::Mixed && analysis.dominantStyle == LineEndingStyle::Lf,
        "LF and CR count ties use the same LF dominant policy as ordinary text analysis");
    analysis = classic_notepad::AnalyzeLineEndingCounts(0U, 3U, 4U);
    Expect(analysis.style == LineEndingStyle::Mixed && analysis.dominantStyle == LineEndingStyle::Cr,
        "CR wins a strict count majority");
    analysis = classic_notepad::AnalyzeLineEndingCounts(
        (std::numeric_limits<std::uint64_t>::max)(),
        (std::numeric_limits<std::uint64_t>::max)(),
        0U);
    Expect(analysis.style == LineEndingStyle::Mixed && analysis.dominantStyle == LineEndingStyle::Crlf,
        "64-bit scanner count ties retain CRLF priority without summing counts");
    analysis = classic_notepad::AnalyzeLineEndingCounts(0U, 0U, 0U);
    Expect(analysis.style == LineEndingStyle::Crlf && analysis.dominantStyle == LineEndingStyle::Crlf,
        "Files without line endings use CRLF as their save default");
}

void TestLineEndingConversion()
{
    using classic_notepad::LineEndingStyle;

    ExpectText(
        classic_notepad::NormalizeLineEndingsForEditor(L"a\nb\rc\r\n"),
        L"a\r\nb\r\nc\r\n",
        "Editor text is normalized to CRLF");

    ExpectText(
        classic_notepad::NormalizeLineEndingsForEditor(L"one\ntwo\nthree"),
        L"one\r\ntwo\r\nthree",
        "LF-only pasted text is normalized to editor line endings");

    for (const std::wstring input : { L"", L"plain", L"a\nb", L"a\rb", L"a\r\nb", L"a\r\nb\nc\r" }) {
        std::wstring inPlace = input;
        classic_notepad::NormalizeLineEndingsForEditorInPlace(inPlace);
        ExpectText(
            inPlace,
            classic_notepad::NormalizeLineEndingsForEditor(input),
            "In-place editor line-ending normalization matches copy normalization");
    }

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

void TestDocumentMetadata()
{
    using classic_notepad::LineEndingStyle;
    using classic_notepad::TextEncoding;

    Document newDocument;
    newDocument.ResetUntitled();
    Expect(newDocument.Encoding() == TextEncoding::Utf8NoBom, "New document reports UTF-8");
    Expect(newDocument.LineEnding() == LineEndingStyle::Crlf, "New document reports CRLF");
    Expect(newDocument.SaveLineEnding() == LineEndingStyle::Crlf, "New document saves as CRLF");

    const std::filesystem::path testRoot =
        std::filesystem::temp_directory_path() / L"classic-notepad-metadata-tests";
    std::filesystem::create_directories(testRoot);

    {
        const std::filesystem::path path = testRoot / L"lf-utf8.txt";
        WriteBinary(path, { 'a', '\n', 'b', '\n' });

        Document document;
        std::wstring editorText;
        std::wstring error;

        Expect(document.Load(path.wstring(), editorText, error), "Document metadata loads LF file");
        Expect(document.Encoding() == TextEncoding::Utf8NoBom, "LF file reports UTF-8");
        Expect(document.LineEnding() == LineEndingStyle::Lf, "LF file reports Unix line endings");
        Expect(document.SaveLineEnding() == LineEndingStyle::Lf, "LF file save style remains LF");
    }

    {
        const std::filesystem::path path = testRoot / L"bom-utf8.txt";
        WriteBinary(path, { 0xEFU, 0xBBU, 0xBFU, 'a', '\r', '\n' });

        Document document;
        std::wstring editorText;
        std::wstring error;

        Expect(document.Load(path.wstring(), editorText, error), "Document metadata loads UTF-8 BOM file");
        Expect(document.Encoding() == TextEncoding::Utf8Bom, "UTF-8 BOM file reports UTF-8 with BOM");
        Expect(document.LineEnding() == LineEndingStyle::Crlf, "UTF-8 BOM file reports CRLF");
    }

    {
        const std::filesystem::path path = testRoot / L"utf16-le.txt";
        WriteBinary(path, { 0xFFU, 0xFEU, 'A', 0x00U, '\r', 0x00U, '\n', 0x00U });

        Document document;
        std::wstring editorText;
        std::wstring error;

        Expect(document.Load(path.wstring(), editorText, error), "Document metadata loads UTF-16 LE file");
        Expect(document.Encoding() == TextEncoding::Utf16LeBom, "UTF-16 LE file reports UTF-16 LE");
        Expect(document.LineEnding() == LineEndingStyle::Crlf, "UTF-16 LE file reports CRLF");
    }

    {
        const std::filesystem::path sourcePath = testRoot / L"mapped-source.txt";
        const std::filesystem::path savedAsPath = testRoot / L"mapped-save-as.txt";
        Document document;
        document.SetModified(true);
        Document::StagedLargeLoad staged;
        document.PrepareReadOnlyLargeFile(
            sourcePath.wstring(), TextEncoding::Utf8Bom, LineEndingStyle::Mixed,
            LineEndingStyle::Lf, staged);
        Expect(document.Path().empty() && document.IsModified(),
            "Staging mapped source metadata does not publish over the active document");
        document.CommitReadOnlyLargeFile(std::move(staged));
        Expect(document.Path() == sourcePath.wstring() && document.Encoding() == TextEncoding::Utf8Bom &&
                document.LineEnding() == LineEndingStyle::Mixed && document.SaveLineEnding() == LineEndingStyle::Lf &&
                !document.IsModified(),
            "Committing mapped source metadata publishes encoding, observed endings, and dominant save policy");
        document.SetModified(true);
        document.AdoptLargeFileSaveAs(savedAsPath.wstring());
        Expect(document.Path() == savedAsPath.wstring() && document.Encoding() == TextEncoding::Utf8Bom &&
                document.LineEnding() == LineEndingStyle::Mixed && document.SaveLineEnding() == LineEndingStyle::Lf &&
                document.IsModified(),
            "Mapped Save As adopts only the new path and retains source metadata and current dirty state");
    }

    std::filesystem::remove_all(testRoot);
}

void TestDocumentLoadAndSaveFailures()
{
    const std::filesystem::path testRoot =
        std::filesystem::temp_directory_path() / L"classic-notepad-a6-integrity-tests";
    std::filesystem::create_directories(testRoot);
    const std::filesystem::path originalPath = testRoot / L"original.txt";
    const std::filesystem::path nulPath = testRoot / L"embedded-nul.txt";
    const std::filesystem::path malformedUtf16Path = testRoot / L"malformed-utf16.txt";
    WriteBinary(originalPath, { 'a', '\r', '\n', 'b', '\r', '\n' });
    WriteBinary(nulPath, { 'a', 0U, 'b' });
    WriteBinary(malformedUtf16Path, { 0xFFU, 0xFEU, 0x00U, 0xD8U });

    Document stagedDocument;
    stagedDocument.ResetNewFile((testRoot / L"prior.txt").wstring());
    stagedDocument.SetModified(true);
    Document::StagedLoad validStage;
    std::wstring stageError;
    Expect(stagedDocument.PrepareLoad(originalPath.wstring(), validStage, stageError),
        "A valid ordinary load can be staged before committing metadata");
    Expect(stagedDocument.Path() == (testRoot / L"prior.txt").wstring() && stagedDocument.IsModified(),
        "Preparing an ordinary load leaves the current path and dirty state untouched");
    stagedDocument.CommitLoad(std::move(validStage));
    Expect(stagedDocument.Path() == originalPath.wstring() && !stagedDocument.IsModified(),
        "Committing an adopted staged load publishes its metadata atomically");

    Document document;
    std::wstring editorText;
    std::wstring error;
    Expect(document.Load(originalPath.wstring(), editorText, error), "A6 fixture opens a valid source document");
    document.SetModified(true);
    const std::wstring previousPath = document.Path();
    const auto previousEncoding = document.Encoding();
    const auto previousLineEnding = document.LineEnding();
    const auto previousSaveLineEnding = document.SaveLineEnding();

    Document::StagedLoad staged;
    Expect(!document.PrepareLoad(nulPath.wstring(), staged, error) && !error.empty(),
        "Ordinary staging rejects embedded NUL before native control adoption");
    Expect(document.Path() == previousPath && document.Encoding() == previousEncoding &&
            document.LineEnding() == previousLineEnding && document.SaveLineEnding() == previousSaveLineEnding &&
            document.IsModified(),
        "Failed staged NUL load preserves path, encoding, line endings, and dirty state");
    Expect(!document.Load(malformedUtf16Path.wstring(), editorText, error) && !error.empty(),
        "Ordinary load rejects a malformed UTF-16 source");
    Expect(document.Path() == previousPath && document.IsModified(),
        "Malformed UTF-16 load preserves the previously open document metadata and dirty state");

    const std::vector<std::uint8_t> originalBytes = ReadBinary(originalPath);
    const std::wstring malformedText(1U, static_cast<wchar_t>(0xD800U));
    Expect(!document.Save(malformedText, error) && !error.empty(),
        "Save refuses malformed Unicode before replacing its destination");
    Expect(ReadBinary(originalPath) == originalBytes && document.Path() == previousPath &&
            document.Encoding() == previousEncoding && document.LineEnding() == previousLineEnding &&
            document.SaveLineEnding() == previousSaveLineEnding && document.IsModified(),
        "Malformed Unicode save preserves output bytes, path, encoding, line-ending policy, and dirty state");

    document.ResetUntitled();
    std::filesystem::remove_all(testRoot);
}

void TestTextMetadataFormatting()
{
    using classic_notepad::LineEndingStyle;
    using classic_notepad::TextEncoding;

    ExpectText(classic_notepad::FormatEncoding(TextEncoding::Utf8NoBom), L"UTF-8", "UTF-8 label formats");
    ExpectText(
        classic_notepad::FormatEncoding(TextEncoding::Utf8Bom),
        L"UTF-8 with BOM",
        "UTF-8 BOM label formats");
    ExpectText(classic_notepad::FormatEncoding(TextEncoding::Utf16LeBom), L"UTF-16 LE", "UTF-16 label formats");
    ExpectText(classic_notepad::FormatEncoding(TextEncoding::Ansi), L"ANSI", "ANSI label formats");

    ExpectText(
        classic_notepad::FormatLineEnding(LineEndingStyle::Crlf),
        L"Windows (CRLF)",
        "CRLF label formats");
    ExpectText(classic_notepad::FormatLineEnding(LineEndingStyle::Lf), L"Unix (LF)", "LF label formats");
    ExpectText(classic_notepad::FormatLineEnding(LineEndingStyle::Cr), L"Macintosh (CR)", "CR label formats");
    ExpectText(classic_notepad::FormatLineEnding(LineEndingStyle::Mixed), L"Mixed", "Mixed label formats");

    ExpectText(classic_notepad::FormatNumberWithSeparators(0U), L"0", "Zero number formats");
    ExpectText(classic_notepad::FormatNumberWithSeparators(1234567U), L"1,234,567", "Large number formats");
    ExpectText(classic_notepad::FormatCharacterCount(1U), L"1 character", "Singular character count formats");
    ExpectText(classic_notepad::FormatCharacterCount(1000U), L"1,000 characters", "Plural character count formats");

    Expect(classic_notepad::CountStatusCharacters(L"a\r\nb") == 3U, "CRLF counts as one status character");
    Expect(
        classic_notepad::CountStatusCharacters(std::wstring(L"a") + wchar_t(0xD83D) + wchar_t(0xDE00) + L"b") == 3U,
        "UTF-16 surrogate pair counts as one status character");
    const std::wstring mixedCountText = std::wstring(L"A") + wchar_t(0xD83D) + wchar_t(0xDE00) +
        L"\t\r\nB\rC\n";
    Expect(classic_notepad::CountStatusCharacters(mixedCountText) == 8U,
        "Displayed count treats tabs, mixed line endings, and one supplementary scalar consistently");
}

void TestDocumentNewFilePath()
{
    const std::filesystem::path testRoot =
        std::filesystem::temp_directory_path() / L"classic-notepad-new-file-tests";
    std::filesystem::create_directories(testRoot);

    const std::filesystem::path path = testRoot / L"missing.txt";
    std::filesystem::remove(path);

    Document document;
    document.ResetNewFile(path.wstring());

    Expect(document.HasPath(), "New file document stores the requested path");
    ExpectText(document.Path(), path.wstring(), "New file document exposes the requested path");
    ExpectText(document.DisplayName(), L"missing.txt", "New file document display name uses the file name");
    Expect(!document.IsModified(), "New file document starts unmodified");
    Expect(!std::filesystem::exists(path), "New file document does not create the file before saving");

    std::wstring error;
    Expect(document.Save(L"", error), "New file document saves to the requested path");
    Expect(std::filesystem::exists(path), "New file document creates the file on save");
    ExpectBytes(ReadBinary(path), {}, "Empty new file saves as an empty file");

    std::filesystem::remove_all(testRoot);
}

void TestLargeFileStreamingIndex()
{
    using classic_notepad::LargeFileEncoding;
    using classic_notepad::LargeFileLineScanner;

    {
        LargeFileLineScanner scanner(LargeFileEncoding::Utf8, 0);
        std::vector<std::uint64_t> lines{0};
        const std::vector<std::uint8_t> first{ 'o', 'n', 'e', '\r' };
        const std::vector<std::uint8_t> second{ '\n', 't', 'w', 'o', '\n' };
        Expect(scanner.Consume(first.data(), first.size(), 0, lines), "Large scanner accepts first chunk");
        Expect(scanner.Consume(second.data(), second.size(), 4, lines), "Large scanner accepts CRLF continuation chunk");
        Expect(scanner.Finish(lines), "Large scanner finishes CRLF input");
        Expect(lines == std::vector<std::uint64_t>({0, 5, 9}), "CRLF split across chunks has correct offsets");
        Expect(scanner.CrlfCount() == 1U && scanner.LfCount() == 1U, "CRLF/LF counts cross chunk boundaries");
        Expect(scanner.MaximumLineColumns() == 3U, "Large scanner tracks the longest decoded line");
    }

    {
        LargeFileLineScanner scanner(LargeFileEncoding::Utf8, 0);
        std::vector<std::uint64_t> lines{0};
        const std::vector<std::uint8_t> text{'a', '\t', 'b', '\r', 'x', 'y'};
        Expect(scanner.Consume(text.data(), text.size(), 0, lines) && scanner.Finish(lines),
            "Large scanner accepts tabs and a lone CR line ending");
        Expect(lines == std::vector<std::uint64_t>({0, 4}), "Lone CR ends its line before following text");
        Expect(scanner.MaximumLineColumns() == 9U, "Longest-line columns expand tabs to eight-column stops");
    }

    {
        LargeFileLineScanner scanner(LargeFileEncoding::Utf8, 0);
        std::vector<std::uint64_t> lines{0};
        const std::vector<std::uint8_t> first{ 'a', 0xE2U };
        const std::vector<std::uint8_t> second{ 0x82U, 0xACU, '\n', 'b' };
        Expect(scanner.Consume(first.data(), first.size(), 0, lines), "UTF-8 split lead byte is retained");
        Expect(scanner.Consume(second.data(), second.size(), 2, lines), "UTF-8 split continuation bytes are accepted");
        Expect(scanner.Finish(lines), "UTF-8 split sequence finishes validly");
        Expect(lines == std::vector<std::uint64_t>({0, 5}), "UTF-8 split sequence line offset is correct");
    }

    {
        const std::vector<std::uint8_t> utf8Bom{0xEFU, 0xBBU, 0xBFU, 'x'};
        const std::vector<std::uint8_t> utf16Bom{0xFFU, 0xFEU, 'x', 0};
        Expect(
            classic_notepad::DetectLargeFileEncoding(utf8Bom.data(), utf8Bom.size()).dataOffset == 3U,
            "Large scanner recognizes UTF-8 BOM");
        const auto utf16 = classic_notepad::DetectLargeFileEncoding(utf16Bom.data(), utf16Bom.size());
        Expect(utf16.encoding == LargeFileEncoding::Utf16Le && utf16.dataOffset == 2U, "Large scanner recognizes UTF-16 LE BOM");
    }

    {
        LargeFileLineScanner scanner(LargeFileEncoding::Utf16Le, 0);
        std::vector<std::uint64_t> lines{0};
        const std::vector<std::uint8_t> first{ 'a', 0, '\r' };
        const std::vector<std::uint8_t> second{ 0, '\n', 0, 'b', 0 };
        Expect(scanner.Consume(first.data(), first.size(), 0, lines), "UTF-16 odd chunk is retained");
        Expect(scanner.Consume(second.data(), second.size(), 3, lines), "UTF-16 CRLF split is accepted");
        Expect(scanner.Finish(lines), "UTF-16 scanner finishes validly");
        Expect(lines == std::vector<std::uint64_t>({0, 6}), "UTF-16 CRLF offset is correct");
    }

    {
        LargeFileLineScanner nulScanner(LargeFileEncoding::Utf8, 0);
        std::vector<std::uint64_t> nulLines{0};
        const std::vector<std::uint8_t> nulText{ 'a', 0U, 'b' };
        Expect(!nulScanner.Consume(nulText.data(), nulText.size(), 0, nulLines),
            "Mapped UTF-8 scanner rejects embedded NUL before document adoption");

        LargeFileLineScanner invalidUtf8(LargeFileEncoding::Utf8, 0);
        std::vector<std::uint64_t> invalidUtf8Lines{0};
        const std::vector<std::uint8_t> surrogateUtf8{ 0xEDU, 0xA0U, 0x80U };
        Expect(!invalidUtf8.Consume(surrogateUtf8.data(), surrogateUtf8.size(), 0, invalidUtf8Lines),
            "Mapped UTF-8 scanner rejects encodings of surrogate code points");

        LargeFileLineScanner invalidUtf16(LargeFileEncoding::Utf16Le, 0);
        std::vector<std::uint64_t> invalidUtf16Lines{0};
        const std::vector<std::uint8_t> unpairedHigh{ 0x00U, 0xD8U };
        Expect(invalidUtf16.Consume(unpairedHigh.data(), unpairedHigh.size(), 0, invalidUtf16Lines) &&
                !invalidUtf16.Finish(invalidUtf16Lines),
            "Mapped UTF-16 scanner rejects an unpaired high surrogate at end of file");
    }

    {
        constexpr std::uint64_t nearMaximum = (std::numeric_limits<std::uint64_t>::max)() - 4U;
        LargeFileLineScanner scanner(LargeFileEncoding::Utf8, nearMaximum);
        std::vector<std::uint64_t> lines{nearMaximum};
        const std::vector<std::uint8_t> text{ 'x', '\n' };
        Expect(scanner.Consume(text.data(), text.size(), nearMaximum, lines), "Large scanner supports near-64-bit-max offsets");
        Expect(scanner.Finish(lines), "Large scanner finishes near-64-bit-max offsets");
        Expect(lines.back() == nearMaximum + 2U, "Large scanner preserves 64-bit offset values");

        LargeFileLineScanner overflowScanner(LargeFileEncoding::Utf8, (std::numeric_limits<std::uint64_t>::max)() - 1U);
        std::vector<std::uint64_t> overflowLines{(std::numeric_limits<std::uint64_t>::max)() - 1U};
        Expect(!overflowScanner.Consume(text.data(), text.size(), (std::numeric_limits<std::uint64_t>::max)() - 1U, overflowLines),
            "Large scanner rejects overflowing chunk range");
    }

    {
        LargeFileLineScanner abandoned(LargeFileEncoding::Utf8, 0);
        std::vector<std::uint64_t> abandonedLines{0};
        const std::vector<std::uint8_t> crOnly{'a', '\r'};
        Expect(abandoned.Consume(crOnly.data(), crOnly.size(), 0, abandonedLines), "Partial old scan accepts CR");
        LargeFileLineScanner replacement(LargeFileEncoding::Utf8, 0);
        std::vector<std::uint64_t> replacementLines{0};
        const std::vector<std::uint8_t> newText{'b', '\n'};
        Expect(replacement.Consume(newText.data(), newText.size(), 0, replacementLines) && replacement.Finish(replacementLines),
            "Replacement scan owns independent state");
        Expect(replacementLines == std::vector<std::uint64_t>({0, 2}), "Replacement scan does not inherit pending CR state");
    }
}

void TestLargeFileScannerCoordinateMatrix()
{
    using classic_notepad::LargeFileEncoding;
    using classic_notepad::LargeFileLineScanner;

    struct Case {
        const char* name;
        std::vector<std::uint8_t> utf8;
        std::vector<std::uint16_t> utf16;
        std::vector<std::uint64_t> utf8BreakBytes;
        std::vector<std::uint64_t> utf16BreakBytes;
        std::vector<std::uint64_t> breakCharacters;
        std::uint64_t characters;
        std::uint64_t crlf;
        std::uint64_t lf;
        std::uint64_t cr;
    };
    const std::vector<Case> cases{
        {"CR-only", {'a', '\r', 'b'}, {'a', '\r', 'b'}, {2U}, {4U}, {2U}, 3U, 0U, 0U, 1U},
        {"CRLF", {'a', '\r', '\n', 'b'}, {'a', '\r', '\n', 'b'}, {3U}, {6U}, {3U}, 4U, 1U, 0U, 0U},
        {"LF", {'a', '\n', 'b'}, {'a', '\n', 'b'}, {2U}, {4U}, {2U}, 3U, 0U, 1U, 0U},
        {"mixed endings", {'x', '\r', 'y', '\n', 'z', '\r', '\n', 'w'},
            {'x', '\r', 'y', '\n', 'z', '\r', '\n', 'w'}, {2U, 4U, 7U}, {4U, 8U, 14U},
            {2U, 4U, 7U}, 8U, 1U, 1U, 1U},
        {"consecutive CR", {'a', '\r', '\r', 'b'}, {'a', '\r', '\r', 'b'}, {2U, 3U}, {4U, 6U},
            {2U, 3U}, 4U, 0U, 0U, 2U},
        {"trailing CR", {'a', '\r'}, {'a', '\r'}, {2U}, {4U}, {2U}, 2U, 0U, 0U, 1U},
        {"no final newline", {'a'}, {'a'}, {}, {}, {}, 1U, 0U, 0U, 0U},
        {"emoji after CR", {'a', '\r', 0xF0U, 0x9FU, 0x98U, 0x80U, 'b'},
            {'a', '\r', 0xD83DU, 0xDE00U, 'b'}, {2U}, {4U}, {2U}, 5U, 0U, 0U, 1U},
    };
    const std::vector<std::vector<std::size_t>> partitions{{1U}, {2U}, {3U}, {1U, 2U, 3U, 4U, 5U}, {7U, 1U, 4U, 2U}};
    constexpr std::uint64_t dataOffset = 11U;

    const auto verify = [&](const Case& test, LargeFileEncoding encoding,
        const std::vector<std::uint8_t>& payload,
        const std::vector<std::uint64_t>& relativeBreakBytes) {
        for (const std::vector<std::size_t>& chunkSizes : partitions) {
            LargeFileLineScanner scanner(encoding, dataOffset);
            std::vector<std::uint64_t> lineOffsets{dataOffset};
            std::vector<std::uint64_t> lineCharacterOffsets;
            std::size_t position = 0;
            std::size_t chunkIndex = 0;
            bool accepted = true;
            while (position < payload.size()) {
                const std::size_t requested = chunkSizes[chunkIndex++ % chunkSizes.size()];
                const std::size_t length = std::min(requested, payload.size() - position);
                if (!scanner.Consume(payload.data() + position, length, dataOffset + position,
                        lineOffsets, &lineCharacterOffsets)) {
                    accepted = false;
                    break;
                }
                position += length;
            }
            accepted = accepted && scanner.Finish(lineOffsets, &lineCharacterOffsets);

            std::vector<std::uint64_t> expectedLineOffsets{dataOffset};
            for (const std::uint64_t offset : relativeBreakBytes) expectedLineOffsets.push_back(dataOffset + offset);
            const std::string label = std::string(test.name) + (encoding == LargeFileEncoding::Utf8 ? " UTF-8" : " UTF-16LE") +
                " scanner matrix accepts arbitrary byte splits and matches exact offsets";
            Expect(accepted && lineOffsets == expectedLineOffsets &&
                lineCharacterOffsets == test.breakCharacters &&
                scanner.CharacterCount() == test.characters && scanner.CrlfCount() == test.crlf &&
                scanner.LfCount() == test.lf && scanner.CrCount() == test.cr,
                label.c_str());
        }
    };

    for (const Case& test : cases) {
        verify(test, LargeFileEncoding::Utf8, test.utf8, test.utf8BreakBytes);
        std::vector<std::uint8_t> utf16Bytes;
        utf16Bytes.reserve(test.utf16.size() * 2U);
        for (const std::uint16_t unit : test.utf16) {
            utf16Bytes.push_back(static_cast<std::uint8_t>(unit & 0xFFU));
            utf16Bytes.push_back(static_cast<std::uint8_t>(unit >> 8U));
        }
        verify(test, LargeFileEncoding::Utf16Le, utf16Bytes, test.utf16BreakBytes);
    }
}

bool SamePieceSummaries(
    const std::vector<classic_notepad::LargeFilePieceSummary>& left,
    const std::vector<classic_notepad::LargeFilePieceSummary>& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.byteOffset != b.byteOffset || a.byteLength != b.byteLength ||
            a.characterOffset != b.characterOffset || a.characterLength != b.characterLength ||
            a.logicalLineBreaks != b.logicalLineBreaks || a.firstCharacter != b.firstCharacter ||
            a.lastCharacter != b.lastCharacter ||
            a.displayedCharacterCount != b.displayedCharacterCount ||
            a.widthSummaryKnown != b.widthSummaryKnown ||
            a.widthSummary.prefixColumns != b.widthSummary.prefixColumns ||
            a.widthSummary.prefixResidues != b.widthSummary.prefixResidues ||
            a.widthSummary.suffixColumns != b.widthSummary.suffixColumns ||
            a.widthSummary.suffixResidue != b.widthSummary.suffixResidue ||
            a.widthSummary.maximumLineColumns != b.widthSummary.maximumLineColumns ||
            a.widthSummary.hasLineBreak != b.widthSummary.hasLineBreak) return false;
    }
    return true;
}

bool BuildPieceSummaries(
    classic_notepad::LargeFileEncoding encoding,
    const std::vector<std::uint8_t>& payload,
    std::uint64_t dataOffset,
    const std::vector<std::size_t>& chunkSizes,
    std::vector<classic_notepad::LargeFilePieceSummary>& pieces,
    std::uint64_t maximumPieceBytes = classic_notepad::kLargeFileOriginalPieceMaximumBytes,
    std::uint64_t maximumPieceCharacters = classic_notepad::kLargeFileOriginalPieceMaximumCharacters)
{
    classic_notepad::LargeFilePieceBuilder builder(encoding, dataOffset,
        maximumPieceBytes, maximumPieceCharacters);
    pieces.clear();
    std::size_t position = 0;
    std::size_t chunkIndex = 0;
    while (position < payload.size()) {
        const std::size_t requested = chunkSizes.empty()
            ? payload.size() - position
            : chunkSizes[chunkIndex++ % chunkSizes.size()];
        const std::size_t length = std::min(requested, payload.size() - position);
        std::vector<classic_notepad::LargeFilePieceSummary> completed;
        if (!builder.Consume(payload.data() + position, length, dataOffset + position, completed)) return false;
        pieces.insert(pieces.end(), completed.begin(), completed.end());
        position += length;
    }
    std::vector<classic_notepad::LargeFilePieceSummary> finalPieces;
    if (!builder.Finish(finalPieces)) return false;
    pieces.insert(pieces.end(), finalPieces.begin(), finalPieces.end());
    return true;
}

bool PieceCoverageAndTotalsMatchScanner(
    classic_notepad::LargeFileEncoding encoding,
    const std::vector<std::uint8_t>& payload,
    std::uint64_t dataOffset,
    const std::vector<classic_notepad::LargeFilePieceSummary>& pieces,
    const std::vector<std::size_t>& chunkSizes)
{
    classic_notepad::LargeFileLineScanner scanner(encoding, dataOffset);
    std::vector<std::uint64_t> lineOffsets{dataOffset};
    std::vector<std::uint64_t> lineCharacterOffsets;
    std::size_t position = 0;
    std::size_t chunkIndex = 0;
    while (position < payload.size()) {
        const std::size_t requested = chunkSizes.empty()
            ? payload.size() - position
            : chunkSizes[chunkIndex++ % chunkSizes.size()];
        const std::size_t length = std::min(requested, payload.size() - position);
        if (!scanner.Consume(payload.data() + position, length, dataOffset + position,
                lineOffsets, &lineCharacterOffsets)) return false;
        position += length;
    }
    if (!scanner.Finish(lineOffsets, &lineCharacterOffsets)) return false;

    std::uint64_t nextByte = dataOffset;
    std::uint64_t nextCharacter = 0;
    std::uint64_t standaloneBreaks = 0;
    wchar_t previousLast = 0;
    bool hasPreviousPiece = false;
    for (const classic_notepad::LargeFilePieceSummary& piece : pieces) {
        if (piece.byteOffset != nextByte || piece.characterOffset != nextCharacter ||
            piece.byteLength == 0U || piece.characterLength == 0U ||
            piece.byteLength > classic_notepad::kLargeFileOriginalPieceMaximumBytes ||
            piece.characterLength > classic_notepad::kLargeFileOriginalPieceMaximumCharacters) return false;
        if (hasPreviousPiece && previousLast == L'\r' && piece.firstCharacter == L'\n') {
            if (standaloneBreaks == 0U) return false;
            --standaloneBreaks;
        }
        standaloneBreaks += piece.logicalLineBreaks;
        nextByte += piece.byteLength;
        nextCharacter += piece.characterLength;
        previousLast = piece.lastCharacter;
        hasPreviousPiece = true;
    }
    const std::uint64_t scannerBreaks = scanner.CrlfCount() + scanner.LfCount() + scanner.CrCount();
    return nextByte == dataOffset + payload.size() && nextCharacter == scanner.CharacterCount() &&
        standaloneBreaks == scannerBreaks;
}

bool PieceBuilderRejectsMalformed(
    classic_notepad::LargeFileEncoding encoding,
    const std::vector<std::uint8_t>& payload,
    const std::vector<std::size_t>& chunkSizes)
{
    classic_notepad::LargeFilePieceBuilder builder(encoding, 0U);
    std::size_t position = 0;
    std::size_t chunkIndex = 0;
    while (position < payload.size()) {
        const std::size_t requested = chunkSizes.empty()
            ? payload.size() - position
            : chunkSizes[chunkIndex++ % chunkSizes.size()];
        const std::size_t length = std::min(requested, payload.size() - position);
        std::vector<classic_notepad::LargeFilePieceSummary> completed;
        if (!builder.Consume(payload.data() + position, length, position, completed)) return true;
        position += length;
    }
    std::vector<classic_notepad::LargeFilePieceSummary> completed;
    return !builder.Finish(completed) && !builder.IsValid();
}

std::vector<classic_notepad::LargeDocumentOriginalPiece> ToOriginalPieces(
    const std::vector<classic_notepad::LargeFilePieceSummary>& pieces)
{
    std::vector<classic_notepad::LargeDocumentOriginalPiece> descriptors;
    descriptors.reserve(pieces.size());
    for (const auto& piece : pieces) {
        descriptors.push_back({piece.byteOffset, piece.byteLength, piece.characterLength,
            piece.logicalLineBreaks, piece.firstCharacter, piece.lastCharacter, true,
            piece.widthSummary, piece.widthSummaryKnown, piece.displayedCharacterCount, true});
    }
    return descriptors;
}

void AppendUtf16CodeUnits(std::wstring& text, std::uint32_t scalar)
{
    if (scalar <= 0xFFFFU) {
        text.push_back(static_cast<wchar_t>(scalar));
        return;
    }
    const std::uint32_t adjusted = scalar - 0x10000U;
    text.push_back(static_cast<wchar_t>(0xD800U + (adjusted >> 10U)));
    text.push_back(static_cast<wchar_t>(0xDC00U + (adjusted & 0x3FFU)));
}

void ExpectLineStarts(
    const classic_notepad::LargeDocument& document,
    const std::vector<std::uint64_t>& expectedStarts,
    const char* message)
{
    std::wstring error;
    bool matches = document.LineCount() == expectedStarts.size();
    for (std::size_t line = 0; matches && line < expectedStarts.size(); ++line) {
        std::uint64_t start = (std::numeric_limits<std::uint64_t>::max)();
        matches = document.LineStart(line, start, error) && start == expectedStarts[line];
    }
    Expect(matches, message);
}

void TestLargeFileBoundedPieceBuilder()
{
    using classic_notepad::LargeFileEncoding;
    using classic_notepad::LargeFilePieceBuilder;
    using classic_notepad::LargeFilePieceSummary;

    {
        LargeFilePieceBuilder empty(LargeFileEncoding::Utf8, 3U);
        std::vector<LargeFilePieceSummary> pieces;
        Expect(empty.Consume(nullptr, 0U, 3U, pieces) && empty.Finish(pieces) && pieces.empty(),
            "Empty payload builds no original pieces after a BOM offset");
    }

    {
        std::vector<std::uint8_t> payload(65535U, static_cast<std::uint8_t>('a'));
        payload.insert(payload.end(), {0xF0U, 0x9FU, 0x98U, 0x80U});
        std::vector<LargeFilePieceSummary> whole;
        std::vector<LargeFilePieceSummary> partitioned;
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf8, payload, 3U, {}, whole),
            "UTF-8 builder accepts supplementary scalar after a BOM");
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf8, payload, 3U, {1U, 2U, 7U, 4093U}, partitioned),
            "UTF-8 builder accepts arbitrary chunks through lead and continuation bytes");
        Expect(SamePieceSummaries(whole, partitioned), "UTF-8 piece output is independent of input chunk partition");
        Expect(whole.size() == 2U && whole[0].byteOffset == 3U && whole[0].byteLength == 65535U &&
            whole[0].characterOffset == 0U && whole[0].characterLength == 65535U &&
            whole[1].byteOffset == 65538U && whole[1].byteLength == 4U &&
            whole[1].characterOffset == 65535U && whole[1].characterLength == 2U,
            "UTF-8 pieces cut before a supplementary scalar without splitting its UTF-16 units");
        Expect(PieceCoverageAndTotalsMatchScanner(LargeFileEncoding::Utf8, payload, 3U, whole, {1U, 17U, 4093U}),
            "UTF-8 piece coverage and totals match the scanner independently of chunk partition");
    }

    {
        std::vector<std::uint8_t> payload;
        for (std::size_t index = 0; index < 32769U; ++index) payload.insert(payload.end(), {0xC3U, 0xA9U});
        std::vector<LargeFilePieceSummary> whole;
        std::vector<LargeFilePieceSummary> partitioned;
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf8, payload, 0U, {}, whole),
            "Dense multibyte UTF-8 source builds bounded pieces");
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf8, payload, 0U, {1U, 3U, 257U, 4095U}, partitioned),
            "Dense multibyte UTF-8 sequences may cross arbitrary input chunks");
        Expect(SamePieceSummaries(whole, partitioned) && whole.size() == 2U &&
            whole[0].byteLength == 65536U && whole[0].characterLength == 32768U &&
            whole[1].byteLength == 2U && whole[1].characterLength == 1U,
            "Dense multibyte UTF-8 cuts at scalar boundaries with exact byte and unit totals");
        Expect(PieceCoverageAndTotalsMatchScanner(LargeFileEncoding::Utf8, payload, 0U, whole, {5U, 2U, 11U}),
            "Dense multibyte descriptor coverage and totals match the scanner");

        std::wstring expected(32769U, L'\u00E9');
        const auto descriptors = ToOriginalPieces(whole);
        classic_notepad::LargeDocument document(LargeFileEncoding::Utf8,
            [&payload](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
                if (offset > payload.size() || length > payload.size() - static_cast<std::size_t>(offset)) return false;
                bytes.assign(payload.begin() + static_cast<std::size_t>(offset),
                    payload.begin() + static_cast<std::size_t>(offset) + length);
                return true;
            });
        std::wstring error;
        std::wstring actual;
        Expect(document.Initialize(descriptors, error) &&
            document.ReadRange(0U, document.CharacterCount(), actual, error) && actual == expected,
            "Generated dense UTF-8 descriptors read back exact text across pieces");
        ExpectLineStarts(document, {0U}, "Dense UTF-8 document has the exact sole line start");
    }

    {
        std::vector<std::uint8_t> payload(65535U, static_cast<std::uint8_t>('x'));
        payload.push_back(static_cast<std::uint8_t>('\r'));
        payload.push_back(static_cast<std::uint8_t>('\n'));
        std::vector<LargeFilePieceSummary> pieces;
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf8, payload, 0U, {3U, 1U, 1024U}, pieces),
            "Piece builder preserves a CRLF split across a bounded piece seam");
        Expect(pieces.size() == 2U && pieces[0].byteLength == 65536U &&
            pieces[0].lastCharacter == L'\r' && pieces[0].logicalLineBreaks == 1U &&
            pieces[1].firstCharacter == L'\n' && pieces[1].logicalLineBreaks == 1U,
            "Each piece has a standalone CR/LF summary at the seam");
        std::vector<LargeFilePieceSummary> whole;
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf8, payload, 0U, {}, whole) &&
            SamePieceSummaries(pieces, whole), "CRLF seam descriptors are chunk-partition independent");
        Expect(PieceCoverageAndTotalsMatchScanner(LargeFileEncoding::Utf8, payload, 0U, pieces, {1U, 5U, 1024U}),
            "CRLF seam totals match the scanner after adjacent summaries are composed");

        std::vector<classic_notepad::LargeDocumentOriginalPiece> descriptors;
        for (const LargeFilePieceSummary& piece : pieces) {
            descriptors.push_back({piece.byteOffset, piece.byteLength, piece.characterLength,
                piece.logicalLineBreaks, piece.firstCharacter, piece.lastCharacter, true,
                piece.widthSummary, piece.widthSummaryKnown, piece.displayedCharacterCount, true});
        }
        classic_notepad::LargeDocument document(LargeFileEncoding::Utf8,
            [&payload](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
                if (offset > payload.size() || length > payload.size() - static_cast<std::size_t>(offset)) return false;
                bytes.assign(payload.begin() + static_cast<std::size_t>(offset),
                    payload.begin() + static_cast<std::size_t>(offset) + length);
                return true;
            });
        std::wstring error;
        Expect(document.Initialize(descriptors, error) && document.LineCount() == 2U &&
            document.CharacterCount() == payload.size(),
            "LargeDocument combines CRLF summaries when the pair crosses original pieces");
        Expect(document.DisplayedCharacterCount() ==
                classic_notepad::CountStatusCharacters(std::wstring(payload.begin(), payload.end())),
            "Displayed-character summary counts CRLF once across an original-piece seam");
        std::wstring actual;
        Expect(document.ReadRange(0U, document.CharacterCount(), actual, error) &&
            actual == std::wstring(payload.begin(), payload.end()),
            "CRLF-seam document reads back exact source content");
        ExpectLineStarts(document, {0U, static_cast<std::uint64_t>(payload.size())},
            "CRLF-seam document has the exact line start after the pair");
    }

    {
        const std::vector<std::uint8_t> payload{'a', '\r', '\n', 'b', '\n', 'c', '\r'};
        std::vector<LargeFilePieceSummary> whole;
        std::vector<LargeFilePieceSummary> partitioned;
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf8, payload, 0U, {}, whole, 1U, 1U),
            "Small configurable limits create one-character UTF-8 pieces");
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf8, payload, 0U, {2U, 1U, 3U}, partitioned, 1U, 1U) &&
            SamePieceSummaries(whole, partitioned),
            "Standalone CR, LF, and trailing CR summaries do not depend on chunk partition");
        Expect(whole.size() == payload.size() && whole[1].lastCharacter == L'\r' &&
            whole[1].logicalLineBreaks == 1U && whole[2].firstCharacter == L'\n' &&
            whole[2].logicalLineBreaks == 1U && whole.back().lastCharacter == L'\r' &&
            whole.back().logicalLineBreaks == 1U,
            "CR/LF seams and a trailing CR each have correct standalone summaries");
        Expect(PieceCoverageAndTotalsMatchScanner(LargeFileEncoding::Utf8, payload, 0U, whole, {1U, 4U, 2U}),
            "Standalone CR/LF and trailing CR composed totals match the scanner");

        std::vector<std::uint8_t> byteLimitedPayload{0xC3U, 0xA9U, 0xC3U, 0xA9U, 0xC3U, 0xA9U};
        std::vector<LargeFilePieceSummary> byteLimited;
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf8, byteLimitedPayload, 0U, {1U, 5U},
                byteLimited, 5U, 100U) && byteLimited.size() == 2U &&
            byteLimited[0].byteLength == 4U && byteLimited[0].characterLength == 2U &&
            byteLimited[1].byteLength == 2U && byteLimited[1].characterLength == 1U,
            "Configurable byte limit binds independently while the UTF-16-unit limit remains permissive");

        std::vector<std::uint8_t> unitLimitedPayload{'a', 0xF0U, 0x9FU, 0x98U, 0x80U, 'b'};
        std::vector<LargeFilePieceSummary> unitLimited;
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf8, unitLimitedPayload, 0U, {2U, 1U, 3U},
                unitLimited, 100U, 2U) && unitLimited.size() == 3U &&
            unitLimited[0].byteLength == 1U && unitLimited[0].characterLength == 1U &&
            unitLimited[1].byteLength == 4U && unitLimited[1].characterLength == 2U &&
            unitLimited[2].byteLength == 1U && unitLimited[2].characterLength == 1U,
            "Configurable UTF-16-unit limit binds independently without splitting a supplementary scalar");
    }

    {
        std::vector<std::uint8_t> payload;
        payload.reserve(32767U * 2U + 4U);
        for (std::size_t index = 0; index < 32767U; ++index) {
            payload.push_back(static_cast<std::uint8_t>('a'));
            payload.push_back(0U);
        }
        payload.insert(payload.end(), {0x3DU, 0xD8U, 0x00U, 0xDEU});
        std::vector<LargeFilePieceSummary> whole;
        std::vector<LargeFilePieceSummary> partitioned;
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf16Le, payload, 2U, {}, whole),
            "UTF-16 builder accepts a supplementary pair after a BOM");
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf16Le, payload, 2U, {1U, 3U, 2U, 8191U}, partitioned),
            "UTF-16 builder retains odd bytes and surrogate pairs across input chunks");
        Expect(SamePieceSummaries(whole, partitioned), "UTF-16 piece output is independent of input chunk partition");
        Expect(whole.size() == 2U && whole[0].byteOffset == 2U && whole[0].byteLength == 65534U &&
            whole[0].characterLength == 32767U && whole[1].byteOffset == 65536U &&
            whole[1].byteLength == 4U && whole[1].characterOffset == 32767U &&
            whole[1].characterLength == 2U,
            "UTF-16 piece boundaries do not split a supplementary scalar");
        Expect(PieceCoverageAndTotalsMatchScanner(LargeFileEncoding::Utf16Le, payload, 2U, whole, {3U, 1U, 8191U}),
            "UTF-16 piece coverage and UTF-16-unit totals match the scanner");

        std::vector<std::uint8_t> completePayload = payload;
        completePayload.insert(completePayload.end(), {0x0DU, 0x00U, 0x0AU, 0x00U,
            0x42U, 0x00U, 0x0AU, 0x00U, 0x43U, 0x00U, 0x0DU, 0x00U});
        std::vector<LargeFilePieceSummary> completePieces;
        Expect(BuildPieceSummaries(LargeFileEncoding::Utf16Le, completePayload, 2U,
            {1U, 5U, 8191U}, completePieces), "UTF-16 mixed-ending source builds through arbitrary chunks");
        Expect(PieceCoverageAndTotalsMatchScanner(LargeFileEncoding::Utf16Le, completePayload, 2U,
            completePieces, {3U, 7U, 1U}), "UTF-16 mixed-ending descriptor totals match the scanner");

        std::wstring expected(32767U, L'a');
        AppendUtf16CodeUnits(expected, 0x1F600U);
        expected += L"\r\nB\nC\r";
        const auto descriptors = ToOriginalPieces(completePieces);
        std::vector<std::uint8_t> fileBytes{0xFFU, 0xFEU};
        fileBytes.insert(fileBytes.end(), completePayload.begin(), completePayload.end());
        classic_notepad::LargeDocument document(LargeFileEncoding::Utf16Le,
            [&fileBytes](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
                if (offset > fileBytes.size() || length > fileBytes.size() - static_cast<std::size_t>(offset)) return false;
                bytes.assign(fileBytes.begin() + static_cast<std::size_t>(offset),
                    fileBytes.begin() + static_cast<std::size_t>(offset) + length);
                return true;
            });
        std::wstring error;
        std::wstring actual;
        Expect(document.Initialize(descriptors, error) &&
            document.ReadRange(0U, document.CharacterCount(), actual, error) && actual == expected,
            "Generated multi-piece UTF-16 descriptors read back exact UTF-16 text");
        ExpectLineStarts(document, {0U, 32771U, 32773U, 32775U},
            "Generated UTF-16 document reports exact line starts across CRLF, LF, and trailing CR");
    }

    {
        std::vector<LargeFilePieceSummary> pieces;
        const std::vector<std::uint8_t> truncatedUtf8{0xF0U, 0x9FU, 0x98U};
        LargeFilePieceBuilder utf8(LargeFileEncoding::Utf8, 0U);
        Expect(utf8.Consume(truncatedUtf8.data(), truncatedUtf8.size(), 0U, pieces) &&
            !utf8.Finish(pieces) && !utf8.IsValid(), "UTF-8 builder rejects a truncated final scalar");

        const std::vector<std::uint8_t> oddUtf16{static_cast<std::uint8_t>('a')};
        LargeFilePieceBuilder odd(LargeFileEncoding::Utf16Le, 0U);
        Expect(odd.Consume(oddUtf16.data(), oddUtf16.size(), 0U, pieces) && !odd.Finish(pieces),
            "UTF-16 builder rejects an odd final byte");

        const std::vector<std::uint8_t> highSurrogate{0x00U, 0xD8U};
        LargeFilePieceBuilder unpaired(LargeFileEncoding::Utf16Le, 0U);
        Expect(unpaired.Consume(highSurrogate.data(), highSurrogate.size(), 0U, pieces) &&
            !unpaired.Finish(pieces), "UTF-16 builder rejects a dangling final high surrogate");

        for (const std::vector<std::uint8_t>& invalid : {
                std::vector<std::uint8_t>{0xC0U, 0xAFU},
                std::vector<std::uint8_t>{0xEDU, 0xA0U, 0x80U},
                std::vector<std::uint8_t>{0xF4U, 0x90U, 0x80U, 0x80U}}) {
            Expect(PieceBuilderRejectsMalformed(LargeFileEncoding::Utf8, invalid, {}) &&
                PieceBuilderRejectsMalformed(LargeFileEncoding::Utf8, invalid, {1U}),
                "UTF-8 builder rejects overlong, surrogate, and out-of-range complete scalar encodings");
        }
        Expect(PieceBuilderRejectsMalformed(LargeFileEncoding::Utf16Le, {0x00U, 0xDCU}, {1U}) &&
            PieceBuilderRejectsMalformed(LargeFileEncoding::Utf16Le, {0x41U, 0x00U, 0x00U, 0xD8U}, {3U, 1U}),
            "UTF-16 builder rejects unmatched low and interrupted high surrogates");
    }
}

std::uint64_t MeasureVisualColumnsForTest(const std::wstring& text)
{
    std::uint64_t maximum = 0U;
    std::uint64_t current = 0U;
    for (wchar_t character : text) {
        if (character == L'\r' || character == L'\n') {
            maximum = std::max(maximum, current);
            current = 0U;
        } else if (character == L'\t') {
            current += 8U - (current % 8U);
        } else {
            ++current;
        }
    }
    return std::max(maximum, current);
}

void TestLargeFileEditedWidthSummaries()
{
    using classic_notepad::LargeDocument;
    using classic_notepad::LargeDocumentOriginalPiece;
    using classic_notepad::LargeFileEncoding;
    using classic_notepad::LargeFilePieceBuilder;
    using classic_notepad::LargeFilePieceSummary;

    const std::string source = "ab\tX\r\nq\t\nZ\r";
    std::vector<std::uint8_t> payload(source.begin(), source.end());
    std::vector<LargeFilePieceSummary> builderPieces;
    Expect(BuildPieceSummaries(LargeFileEncoding::Utf8, payload, 0U, {1U, 2U, 5U}, builderPieces),
        "Width summaries are generated while bounded source pieces are indexed");
    Expect(!builderPieces.empty() && std::all_of(builderPieces.begin(), builderPieces.end(),
            [](const LargeFilePieceSummary& piece) { return piece.widthSummaryKnown; }),
        "Every indexed source descriptor carries a known width summary");

    std::uint64_t sourceReads = 0U;
    LargeDocument indexed(LargeFileEncoding::Utf8,
        [&payload, &sourceReads](std::uint64_t offset, std::size_t length,
            std::vector<std::uint8_t>& bytes) {
            ++sourceReads;
            if (offset > payload.size() || length > payload.size() - static_cast<std::size_t>(offset)) return false;
            bytes.assign(payload.begin() + static_cast<std::size_t>(offset),
                payload.begin() + static_cast<std::size_t>(offset) + length);
            return true;
        });
    std::wstring error;
    const auto descriptors = ToOriginalPieces(builderPieces);
    Expect(indexed.Initialize(descriptors, error) && sourceReads == 0U,
        "Known indexed width summaries transfer into the tree without source rereads");
    Expect(indexed.MaximumLineColumns() == MeasureVisualColumnsForTest(
            std::wstring(source.begin(), source.end())) && indexed.MaximumLineColumns() == 9U,
        "Tree width summary composes tabs and CRLF, LF, CR, and trailing empty rows exactly");
    Expect(indexed.LineCount() == 4U,
        "Width summary composition preserves CRLF seam line counts and trailing rows");

    // Force every ASCII character, including tab and both CR/LF units, across
    // an original-piece seam. The logical row still expands its tab from the
    // column accumulated in the preceding piece.
    std::vector<LargeDocumentOriginalPiece> singleCharacterPieces;
    for (std::size_t index = 0; index < payload.size(); ++index) {
        singleCharacterPieces.push_back({index, 1U, 1U});
    }
    LargeDocument segmented(LargeFileEncoding::Utf8,
        [&payload](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
            if (offset > payload.size() || length > payload.size() - static_cast<std::size_t>(offset)) return false;
            bytes.assign(payload.begin() + static_cast<std::size_t>(offset),
                payload.begin() + static_cast<std::size_t>(offset) + length);
            return true;
        });
    Expect(segmented.Initialize(singleCharacterPieces, error) && segmented.MaximumLineColumns() == 9U,
        "Tab residue and line widths compose across one-character piece seams");

    LargeDocument inserted(LargeFileEncoding::Utf8,
        [](std::uint64_t, std::size_t, std::vector<std::uint8_t>&) { return false; });
    Expect(inserted.Initialize({}, error), "An empty model initializes for inserted-width tests");
    std::wstring supplementary;
    supplementary.push_back(L'A');
    AppendUtf16CodeUnits(supplementary, 0x1F600U);
    supplementary.push_back(L'\t');
    std::uint64_t afterStart = 0U;
    std::uint64_t afterEnd = 0U;
    Expect(inserted.Replace(0U, 0U, supplementary, 0U, 0U, afterStart, afterEnd, error) &&
            inserted.MaximumLineColumns() == 8U,
        "Inserted supplementary scalars and tabs summarize UTF-16 visual columns");
    auto clone = inserted.Clone();
    Expect(clone != nullptr && clone->MaximumLineColumns() == 8U,
        "Cloned document snapshots retain cached width summaries");

    const std::string narrowSource = "small\n";
    std::vector<std::uint8_t> narrowBytes(narrowSource.begin(), narrowSource.end());
    LargeDocument edits(LargeFileEncoding::Utf8,
        [&narrowBytes](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
            if (offset > narrowBytes.size() || length > narrowBytes.size() - static_cast<std::size_t>(offset)) return false;
            bytes.assign(narrowBytes.begin() + static_cast<std::size_t>(offset),
                narrowBytes.begin() + static_cast<std::size_t>(offset) + length);
            return true;
        });
    Expect(edits.Initialize({LargeDocumentOriginalPiece{0U, narrowBytes.size(), narrowBytes.size()}}, error),
        "Width edit baseline initializes");
    const std::wstring wide(40U, L'W');
    Expect(edits.Replace(edits.CharacterCount(), edits.CharacterCount(), wide + L"\n", 6U, 6U,
            afterStart, afterEnd, error) && edits.MaximumLineColumns() == 40U,
        "Inserting a unique widest row raises the cached extent");
    Expect(edits.Replace(6U, 47U, L"", 6U, 47U, afterStart, afterEnd, error) &&
            edits.MaximumLineColumns() == 5U,
        "Deleting the unique widest row shrinks the extent to the next widest row");
    Expect(edits.Undo(afterStart, afterEnd, error) && edits.MaximumLineColumns() == 40U,
        "Undo restores the deleted widest row extent");
    Expect(edits.Undo(afterStart, afterEnd, error) && edits.MaximumLineColumns() == 5U,
        "Undo removes the inserted wide row and restores the baseline extent");

    const std::string replaceSource = "a\na\n";
    std::vector<std::uint8_t> replaceBytes(replaceSource.begin(), replaceSource.end());
    LargeDocument replaced(LargeFileEncoding::Utf8,
        [&replaceBytes](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
            if (offset > replaceBytes.size() || length > replaceBytes.size() - static_cast<std::size_t>(offset)) return false;
            bytes.assign(replaceBytes.begin() + static_cast<std::size_t>(offset),
                replaceBytes.begin() + static_cast<std::size_t>(offset) + length);
            return true;
        });
    Expect(replaced.Initialize({LargeDocumentOriginalPiece{0U, replaceBytes.size(), replaceBytes.size()}}, error),
        "Replace All width baseline initializes");
    bool cancelled = false;
    const std::uint64_t replacements = replaced.ReplaceAll(L"a", L"longer", true, false,
        cancelled, {}, error);
    Expect(replacements == 2U && !cancelled && replaced.MaximumLineColumns() == 6U,
        "Synchronous Replace All adoption updates the cached width summary");
    Expect(replaced.Undo(afterStart, afterEnd, error) && replaced.MaximumLineColumns() == 1U,
        "Undo after Replace All restores the pre-adoption width summary");
}

void TestLargeFileScrollMapping()
{
    using classic_notepad::LargeFileScrollMapping;
    const std::uint64_t intRange = static_cast<std::uint64_t>((std::numeric_limits<int>::max)());
    for (const std::pair<std::uint64_t, std::uint64_t> test : {
            std::pair<std::uint64_t, std::uint64_t>{5U, 6U}, {5U, 5U}, {5U, 4U}}) {
        const auto mapping = LargeFileScrollMapping::Create(test.first, test.second);
        const std::uint64_t expectedTop = test.first > test.second ? test.first - test.second : 0U;
        Expect(mapping.MaximumTop() == expectedTop,
            "Scroll mapping handles a page below, equal to, and larger than the document");
        Expect(mapping.TopToPosition(0U) == 0 &&
            mapping.TopToPosition(mapping.MaximumTop()) == static_cast<int>(mapping.MaximumPosition()),
            "Scroll mapping preserves both legal thumb endpoints");
        Expect(static_cast<std::uint64_t>(mapping.RangeMaximum()) + 1U - mapping.Page() ==
                mapping.MaximumPosition(),
            "Scroll mapping maximum thumb position agrees with the Win32 range and page");
    }

    const std::vector<std::uint64_t> totals{
        intRange + 1U, (1ULL << 53U) + 17U,
        (std::numeric_limits<std::uint64_t>::max)() - 3U,
        (std::numeric_limits<std::uint64_t>::max)()};
    for (const std::uint64_t total : totals) {
        const auto mapping = LargeFileScrollMapping::Create(total, 37U);
        Expect(mapping.RangeMaximum() == (std::numeric_limits<int>::max)() - 1 &&
            mapping.MaximumPosition() > 0U,
            "Very large row counts use a bounded legal Win32 thumb range");
        std::uint64_t previousTop = 0U;
        std::uint64_t previousPosition = 0U;
        bool monotonic = true;
        for (std::uint64_t index = 0U; index <= 2048U; ++index) {
            const std::uint64_t position = mapping.MaximumPosition() * index / 2048U;
            const std::uint64_t top = mapping.PositionToTop(position);
            monotonic = monotonic && top >= previousTop && position >= previousPosition;
            const std::uint64_t restoredPosition = static_cast<std::uint64_t>(mapping.TopToPosition(top));
            const std::uint64_t distance = restoredPosition > position
                ? restoredPosition - position : position - restoredPosition;
            monotonic = monotonic && distance <= 2U;
            previousTop = top;
            previousPosition = position;
        }
        Expect(monotonic, "Large-count thumb conversion is monotonic with bounded inverse error");
        Expect(mapping.PositionToTop(0U) == 0U &&
            mapping.PositionToTop(mapping.MaximumPosition()) == mapping.MaximumTop() &&
            mapping.TopToPosition(mapping.MaximumTop()) ==
                static_cast<int>(mapping.MaximumPosition()),
            "Large-count thumb conversion preserves first and last-page endpoints without overflow");
    }
}

void TestLargeDocumentPieceTable()
{
    using classic_notepad::LargeDocument;
    using classic_notepad::LargeDocumentOriginalPiece;
    using classic_notepad::LargeFileEncoding;

    const std::string source = "one\r\ntwo\r\nthree\r\n";
    auto reader = [&source](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
        if (offset > source.size() || length > source.size() - static_cast<std::size_t>(offset)) return false;
        bytes.assign(source.begin() + static_cast<std::size_t>(offset),
            source.begin() + static_cast<std::size_t>(offset) + length);
        return true;
    };
    LargeDocument document(LargeFileEncoding::Utf8, reader);
    std::wstring error;
    Expect(document.Initialize({ LargeDocumentOriginalPiece{ 0, static_cast<std::uint64_t>(source.size()), static_cast<std::uint64_t>(source.size()) } }, error),
        "Large document initializes an immutable source piece");

    std::uint64_t afterStart = 0;
    std::uint64_t afterEnd = 0;
    Expect(document.Replace(0, 0, L"BEGIN ", 0, 0, afterStart, afterEnd, error), "Large document inserts at beginning");
    Expect(document.Replace(11, 14, L"TWO", 11, 14, afterStart, afterEnd, error), "Large document replaces across a split source piece");
    Expect(document.Replace(document.CharacterCount(), document.CharacterCount(), L"END", document.CharacterCount(), document.CharacterCount(), afterStart, afterEnd, error),
        "Large document inserts at end");
    std::wstring text;
    Expect(document.ReadRange(0, document.CharacterCount(), text, error), "Large document reads a cross-piece range");
    ExpectText(text, L"BEGIN one\r\nTWO\r\nthree\r\nEND", "Large document cross-piece edits preserve text");
    Expect(document.PieceCount() >= 3U, "Large document keeps multiple pieces instead of copying the original suffix");
    Expect(document.InsertedBufferBytes() >= 2U * sizeof(wchar_t), "Large document tracks append-only inserted buffers");
    Expect(document.LineCount() == 4U, "Large document aggregates edited logical lines");
    classic_notepad::LargeDocumentLineColumn coordinate;
    Expect(document.PositionToLineColumn(11U, coordinate, error) && coordinate.line == 1U && coordinate.column == 0U,
        "Large document converts edited positions to line and column");
    Expect(document.PositionToLineColumn(10U, coordinate, error) && coordinate.line == 1U && coordinate.column == 0U,
        "A caret between the CR and LF of CRLF keeps a valid logical line coordinate");
    std::uint64_t lineStart = 0;
    std::uint64_t lineEnd = 0;
    std::uint64_t nextLine = 0;
    Expect(document.LineRange(1U, lineStart, lineEnd, nextLine, error) && lineStart == 11U && lineEnd == 14U,
        "Large document line lookup is driven by the edited tree");

    classic_notepad::LargeDocumentMatch match;
    Expect(document.Find(L"TWO", 0, true, true, true, match, error), "Large document finds text across pieces");
    Expect(match.start == 11U && match.length == 3U, "Large document find returns 64-bit logical coordinates");
    Expect(document.Find(L"one", document.CharacterCount(), false, true, false, match, error), "Large document finds backward");

    bool cancelled = false;
    Expect(document.ReplaceAll(L"e", L"E", false, false, cancelled, {}, error) > 0U && !cancelled,
        "Large document replace-all streams through pieces");
    Expect(document.Undo(afterStart, afterEnd, error), "Large document undoes replace-all edits without a snapshot");
    Expect(document.Undo(afterStart, afterEnd, error), "Large document supports multiple undo records");

    LargeDocument utf16(LargeFileEncoding::Utf8, [](std::uint64_t, std::size_t, std::vector<std::uint8_t>&) { return false; });
    Expect(utf16.Initialize({}, error), "Large document initializes an empty document");
    const std::wstring nonBmp = std::wstring(L"A") + wchar_t(0xD83D) + wchar_t(0xDE00) + L"B";
    Expect(utf16.Replace(0, 0, nonBmp, 0, 0, afterStart, afterEnd, error), "Large document accepts non-BMP inserted Unicode");
    Expect(utf16.DisplayedCharacterCount() == 3U,
        "Inserted summary counts a valid surrogate pair as one displayed character");
    Expect(utf16.ReadRange(0, utf16.CharacterCount(), text, error), "Large document reads non-BMP inserted Unicode");
    ExpectText(text, nonBmp, "Large document preserves non-BMP inserted Unicode");
    Expect(!utf16.IsCharacterBoundary(2U, error) && !error.empty(),
        "Large document rejects a caret inside a surrogate pair");
    error.clear();
    Expect(!utf16.Replace(2U, 2U, L"x", 2U, 2U, afterStart, afterEnd, error),
        "Large document rejects an edit inside a surrogate pair");
    utf16.MarkSaved();
    Expect(!utf16.IsModified(), "Large document saved-state marker clears modified state");
    Expect(utf16.Replace(utf16.CharacterCount(), utf16.CharacterCount(), L"!", 0, 0, afterStart, afterEnd, error),
        "Large document can edit after a saved marker");
    Expect(utf16.IsModified(), "Large document reports edits after the saved marker");
    Expect(utf16.Undo(afterStart, afterEnd, error) && !utf16.IsModified(),
        "Undoing to the saved state clears modified state");

    const std::string splitSource = "a\r\n";
    auto splitReader = [&splitSource](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
        if (offset > splitSource.size() || length > splitSource.size() - static_cast<std::size_t>(offset)) return false;
        bytes.assign(splitSource.begin() + static_cast<std::size_t>(offset),
            splitSource.begin() + static_cast<std::size_t>(offset) + length);
        return true;
    };
    LargeDocument splitCrlf(LargeFileEncoding::Utf8, splitReader);
    Expect(splitCrlf.Initialize({{0, 2, 2}, {2, 1, 1}}, error),
        "Large document initializes CRLF split across original pieces");
    Expect(splitCrlf.LineCount() == 2U, "CRLF split across pieces counts as one logical break");

    LargeDocument insertedCrlf(LargeFileEncoding::Utf8,
        [](std::uint64_t, std::size_t, std::vector<std::uint8_t>&) { return false; });
    Expect(insertedCrlf.Initialize({}, error), "Inserted-boundary document initializes");
    Expect(insertedCrlf.Replace(0, 0, L"\r", 0, 0, afterStart, afterEnd, error) &&
           insertedCrlf.Replace(1, 1, L"\n", 1, 1, afterStart, afterEnd, error),
        "CR and LF can be inserted in separate pieces");
    Expect(insertedCrlf.LineCount() == 2U, "Inserted CRLF split across pieces is one logical break");
    Expect(insertedCrlf.DisplayedCharacterCount() == 1U,
        "Inserted displayed count combines CRLF across separate edit pieces");
    Expect(insertedCrlf.Replace(0, 1, L"", 0, 0, afterStart, afterEnd, error) &&
           insertedCrlf.LineCount() == 2U && insertedCrlf.DisplayedCharacterCount() == 1U,
        "Deleting CR from a split CRLF leaves one LF break");

    const std::string newlineSource = "a\nb\rc";
    auto newlineReader = [&newlineSource](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
        if (offset > newlineSource.size() || length > newlineSource.size() - static_cast<std::size_t>(offset)) return false;
        bytes.assign(newlineSource.begin() + static_cast<std::size_t>(offset),
            newlineSource.begin() + static_cast<std::size_t>(offset) + length);
        return true;
    };
    LargeDocument mixedSingleBreaks(LargeFileEncoding::Utf8, newlineReader);
    Expect(mixedSingleBreaks.Initialize({{0, static_cast<std::uint64_t>(newlineSource.size()),
        static_cast<std::uint64_t>(newlineSource.size())}}, error) && mixedSingleBreaks.LineCount() == 3U,
        "LF and CR source documents retain independent logical breaks");

    const std::vector<std::uint8_t> utf16Source{'A', 0, '\r', 0, '\n', 0, 'B', 0};
    auto utf16Reader = [&utf16Source](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
        if (offset > utf16Source.size() || length > utf16Source.size() - static_cast<std::size_t>(offset)) return false;
        bytes.assign(utf16Source.begin() + static_cast<std::size_t>(offset),
            utf16Source.begin() + static_cast<std::size_t>(offset) + length);
        return true;
    };
    LargeDocument utf16Original(LargeFileEncoding::Utf16Le, utf16Reader);
    Expect(utf16Original.Initialize({{0, 8, 4}}, error), "UTF-16 LE original piece initializes");
    Expect(utf16Original.LineCount() == 2U &&
           utf16Original.Replace(1, 1, L"X", 1, 1, afterStart, afterEnd, error),
        "UTF-16 LE original content remains editable");
    Expect(utf16Original.ReadRange(0, utf16Original.CharacterCount(), text, error),
        "Edited UTF-16 original content is readable");
    ExpectText(text, L"AX\r\nB", "UTF-16 original and inserted pieces compose correctly");

    const std::string boundarySearchSource = "hello world";
    auto boundarySearchReader = [&boundarySearchSource](std::uint64_t offset, std::size_t length,
        std::vector<std::uint8_t>& bytes) {
        if (offset > boundarySearchSource.size() || length > boundarySearchSource.size() - static_cast<std::size_t>(offset)) return false;
        bytes.assign(boundarySearchSource.begin() + static_cast<std::size_t>(offset),
            boundarySearchSource.begin() + static_cast<std::size_t>(offset) + length);
        return true;
    };
    LargeDocument boundarySearch(LargeFileEncoding::Utf8, boundarySearchReader);
    Expect(boundarySearch.Initialize({{0, 5, 5}, {5, 1, 1}, {6, 5, 5}}, error),
        "Search boundary source initializes as multiple pieces");
    Expect(boundarySearch.Find(L"o w", 0, true, false, true, match, error) && match.start == 4U,
        "Forward search crosses original-piece boundaries");
    Expect(boundarySearch.Find(L"WORLD", boundarySearch.CharacterCount(), false, true, false, match, error) &&
           match.start == 6U,
        "Backward whole-word case-insensitive search crosses pieces");
    error.clear();
    Expect(!boundarySearch.Find(L"absent", 0, true, false, true, match, error, [] { return true; }) &&
           !error.empty(),
        "Streaming search observes cancellation between source pieces");

    boundarySearch.SetUndoByteLimit(1U);
    Expect(boundarySearch.Replace(0, 0, L"evicted", 0, 0, afterStart, afterEnd, error),
        "Edit succeeds with a tiny undo budget");
    Expect(!boundarySearch.CanUndo() && boundarySearch.UndoBytes() <= 1U,
        "Undo records are evicted to the configured memory limit");

    bool replaceCancelled = false;
    Expect(boundarySearch.ReplaceAll(L"l", L"L", true, false, replaceCancelled,
        [] { return true; }, error) == 0U && replaceCancelled,
        "Replace All cancellation leaves the immutable generation uncommitted");

    constexpr std::uint64_t syntheticOffset = 5ULL * 1024ULL * 1024ULL * 1024ULL;
    auto syntheticReader = [syntheticOffset](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
        if (offset != syntheticOffset || length != 4U) return false;
        bytes = {'A', 0, 'B', 0};
        return true;
    };
    LargeDocument syntheticLarge(LargeFileEncoding::Utf16Le, syntheticReader);
    Expect(syntheticLarge.Initialize({{syntheticOffset, 4, 2, 0, L'A', L'B', true}}, error),
        "Original pieces support synthetic byte offsets beyond 4 GiB");
    Expect(syntheticLarge.ReadRange(0, 2, text, error), "Synthetic >4 GiB original range is readable");
    ExpectText(text, L"AB", "Synthetic >4 GiB source coordinates preserve text");

    LargeDocument overflowing(LargeFileEncoding::Utf8, boundarySearchReader);
    error.clear();
    Expect(!overflowing.Initialize({{(std::numeric_limits<std::uint64_t>::max)() - 1U, 4U, 4U}}, error) &&
           !error.empty(),
        "Original-piece byte ranges reject overflow near UINT64_MAX");
}

void TestLargeDocumentUnicodeBoundaries()
{
    using classic_notepad::LargeDocument;
    using classic_notepad::LargeDocumentOriginalPiece;
    using classic_notepad::LargeFileEncoding;

    const std::wstring emojiUnits{
        static_cast<wchar_t>(0xD83DU), static_cast<wchar_t>(0xDE00U) };
    const std::wstring expected = std::wstring(65535U, L'x') + emojiUnits + L'z';
    std::vector<std::uint8_t> utf8Source(65535U, static_cast<std::uint8_t>('x'));
    utf8Source.insert(utf8Source.end(), { 0xF0U, 0x9FU, 0x98U, 0x80U, static_cast<std::uint8_t>('z') });
    auto utf8Reader = [&utf8Source](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
        if (offset > utf8Source.size() || length > utf8Source.size() - static_cast<std::size_t>(offset)) return false;
        bytes.assign(utf8Source.begin() + static_cast<std::size_t>(offset),
            utf8Source.begin() + static_cast<std::size_t>(offset) + length);
        return true;
    };

    std::wstring error;
    LargeDocument utf8(LargeFileEncoding::Utf8, utf8Reader);
    Expect(utf8.Initialize({ LargeDocumentOriginalPiece{
        0U, static_cast<std::uint64_t>(utf8Source.size()), static_cast<std::uint64_t>(expected.size()) } }, error),
        "UTF-8 large document initializes with a scalar crossing the 64 KiB mapping edge");
    std::wstring oneUnit;
    Expect(utf8.ReadRange(65535U, 1U, oneUnit, error) && oneUnit == std::wstring(1U, emojiUnits[0]),
        "ReadRange returns exactly the high UTF-16 code unit at a supplementary scalar");
    Expect(utf8.ReadRange(65536U, 1U, oneUnit, error) && oneUnit == std::wstring(1U, emojiUnits[1]),
        "ReadRange can begin at the low-surrogate code-unit offset");
    Expect(utf8.ReadRange(65537U, 1U, oneUnit, error) && oneUnit == L"z",
        "ReadRange returns the code unit immediately after a supplementary scalar");
    Expect(utf8.IsCharacterBoundary(65535U, error) && !utf8.IsCharacterBoundary(65536U, error) &&
            utf8.IsCharacterBoundary(65537U, error),
        "Only the position between a valid surrogate pair is rejected as an edit boundary");

    std::wstring streamed;
    std::vector<std::wstring> chunks;
    Expect(utf8.StreamText([&](const std::wstring& chunk) {
            chunks.push_back(chunk);
            streamed += chunk;
            return true;
        }, {}, error),
        "UTF-8 streaming succeeds when a supplementary scalar crosses both mapping and output chunk boundaries");
    ExpectText(streamed, expected, "UTF-8 streaming preserves the complete supplementary scalar");
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        Expect(chunks[index].empty() || chunks[index].back() != emojiUnits[0],
            "StreamText never ends a callback chunk with a high surrogate");
        Expect(chunks[index].empty() || chunks[index].front() != emojiUnits[1],
            "StreamText never begins a callback chunk with a low surrogate");
    }

    const auto encodeStream = [&](const LargeDocument& source, classic_notepad::TextEncoding encoding,
        std::vector<std::uint8_t>& output) {
        bool includeBom = true;
        output.clear();
        return source.StreamText([&](const std::wstring& chunk) {
            std::vector<std::uint8_t> encoded;
            if (!classic_notepad::EncodeTextChunk(chunk, encoding, includeBom, encoded, error)) return false;
            includeBom = false;
            output.insert(output.end(), encoded.begin(), encoded.end());
            return true;
        }, {}, error);
    };
    std::vector<std::uint8_t> saved;
    Expect(encodeStream(utf8, classic_notepad::TextEncoding::Utf8NoBom, saved),
        "Chunked UTF-8 no-BOM save accepts the complete streamed scalar");
    Expect(saved == utf8Source, "Chunked UTF-8 no-BOM save matches the expected source bytes");
    Expect(encodeStream(utf8, classic_notepad::TextEncoding::Utf8Bom, saved),
        "Chunked UTF-8 BOM save accepts the complete streamed scalar");
    Expect(saved.size() == utf8Source.size() + 3U && saved[0] == 0xEFU && saved[1] == 0xBBU &&
            saved[2] == 0xBFU && std::equal(utf8Source.begin(), utf8Source.end(), saved.begin() + 3U),
        "Chunked UTF-8 BOM save emits BOM once and preserves every code unit");

    std::vector<std::uint8_t> utf16Expected{ 0xFFU, 0xFEU };
    for (wchar_t character : expected) {
        const std::uint16_t unit = static_cast<std::uint16_t>(character);
        utf16Expected.push_back(static_cast<std::uint8_t>(unit & 0xFFU));
        utf16Expected.push_back(static_cast<std::uint8_t>(unit >> 8U));
    }
    Expect(encodeStream(utf8, classic_notepad::TextEncoding::Utf16LeBom, saved),
        "Chunked UTF-16 LE save accepts the complete streamed scalar");
    Expect(saved == utf16Expected, "Chunked UTF-16 LE save matches the expected source bytes");

    // A UTF-8 BOM shifts the physical mapping edge without changing the
    // document coordinate. Put the supplementary scalar across that edge and
    // verify exact code-unit reads as well as save streaming from this source.
    constexpr std::size_t bomPrefixUnits = 65532U;
    const std::wstring bomExpected = std::wstring(bomPrefixUnits, L'x') + emojiUnits + L'z';
    std::vector<std::uint8_t> utf8BomSource{ 0xEFU, 0xBBU, 0xBFU };
    utf8BomSource.insert(utf8BomSource.end(), bomPrefixUnits, static_cast<std::uint8_t>('x'));
    utf8BomSource.insert(utf8BomSource.end(), { 0xF0U, 0x9FU, 0x98U, 0x80U, static_cast<std::uint8_t>('z') });
    auto utf8BomReader = [&utf8BomSource](std::uint64_t offset, std::size_t length,
        std::vector<std::uint8_t>& bytes) {
        if (offset > utf8BomSource.size() || length > utf8BomSource.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        bytes.assign(utf8BomSource.begin() + static_cast<std::size_t>(offset),
            utf8BomSource.begin() + static_cast<std::size_t>(offset) + length);
        return true;
    };
    LargeDocument utf8Bom(LargeFileEncoding::Utf8, utf8BomReader);
    Expect(utf8Bom.Initialize({ LargeDocumentOriginalPiece{
            3U, static_cast<std::uint64_t>(utf8BomSource.size() - 3U),
            static_cast<std::uint64_t>(bomExpected.size()) } }, error),
        "UTF-8 BOM large document initializes with a scalar crossing the physical mapping edge");
    Expect(utf8Bom.ReadRange(bomPrefixUnits, 1U, oneUnit, error) && oneUnit == std::wstring(1U, emojiUnits[0]) &&
            utf8Bom.ReadRange(bomPrefixUnits + 1U, 1U, oneUnit, error) &&
            oneUnit == std::wstring(1U, emojiUnits[1]) &&
            utf8Bom.ReadRange(bomPrefixUnits + 2U, 1U, oneUnit, error) && oneUnit == L"z",
        "UTF-8 BOM ReadRange preserves exact high, low, and following UTF-16 units");
    Expect(utf8Bom.IsCharacterBoundary(bomPrefixUnits, error) &&
            !utf8Bom.IsCharacterBoundary(bomPrefixUnits + 1U, error) &&
            utf8Bom.IsCharacterBoundary(bomPrefixUnits + 2U, error),
        "UTF-8 BOM edit boundary rejects only the position between the surrogate pair");
    streamed.clear();
    Expect(utf8Bom.StreamText([&](const std::wstring& chunk) {
            streamed += chunk;
            return true;
        }, {}, error), "UTF-8 BOM streaming crosses a source mapping boundary without splitting a scalar");
    ExpectText(streamed, bomExpected, "UTF-8 BOM streamed text matches its complete expected code units");

    const auto expectedUtf8Bytes = [](std::size_t prefixUnits, bool includeBom) {
        std::vector<std::uint8_t> bytes;
        if (includeBom) bytes.insert(bytes.end(), { 0xEFU, 0xBBU, 0xBFU });
        bytes.insert(bytes.end(), prefixUnits, static_cast<std::uint8_t>('x'));
        bytes.insert(bytes.end(), { 0xF0U, 0x9FU, 0x98U, 0x80U, static_cast<std::uint8_t>('z') });
        return bytes;
    };
    const auto expectedUtf16Bytes = [](std::size_t prefixUnits, bool includeBom) {
        std::vector<std::uint8_t> bytes;
        if (includeBom) bytes.insert(bytes.end(), { 0xFFU, 0xFEU });
        for (std::size_t index = 0; index < prefixUnits; ++index) bytes.insert(bytes.end(), { 0x78U, 0x00U });
        bytes.insert(bytes.end(), { 0x3DU, 0xD8U, 0x00U, 0xDEU, 0x7AU, 0x00U });
        return bytes;
    };
    const auto expectNamed = [](bool result, const std::string& message) {
        Expect(result, message.c_str());
    };
    const auto verifySaves = [&](const LargeDocument& source, std::size_t prefixUnits, const char* sourceName) {
        expectNamed(encodeStream(source, classic_notepad::TextEncoding::Utf8NoBom, saved),
            std::string(sourceName) + " streams safely to UTF-8 without a BOM");
        expectNamed(saved == expectedUtf8Bytes(prefixUnits, false),
            std::string(sourceName) + " UTF-8 no-BOM save preserves exact scalar bytes");
        expectNamed(encodeStream(source, classic_notepad::TextEncoding::Utf8Bom, saved),
            std::string(sourceName) + " streams safely to UTF-8 with a BOM");
        expectNamed(saved == expectedUtf8Bytes(prefixUnits, true),
            std::string(sourceName) + " UTF-8 BOM save emits one BOM and preserves exact scalar bytes");
        expectNamed(encodeStream(source, classic_notepad::TextEncoding::Utf16LeBom, saved),
            std::string(sourceName) + " streams safely to UTF-16 LE");
        expectNamed(saved == expectedUtf16Bytes(prefixUnits, true),
            std::string(sourceName) + " UTF-16 LE save preserves exact surrogate code units");
    };
    verifySaves(utf8, 65535U, "UTF-8 no-BOM source");
    verifySaves(utf8Bom, bomPrefixUnits, "UTF-8 BOM source");

    const std::wstring utf16ExpectedText = std::wstring(32767U, L'x') + emojiUnits + L'z';
    std::vector<std::uint8_t> utf16Source;
    utf16Source.reserve(utf16ExpectedText.size() * 2U);
    for (wchar_t character : utf16ExpectedText) {
        const std::uint16_t unit = static_cast<std::uint16_t>(character);
        utf16Source.push_back(static_cast<std::uint8_t>(unit & 0xFFU));
        utf16Source.push_back(static_cast<std::uint8_t>(unit >> 8U));
    }
    auto utf16Reader = [&utf16Source](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
        if (offset > utf16Source.size() || length > utf16Source.size() - static_cast<std::size_t>(offset)) return false;
        bytes.assign(utf16Source.begin() + static_cast<std::size_t>(offset),
            utf16Source.begin() + static_cast<std::size_t>(offset) + length);
        return true;
    };
    LargeDocument utf16(LargeFileEncoding::Utf16Le, utf16Reader);
    const std::uint64_t firstPieceBytes = 32768U * 2U;
    Expect(utf16.Initialize({
            LargeDocumentOriginalPiece{ 0U, firstPieceBytes, 32768U, 0U, L'x', emojiUnits[0], true,
                {}, false, 32768U, true },
            LargeDocumentOriginalPiece{ firstPieceBytes,
                static_cast<std::uint64_t>(utf16Source.size()) - firstPieceBytes,
                static_cast<std::uint64_t>(utf16ExpectedText.size()) - 32768U, 0U,
                emojiUnits[1], L'z', true, {}, false, 2U, true } }, error),
        "UTF-16 large document initializes with a supplementary scalar split across a piece and 64 KiB edge");
    Expect(utf16.DisplayedCharacterCount() ==
            classic_notepad::CountStatusCharacters(utf16ExpectedText),
        "Displayed-character summary joins a surrogate pair across original-piece summaries");
    streamed.clear();
    chunks.clear();
    Expect(utf16.StreamText([&](const std::wstring& chunk) {
            chunks.push_back(chunk);
            streamed += chunk;
            return true;
        }, {}, error),
        "UTF-16 streaming joins a surrogate pair split between original pieces");
    ExpectText(streamed, utf16ExpectedText, "UTF-16 piece-boundary streaming preserves both surrogate code units");
    Expect(utf16.ReadRange(32767U, 1U, oneUnit, error) && oneUnit == std::wstring(1U, emojiUnits[0]) &&
            utf16.ReadRange(32768U, 1U, oneUnit, error) && oneUnit == std::wstring(1U, emojiUnits[1]) &&
            utf16.ReadRange(32769U, 1U, oneUnit, error) && oneUnit == L"z",
        "UTF-16 ReadRange preserves exact units when a pair crosses a source piece");
    Expect(utf16.IsCharacterBoundary(32767U, error) && !utf16.IsCharacterBoundary(32768U, error) &&
            utf16.IsCharacterBoundary(32769U, error),
        "UTF-16 edit boundary rejects only the position between a valid pair");
    verifySaves(utf16, 32767U, "UTF-16 source");
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        Expect(chunks[index].empty() || chunks[index].back() != emojiUnits[0],
            "UTF-16 stream chunks never split a pair at the high surrogate");
        Expect(chunks[index].empty() || chunks[index].front() != emojiUnits[1],
            "UTF-16 stream chunks never split a pair at the low surrogate");
    }
}

void TestLargeDocumentBoundedStreamingAndCancellation()
{
    using classic_notepad::LargeDocument;
    using classic_notepad::LargeDocumentOriginalPiece;
    using classic_notepad::LargeFileEncoding;

    struct ReadRequest {
        std::uint64_t offset = 0;
        std::size_t length = 0;
    };
    struct ReaderState {
        std::vector<std::uint8_t> bytes;
        std::vector<ReadRequest> reads;
        std::uint64_t failFrom = (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t shortAt = (std::numeric_limits<std::uint64_t>::max)();
        bool* cancelAfterRead = nullptr;
        std::size_t cancelOnRead = 1U;
    };
    const auto makeReader = [](const std::shared_ptr<ReaderState>& state) {
        return [state](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
            state->reads.push_back({offset, length});
            if (offset >= state->failFrom || offset > state->bytes.size() ||
                length > state->bytes.size() - static_cast<std::size_t>(offset)) return false;
            bytes.assign(state->bytes.begin() + static_cast<std::size_t>(offset),
                state->bytes.begin() + static_cast<std::size_t>(offset) + length);
            if (offset == state->shortAt && !bytes.empty()) bytes.pop_back();
            if (state->cancelAfterRead != nullptr && state->reads.size() == state->cancelOnRead) {
                *state->cancelAfterRead = true;
            }
            return true;
        };
    };

    constexpr std::size_t largeByteCount = 1024U * 1024U;
    constexpr std::size_t needleOffset = 900000U;
    auto largeState = std::make_shared<ReaderState>();
    largeState->bytes.assign(largeByteCount, static_cast<std::uint8_t>('a'));
    std::copy_n("needle", 6U, largeState->bytes.begin() + needleOffset);
    LargeDocument large(LargeFileEncoding::Utf8, makeReader(largeState));
    std::wstring error;
    Expect(large.Initialize({LargeDocumentOriginalPiece{
            0U, largeState->bytes.size(), largeState->bytes.size(), 0U, L'a', L'a', true,
            PlainWidthSummary(largeState->bytes.size()), true}}, error),
        "Large summary-known original source initializes without reading or materializing its contents");

    std::wstring streamed;
    std::size_t readsAtFirstCallback = 0;
    std::uint64_t firstCallbackSourceEnd = 0;
    std::size_t maximumCallback = 0;
    Expect(large.StreamText([&](const std::wstring& chunk) {
            if (readsAtFirstCallback == 0U && !largeState->reads.empty()) {
                readsAtFirstCallback = largeState->reads.size();
                const ReadRequest& first = largeState->reads.back();
                firstCallbackSourceEnd = first.offset + first.length;
            }
            maximumCallback = std::max(maximumCallback, chunk.size());
            streamed.append(chunk);
            return true;
        }, {}, error), "Large original source streams through bounded decoded blocks");
    std::wstring expected(largeState->bytes.begin(), largeState->bytes.end());
    ExpectText(streamed, expected, "Bounded source streaming preserves the exact large ASCII text");
    Expect(readsAtFirstCallback != 0U && firstCallbackSourceEnd < largeState->bytes.size(),
        "First stream callback arrives before the source tail is read");
    Expect(maximumCallback <= 65537U, "Large stream callbacks remain within the 65,537-unit cap");
    std::uint64_t expectedOffset = 0;
    bool requestsBounded = !largeState->reads.empty();
    for (const ReadRequest& request : largeState->reads) {
        requestsBounded = requestsBounded && request.length <= 64U * 1024U && request.offset == expectedOffset;
        expectedOffset = request.offset + request.length;
    }
    Expect(requestsBounded && expectedOffset == largeState->bytes.size(),
        "Source decoding uses bounded sequential requests after its one start translation");

    largeState->reads.clear();
    std::size_t earlyStopCallbacks = 0;
    error.clear();
    const bool earlyStopped = large.StreamText([&](const std::wstring&) {
        ++earlyStopCallbacks;
        return false;
    }, {}, error);
    Expect(!earlyStopped && error.empty() && earlyStopCallbacks == 1U && largeState->reads.size() == 1U,
        "Visitor early-stop preserves no-error semantics and prevents subsequent source reads");

    largeState->reads.clear();
    classic_notepad::LargeDocumentMatch match{};
    error.clear();
    Expect(large.Find(L"needle", 0U, true, false, true, match, error) &&
            match.start == needleOffset && match.length == 6U,
        "Find scans the large original source through bounded StreamText decoding");
    bool findRequestsBounded = !largeState->reads.empty();
    for (const ReadRequest& request : largeState->reads) {
        findRequestsBounded = findRequestsBounded && request.length <= 64U * 1024U;
    }
    Expect(findRequestsBounded, "Find source reads remain bounded to 64 KiB");

    largeState->reads.clear();
    bool replaceAllCancelled = false;
    error.clear();
    const std::uint64_t replacements = large.ReplaceAll(
        L"not-present", L"unused", true, false, replaceAllCancelled, {}, error);
    bool replaceAllRequestsBounded = !largeState->reads.empty();
    for (const ReadRequest& request : largeState->reads) {
        replaceAllRequestsBounded = replaceAllRequestsBounded && request.length <= 64U * 1024U;
    }
    Expect(replacements == 0U && !replaceAllCancelled && error.empty() && replaceAllRequestsBounded,
        "Replace All scan uses bounded StreamText decoding across the source");

    auto backwardState = std::make_shared<ReaderState>();
    backwardState->bytes.assign(largeByteCount, static_cast<std::uint8_t>('a'));
    backwardState->bytes.front() = static_cast<std::uint8_t>('z');
    LargeDocument backward(LargeFileEncoding::Utf8, makeReader(backwardState));
    error.clear();
    Expect(backward.Initialize({LargeDocumentOriginalPiece{
            0U, backwardState->bytes.size(), backwardState->bytes.size(), 0U, L'z', L'a', true,
            PlainWidthSummary(backwardState->bytes.size()), true}}, error),
        "Backward Find cancellation fixture initializes with an early candidate");

    backwardState->cancelOnRead = 2U;
    bool cancelBackward = false;
    backwardState->cancelAfterRead = &cancelBackward;
    classic_notepad::LargeDocumentMatch backwardMatch{17U, 19U};
    error.clear();
    const bool foundAfterCancel = backward.Find(L"z", backward.CharacterCount(), true, false, false,
        backwardMatch, error, [&cancelBackward] { return cancelBackward; });
    backwardState->cancelAfterRead = nullptr;
    Expect(!foundAfterCancel && error == L"The operation was cancelled." &&
            backwardMatch.start == 0U && backwardMatch.length == 0U &&
            backwardState->reads.size() == 2U && cancelBackward,
        "Backward Find never reports an early candidate as success after a later read cancellation");

    backwardState->reads.clear();
    backwardState->failFrom = 64U * 1024U;
    backwardMatch = {17U, 19U};
    error.clear();
    const bool foundAfterReadFailure = backward.Find(L"z", backward.CharacterCount(), true, false, false,
        backwardMatch, error);
    backwardState->failFrom = (std::numeric_limits<std::uint64_t>::max)();
    Expect(!foundAfterReadFailure && error == L"The mapped source could not be read." &&
            backwardMatch.start == 0U && backwardMatch.length == 0U &&
            backwardState->reads.size() == 2U,
        "Backward Find never reports an early candidate as success after a later source read failure");

    auto summaryState = std::make_shared<ReaderState>();
    summaryState->bytes.assign(131072U, static_cast<std::uint8_t>('x'));
    summaryState->bytes[65535U] = static_cast<std::uint8_t>('\r');
    summaryState->bytes[65536U] = static_cast<std::uint8_t>('\n');
    LargeDocument unknownSummary(LargeFileEncoding::Utf8, makeReader(summaryState));
    error.clear();
    Expect(unknownSummary.Initialize({LargeDocumentOriginalPiece{
            0U, summaryState->bytes.size(), summaryState->bytes.size()}}, error),
        "Unknown-summary original source initializes with incremental bounded decoding");
    bool summaryRequestsBounded = !summaryState->reads.empty();
    for (const ReadRequest& request : summaryState->reads) {
        summaryRequestsBounded = summaryRequestsBounded && request.length <= 64U * 1024U;
    }
    Expect(summaryRequestsBounded && unknownSummary.LineCount() == 2U,
        "Incremental source summary handles CRLF split across decoded blocks with bounded reads");

    const std::wstring emojiUnits{
        static_cast<wchar_t>(0xD83DU), static_cast<wchar_t>(0xDE00U) };
    std::wstring utf16Expected(32767U, static_cast<wchar_t>(0x4E2DU));
    utf16Expected += emojiUnits;
    utf16Expected += L"\r\n";
    utf16Expected.append(32768U, L'Q');
    auto utf16State = std::make_shared<ReaderState>();
    utf16State->bytes.reserve(utf16Expected.size() * 2U);
    for (wchar_t character : utf16Expected) {
        const std::uint16_t unit = static_cast<std::uint16_t>(character);
        utf16State->bytes.push_back(static_cast<std::uint8_t>(unit & 0xFFU));
        utf16State->bytes.push_back(static_cast<std::uint8_t>(unit >> 8U));
    }
    LargeDocument utf16(LargeFileEncoding::Utf16Le, makeReader(utf16State));
    error.clear();
    Expect(utf16.Initialize({LargeDocumentOriginalPiece{
            0U, utf16State->bytes.size(), utf16Expected.size(), 1U,
            utf16Expected.front(), utf16Expected.back(), true}}, error),
        "UTF-16 source with a supplementary pair across the 64 KiB byte edge initializes");
    std::wstring utf16Streamed;
    std::size_t maximumUtf16Callback = 0;
    std::vector<std::wstring> utf16Chunks;
    Expect(utf16.StreamText([&](const std::wstring& chunk) {
            utf16Chunks.push_back(chunk);
            maximumUtf16Callback = std::max(maximumUtf16Callback, chunk.size());
            utf16Streamed.append(chunk);
            return true;
        }, {}, error), "UTF-16 source streams across byte and scalar boundaries");
    ExpectText(utf16Streamed, utf16Expected,
        "UTF-16 streaming preserves ASCII, BMP, supplementary, and CRLF text exactly");
    bool utf16PairsIntact = maximumUtf16Callback <= 65537U;
    for (const std::wstring& chunk : utf16Chunks) {
        utf16PairsIntact = utf16PairsIntact && (chunk.empty() || chunk.back() != emojiUnits[0]) &&
            (chunk.empty() || chunk.front() != emojiUnits[1]);
    }
    Expect(utf16PairsIntact, "UTF-16 callback chunks do not split valid surrogate pairs");

    auto cancelState = std::make_shared<ReaderState>();
    cancelState->bytes.assign(8192U, static_cast<std::uint8_t>('c'));
    LargeDocument cancelDocument(LargeFileEncoding::Utf8, makeReader(cancelState));
    error.clear();
    Expect(cancelDocument.Initialize({LargeDocumentOriginalPiece{
            0U, cancelState->bytes.size(), cancelState->bytes.size(), 0U, L'c', L'c', true}}, error),
        "Cancellation fixture initializes with a known source summary");
    std::uint64_t afterStart = 0;
    std::uint64_t afterEnd = 0;
    Expect(cancelDocument.Replace(0U, 1U, L"", 0U, 1U, afterStart, afterEnd, error),
        "Cancellation fixture creates a cold nonzero source coordinate");
    cancelState->reads.clear();
    bool shouldCancel = false;
    cancelState->cancelAfterRead = &shouldCancel;
    std::size_t cancelledCallbacks = 0;
    error.clear();
    const bool cancelledStream = cancelDocument.StreamText([&](const std::wstring&) {
        ++cancelledCallbacks;
        return true;
    }, [&shouldCancel] { return shouldCancel; }, error);
    cancelState->cancelAfterRead = nullptr;
    Expect(!cancelledStream && error == L"The operation was cancelled." &&
            cancelState->reads.size() == 1U && cancelledCallbacks == 0U,
        "Cancellation during cold coordinate translation prevents every later read and callback");

    auto transactionState = std::make_shared<ReaderState>();
    transactionState->bytes = {'o', 'l', 'd', 'n', 'e', 'w'};
    LargeDocument transactional(LargeFileEncoding::Utf8, makeReader(transactionState));
    error.clear();
    Expect(transactional.Initialize({LargeDocumentOriginalPiece{0U, 3U, 3U, 0U, L'o', L'd', true}}, error),
        "Unknown-summary failure transaction baseline initializes");
    Expect(transactional.Replace(3U, 3U, L"!", 3U, 3U, afterStart, afterEnd, error),
        "Unknown-summary failure transaction baseline becomes dirty and undoable");
    const auto expectAppendFailurePreservesState = [&](std::uint64_t failOffset, std::uint64_t shortOffset,
        const char* label) {
        std::wstring beforeText;
        error.clear();
        const bool capturedText = transactional.ReadRange(0U, transactional.CharacterCount(), beforeText, error);
        const std::uint64_t beforeCharacters = transactional.CharacterCount();
        const std::uint64_t beforeLines = transactional.LineCount();
        const std::uint64_t beforeGeneration = transactional.Generation();
        const std::uint64_t beforeUndo = transactional.UndoBytes();
        const bool beforeDirty = transactional.IsModified();
        const bool beforeCanUndo = transactional.CanUndo();
        const std::uint64_t beforeSavedState = transactional.SavedState();
        transactionState->failFrom = failOffset;
        transactionState->shortAt = shortOffset;
        error.clear();
        const bool appended = transactional.AppendOriginalPieces({LargeDocumentOriginalPiece{
            3U, 3U, 3U}}, error);
        transactionState->failFrom = (std::numeric_limits<std::uint64_t>::max)();
        transactionState->shortAt = (std::numeric_limits<std::uint64_t>::max)();
        std::wstring afterText;
        std::wstring readError;
        const bool capturedAfter = transactional.ReadRange(0U, transactional.CharacterCount(), afterText, readError);
        Expect(capturedText && !appended && !error.empty() && capturedAfter && beforeText == afterText &&
                transactional.CharacterCount() == beforeCharacters && transactional.LineCount() == beforeLines &&
                transactional.Generation() == beforeGeneration && transactional.UndoBytes() == beforeUndo &&
                transactional.IsModified() == beforeDirty && transactional.CanUndo() == beforeCanUndo &&
                transactional.SavedState() == beforeSavedState,
            label);
    };
    expectAppendFailurePreservesState(3U, (std::numeric_limits<std::uint64_t>::max)(),
        "Failed unknown-summary source read preserves content, count, dirty state, generation, and Undo");
    expectAppendFailurePreservesState((std::numeric_limits<std::uint64_t>::max)(), 3U,
        "Short unknown-summary source read preserves content, count, dirty state, generation, and Undo");
}

struct CountedLargeDocumentRead {
    std::uint64_t offset = 0;
    std::size_t length = 0;
};

struct CountedLargeDocumentReaderState {
    std::vector<std::uint8_t> bytes;
    std::vector<CountedLargeDocumentRead> reads;
};

void TestLargeDocumentCheckpointReuseAndUnicodeWindows()
{
    using classic_notepad::LargeDocument;
    using classic_notepad::LargeDocumentOriginalPiece;
    using classic_notepad::LargeFileEncoding;

    const auto makeReader = [](const std::shared_ptr<CountedLargeDocumentReaderState>& state) {
        return [state](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
            state->reads.push_back({offset, length});
            if (offset > state->bytes.size() || length > state->bytes.size() - static_cast<std::size_t>(offset)) {
                return false;
            }
            bytes.assign(state->bytes.begin() + static_cast<std::size_t>(offset),
                state->bytes.begin() + static_cast<std::size_t>(offset) + length);
            return true;
        };
    };

    {
        // This source fits in a single maximum-sized B1 descriptor. The cold
        // read must seed an interior ASCII checkpoint, and a nearby warm read
        // must translate from it using only the bounded prefix it needs.
        auto state = std::make_shared<CountedLargeDocumentReaderState>();
        state->bytes.assign(60U * 1024U, static_cast<std::uint8_t>('a'));
        LargeDocument document(LargeFileEncoding::Utf8, makeReader(state));
        std::wstring error;
        Expect(document.Initialize({LargeDocumentOriginalPiece{
                0U, state->bytes.size(), state->bytes.size(), 0U, L'a', L'a', true,
                PlainWidthSummary(state->bytes.size()), true}}, error),
            "Checkpoint fixture initializes from a B1-sized ASCII descriptor without reading it");

        std::wstring oneUnit;
        Expect(document.ReadRange(8000U, 1U, oneUnit, error) && oneUnit == L"a" && state->reads.size() == 2U,
            "Cold ASCII read succeeds and records its translation plus code-unit read");
        Expect(state->reads[0].offset == 0U && state->reads[0].length == 3U * 8000U + 1U,
            "Cold ASCII translation starts at the source origin and is bounded by target distance");

        state->reads.clear();
        Expect(document.ReadRange(8001U, 1U, oneUnit, error) && oneUnit == L"a" && state->reads.size() == 2U,
            "Nearby warm ASCII read succeeds");
        const CountedLargeDocumentRead translation = state->reads.front();
        Expect(translation.offset == 4096U && translation.length == 3U * (8001U - 4096U) + 1U &&
                translation.length < state->bytes.size() - translation.offset,
            "Warm ASCII translation starts from the interior checkpoint and requests less than the remaining suffix");
    }

    {
        // A single shared-core descriptor may be larger than the Windows
        // indexer's B1 limit. Sparse non-ASCII scalars make ASCII checkpoints
        // observable before the first multibyte scalar.
        auto state = std::make_shared<CountedLargeDocumentReaderState>();
        constexpr std::size_t cycleCount = 40U;
        constexpr std::size_t asciiRun = 5000U;
        for (std::size_t cycle = 0; cycle < cycleCount; ++cycle) {
            state->bytes.insert(state->bytes.end(), asciiRun, static_cast<std::uint8_t>('a'));
            state->bytes.insert(state->bytes.end(), {0xC3U, 0xA9U, 0xF0U, 0x9FU, 0x98U, 0x80U});
        }
        const std::uint64_t characterCount = cycleCount * (asciiRun + 3U);
        Expect(state->bytes.size() > classic_notepad::kLargeFileOriginalPieceMaximumBytes,
            "Shared-core checkpoint fixture exceeds the B1 descriptor byte limit");

        LargeDocument document(LargeFileEncoding::Utf8, makeReader(state));
        std::wstring error;
        Expect(document.Initialize({LargeDocumentOriginalPiece{
                0U, state->bytes.size(), characterCount, 0U, L'a', static_cast<wchar_t>(0xDE00U), true,
                PlainWidthSummary(characterCount), true}}, error),
            "Large shared-core checkpoint fixture initializes without source reads");

        std::wstring oneUnit;
        Expect(document.ReadRange(4500U, 1U, oneUnit, error) && oneUnit == L"a" && state->reads.size() == 2U,
            "Cold mixed-UTF-8 read succeeds in the long initial ASCII run");
        Expect(state->reads[0].offset == 0U && state->reads[0].length == 3U * 4500U + 1U,
            "Cold mixed-UTF-8 translation is bounded by its remaining target distance");

        state->reads.clear();
        Expect(document.ReadRange(4501U, 1U, oneUnit, error) && oneUnit == L"a" && state->reads.size() == 2U,
            "Nearby warm mixed-UTF-8 read succeeds");
        const CountedLargeDocumentRead warmTranslation = state->reads.front();
        Expect(warmTranslation.offset == 4096U && warmTranslation.length <= 3U * (4501U - 4096U) + 1U &&
                warmTranslation.length < state->bytes.size() - warmTranslation.offset,
            "Warm mixed-UTF-8 translation reuses an ASCII checkpoint and stays bounded");

        auto snapshot = document.Clone();
        std::uint64_t selectionStart = 0;
        std::uint64_t selectionEnd = 0;
        Expect(snapshot != nullptr && snapshot->Replace(100U, 100U, L"Z", 100U, 100U,
                selectionStart, selectionEnd, error),
            "Snapshot edit splits the original source piece while retaining shared checkpoint data");

        state->reads.clear();
        Expect(snapshot->ReadRange(4503U, 1U, oneUnit, error) && oneUnit == L"a" && !state->reads.empty() &&
                state->reads.front().offset == 4096U &&
                state->reads.front().length <= 3U * (4502U - 4096U) + 1U,
            "Edited snapshot reuses its shared source checkpoint after piece splitting");

        state->reads.clear();
        Expect(document.ReadRange(4502U, 1U, oneUnit, error) && oneUnit == L"a" && !state->reads.empty() &&
                state->reads.front().offset == 4096U,
            "Unedited snapshot retains the shared checkpoint after its sibling edit");
    }

    {
        auto state = std::make_shared<CountedLargeDocumentReaderState>();
        state->bytes.assign(4095U, static_cast<std::uint8_t>('a'));
        state->bytes.insert(state->bytes.end(), {0xC3U, 0xA9U, 0xF0U, 0x9FU, 0x98U, 0x80U, 'z'});
        const std::wstring emojiUnits{static_cast<wchar_t>(0xD83DU), static_cast<wchar_t>(0xDE00U)};
        std::wstring expected(4095U, L'a');
        expected += L'\u00E9';
        expected += emojiUnits;
        expected += L'z';
        LargeDocument document(LargeFileEncoding::Utf8, makeReader(state));
        std::wstring error;
        Expect(document.Initialize({LargeDocumentOriginalPiece{
                0U, state->bytes.size(), expected.size(), 0U, expected.front(), expected.back(), true}}, error),
            "Checkpoint-boundary Unicode fixture initializes without source reads");

        const std::vector<std::pair<std::uint64_t, std::wstring>> windows{
            {4094U, std::wstring(1U, L'a')},
            {4095U, std::wstring(1U, L'\u00E9')},
            {4096U, std::wstring(1U, emojiUnits[0])},
            {4097U, std::wstring(1U, emojiUnits[1])},
            {4098U, std::wstring(1U, L'z')},
        };
        for (const auto& window : windows) {
            std::wstring actual;
            Expect(document.ReadRange(window.first, 1U, actual, error) && actual == window.second,
                "One-unit ASCII/BMP/emoji read is exact before, on, and after a checkpoint boundary");
        }
        std::wstring shortWindow;
        Expect(document.ReadRange(4094U, 5U, shortWindow, error) && shortWindow == expected.substr(4094U, 5U),
            "Short window spanning a checkpoint and a supplementary scalar preserves exact UTF-16 units");
        Expect(document.IsCharacterBoundary(4096U, error) && !document.IsCharacterBoundary(4097U, error) &&
                document.IsCharacterBoundary(4098U, error),
            "Checkpoint lookup preserves scalar-safe edit-boundary semantics");
        std::uint64_t selectionStart = 17U;
        std::uint64_t selectionEnd = 23U;
        Expect(!document.Replace(4097U, 4097U, L"X", selectionStart, selectionEnd,
                selectionStart, selectionEnd, error) && !error.empty(),
            "Edit at the low-surrogate boundary is explicitly rejected");
    }
}

struct LargeDocumentFaultReaderState {
    std::string source;
    std::size_t calls = 0;
    std::size_t failAt = 0;
};

struct LargeDocumentFaultFixture {
    std::unique_ptr<classic_notepad::LargeDocument> document;
    std::shared_ptr<LargeDocumentFaultReaderState> reader;
};

struct LargeDocumentSnapshot {
    std::wstring text;
    std::uint64_t characters = 0;
    std::uint64_t lines = 0;
    std::uint64_t maximumLineColumns = 0;
    std::uint64_t pieces = 0;
    std::uint64_t insertedBytes = 0;
    std::uint64_t generation = 0;
    std::uint64_t savedState = 0;
    std::uint64_t undoBytes = 0;
    bool modified = false;
    bool canUndo = false;
};

LargeDocumentFaultFixture MakeLargeDocumentFaultFixture()
{
    using classic_notepad::LargeDocument;
    using classic_notepad::LargeDocumentOriginalPiece;
    using classic_notepad::LargeFileEncoding;

    LargeDocumentFaultFixture fixture;
    fixture.reader = std::make_shared<LargeDocumentFaultReaderState>();
    fixture.reader->source = "abc\nabc\nabc\nabc\nabc\nxyz";
    fixture.document = std::make_unique<LargeDocument>(LargeFileEncoding::Utf8,
        [reader = fixture.reader](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
            ++reader->calls;
            if (reader->failAt != 0U && reader->calls == reader->failAt) return false;
            if (offset > reader->source.size() || length > reader->source.size() - static_cast<std::size_t>(offset)) {
                return false;
            }
            bytes.assign(reader->source.begin() + static_cast<std::size_t>(offset),
                reader->source.begin() + static_cast<std::size_t>(offset) + length);
            return true;
        });

    std::uint64_t lineBreaks = 0;
    for (std::size_t index = 0; index < fixture.reader->source.size(); ++index) {
        if (fixture.reader->source[index] == '\r') {
            ++lineBreaks;
            if (index + 1U < fixture.reader->source.size() && fixture.reader->source[index + 1U] == '\n') ++index;
        } else if (fixture.reader->source[index] == '\n') {
            ++lineBreaks;
        }
    }
    const std::string& source = fixture.reader->source;
    std::wstring error;
    const LargeDocumentOriginalPiece piece{
        0,
        static_cast<std::uint64_t>(source.size()),
        static_cast<std::uint64_t>(source.size()),
        lineBreaks,
        static_cast<wchar_t>(static_cast<unsigned char>(source.front())),
        static_cast<wchar_t>(static_cast<unsigned char>(source.back())),
        true};
    Expect(fixture.document->Initialize({piece}, error), "Fault-injection document initializes without source reads");
    return fixture;
}

bool CaptureLargeDocumentSnapshot(
    const classic_notepad::LargeDocument& document,
    LargeDocumentSnapshot& snapshot)
{
    std::wstring error;
    if (!document.ReadRange(0, document.CharacterCount(), snapshot.text, error)) return false;
    snapshot.characters = document.CharacterCount();
    snapshot.lines = document.LineCount();
    snapshot.maximumLineColumns = document.MaximumLineColumns();
    snapshot.pieces = document.PieceCount();
    snapshot.insertedBytes = document.InsertedBufferBytes();
    snapshot.generation = document.Generation();
    snapshot.savedState = document.SavedState();
    snapshot.undoBytes = document.UndoBytes();
    snapshot.modified = document.IsModified();
    snapshot.canUndo = document.CanUndo();
    return true;
}

bool SameLargeDocumentSnapshot(const LargeDocumentSnapshot& left, const LargeDocumentSnapshot& right)
{
    return left.text == right.text && left.characters == right.characters && left.lines == right.lines &&
        left.maximumLineColumns == right.maximumLineColumns &&
        left.pieces == right.pieces && left.insertedBytes == right.insertedBytes &&
        left.generation == right.generation && left.savedState == right.savedState &&
        left.undoBytes == right.undoBytes && left.modified == right.modified && left.canUndo == right.canUndo;
}

enum class LargeDocumentFaultScenario {
    Insertion,
    Deletion,
    Replacement,
    Undo,
    ReplaceAll,
    CompoundUndo,
};

const char* LargeDocumentFaultScenarioName(LargeDocumentFaultScenario scenario)
{
    switch (scenario) {
    case LargeDocumentFaultScenario::Insertion: return "insertion";
    case LargeDocumentFaultScenario::Deletion: return "deletion";
    case LargeDocumentFaultScenario::Replacement: return "replacement";
    case LargeDocumentFaultScenario::Undo: return "undo";
    case LargeDocumentFaultScenario::ReplaceAll: return "Replace All";
    case LargeDocumentFaultScenario::CompoundUndo: return "compound undo";
    }
    return "unknown";
}

bool PrepareLargeDocumentFaultScenario(
    LargeDocumentFaultFixture& fixture,
    LargeDocumentFaultScenario scenario)
{
    std::wstring error;
    std::uint64_t afterStart = 0;
    std::uint64_t afterEnd = 0;
    if (!fixture.document->Replace(9, 10, L"S", 4, 5, afterStart, afterEnd, error)) return false;
    fixture.document->MarkSaved();
    if (scenario == LargeDocumentFaultScenario::Undo) {
        return fixture.document->Replace(5, 5, L"", 5, 5, afterStart, afterEnd, error);
    }
    if (scenario == LargeDocumentFaultScenario::CompoundUndo) {
        bool cancelled = false;
        return fixture.document->ReplaceAll(L"abc", L"Z", true, false, cancelled, {}, error) > 1U &&
            !cancelled && error.empty();
    }
    return true;
}

bool RunLargeDocumentFaultScenario(
    classic_notepad::LargeDocument& document,
    LargeDocumentFaultScenario scenario,
    std::wstring& errorMessage,
    bool& cancelled,
    std::uint64_t& selectionStart,
    std::uint64_t& selectionEnd)
{
    std::uint64_t afterStart = 701;
    std::uint64_t afterEnd = 702;
    selectionStart = 801;
    selectionEnd = 802;
    cancelled = false;
    switch (scenario) {
    case LargeDocumentFaultScenario::Insertion: {
        const bool succeeded = document.Replace(5, 5, L"insert", 31, 37, afterStart, afterEnd, errorMessage);
        selectionStart = afterStart;
        selectionEnd = afterEnd;
        return succeeded;
    }
    case LargeDocumentFaultScenario::Deletion: {
        const bool succeeded = document.Replace(2, 13, L"", 31, 37, afterStart, afterEnd, errorMessage);
        selectionStart = afterStart;
        selectionEnd = afterEnd;
        return succeeded;
    }
    case LargeDocumentFaultScenario::Replacement: {
        const bool succeeded = document.Replace(4, 13, L"replacement", 31, 37, afterStart, afterEnd, errorMessage);
        selectionStart = afterStart;
        selectionEnd = afterEnd;
        return succeeded;
    }
    case LargeDocumentFaultScenario::Undo:
    case LargeDocumentFaultScenario::CompoundUndo:
        return document.Undo(selectionStart, selectionEnd, errorMessage);
    case LargeDocumentFaultScenario::ReplaceAll:
        return document.ReplaceAll(L"abc", L"Q", true, false, cancelled, {}, errorMessage) != 0U;
    }
    return false;
}

void TestLargeDocumentTransactionalReadFailures()
{
    using classic_notepad::LargeDocument;
    const std::vector<LargeDocumentFaultScenario> scenarios{
        LargeDocumentFaultScenario::Insertion,
        LargeDocumentFaultScenario::Deletion,
        LargeDocumentFaultScenario::Replacement,
        LargeDocumentFaultScenario::Undo,
        LargeDocumentFaultScenario::ReplaceAll,
        LargeDocumentFaultScenario::CompoundUndo,
    };

    for (const LargeDocumentFaultScenario scenario : scenarios) {
        LargeDocumentFaultFixture baseline = MakeLargeDocumentFaultFixture();
        Expect(PrepareLargeDocumentFaultScenario(baseline, scenario), "Fault scenario baseline preparation succeeds");
        baseline.reader->calls = 0;
        std::wstring baselineError;
        bool baselineCancelled = false;
        std::uint64_t baselineSelectionStart = 0;
        std::uint64_t baselineSelectionEnd = 0;
        const bool baselineSucceeded = RunLargeDocumentFaultScenario(*baseline.document, scenario,
            baselineError, baselineCancelled, baselineSelectionStart, baselineSelectionEnd);
        const std::size_t sourceReadCount = baseline.reader->calls;
        const std::string baseLabel = std::string("Successful ") + LargeDocumentFaultScenarioName(scenario) + " baseline executes";
        Expect(baselineSucceeded && baselineError.empty() && !baselineCancelled, baseLabel.c_str());
        const std::string readLabel = std::string(LargeDocumentFaultScenarioName(scenario)) + " exercises source reads";
        if (scenario == LargeDocumentFaultScenario::Undo ||
            scenario == LargeDocumentFaultScenario::CompoundUndo) {
            const std::string noReadLabel = std::string(LargeDocumentFaultScenarioName(scenario)) +
                " reuses retained pieces without rereading source bytes";
            Expect(sourceReadCount == 0U, noReadLabel.c_str());
            LargeDocumentFaultFixture target = MakeLargeDocumentFaultFixture();
            LargeDocumentFaultFixture control = MakeLargeDocumentFaultFixture();
            Expect(PrepareLargeDocumentFaultScenario(target, scenario) &&
                PrepareLargeDocumentFaultScenario(control, scenario), "No-read undo fixtures prepare");
            target.reader->calls = 0;
            target.reader->failAt = 1U;
            std::wstring targetError;
            std::wstring controlError;
            bool targetCancelled = false;
            bool controlCancelled = false;
            std::uint64_t targetSelectionStart = 0;
            std::uint64_t targetSelectionEnd = 0;
            std::uint64_t controlSelectionStart = 0;
            std::uint64_t controlSelectionEnd = 0;
            const bool targetSucceeded = RunLargeDocumentFaultScenario(*target.document, scenario,
                targetError, targetCancelled, targetSelectionStart, targetSelectionEnd);
            const bool controlSucceeded = RunLargeDocumentFaultScenario(*control.document, scenario,
                controlError, controlCancelled, controlSelectionStart, controlSelectionEnd);
            Expect(targetSucceeded && controlSucceeded && targetError.empty() && controlError.empty() &&
                !targetCancelled && !controlCancelled && target.reader->calls == 0U &&
                target.reader->failAt == 1U && targetSelectionStart == controlSelectionStart &&
                targetSelectionEnd == controlSelectionEnd,
                "Undo succeeds without touching a source reader when its retained pieces suffice");
            target.reader->failAt = 0;
            LargeDocumentSnapshot targetAfterUndo;
            LargeDocumentSnapshot controlAfterUndo;
            Expect(CaptureLargeDocumentSnapshot(*target.document, targetAfterUndo) &&
                CaptureLargeDocumentSnapshot(*control.document, controlAfterUndo) &&
                SameLargeDocumentSnapshot(targetAfterUndo, controlAfterUndo),
                "Undo result matches control with the next source read fault armed");
            continue;
        }
        Expect(sourceReadCount != 0U, readLabel.c_str());

        for (std::size_t failAt = 1; failAt <= sourceReadCount; ++failAt) {
            LargeDocumentFaultFixture target = MakeLargeDocumentFaultFixture();
            LargeDocumentFaultFixture control = MakeLargeDocumentFaultFixture();
            const bool targetPrepared = PrepareLargeDocumentFaultScenario(target, scenario);
            const bool controlPrepared = PrepareLargeDocumentFaultScenario(control, scenario);
            const std::string prepLabel = std::string("Prepare injected ") + LargeDocumentFaultScenarioName(scenario);
            Expect(targetPrepared && controlPrepared, prepLabel.c_str());

            LargeDocumentSnapshot before;
            Expect(CaptureLargeDocumentSnapshot(*target.document, before), "Pre-failure document snapshot reads");
            target.reader->calls = 0;
            target.reader->failAt = failAt;

            std::wstring error;
            bool cancelled = false;
            std::uint64_t selectionStart = 123456;
            std::uint64_t selectionEnd = 654321;
            const bool succeeded = RunLargeDocumentFaultScenario(*target.document, scenario,
                error, cancelled, selectionStart, selectionEnd);
            const std::string failLabel = std::string("Injected source read fails ") +
                LargeDocumentFaultScenarioName(scenario) + " call " + std::to_string(failAt);
            Expect(!succeeded && !error.empty() && !cancelled, failLabel.c_str());
            if (scenario == LargeDocumentFaultScenario::Insertion ||
                scenario == LargeDocumentFaultScenario::Deletion ||
                scenario == LargeDocumentFaultScenario::Replacement) {
                const std::string selectionLabel = std::string("Failed ") + LargeDocumentFaultScenarioName(scenario) +
                    " preserves selection outputs";
                Expect(selectionStart == 701U && selectionEnd == 702U, selectionLabel.c_str());
            } else if (scenario == LargeDocumentFaultScenario::Undo ||
                scenario == LargeDocumentFaultScenario::CompoundUndo) {
                const std::string selectionLabel = std::string("Failed ") + LargeDocumentFaultScenarioName(scenario) +
                    " preserves selection outputs";
                Expect(selectionStart == 123456U && selectionEnd == 654321U, selectionLabel.c_str());
            }

            target.reader->failAt = 0;
            LargeDocumentSnapshot after;
            Expect(CaptureLargeDocumentSnapshot(*target.document, after), "Post-failure document snapshot reads");
            const std::string unchangedLabel = std::string("Failed ") + LargeDocumentFaultScenarioName(scenario) +
                " preserves document, history, counters, saved marker, and dirty state at call " + std::to_string(failAt);
            Expect(SameLargeDocumentSnapshot(before, after), unchangedLabel.c_str());

            std::wstring targetUndoError;
            std::wstring controlUndoError;
            std::uint64_t targetUndoStart = 0;
            std::uint64_t targetUndoEnd = 0;
            std::uint64_t controlUndoStart = 0;
            std::uint64_t controlUndoEnd = 0;
            const bool targetUndo = target.document->Undo(targetUndoStart, targetUndoEnd, targetUndoError);
            const bool controlUndo = control.document->Undo(controlUndoStart, controlUndoEnd, controlUndoError);
            LargeDocumentSnapshot targetAfterUndo;
            LargeDocumentSnapshot controlAfterUndo;
            const bool targetCaptured = CaptureLargeDocumentSnapshot(*target.document, targetAfterUndo);
            const bool controlCaptured = CaptureLargeDocumentSnapshot(*control.document, controlAfterUndo);
            const std::string undoLabel = std::string("Subsequent Undo after failed ") +
                LargeDocumentFaultScenarioName(scenario) + " matches an untouched control at call " + std::to_string(failAt);
            Expect(targetUndo && controlUndo && targetUndoError.empty() && controlUndoError.empty() &&
                targetUndoStart == controlUndoStart && targetUndoEnd == controlUndoEnd &&
                targetCaptured && controlCaptured && SameLargeDocumentSnapshot(targetAfterUndo, controlAfterUndo), undoLabel.c_str());
        }
    }

    LargeDocumentFaultFixture tinyBudget = MakeLargeDocumentFaultFixture();
    tinyBudget.document->SetUndoByteLimit(sizeof(wchar_t));
    bool cancelled = false;
    std::wstring error;
    const std::uint64_t replaced = tinyBudget.document->ReplaceAll(L"abc", L"Q", true, false,
        cancelled, {}, error);
    Expect(replaced > 1U && !cancelled && error.empty(), "Replace All succeeds with a tiny undo budget");
    Expect(!tinyBudget.document->CanUndo() && tinyBudget.document->UndoBytes() == 0U,
        "Tiny undo budget evicts the complete Replace All transaction");
    std::wstring text;
    Expect(tinyBudget.document->ReadRange(0, tinyBudget.document->CharacterCount(), text, error),
        "Tiny-budget Replace All result remains readable");
    ExpectText(text, L"Q\nQ\nQ\nQ\nQ\nxyz", "Tiny undo budget never leaves a partially undoable Replace All");
}

void TestLargeDocumentUndoBudgetAndSharedOwnership()
{
    using classic_notepad::LargeDocument;
    using classic_notepad::LargeDocumentOriginalPiece;
    using classic_notepad::LargeFileEncoding;

    const auto makeEmptyDocument = []() {
        auto document = std::make_unique<LargeDocument>(LargeFileEncoding::Utf8,
            [](std::uint64_t, std::size_t, std::vector<std::uint8_t>&) { return false; });
        std::wstring error;
        Expect(document->Initialize({}, error), "Empty undo-budget document initializes");
        return document;
    };
    const auto makeSourceDocument = [](std::string source) {
        auto bytes = std::make_shared<std::string>(std::move(source));
        std::uint64_t lineBreaks = 0U;
        for (std::size_t index = 0U; index < bytes->size(); ++index) {
            if ((*bytes)[index] == '\r') {
                ++lineBreaks;
                if (index + 1U < bytes->size() && (*bytes)[index + 1U] == '\n') ++index;
            } else if ((*bytes)[index] == '\n') {
                ++lineBreaks;
            }
        }
        auto document = std::make_unique<LargeDocument>(LargeFileEncoding::Utf8,
            [bytes](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& output) {
                if (offset > bytes->size() || length > bytes->size() - static_cast<std::size_t>(offset)) return false;
                output.assign(bytes->begin() + static_cast<std::size_t>(offset),
                    bytes->begin() + static_cast<std::size_t>(offset) + length);
                return true;
            });
        std::wstring error;
        const bool initialized = document->Initialize({LargeDocumentOriginalPiece{
            0U, static_cast<std::uint64_t>(bytes->size()), static_cast<std::uint64_t>(bytes->size()),
            lineBreaks, bytes->empty() ? wchar_t(0) : static_cast<wchar_t>(static_cast<unsigned char>(bytes->front())),
            bytes->empty() ? wchar_t(0) : static_cast<wchar_t>(static_cast<unsigned char>(bytes->back())), true}}, error);
        Expect(initialized, "Source-backed undo-budget document initializes");
        return document;
    };
    const auto readAll = [](const LargeDocument& document, std::wstring& text) {
        std::wstring error;
        return document.ReadRange(0U, document.CharacterCount(), text, error);
    };

    auto simpleProbe = makeEmptyDocument();
    std::uint64_t afterStart = 0U;
    std::uint64_t afterEnd = 0U;
    std::wstring error;
    Expect(simpleProbe->Replace(0U, 0U, L"x", 0U, 0U, afterStart, afterEnd, error),
        "Single-edit undo charge probe succeeds");
    const std::uint64_t simpleCost = simpleProbe->UndoBytes();
    Expect(simpleCost > 8U, "An original-independent insertion has a nonzero metadata and owner charge");

    const auto checkSimpleBudget = [&makeEmptyDocument, &readAll](std::uint64_t budget,
        bool expectUndo, const char* label) {
        auto document = makeEmptyDocument();
        document->SetUndoByteLimit(budget);
        std::uint64_t selectionStart = 0U;
        std::uint64_t selectionEnd = 0U;
        std::wstring operationError;
        const bool edited = document->Replace(0U, 0U, L"x", 0U, 0U,
            selectionStart, selectionEnd, operationError);
        std::wstring text;
        const bool readable = readAll(*document, text);
        if (expectUndo) {
            const bool canUndoBefore = document->CanUndo();
            const bool undone = document->Undo(selectionStart, selectionEnd, operationError);
            Expect(edited && readable && text == L"x" && canUndoBefore && undone &&
                readAll(*document, text) && text.empty() && document->UndoBytes() == 0U, label);
        } else {
            Expect(edited && readable && text == L"x" && !document->CanUndo() &&
                document->UndoBytes() == 0U, label);
        }
    };
    checkSimpleBudget(0U, false, "Zero-byte budget applies the edit and drops the complete action");
    checkSimpleBudget(1U, false, "Tiny budget applies the edit and drops the complete action");
    checkSimpleBudget(simpleCost - 1U, false, "One byte below the measured action cost drops that whole action");
    checkSimpleBudget(simpleCost, true, "Exact action cost retains one complete undo record");
    checkSimpleBudget(simpleCost + 1U, true, "One byte above the action cost retains one complete undo record");

    auto sourceDelete = makeSourceDocument("source only");
    Expect(sourceDelete->Replace(0U, sourceDelete->CharacterCount(), L"", 0U,
            sourceDelete->CharacterCount(), afterStart, afterEnd, error) && sourceDelete->UndoBytes() > 0U,
        "Deleting only original source text still charges retained undo metadata and descriptors");
    Expect(sourceDelete->Undo(afterStart, afterEnd, error), "Source-only deletion remains fully undoable");
    std::wstring text;
    Expect(readAll(*sourceDelete, text) && text == L"source only",
        "Undo restores a source-only deletion exactly");

    constexpr std::uint64_t checkpointedSourceCharacters = 200U * 1024U;
    auto checkpointedSource = makeSourceDocument(std::string(
        static_cast<std::size_t>(checkpointedSourceCharacters), 's'));
    constexpr std::uint64_t sourceDeletionBudget = 4096U;
    checkpointedSource->SetUndoByteLimit(sourceDeletionBudget);
    Expect(checkpointedSource->IsCharacterBoundary(180000U, error),
        "Source-only budget fixture populates translation checkpoints before deletion");
    auto checkpointSnapshot = checkpointedSource->Clone();
    Expect(checkpointSnapshot != nullptr && checkpointedSource->Replace(0U,
            checkpointedSource->CharacterCount(), L"", 0U, checkpointedSource->CharacterCount(),
            afterStart, afterEnd, error) && checkpointedSource->CanUndo() &&
        checkpointedSource->UndoBytes() != 0U &&
        checkpointedSource->UndoBytes() <= sourceDeletionBudget,
        "Checkpointed original-only deletion charges retained source metadata within its budget");
    const std::uint64_t checkpointedUndoCharge = checkpointedSource->UndoBytes();
    Expect(checkpointSnapshot->IsCharacterBoundary(199999U, error) &&
        checkpointedSource->UndoBytes() == checkpointedUndoCharge &&
        checkpointedSource->UndoBytes() <= sourceDeletionBudget,
        "Shared source checkpoint growth stays inside the capacity charged by Undo");
    Expect(checkpointedSource->Undo(afterStart, afterEnd, error) &&
        checkpointedSource->CharacterCount() == checkpointedSourceCharacters &&
        readAll(*checkpointedSource, text) && text.size() == checkpointedSourceCharacters &&
        std::all_of(text.begin(), text.end(), [](wchar_t character) { return character == L's'; }),
        "Undo restores checkpointed original-only text after snapshot cache growth");

    auto coalesced = makeEmptyDocument();
    Expect(coalesced->Replace(0U, 0U, L"a", 0U, 0U, afterStart, afterEnd, error),
        "First typing action for coalescing budget test succeeds");
    const std::uint64_t firstTypingCost = coalesced->UndoBytes();
    coalesced->SetUndoByteLimit(firstTypingCost);
    Expect(coalesced->Replace(1U, 1U, L"b", 1U, 1U, afterStart, afterEnd, error),
        "Second coalesced typing edit still applies when the complete group exceeds its budget");
    Expect(!coalesced->CanUndo() && coalesced->UndoBytes() == 0U &&
        readAll(*coalesced, text) && text == L"ab",
        "Over-budget coalesced typing drops the complete group rather than exposing part of it");

    auto dropsOlderHistory = makeEmptyDocument();
    Expect(dropsOlderHistory->Replace(0U, 0U, L"x", 0U, 0U, afterStart, afterEnd, error),
        "Prior action for over-budget history replacement succeeds");
    const std::uint64_t priorActionCost = dropsOlderHistory->UndoBytes();
    dropsOlderHistory->SetUndoByteLimit(priorActionCost);
    const std::wstring oversizedNewAction(32U, L'n');
    Expect(dropsOlderHistory->Replace(1U, 1U, oversizedNewAction, 0U, 1U,
            afterStart, afterEnd, error) && readAll(*dropsOlderHistory, text) &&
        text == L"x" + oversizedNewAction && !dropsOlderHistory->CanUndo() &&
        dropsOlderHistory->UndoBytes() == 0U,
        "Dropping a newer over-budget action also drops stale older history");

    auto insertedDelete = makeEmptyDocument();
    Expect(insertedDelete->Replace(0U, 0U, L"inserted", 0U, 0U, afterStart, afterEnd, error),
        "Inserted-owner Undo test creates text");
    const std::uint64_t insertedOwnerBytes = insertedDelete->InsertedBufferBytes();
    Expect(insertedOwnerBytes != 0U && insertedDelete->Replace(0U, 8U, L"", 0U, 8U,
            afterStart, afterEnd, error) && insertedDelete->InsertedBufferBytes() == insertedOwnerBytes,
        "Deleting inserted text retains its immutable owner through Undo history");
    Expect(insertedDelete->Undo(afterStart, afterEnd, error) && readAll(*insertedDelete, text) &&
        text == L"inserted" && insertedDelete->InsertedBufferBytes() == insertedOwnerBytes,
        "Undo restores inserted text from its shared chunk owner");
    Expect(insertedDelete->Undo(afterStart, afterEnd, error) && readAll(*insertedDelete, text) &&
        text.empty() && insertedDelete->InsertedBufferBytes() == 0U,
        "Undo releases an inserted owner after neither live text nor history references it");

    const auto runReplaceAll = [&makeSourceDocument, &readAll](std::uint64_t budget,
        bool expectUndo, const char* label) {
        auto document = makeSourceDocument("aaaa");
        document->SetUndoByteLimit(budget);
        document->MarkSaved();
        bool cancelled = false;
        std::wstring operationError;
        std::uint64_t selectionStart = 0U;
        std::uint64_t selectionEnd = 0U;
        const std::uint64_t count = document->ReplaceAll(L"a", L"b", true, false,
            cancelled, {}, operationError);
        std::wstring result;
        const bool contentReady = readAll(*document, result);
        if (expectUndo) {
            const bool canUndoBefore = document->CanUndo();
            const bool undone = document->Undo(selectionStart, selectionEnd, operationError);
            Expect(count == 4U && !cancelled && contentReady && result == L"bbbb" &&
                canUndoBefore && undone && readAll(*document, result) && result == L"aaaa" &&
                !document->IsModified(), label);
        } else {
            Expect(count == 4U && !cancelled && contentReady && result == L"bbbb" &&
                !document->CanUndo() && document->UndoBytes() == 0U && document->IsModified(), label);
        }
    };
    auto compoundProbe = makeSourceDocument("aaaa");
    bool compoundCancelled = false;
    Expect(compoundProbe->ReplaceAll(L"a", L"b", true, false, compoundCancelled, {}, error) == 4U &&
        !compoundCancelled, "Replace All compound charge probe succeeds");
    const std::uint64_t compoundCost = compoundProbe->UndoBytes();
    Expect(compoundCost > 8U, "Compound action includes metadata, child records, and replacement owners");
    runReplaceAll(8U, false, "The reported aaaa-to-bbbb eight-byte regression has no partial Undo");
    runReplaceAll(compoundCost - 1U, false, "One byte below compound cost drops the complete Replace All");
    runReplaceAll(compoundCost, true, "Exact compound cost keeps the whole Replace All undoable");
    runReplaceAll(compoundCost + 1U, true, "One byte above compound cost keeps the whole Replace All undoable");

    auto savedWidth = makeSourceDocument("a\na\n");
    savedWidth->MarkSaved();
    const std::uint64_t originalLines = savedWidth->LineCount();
    const std::uint64_t originalWidth = savedWidth->MaximumLineColumns();
    bool widthCancelled = false;
    Expect(savedWidth->ReplaceAll(L"a", L"wide", true, false, widthCancelled, {}, error) == 2U &&
        !widthCancelled && savedWidth->IsModified() && savedWidth->LineCount() == originalLines &&
        savedWidth->MaximumLineColumns() == 4U,
        "Compound Replace All preserves saved-state tracking and updates line/width summaries");
    Expect(savedWidth->Undo(afterStart, afterEnd, error) && !savedWidth->IsModified() &&
        savedWidth->LineCount() == originalLines && savedWidth->MaximumLineColumns() == originalWidth &&
        readAll(*savedWidth, text) && text == L"a\na\n",
        "Compound Undo restores saved state, width, and line summaries as one action");

    auto cancelledCompound = makeEmptyDocument();
    Expect(cancelledCompound->Replace(0U, 0U, L"a a a", 0U, 0U, afterStart, afterEnd, error),
        "Compound cancellation source is inserted");
    cancelledCompound->MarkSaved();
    LargeDocumentSnapshot beforeCancellation;
    Expect(CaptureLargeDocumentSnapshot(*cancelledCompound, beforeCancellation),
        "Compound cancellation baseline is captured");
    bool scanComplete = false;
    std::size_t postScanChecks = 0U;
    bool cancellationReported = false;
    error.clear();
    const std::uint64_t cancelledCount = cancelledCompound->ReplaceAll(L"a", L"long", true, false,
        cancellationReported,
        [&scanComplete, &postScanChecks]() {
            if (!scanComplete) return false;
            ++postScanChecks;
            return postScanChecks == 5U;
        }, error, [&scanComplete](std::uint64_t progress) {
            if (progress == 5U) scanComplete = true;
        });
    LargeDocumentSnapshot afterCancellation;
    Expect(cancelledCount == 0U && cancellationReported && error.empty() &&
        CaptureLargeDocumentSnapshot(*cancelledCompound, afterCancellation) &&
        SameLargeDocumentSnapshot(beforeCancellation, afterCancellation),
        "Cancellation after staging part of a compound edit leaves text, owners, history, and state untouched");

    auto repeated = makeEmptyDocument();
    constexpr std::uint64_t retentionLimit = 2ULL * 1024ULL * 1024ULL;
    repeated->SetUndoByteLimit(retentionLimit);
    const std::wstring payload(256U * 1024U, L'p');
    bool chunksBounded = true;
    bool deletionHistoryRetained = false;
    for (std::size_t iteration = 0; iteration < 12U; ++iteration) {
        Expect(repeated->Replace(0U, 0U, payload, 0U, 0U, afterStart, afterEnd, error),
            "Repeated large paste applies");
        chunksBounded = chunksBounded && repeated->PieceCount() == 4U &&
            repeated->InsertedBufferBytes() >= payload.size() * sizeof(wchar_t);
        Expect(repeated->Replace(0U, repeated->CharacterCount(), L"", 0U,
                repeated->CharacterCount(), afterStart, afterEnd, error),
            "Repeated large paste can be deleted");
        if (iteration == 0U) deletionHistoryRetained = repeated->InsertedBufferBytes() != 0U;
        Expect(repeated->InsertedBufferBytes() <= retentionLimit && repeated->UndoBytes() <= retentionLimit,
            "Repeated paste/delete retains only storage represented by bounded complete history");
    }
    Expect(chunksBounded, "Large inserted text stays in bounded immutable chunks");
    Expect(deletionHistoryRetained, "Undo retains a deleted inserted owner while its action remains reachable");
    repeated->SetUndoByteLimit(0U);
    Expect(repeated->InsertedBufferBytes() == 0U && repeated->UndoBytes() == 0U,
        "History trimming releases deleted inserted owners when no snapshot retains them");
    repeated->SetUndoByteLimit(retentionLimit);
    Expect(repeated->Replace(0U, 0U, payload, 0U, 0U, afterStart, afterEnd, error) &&
        repeated->InsertedBufferBytes() != 0U,
        "Live text owns inserted chunks independently of its undo records");
    repeated->Clear();
    Expect(repeated->InsertedBufferBytes() == 0U && repeated->UndoBytes() == 0U && !repeated->CanUndo(),
        "Clear releases inserted owners and all undo records when no snapshot retains them");

    auto shared = makeEmptyDocument();
    constexpr std::size_t historyLength = 1024U;
    for (std::size_t index = 0; index < historyLength; ++index) {
        const std::uint64_t position = shared->CharacterCount();
        Expect(shared->Replace(position, position, L"x", 0U, 1U, afterStart, afterEnd, error),
            "Long-history instrumentation edit succeeds");
        shared->MarkSaved();
    }
    const std::uint64_t familyBytesBeforeClone = shared->InsertedBufferBytes();
    auto snapshot = shared->Clone();
    Expect(snapshot != nullptr && snapshot->CharacterCount() == historyLength &&
        snapshot->UndoBytes() == shared->UndoBytes() && snapshot->CanUndo() &&
        snapshot->InsertedBufferBytes() == familyBytesBeforeClone,
        "Clone shares the persistent tree root, immutable history payloads, and owner accounting");
    Expect(shared->Replace(historyLength, historyLength, L"y", 0U, 1U, afterStart, afterEnd, error),
        "Live document can edit after a shared snapshot is taken");
    Expect(snapshot->CharacterCount() == historyLength &&
        snapshot->InsertedBufferBytes() == shared->InsertedBufferBytes(),
        "Snapshot contents stay immutable while the shared family ledger tracks new owners");
    shared->SetUndoByteLimit(0U);
    shared->Clear();
    Expect(snapshot->CanUndo() && snapshot->CharacterCount() == historyLength &&
        snapshot->InsertedBufferBytes() == familyBytesBeforeClone,
        "Live history trim and Clear do not mutate a snapshot's shared history or text");
    Expect(snapshot->Undo(afterStart, afterEnd, error) && snapshot->CharacterCount() == historyLength - 1U,
        "Clone retains and can undo its pre-edit immutable history after source trim and Clear");
    snapshot->Clear();
    Expect(shared->InsertedBufferBytes() == 0U && snapshot->InsertedBufferBytes() == 0U,
        "Final snapshot Clear releases the last inserted chunk owners in its family");

    auto sliced = makeEmptyDocument();
    const std::wstring largeChunk(64U * 1024U, L'p');
    Expect(sliced->Replace(0U, 0U, largeChunk, 0U, 0U, afterStart, afterEnd, error),
        "Small-slice retention fixture creates one bounded immutable chunk");
    const std::uint64_t fullChunkBytes = sliced->InsertedBufferBytes();
    sliced->SetUndoByteLimit(0U);
    constexpr std::uint64_t retainedSliceLength = 8U;
    Expect(sliced->Replace(0U, sliced->CharacterCount() - retainedSliceLength, L"", 0U,
            sliced->CharacterCount() - retainedSliceLength, afterStart, afterEnd, error) &&
        readAll(*sliced, text) && text == std::wstring(retainedSliceLength, L'p') &&
        !sliced->CanUndo() && sliced->InsertedBufferBytes() == fullChunkBytes,
        "A small retained slice keeps its full immutable chunk owner after deletion history is trimmed");
    sliced->Clear();
    Expect(sliced->InsertedBufferBytes() == 0U,
        "Clearing the final small slice releases its complete immutable chunk allocation");

    auto historyFamily = makeSourceDocument("a\nb");
    historyFamily->MarkSaved();
    Expect(historyFamily->Replace(3U, 3U, L"\nlong", 0U, 1U, afterStart, afterEnd, error),
        "First snapshot-family history edit appends a line");
    const std::uint64_t firstHistoryOwnerBytes = historyFamily->InsertedBufferBytes();
    auto firstHistorySnapshot = historyFamily->Clone();
    Expect(firstHistorySnapshot != nullptr && firstHistorySnapshot->CanUndo() &&
        readAll(*firstHistorySnapshot, text) && text == L"a\nb\nlong" &&
        firstHistorySnapshot->LineCount() == 3U && firstHistorySnapshot->MaximumLineColumns() == 4U &&
        firstHistorySnapshot->IsModified(),
        "First snapshot preserves its content, summary, saved state, and undo history");
    Expect(historyFamily->Undo(afterStart, afterEnd, error) && !historyFamily->IsModified() &&
        historyFamily->LineCount() == 2U && historyFamily->MaximumLineColumns() == 1U &&
        readAll(*historyFamily, text) && text == L"a\nb" &&
        historyFamily->InsertedBufferBytes() == firstHistoryOwnerBytes,
        "Live Undo returns to saved source while a snapshot retains the first inserted owner");
    Expect(historyFamily->Replace(0U, 0U, L"B", 0U, 1U, afterStart, afterEnd, error),
        "Second snapshot-family history edit creates a distinct action");
    auto secondHistorySnapshot = historyFamily->Clone();
    const std::uint64_t bothSnapshotOwnerBytes = historyFamily->InsertedBufferBytes();
    Expect(secondHistorySnapshot != nullptr && secondHistorySnapshot->CanUndo() &&
        bothSnapshotOwnerBytes > firstHistoryOwnerBytes &&
        readAll(*secondHistorySnapshot, text) && text == L"Ba\nb" &&
        secondHistorySnapshot->LineCount() == 2U && secondHistorySnapshot->MaximumLineColumns() == 2U &&
        secondHistorySnapshot->IsModified(),
        "Second snapshot has a different root and history while both share a stable family ledger");
    historyFamily->SetUndoByteLimit(0U);
    historyFamily->Clear();
    Expect(firstHistorySnapshot->CanUndo() && secondHistorySnapshot->CanUndo() &&
        readAll(*firstHistorySnapshot, text) && text == L"a\nb\nlong" &&
        readAll(*secondHistorySnapshot, text) && text == L"Ba\nb" &&
        firstHistorySnapshot->InsertedBufferBytes() == bothSnapshotOwnerBytes &&
        secondHistorySnapshot->InsertedBufferBytes() == bothSnapshotOwnerBytes &&
        historyFamily->InsertedBufferBytes() == bothSnapshotOwnerBytes,
        "Live trim and Clear preserve two independent snapshot histories and their shared ledger");
    Expect(firstHistorySnapshot->Undo(afterStart, afterEnd, error) &&
        readAll(*firstHistorySnapshot, text) && text == L"a\nb" &&
        !firstHistorySnapshot->IsModified() && firstHistorySnapshot->LineCount() == 2U &&
        firstHistorySnapshot->MaximumLineColumns() == 1U &&
        firstHistorySnapshot->InsertedBufferBytes() == bothSnapshotOwnerBytes - firstHistoryOwnerBytes &&
        secondHistorySnapshot->InsertedBufferBytes() == bothSnapshotOwnerBytes - firstHistoryOwnerBytes,
        "Undoing the first snapshot releases only its owner and leaves the other history intact");
    Expect(secondHistorySnapshot->Undo(afterStart, afterEnd, error) &&
        readAll(*secondHistorySnapshot, text) && text == L"a\nb" &&
        !secondHistorySnapshot->IsModified() && secondHistorySnapshot->LineCount() == 2U &&
        secondHistorySnapshot->MaximumLineColumns() == 1U &&
        firstHistorySnapshot->InsertedBufferBytes() == 0U &&
        secondHistorySnapshot->InsertedBufferBytes() == 0U && historyFamily->InsertedBufferBytes() == 0U,
        "Destroying the final live and snapshot owners returns the shared family ledger to zero");
}

std::uint64_t CountLogicalLineBreaks(const std::wstring& text)
{
    std::uint64_t lineBreaks = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            ++lineBreaks;
            if (index + 1U < text.size() && text[index + 1U] == L'\n') ++index;
        } else if (text[index] == L'\n') {
            ++lineBreaks;
        }
    }
    return lineBreaks;
}

void TestLargeDocumentRandomizedEditsAgainstTextModel()
{
    using classic_notepad::LargeDocument;
    using classic_notepad::LargeDocumentOriginalPiece;
    using classic_notepad::LargeFileEncoding;

    const std::string source = "a\r\nbc\nde";
    LargeDocument document(LargeFileEncoding::Utf8,
        [&source](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
            if (offset > source.size() || length > source.size() - static_cast<std::size_t>(offset)) return false;
            bytes.assign(source.begin() + static_cast<std::size_t>(offset),
                source.begin() + static_cast<std::size_t>(offset) + length);
            return true;
        });
    std::wstring error;
    Expect(document.Initialize({LargeDocumentOriginalPiece{0, source.size(), source.size(), 2, L'a', L'e', true}}, error),
        "Randomized model document initializes");

    std::wstring model(source.begin(), source.end());
    std::vector<std::wstring> history;
    std::mt19937 random(0xA3C0FFEEU);
    const std::vector<std::wstring> replacements{
        L"", L"x", L"\t", L"a\t", L"\r", L"\n", L"xy", L"q\r\n", L"q\r\n\t", L"z"};
    for (std::size_t step = 0; step < 240U; ++step) {
        if (step % 9U == 8U && !history.empty()) {
            std::uint64_t selectionStart = 0;
            std::uint64_t selectionEnd = 0;
            Expect(document.Undo(selectionStart, selectionEnd, error), "Randomized model Undo succeeds");
            model = std::move(history.back());
            history.pop_back();
        } else {
            std::uniform_int_distribution<std::size_t> startDistribution(0U, model.size());
            const std::size_t start = startDistribution(random);
            const std::size_t maxLength = std::min<std::size_t>(4U, model.size() - start);
            std::uniform_int_distribution<std::size_t> lengthDistribution(0U, maxLength);
            const std::size_t length = lengthDistribution(random);
            std::uniform_int_distribution<std::size_t> replacementDistribution(0U, replacements.size() - 1U);
            const std::wstring& replacement = replacements[replacementDistribution(random)];
            history.push_back(model);
            std::uint64_t afterStart = 0;
            std::uint64_t afterEnd = 0;
            Expect(document.Replace(start, start + length, replacement, 0, 1, afterStart, afterEnd, error),
                "Randomized model Replace succeeds");
            model.replace(start, length, replacement);
        }

        std::wstring actual;
        Expect(document.ReadRange(0, document.CharacterCount(), actual, error), "Randomized document text reads");
        const std::string textLabel = "Randomized piece tree matches independent text model";
        ExpectText(actual, model, textLabel.c_str());
        Expect(document.CharacterCount() == model.size(), "Randomized character count matches independent text model");
        Expect(document.LineCount() == CountLogicalLineBreaks(model) + 1U,
            "Randomized line count matches independent text model");
        Expect(document.MaximumLineColumns() == MeasureVisualColumnsForTest(model),
            "Randomized width summary matches independent tab-aware text model");
        Expect(document.CanUndo() == !history.empty(), "Randomized undo availability matches independent history model");
    }
}

void TestSpellWordExpansion()
{
    using classic_notepad::ExpandWordRangeAt;
    using classic_notepad::FindSpellCheckWordRanges;
    using classic_notepad::TextRange;

    const std::wstring text = L"can't re-enter foo_bar end-";
    TextRange range{};

    Expect(ExpandWordRangeAt(text, 1, range), "Word expansion works in plain word");
    Expect(range.start == 0U && range.length == 5U, "Word expansion includes apostrophe inside a word");

    Expect(ExpandWordRangeAt(text, 8, range), "Word expansion works for hyphenated words");
    Expect(range.start == 6U && range.length == 8U, "Word expansion includes internal hyphen");

    Expect(!ExpandWordRangeAt(text, text.size() - 1U, range), "Trailing hyphen is not treated as a word");

    const std::vector<TextRange> ranges = FindSpellCheckWordRanges(L"teh, colour; can't re-enter -");
    Expect(ranges.size() == 4U, "Spell word range scan finds standalone spelling words");
    Expect(ranges[0].start == 0U && ranges[0].length == 3U, "Spell word scan captures first word");
    Expect(ranges[1].start == 5U && ranges[1].length == 6U, "Spell word scan skips punctuation");
    Expect(ranges[2].start == 13U && ranges[2].length == 5U, "Spell word scan keeps apostrophes inside words");
    Expect(ranges[3].start == 19U && ranges[3].length == 8U, "Spell word scan keeps internal hyphens");
}

void TestSpellRangeOverlap()
{
    using classic_notepad::RangesOverlap;

    Expect(RangesOverlap(10U, 5U, 12U, 4U), "Overlapping ranges are detected");
    Expect(!RangesOverlap(10U, 2U, 12U, 4U), "Adjacent ranges are not overlapping");
    Expect(!RangesOverlap(0U, 0U, 5U, 3U), "Empty range does not overlap");

    constexpr std::size_t nearMax = std::numeric_limits<std::size_t>::max() - 2U;
    Expect(!RangesOverlap(nearMax, 2U, 0U, 2U), "Overflow-safe overlap check handles far-apart ranges");
    Expect(RangesOverlap(nearMax, 3U, nearMax + 1U, 1U), "Overflow-safe overlap check handles near-max overlaps");
}

void TestSpellingWindowMappingAndRevisionTags()
{
    using classic_notepad::MapSpellingBackendRanges;
    using classic_notepad::SpellingAnnotation;
    using classic_notepad::SpellingAnnotationBatch;
    using classic_notepad::SpellingBatchIsCurrent;
    using classic_notepad::SpellingRevision;
    using classic_notepad::SpellingTextWindow;
    using classic_notepad::CheckSpellingWindows;
    using classic_notepad::ResolveSpellingContextTarget;
    using classic_notepad::SpellingContextTarget;
    using classic_notepad::SetSpellingEligibleBounds;
    using classic_notepad::TryPublishSpellingBatch;
    using classic_notepad::TextRange;

    SpellingTextWindow window;
    window.revision = {7U, 42U, 13U};
    window.absoluteStart = 70000U;
    window.text = L"prefix wrong 😀 centre\r\nsuffix";
    window.eligibleStart = 7U;
    window.eligibleEnd = window.text.size() - 7U;
    const std::size_t centreStart = window.text.find(L"centre");

    // Controlled backend offsets stay window-relative until the checked map.
    std::vector<SpellingAnnotation> annotations;
    Expect(MapSpellingBackendRanges(window, {{7U, 5U}, {centreStart, 6U}}, annotations),
        "Spelling windows map multiple backend-relative ranges");
    Expect(annotations.size() == 2U && annotations[0].start == 70007U &&
        annotations[0].length == 5U && annotations[1].start == 70000U + centreStart &&
        annotations[1].length == 6U,
        "Spelling windows preserve full-width absolute UTF-16 coordinates above 65,535");

    const std::size_t emoji = window.text.find(L"😀");
    if constexpr (sizeof(wchar_t) == sizeof(std::uint16_t)) {
        Expect(!MapSpellingBackendRanges(window, {{emoji, 1U}}, annotations),
            "Spelling mapping rejects a backend range that splits an emoji surrogate pair");
    }
    Expect(!MapSpellingBackendRanges(window, {{window.text.size(), 1U}}, annotations),
        "Spelling mapping rejects ranges outside the supplied text window");
    Expect(!MapSpellingBackendRanges(window, {{6U, 1U}}, annotations),
        "Spelling mapping rejects results outside eligible checking bounds");
    Expect(!MapSpellingBackendRanges(window, {{7U, 0U}}, annotations),
        "Spelling mapping rejects zero-length backend results");

    SpellingTextWindow clipped;
    clipped.revision = window.revision;
    clipped.absoluteStart = 100U;
    clipped.text = L"fragment kept fragment";
    SetSpellingEligibleBounds(clipped, true, true);
    const std::size_t eligibleWord = clipped.text.find(L"kept");
    Expect(clipped.eligibleStart == std::wstring(L"fragment").size() &&
        clipped.eligibleEnd == clipped.text.rfind(L"fragment") &&
        clipped.eligibleStart <= eligibleWord && eligibleWord + 4U <= clipped.eligibleEnd,
        "Clipped spelling windows exclude leading and trailing partial word fragments with UTF-16 unit bounds");
    Expect(!MapSpellingBackendRanges(clipped, {{0U, 8U}}, annotations) &&
        !MapSpellingBackendRanges(clipped, {{clipped.text.rfind(L"fragment"), 8U}}, annotations),
        "Spelling mapping rejects results that target clipped boundary fragments");
    Expect(MapSpellingBackendRanges(clipped, {{eligibleWord, 4U}}, annotations) &&
        annotations.size() == 1U && annotations[0].start == 100U + eligibleWord,
        "Spelling mapping accepts complete words inside the eligible portion of a clipped window");

    const auto controlledBackend = [](const std::wstring& text) {
        std::vector<TextRange> ranges;
        for (const TextRange& range : classic_notepad::FindSpellCheckWordRanges(text)) {
            const std::wstring word = text.substr(range.start, range.length);
            if (word == L"fragment" || word == L"kept") ranges.push_back(range);
        }
        return ranges;
    };
    const std::vector<TextRange> wholeWindowErrors = controlledBackend(clipped.text);
    Expect(wholeWindowErrors.size() == 3U,
        "Controlled backend marks both clipped fragments and the complete interior word when given the whole window");
    std::vector<std::wstring> checkedText;
    const auto recordingBackend = [&controlledBackend, &checkedText](const std::wstring& text) {
        checkedText.push_back(text);
        return controlledBackend(text);
    };
    SpellingAnnotationBatch checkedCandidate;
    Expect(CheckSpellingWindows({clipped}, recordingBackend, checkedCandidate) &&
        checkedCandidate.valid && checkedCandidate.annotations.size() == 1U &&
        checkedCandidate.annotations[0].start == 100U + eligibleWord &&
        checkedCandidate.annotations[0].length == 4U,
        "Production spelling-window check publishes the interior backend error while excluding both clipped fragments");
    Expect(checkedText.size() == 1U &&
        checkedText[0] == clipped.text.substr(clipped.eligibleStart,
            clipped.eligibleEnd - clipped.eligibleStart),
        "Production spelling-window check gives the controlled backend only the eligible substring");
    SpellingAnnotationBatch checkedPublished;
    Expect(TryPublishSpellingBatch(checkedPublished, checkedCandidate, clipped.revision) &&
        checkedPublished.annotations.size() == 1U && checkedPublished.annotations[0].start == 109U,
        "Eligible-substring backend offsets publish at the correct full-document coordinate");
    SpellingAnnotationBatch malformedCandidate;
    const auto malformedBackend = [](const std::wstring& text) {
        return std::vector<TextRange>{{text.size(), 1U}};
    };
    Expect(!CheckSpellingWindows({clipped}, malformedBackend, malformedCandidate) &&
        !malformedCandidate.valid,
        "Production spelling-window check rejects a malformed eligible-substring backend range");

    SpellingTextWindow repeatedFirst;
    repeatedFirst.revision = window.revision;
    repeatedFirst.absoluteStart = 70000U;
    repeatedFirst.text = L"sameword";
    repeatedFirst.eligibleEnd = repeatedFirst.text.size();
    SpellingTextWindow repeatedSecond = repeatedFirst;
    repeatedSecond.absoluteStart = 90000U;
    SpellingContextTarget contextTarget;
    Expect(ResolveSpellingContextTarget({repeatedFirst, repeatedSecond}, window.revision,
        90003U, false, contextTarget) && contextTarget.start == 90000U &&
        contextTarget.length == 8U && contextTarget.text == L"sameword",
        "Context-target resolution preserves exact full-width coordinates for repeated distant words");
    Expect(!ResolveSpellingContextTarget({repeatedFirst, repeatedSecond}, window.revision,
        60000U, false, contextTarget),
        "Context-target resolution rejects a position outside every visible window");
    SpellingTextWindow staleRepeated = repeatedSecond;
    staleRepeated.revision.layout += 1U;
    Expect(!ResolveSpellingContextTarget({repeatedFirst, staleRepeated}, window.revision,
        90003U, false, contextTarget),
        "Context-target resolution rejects mixed or stale window tags");
    Expect(!ResolveSpellingContextTarget({clipped}, clipped.revision,
        clipped.absoluteStart, false, contextTarget) &&
        ResolveSpellingContextTarget({clipped}, clipped.revision,
            clipped.absoluteStart + eligibleWord, false, contextTarget) &&
        contextTarget.text == L"kept" && contextTarget.start == 109U,
        "Context-target resolution rejects clipped edge fragments and accepts the eligible interior word");

    SpellingTextWindow overflow = window;
    overflow.absoluteStart = (std::numeric_limits<std::uint64_t>::max)() - 2U;
    Expect(!MapSpellingBackendRanges(overflow, {{7U, 1U}}, annotations),
        "Spelling mapping rejects checked absolute-coordinate overflow");
    SpellingTextWindow oversized = window;
    oversized.text.assign(classic_notepad::kMaximumSpellingWindowUnits + 1U, L'x');
    oversized.eligibleStart = 0U;
    oversized.eligibleEnd = oversized.text.size();
    Expect(!MapSpellingBackendRanges(oversized, {}, annotations),
        "Spelling mapping rejects an oversized copied window");

    SpellingAnnotationBatch batch;
    batch.revision = window.revision;
    batch.annotations = {{70007U, 5U}, {70000U + centreStart, 6U}};
    batch.valid = true;
    Expect(SpellingBatchIsCurrent(batch, window.revision),
        "Spelling annotations publish while all revision tags match");
    Expect(!SpellingBatchIsCurrent(batch, SpellingRevision{7U, 43U, 13U}),
        "Spelling annotations reject a stale content revision");
    Expect(!SpellingBatchIsCurrent(batch, SpellingRevision{7U, 42U, 14U}),
        "Spelling annotations reject a stale layout revision");
    Expect(!SpellingBatchIsCurrent(batch, SpellingRevision{8U, 42U, 13U}),
        "Spelling annotations reject a reused document session");
    batch.valid = false;
    Expect(!SpellingBatchIsCurrent(batch, window.revision),
        "Spelling annotations reject explicitly invalidated batches");

    batch.valid = true;
    SpellingAnnotationBatch published;
    const SpellingRevision changedContent{7U, 43U, 13U};
    Expect(!TryPublishSpellingBatch(published, batch, changedContent) && !published.valid,
        "Spelling publication rejects a completed backend result after a content revision change");
    SpellingTextWindow sameSizeEdit = window;
    sameSizeEdit.revision.content += 1U;
    Expect(sameSizeEdit.text.size() == window.text.size() &&
        !TryPublishSpellingBatch(published, batch, sameSizeEdit.revision),
        "Spelling publication rejects stale results after a same-size edit");
    Expect(TryPublishSpellingBatch(published, batch, window.revision) &&
        SpellingBatchIsCurrent(published, window.revision) && published.annotations.size() == 2U,
        "Spelling publication accepts controlled backend ranges only for the matching revision");
    const SpellingAnnotationBatch previousPublication = published;
    SpellingAnnotationBatch staleCandidate = batch;
    staleCandidate.revision.layout += 1U;
    Expect(!TryPublishSpellingBatch(published, staleCandidate, window.revision) &&
        published.revision.layout == previousPublication.revision.layout &&
        published.annotations.size() == previousPublication.annotations.size(),
        "A stale spelling publication leaves the prior published batch untouched");
}

void TestSpellCapabilityLabels()
{
    using classic_notepad::SpellCapability;

    ExpectText(
        classic_notepad::SpellCapabilityLabel(SpellCapability::Available),
        L"Available",
        "Available spell capability label formats");
    ExpectText(
        classic_notepad::SpellCapabilityLabel(SpellCapability::MissingBackend),
        L"MissingBackend",
        "Missing backend spell capability label formats");
    ExpectText(
        classic_notepad::SpellCapabilityLabel(SpellCapability::MissingDictionary),
        L"MissingDictionary",
        "Missing dictionary spell capability label formats");
    ExpectText(
        classic_notepad::SpellCapabilityLabel(SpellCapability::DisabledByBuild),
        L"DisabledByBuild",
        "Disabled build spell capability label formats");
}

void TestAppearanceThemeParsing()
{
    using classic_notepad::AppearanceTheme;

    Expect(
        classic_notepad::TryParseAppearanceTheme("system").value_or(AppearanceTheme::Dark) == AppearanceTheme::System,
        "System appearance value parses");
    Expect(
        classic_notepad::TryParseAppearanceTheme(" light ").value_or(AppearanceTheme::Dark) == AppearanceTheme::Light,
        "Light appearance value parses with whitespace");
    Expect(
        classic_notepad::TryParseAppearanceTheme("DARK").value_or(AppearanceTheme::System) == AppearanceTheme::Dark,
        "Dark appearance value parses case-insensitively");
    Expect(
        !classic_notepad::TryParseAppearanceTheme("sepia").has_value(),
        "Invalid appearance values are rejected");
    Expect(
        classic_notepad::ParseAppearanceThemeOrSystem("sepia") == AppearanceTheme::System,
        "Invalid appearance override falls back to System");

    ExpectText(classic_notepad::AppearanceThemeLabel(AppearanceTheme::System), L"System", "System label formats");
    ExpectText(classic_notepad::AppearanceThemeLabel(AppearanceTheme::Light), L"Light", "Light label formats");
    ExpectText(classic_notepad::AppearanceThemeLabel(AppearanceTheme::Dark), L"Dark", "Dark label formats");

    Expect(
        std::string(classic_notepad::AppearanceThemeName(AppearanceTheme::System)) == "system",
        "System JSON name formats");
    Expect(
        std::string(classic_notepad::AppearanceThemeName(AppearanceTheme::Light)) == "light",
        "Light JSON name formats");
    Expect(
        std::string(classic_notepad::AppearanceThemeName(AppearanceTheme::Dark)) == "dark",
        "Dark JSON name formats");

    Expect(!classic_notepad::ResolveDarkMode(AppearanceTheme::Light, true, false), "Light override disables dark");
    Expect(classic_notepad::ResolveDarkMode(AppearanceTheme::Dark, false, false), "Dark override enables dark");
    Expect(classic_notepad::ResolveDarkMode(AppearanceTheme::System, true, false), "System follows dark preference");
    Expect(!classic_notepad::ResolveDarkMode(AppearanceTheme::System, false, false), "System follows light preference");
    Expect(!classic_notepad::ResolveDarkMode(AppearanceTheme::Dark, true, true), "High contrast suppresses forced dark");
}

void TestEditorTextBoundarySemantics()
{
    using classic_notepad::CollapsedDeleteRange;
    using classic_notepad::NextScalarBoundary;
    using classic_notepad::PreviousScalarBoundary;
    using classic_notepad::TextUnitRange;

    const std::wstring emoji{L'x', static_cast<wchar_t>(0xD83D),
        static_cast<wchar_t>(0xDE00), L'y'};
    Expect(NextScalarBoundary(emoji, 1U) == 3U, "Next scalar boundary skips both UTF-16 surrogate units");
    Expect(PreviousScalarBoundary(emoji, 3U) == 1U, "Previous scalar boundary skips both UTF-16 surrogate units");
    TextUnitRange range{};
    Expect(CollapsedDeleteRange(emoji, 3U, true, range) && range.start == 1U && range.end == 3U,
        "Backspace deletes a complete supplementary scalar");
    Expect(CollapsedDeleteRange(emoji, 1U, false, range) && range.start == 1U && range.end == 3U,
        "Delete deletes a complete supplementary scalar");
    Expect(CollapsedDeleteRange(emoji, 2U, true, range) && range.start == 1U && range.end == 3U,
        "Backspace repairs a caret inside a surrogate pair without leaving a half");
    Expect(CollapsedDeleteRange(emoji, 2U, false, range) && range.start == 1U && range.end == 3U,
        "Delete repairs a caret inside a surrogate pair without leaving a half");

    const std::wstring crlf = L"abc\r\ndef";
    Expect(CollapsedDeleteRange(crlf, 3U, false, range) && range.start == 3U && range.end == 5U,
        "Delete treats CRLF as one line break");
    Expect(CollapsedDeleteRange(crlf, 5U, true, range) && range.start == 3U && range.end == 5U,
        "Backspace treats CRLF as one line break");
    Expect(CollapsedDeleteRange(crlf, 4U, true, range) && range.start == 3U && range.end == 5U,
        "Backspace at the CRLF interior removes the pair");
    Expect(CollapsedDeleteRange(crlf, 4U, false, range) && range.start == 3U && range.end == 5U,
        "Delete at the CRLF interior removes the pair");
}

} // namespace

int main()
{
    TestEncodingDetection();
    TestEncodingOutput();
    TestLineEndingAnalysis();
    TestLineEndingConversion();
    TestDocumentRoundTrips();
    TestDocumentMetadata();
    TestDocumentLoadAndSaveFailures();
    TestTextMetadataFormatting();
    TestDocumentNewFilePath();
    TestLargeFileStreamingIndex();
    TestLargeFileScannerCoordinateMatrix();
    TestLargeFileBoundedPieceBuilder();
    TestLargeFileEditedWidthSummaries();
    TestLargeFileScrollMapping();
    TestLargeDocumentPieceTable();
    TestLargeDocumentUnicodeBoundaries();
    TestLargeDocumentBoundedStreamingAndCancellation();
    TestLargeDocumentCheckpointReuseAndUnicodeWindows();
    TestLargeDocumentTransactionalReadFailures();
    TestLargeDocumentUndoBudgetAndSharedOwnership();
    TestLargeDocumentRandomizedEditsAgainstTextModel();
    TestSpellWordExpansion();
    TestSpellRangeOverlap();
    TestSpellingWindowMappingAndRevisionTags();
    TestSpellCapabilityLabels();
    TestEditorTextBoundarySemantics();
    TestAppearanceThemeParsing();

    if (g_failureCount != 0) {
        std::cerr << g_failureCount << " test failure(s).\n";
        return 1;
    }

    std::cout << "All text conversion tests passed.\n";
    return 0;
}
