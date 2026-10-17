#pragma once

#include "large_text_width_summary.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace classic_notepad {

// The only encodings accepted by the streaming Windows large-file viewer.
// UTF-8 includes plain ASCII and optional UTF-8 BOMs.
enum class LargeFileEncoding {
    Utf8,
    Utf16Le,
    Unsupported
};

inline constexpr std::uint64_t kLargeFileOriginalPieceMaximumBytes = 64ULL * 1024ULL;
inline constexpr std::uint64_t kLargeFileOriginalPieceMaximumCharacters = 64ULL * 1024ULL;

// A bounded descriptor for a contiguous source range. logicalLineBreaks is
// summarized within this piece; a CR/LF pair split across adjacent pieces is
// combined by the document tree when it composes the summaries.
struct LargeFilePieceSummary {
    std::uint64_t byteOffset = 0;
    std::uint64_t byteLength = 0;
    std::uint64_t characterOffset = 0;
    std::uint64_t characterLength = 0;
    std::uint64_t logicalLineBreaks = 0;
    wchar_t firstCharacter = 0;
    wchar_t lastCharacter = 0;
    LargeFileWidthSummary widthSummary{};
    bool widthSummaryKnown = false;
    std::uint64_t displayedCharacterCount = 0;
};

// Builds canonical original-piece summaries while accepting arbitrary input
// chunk boundaries. It retains only decoder state and one bounded piece.
class LargeFilePieceBuilder {
public:
    LargeFilePieceBuilder(
        LargeFileEncoding encoding,
        std::uint64_t dataOffset,
        std::uint64_t maximumPieceBytes = kLargeFileOriginalPieceMaximumBytes,
        std::uint64_t maximumPieceCharacters = kLargeFileOriginalPieceMaximumCharacters);

    bool Consume(
        const std::uint8_t* bytes,
        std::size_t size,
        std::uint64_t fileOffset,
        std::vector<LargeFilePieceSummary>& completedPieces);
    bool Finish(std::vector<LargeFilePieceSummary>& completedPieces);

    bool IsValid() const;
    std::uint64_t NextOffset() const;
    std::uint64_t SafeOffset() const;
    std::uint64_t CharacterCount() const;

private:
    bool FeedUtf8Byte(std::uint8_t byte, std::uint64_t byteOffset,
        std::vector<LargeFilePieceSummary>& completedPieces);
    bool FeedUtf16Byte(std::uint8_t byte, std::uint64_t byteOffset,
        std::vector<LargeFilePieceSummary>& completedPieces);
    bool FeedScalar(std::uint32_t scalar, std::uint64_t byteEnd, std::uint64_t byteLength,
        std::vector<LargeFilePieceSummary>& completedPieces);
    bool FlushPiece(std::vector<LargeFilePieceSummary>& completedPieces);
    void FeedCodeUnit(wchar_t character);
    void FeedWidthCharacter(wchar_t character);

    LargeFileEncoding encoding_;
    std::uint64_t nextOffset_;
    std::uint64_t safeOffset_;
    std::uint64_t maximumPieceBytes_;
    std::uint64_t maximumPieceCharacters_;
    std::uint64_t characterCount_ = 0;
    bool valid_ = true;
    bool finished_ = false;

    int utf8Remaining_ = 0;
    std::uint32_t utf8CodePoint_ = 0;
    std::uint32_t utf8Minimum_ = 0;
    std::uint64_t utf8ScalarStart_ = 0;
    bool utf16HasLowByte_ = false;
    std::uint8_t utf16LowByte_ = 0;
    bool utf16HasHighSurrogate_ = false;
    std::uint16_t utf16HighSurrogate_ = 0;
    std::uint64_t utf16HighSurrogateStart_ = 0;

    std::uint64_t pieceStartByte_ = 0;
    std::uint64_t pieceStartCharacter_ = 0;
    std::uint64_t pieceByteLength_ = 0;
    std::uint64_t pieceCharacterLength_ = 0;
    std::uint64_t pieceDisplayedCharacterCount_ = 0;
    std::uint64_t pieceLineBreaks_ = 0;
    bool pieceHasCharacters_ = false;
    wchar_t pieceFirstCharacter_ = 0;
    wchar_t pieceLastCharacter_ = 0;
    bool piecePendingCr_ = false;
    bool pieceHasPreviousCodeUnit_ = false;
    wchar_t piecePreviousCodeUnit_ = 0;
    LargeFileWidthSummary pieceWidthSummary_{};
    bool pieceWidthPrefixOpen_ = true;
};

struct LargeFileEncodingInfo {
    LargeFileEncoding encoding = LargeFileEncoding::Utf8;
    std::uint64_t dataOffset = 0;
};

LargeFileEncodingInfo DetectLargeFileEncoding(const std::uint8_t* bytes, std::size_t size);

// Stateful line splitter and strict encoding validator.  Chunks may begin/end at
// any byte; all offsets are file offsets rather than addresses or size_t values.
class LargeFileLineScanner {
public:
    LargeFileLineScanner(LargeFileEncoding encoding, std::uint64_t dataOffset);

    bool Consume(
        const std::uint8_t* bytes,
        std::size_t size,
        std::uint64_t fileOffset,
        std::vector<std::uint64_t>& lineOffsets,
        std::vector<std::uint64_t>* lineCharacterOffsets = nullptr);
    bool Finish(
        std::vector<std::uint64_t>& lineOffsets,
        std::vector<std::uint64_t>* lineCharacterOffsets = nullptr);

    bool IsValid() const;
    std::uint64_t NextOffset() const;
    std::uint64_t CrlfCount() const;
    std::uint64_t LfCount() const;
    std::uint64_t CrCount() const;
    std::uint64_t CharacterCount() const;
    std::uint64_t MaximumLineColumns() const;

private:
    bool FeedUtf8Byte(std::uint8_t byte);
    bool FeedUtf16Byte(
        std::uint8_t byte,
        std::uint64_t byteOffset,
        std::vector<std::uint64_t>& lineOffsets,
        std::vector<std::uint64_t>* lineCharacterOffsets);
    bool FeedCharacter(
        std::uint32_t character,
        std::uint64_t characterEnd,
        std::vector<std::uint64_t>& lineOffsets,
        std::vector<std::uint64_t>* lineCharacterOffsets);
    bool AppendLineOffset(
        std::uint64_t offset,
        std::vector<std::uint64_t>& lineOffsets,
        std::vector<std::uint64_t>* lineCharacterOffsets,
        std::uint64_t characterOffset);

    LargeFileEncoding encoding_;
    std::uint64_t nextOffset_;
    bool valid_ = true;
    bool pendingCr_ = false;
    std::uint64_t pendingCrEnd_ = 0;
    std::uint64_t pendingCrCharacterEnd_ = 0;
    std::uint64_t crlfCount_ = 0;
    std::uint64_t lfCount_ = 0;
    std::uint64_t crCount_ = 0;
    std::uint64_t characterCount_ = 0;
    std::uint64_t currentLineColumns_ = 0;
    std::uint64_t maximumLineColumns_ = 0;

    int utf8Remaining_ = 0;
    std::uint32_t utf8CodePoint_ = 0;
    std::uint32_t utf8Minimum_ = 0;
    bool utf16HasLowByte_ = false;
    std::uint8_t utf16LowByte_ = 0;
    bool utf16HasHighSurrogate_ = false;
};

} // namespace classic_notepad
