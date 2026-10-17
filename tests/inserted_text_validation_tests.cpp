#include "inserted_text_validation.h"
#include "large_document.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
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

std::wstring ValidPair()
{
    return {static_cast<wchar_t>(0xD83DU), static_cast<wchar_t>(0xDE00U)};
}

void TestPredicateCompatibility()
{
    using classic_notepad::IsValidInsertedText;

    const std::wstring empty;
    const std::wstring ordinary = L"ordinary text\r\nwith tabs\t";
    std::wstring controls{L'\x01', L'\t', L'\r', L'\n', L'\x7F'};
    controls.push_back(L'\0');
    const std::wstring pair = ValidPair();
    const std::wstring high{static_cast<wchar_t>(0xD800U)};
    const std::wstring low{static_cast<wchar_t>(0xDC00U)};
    const std::wstring interrupted{static_cast<wchar_t>(0xD800U), L'x',
        static_cast<wchar_t>(0xDC00U)};

    Expect(IsValidInsertedText(empty), "Empty insertion remains valid");
    Expect(IsValidInsertedText(ordinary), "Ordinary UTF-16 text remains valid");
    Expect(IsValidInsertedText(controls), "Controls and embedded NUL retain the existing accepted policy");
    Expect(IsValidInsertedText(pair), "A correctly paired UTF-16 surrogate sequence is valid");
    Expect(!IsValidInsertedText(high), "A dangling high surrogate is invalid");
    Expect(!IsValidInsertedText(low), "A lone low surrogate is invalid");
    Expect(!IsValidInsertedText(interrupted), "A high surrogate followed by a non-low unit is invalid");

    if constexpr (sizeof(wchar_t) > 2U) {
        const std::wstring scalar{static_cast<wchar_t>(0x1F600U)};
        Expect(IsValidInsertedText(scalar),
            "A 32-bit wchar outside the surrogate ranges retains the existing accepted policy");
    }
}

struct ModelSnapshot {
    std::wstring text;
    std::uint64_t characters = 0U;
    std::uint64_t displayedCharacters = 0U;
    std::uint64_t lines = 0U;
    std::uint64_t maximumLineColumns = 0U;
    std::uint64_t pieces = 0U;
    std::uint64_t generation = 0U;
    std::uint64_t savedState = 0U;
    std::uint64_t undoBytes = 0U;
    std::uint64_t insertedBufferBytes = 0U;
    bool displayedCountKnown = false;
    bool modified = false;
    bool canUndo = false;
};

bool CaptureSnapshot(classic_notepad::LargeDocument& document, ModelSnapshot& snapshot)
{
    snapshot = {};
    std::wstring error;
    if (!document.StreamText([&snapshot](const std::wstring& chunk) {
            snapshot.text.append(chunk);
            return true;
        }, {}, error)) {
        return false;
    }
    snapshot.characters = document.CharacterCount();
    snapshot.displayedCharacters = document.DisplayedCharacterCount();
    snapshot.displayedCountKnown = document.HasExactDisplayedCharacterCount();
    snapshot.lines = document.LineCount();
    snapshot.maximumLineColumns = document.MaximumLineColumns();
    snapshot.pieces = document.PieceCount();
    snapshot.generation = document.Generation();
    snapshot.savedState = document.SavedState();
    snapshot.undoBytes = document.UndoBytes();
    snapshot.insertedBufferBytes = document.InsertedBufferBytes();
    snapshot.modified = document.IsModified();
    snapshot.canUndo = document.CanUndo();
    return true;
}

bool SameSnapshot(const ModelSnapshot& left, const ModelSnapshot& right)
{
    return left.text == right.text &&
        left.characters == right.characters &&
        left.displayedCharacters == right.displayedCharacters &&
        left.lines == right.lines &&
        left.maximumLineColumns == right.maximumLineColumns &&
        left.pieces == right.pieces &&
        left.generation == right.generation &&
        left.savedState == right.savedState &&
        left.undoBytes == right.undoBytes &&
        left.insertedBufferBytes == right.insertedBufferBytes &&
        left.displayedCountKnown == right.displayedCountKnown &&
        left.modified == right.modified &&
        left.canUndo == right.canUndo;
}

std::unique_ptr<classic_notepad::LargeDocument> MakeSourceDocument(const std::string& source)
{
    const auto reader = [&source](std::uint64_t offset, std::size_t length,
                              std::vector<std::uint8_t>& bytes) {
        if (offset > source.size() || length > source.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        bytes.assign(source.begin() + static_cast<std::ptrdiff_t>(offset),
            source.begin() + static_cast<std::ptrdiff_t>(offset + length));
        return true;
    };
    auto document = std::make_unique<classic_notepad::LargeDocument>(
        classic_notepad::LargeFileEncoding::Utf8, reader);
    std::wstring error;
    const bool initialized = document->Initialize({{
        0U, static_cast<std::uint64_t>(source.size()), static_cast<std::uint64_t>(source.size())}}, error);
    if (!initialized) {
        std::cerr << "FAIL: test source document initializes: " << error.size() << " error units\n";
        ++g_failureCount;
    }
    return document;
}

void TestMalformedReplacementTransactions()
{
    using classic_notepad::LargeDocument;

    const std::string source = "a a";
    auto documentOwner = MakeSourceDocument(source);
    LargeDocument& document = *documentOwner;
    std::wstring error;
    std::uint64_t afterStart = 0U;
    std::uint64_t afterEnd = 0U;
    document.MarkSaved();
    Expect(document.Replace(0U, 0U, L"!", 1U, 2U, afterStart, afterEnd, error),
        "Prior valid edit establishes retained dirty state and Undo history");

    ModelSnapshot baseline;
    Expect(CaptureSnapshot(document, baseline), "Malformed-replacement baseline can be captured");
    Expect(baseline.modified && baseline.canUndo && baseline.undoBytes != 0U &&
        baseline.insertedBufferBytes != 0U,
        "Malformed-replacement baseline has dirty state, Undo, and inserted storage to preserve");

    const std::wstring high{static_cast<wchar_t>(0xD800U)};
    afterStart = 0x11111111U;
    afterEnd = 0x22222222U;
    error.clear();
    Expect(!document.Replace(1U, 2U, high, 7U, 8U, afterStart, afterEnd, error) &&
        error == L"Inserted text contains an unmatched Unicode surrogate." &&
        afterStart == 0x11111111U && afterEnd == 0x22222222U,
        "Malformed Replace rejects at the existing validation stage with unchanged outputs and error text");
    ModelSnapshot afterReplace;
    Expect(CaptureSnapshot(document, afterReplace) && SameSnapshot(baseline, afterReplace),
        "Malformed matched Replace leaves all model counts, text, generation, dirty state, Undo, and storage unchanged");

    const std::wstring low{static_cast<wchar_t>(0xDC00U)};
    bool cancelled = false;
    error.clear();
    Expect(document.ReplaceAll(L"a", low, true, false, cancelled, {}, error) == 0U &&
        !cancelled && error == L"Inserted text contains an unmatched Unicode surrogate.",
        "Malformed matched Replace All returns the established surrogate error");
    ModelSnapshot afterReplaceAll;
    Expect(CaptureSnapshot(document, afterReplaceAll) && SameSnapshot(baseline, afterReplaceAll),
        "Malformed matched Replace All leaves all model state, Undo, and inserted storage unchanged");

    error.clear();
    cancelled = false;
    Expect(document.ReplaceAll(L"not-present", high, true, false, cancelled, {}, error) == 0U &&
        !cancelled && error.empty(),
        "Replace All with no matches returns before validating replacement text");
    ModelSnapshot afterNoMatch;
    Expect(CaptureSnapshot(document, afterNoMatch) && SameSnapshot(baseline, afterNoMatch),
        "No-match malformed replacement leaves all model state unchanged");

    error.clear();
    cancelled = false;
    Expect(document.ReplaceAll(L"a", high, true, false, cancelled,
            []() { return true; }, error) == 0U && cancelled && error.empty(),
        "Cancelled Replace All returns before replacement validation and keeps its cancellation result");
    ModelSnapshot afterCancellation;
    Expect(CaptureSnapshot(document, afterCancellation) && SameSnapshot(baseline, afterCancellation),
        "Cancellation with malformed replacement leaves all model state unchanged");

    std::uint64_t undoSelectionStart = 99U;
    std::uint64_t undoSelectionEnd = 99U;
    error.clear();
    Expect(document.Undo(undoSelectionStart, undoSelectionEnd, error) &&
        undoSelectionStart == 1U && undoSelectionEnd == 2U,
        "The previous valid Undo action remains available after rejected malformed replacements");
    ModelSnapshot afterUndo;
    Expect(CaptureSnapshot(document, afterUndo) && afterUndo.text == L"a a" &&
        !afterUndo.modified && !afterUndo.canUndo && afterUndo.insertedBufferBytes == 0U,
        "Undo after rejections restores the saved source and releases inserted storage");
}

void TestValidUtf16ReplaceAllUndo()
{
    using classic_notepad::LargeDocument;

    const std::string source = "a-a";
    auto documentOwner = MakeSourceDocument(source);
    LargeDocument& document = *documentOwner;
    document.MarkSaved();
    std::wstring error;
    std::uint64_t afterStart = 0U;
    std::uint64_t afterEnd = 0U;
    const std::wstring pair = ValidPair();
    Expect(document.Replace(0U, 1U, pair, 0U, 1U, afterStart, afterEnd, error),
        "Replace accepts a valid UTF-16 surrogate pair");
    ModelSnapshot replacedOne;
    Expect(CaptureSnapshot(document, replacedOne) && replacedOne.text == pair + L"-a" &&
        replacedOne.characters == 4U && replacedOne.displayedCharacters == 3U,
        "Single replacement retains UTF-16 unit length and displayed count semantics");
    Expect(document.Undo(afterStart, afterEnd, error), "Valid UTF-16 replacement can be undone");
    ModelSnapshot restoredOne;
    Expect(CaptureSnapshot(document, restoredOne) && restoredOne.text == L"a-a" &&
        !restoredOne.modified,
        "One Undo restores the exact original after valid UTF-16 replacement");

    const std::string repeatedSource = "a a";
    auto repeatedOwner = MakeSourceDocument(repeatedSource);
    LargeDocument& repeated = *repeatedOwner;
    repeated.MarkSaved();
    bool cancelled = false;
    cancelled = false;
    error.clear();
    const std::wstring replacement = std::wstring(L"x") + pair + L"y";
    const std::uint64_t count = repeated.ReplaceAll(L"a", replacement, true, false,
        cancelled, {}, error);
    ModelSnapshot replacedAll;
    Expect(count == 2U && !cancelled && error.empty() &&
        CaptureSnapshot(repeated, replacedAll) && replacedAll.text == replacement + L" " + replacement,
        "Replace All accepts valid UTF-16 replacement text at every match");
    Expect(repeated.Undo(afterStart, afterEnd, error), "Valid UTF-16 Replace All is one undoable action");
    ModelSnapshot restoredAll;
    Expect(CaptureSnapshot(repeated, restoredAll) && restoredAll.text == L"a a" &&
        !restoredAll.modified && !restoredAll.canUndo,
        "A single Undo restores the exact source after valid UTF-16 Replace All");
}

} // namespace

int main()
{
    TestPredicateCompatibility();
    TestMalformedReplacementTransactions();
    TestValidUtf16ReplaceAllUndo();

    if (g_failureCount != 0) {
        std::cerr << g_failureCount << " inserted text validation test(s) failed.\n";
        return 1;
    }
    std::cout << "InsertedTextValidationTests passed.\n";
    return 0;
}
