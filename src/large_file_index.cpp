#include "large_file_index.h"

#include "editor_text_semantics.h"

#include <algorithm>
#include <limits>

namespace classic_notepad {

LargeFileEncodingInfo DetectLargeFileEncoding(const std::uint8_t* bytes, std::size_t size)
{
    if (bytes == nullptr && size != 0U) {
        return { LargeFileEncoding::Unsupported, 0 };
    }
    if (size >= 2U && bytes[0] == 0xFEU && bytes[1] == 0xFFU) {
        return { LargeFileEncoding::Unsupported, 0 };
    }
    if (size >= 2U && bytes[0] == 0xFFU && bytes[1] == 0xFEU) {
        return { LargeFileEncoding::Utf16Le, 2 };
    }
    if (size >= 3U && bytes[0] == 0xEFU && bytes[1] == 0xBBU && bytes[2] == 0xBFU) {
        return { LargeFileEncoding::Utf8, 3 };
    }
    return { LargeFileEncoding::Utf8, 0 };
}

LargeFilePieceBuilder::LargeFilePieceBuilder(
    LargeFileEncoding encoding,
    std::uint64_t dataOffset,
    std::uint64_t maximumPieceBytes,
    std::uint64_t maximumPieceCharacters)
    : encoding_(encoding)
    , nextOffset_(dataOffset)
    , safeOffset_(dataOffset)
    , maximumPieceBytes_(maximumPieceBytes)
    , maximumPieceCharacters_(maximumPieceCharacters)
    , pieceStartByte_(dataOffset)
{
    valid_ = encoding != LargeFileEncoding::Unsupported && maximumPieceBytes_ != 0U &&
        maximumPieceBytes_ <= kLargeFileOriginalPieceMaximumBytes && maximumPieceCharacters_ != 0U &&
        maximumPieceCharacters_ <= kLargeFileOriginalPieceMaximumCharacters;
}

bool LargeFilePieceBuilder::Consume(
    const std::uint8_t* bytes,
    std::size_t size,
    std::uint64_t fileOffset,
    std::vector<LargeFilePieceSummary>& completedPieces)
{
    completedPieces.clear();
    if (!valid_ || finished_ || (bytes == nullptr && size != 0U) || fileOffset != nextOffset_ ||
        size > static_cast<std::size_t>((std::numeric_limits<std::uint64_t>::max)() - fileOffset)) {
        valid_ = false;
        return false;
    }
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint64_t offset = fileOffset + static_cast<std::uint64_t>(index);
        const bool accepted = encoding_ == LargeFileEncoding::Utf8
            ? FeedUtf8Byte(bytes[index], offset, completedPieces)
            : FeedUtf16Byte(bytes[index], offset, completedPieces);
        if (!accepted) {
            valid_ = false;
            return false;
        }
    }
    nextOffset_ = fileOffset + static_cast<std::uint64_t>(size);
    return true;
}

bool LargeFilePieceBuilder::FeedUtf8Byte(
    std::uint8_t byte,
    std::uint64_t byteOffset,
    std::vector<LargeFilePieceSummary>& completedPieces)
{
    if (utf8Remaining_ == 0) {
        if (byte <= 0x7FU) {
            utf8CodePoint_ = byte;
            utf8ScalarStart_ = byteOffset;
            return FeedScalar(utf8CodePoint_, byteOffset + 1U, 1U, completedPieces);
        }
        if (byte >= 0xC2U && byte <= 0xDFU) {
            utf8Remaining_ = 1;
            utf8CodePoint_ = byte & 0x1FU;
            utf8Minimum_ = 0x80U;
        } else if (byte >= 0xE0U && byte <= 0xEFU) {
            utf8Remaining_ = 2;
            utf8CodePoint_ = byte & 0x0FU;
            utf8Minimum_ = 0x800U;
        } else if (byte >= 0xF0U && byte <= 0xF4U) {
            utf8Remaining_ = 3;
            utf8CodePoint_ = byte & 0x07U;
            utf8Minimum_ = 0x10000U;
        } else {
            return false;
        }
        utf8ScalarStart_ = byteOffset;
        return true;
    }

    if ((byte & 0xC0U) != 0x80U) return false;
    utf8CodePoint_ = (utf8CodePoint_ << 6U) | (byte & 0x3FU);
    --utf8Remaining_;
    if (utf8Remaining_ != 0) return true;
    if (utf8CodePoint_ < utf8Minimum_ || utf8CodePoint_ > 0x10FFFFU ||
        (utf8CodePoint_ >= 0xD800U && utf8CodePoint_ <= 0xDFFFU)) {
        return false;
    }
    return FeedScalar(utf8CodePoint_, byteOffset + 1U,
        byteOffset + 1U - utf8ScalarStart_, completedPieces);
}

bool LargeFilePieceBuilder::FeedUtf16Byte(
    std::uint8_t byte,
    std::uint64_t byteOffset,
    std::vector<LargeFilePieceSummary>& completedPieces)
{
    if (!utf16HasLowByte_) {
        utf16HasLowByte_ = true;
        utf16LowByte_ = byte;
        return true;
    }

    const std::uint16_t unit = static_cast<std::uint16_t>(utf16LowByte_) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(byte) << 8U);
    utf16HasLowByte_ = false;
    const std::uint64_t unitStart = byteOffset - 1U;
    if (utf16HasHighSurrogate_) {
        if (unit < 0xDC00U || unit > 0xDFFFU) return false;
        const std::uint32_t scalar = 0x10000U +
            ((static_cast<std::uint32_t>(utf16HighSurrogate_) - 0xD800U) << 10U) +
            (static_cast<std::uint32_t>(unit) - 0xDC00U);
        const std::uint64_t scalarStart = utf16HighSurrogateStart_;
        utf16HasHighSurrogate_ = false;
        return FeedScalar(scalar, byteOffset + 1U, byteOffset + 1U - scalarStart, completedPieces);
    }
    if (unit >= 0xD800U && unit <= 0xDBFFU) {
        utf16HasHighSurrogate_ = true;
        utf16HighSurrogate_ = unit;
        utf16HighSurrogateStart_ = unitStart;
        return true;
    }
    if (unit >= 0xDC00U && unit <= 0xDFFFU) return false;
    return FeedScalar(unit, byteOffset + 1U, 2U, completedPieces);
}

bool LargeFilePieceBuilder::FeedScalar(
    std::uint32_t scalar,
    std::uint64_t byteEnd,
    std::uint64_t byteLength,
    std::vector<LargeFilePieceSummary>& completedPieces)
{
    if (scalar == 0U || byteLength == 0U) return false;
    const std::uint64_t units = scalar > 0xFFFFU ? 2U : 1U;
    if (characterCount_ > (std::numeric_limits<std::uint64_t>::max)() - units) return false;

    if (pieceByteLength_ != 0U &&
        (byteLength > maximumPieceBytes_ - pieceByteLength_ ||
            units > maximumPieceCharacters_ - pieceCharacterLength_)) {
        if (!FlushPiece(completedPieces)) return false;
    }
    if (byteLength > maximumPieceBytes_ || units > maximumPieceCharacters_ ||
        pieceByteLength_ > maximumPieceBytes_ - byteLength ||
        pieceCharacterLength_ > maximumPieceCharacters_ - units) {
        return false;
    }

    wchar_t firstUnit = 0;
    wchar_t lastUnit = 0;
    if (scalar <= 0xFFFFU) {
        firstUnit = lastUnit = static_cast<wchar_t>(scalar);
    } else {
        const std::uint32_t adjusted = scalar - 0x10000U;
        firstUnit = static_cast<wchar_t>(0xD800U + (adjusted >> 10U));
        lastUnit = static_cast<wchar_t>(0xDC00U + (adjusted & 0x3FFU));
    }
    if (!pieceHasCharacters_) {
        if (byteEnd < byteLength || byteEnd - byteLength != pieceStartByte_) return false;
        pieceFirstCharacter_ = firstUnit;
        pieceHasCharacters_ = true;
    }
    pieceLastCharacter_ = lastUnit;
    pieceByteLength_ += byteLength;
    pieceCharacterLength_ += units;
    characterCount_ += units;

    if (units == 1U) {
        FeedCodeUnit(firstUnit);
    } else {
        FeedCodeUnit(firstUnit);
        FeedCodeUnit(lastUnit);
    }
    safeOffset_ = byteEnd;
    if (pieceByteLength_ == maximumPieceBytes_ ||
        pieceCharacterLength_ == maximumPieceCharacters_) {
        return FlushPiece(completedPieces);
    }
    return true;
}

void LargeFilePieceBuilder::FeedCodeUnit(wchar_t character)
{
    ++pieceDisplayedCharacterCount_;
    if (pieceHasPreviousCodeUnit_ &&
        ((piecePreviousCodeUnit_ == L'\r' && character == L'\n') ||
            (IsHighSurrogateUnit(piecePreviousCodeUnit_) && IsLowSurrogateUnit(character)))) {
        --pieceDisplayedCharacterCount_;
    }
    piecePreviousCodeUnit_ = character;
    pieceHasPreviousCodeUnit_ = true;

    FeedWidthCharacter(character);
    if (piecePendingCr_) {
        ++pieceLineBreaks_;
        piecePendingCr_ = false;
        if (character == L'\n') return;
    }
    if (character == L'\r') {
        piecePendingCr_ = true;
    } else if (character == L'\n') {
        ++pieceLineBreaks_;
    }
}

void LargeFilePieceBuilder::FeedWidthCharacter(wchar_t character)
{
    AppendLargeTextWidthCharacter(pieceWidthSummary_, pieceWidthPrefixOpen_, character);
}

bool LargeFilePieceBuilder::FlushPiece(std::vector<LargeFilePieceSummary>& completedPieces)
{
    if (pieceByteLength_ == 0U) return true;
    if (piecePendingCr_) {
        ++pieceLineBreaks_;
        piecePendingCr_ = false;
    }
    completedPieces.push_back({pieceStartByte_, pieceByteLength_, pieceStartCharacter_,
        pieceCharacterLength_, pieceLineBreaks_, pieceFirstCharacter_, pieceLastCharacter_,
        pieceWidthSummary_, true, pieceDisplayedCharacterCount_});
    pieceStartByte_ += pieceByteLength_;
    pieceStartCharacter_ += pieceCharacterLength_;
    pieceByteLength_ = 0;
    pieceCharacterLength_ = 0;
    pieceDisplayedCharacterCount_ = 0;
    pieceLineBreaks_ = 0;
    pieceHasCharacters_ = false;
    pieceFirstCharacter_ = 0;
    pieceLastCharacter_ = 0;
    pieceHasPreviousCodeUnit_ = false;
    piecePreviousCodeUnit_ = 0;
    pieceWidthSummary_ = {};
    pieceWidthPrefixOpen_ = true;
    return true;
}

bool LargeFilePieceBuilder::Finish(std::vector<LargeFilePieceSummary>& completedPieces)
{
    completedPieces.clear();
    if (!valid_ || finished_ || utf8Remaining_ != 0 || utf16HasLowByte_ || utf16HasHighSurrogate_) {
        valid_ = false;
        return false;
    }
    finished_ = true;
    return FlushPiece(completedPieces);
}

bool LargeFilePieceBuilder::IsValid() const { return valid_; }
std::uint64_t LargeFilePieceBuilder::NextOffset() const { return nextOffset_; }
std::uint64_t LargeFilePieceBuilder::SafeOffset() const { return safeOffset_; }
std::uint64_t LargeFilePieceBuilder::CharacterCount() const { return characterCount_; }

LargeFileLineScanner::LargeFileLineScanner(LargeFileEncoding encoding, std::uint64_t dataOffset)
    : encoding_(encoding)
    , nextOffset_(dataOffset)
{
    valid_ = encoding != LargeFileEncoding::Unsupported;
}

bool LargeFileLineScanner::AppendLineOffset(
    std::uint64_t offset,
    std::vector<std::uint64_t>& lineOffsets,
    std::vector<std::uint64_t>* lineCharacterOffsets,
    std::uint64_t characterOffset)
{
    if (!lineOffsets.empty() && offset <= lineOffsets.back()) {
        valid_ = false;
        return false;
    }
    lineOffsets.push_back(offset);
    if (lineCharacterOffsets != nullptr) {
        if (!lineCharacterOffsets->empty() && characterOffset < lineCharacterOffsets->back()) {
            valid_ = false;
            return false;
        }
        lineCharacterOffsets->push_back(characterOffset);
    }
    return true;
}

bool LargeFileLineScanner::FeedCharacter(
    std::uint32_t character,
    std::uint64_t characterEnd,
    std::vector<std::uint64_t>& lineOffsets,
    std::vector<std::uint64_t>* lineCharacterOffsets)
{
    // Native edit controls expose text through NUL-terminated APIs. Reject an
    // embedded NUL while indexing so the mapped model cannot later present or
    // save a shortened version of the source.
    if (character == 0U) {
        valid_ = false;
        return false;
    }
    const auto finishLine = [this]() {
        maximumLineColumns_ = std::max(maximumLineColumns_, currentLineColumns_);
        currentLineColumns_ = 0;
    };
    if (pendingCr_) {
        if (character == static_cast<std::uint32_t>('\n')) {
            ++crlfCount_;
            pendingCr_ = false;
            finishLine();
            return AppendLineOffset(characterEnd, lineOffsets, lineCharacterOffsets, characterCount_);
        }
        ++crCount_;
        finishLine();
        if (!AppendLineOffset(pendingCrEnd_, lineOffsets, lineCharacterOffsets, pendingCrCharacterEnd_)) {
            return false;
        }
        pendingCr_ = false;
    }

    if (character == static_cast<std::uint32_t>('\r')) {
        pendingCr_ = true;
        pendingCrEnd_ = characterEnd;
        pendingCrCharacterEnd_ = characterCount_;
    } else if (character == static_cast<std::uint32_t>('\n')) {
        ++lfCount_;
        finishLine();
        return AppendLineOffset(characterEnd, lineOffsets, lineCharacterOffsets, characterCount_);
    } else {
        const std::uint64_t units = character > 0xFFFFU ? 2U : 1U;
        const std::uint64_t advance = character == static_cast<std::uint32_t>('\t')
            ? 8U - (currentLineColumns_ % 8U) : units;
        if (currentLineColumns_ > (std::numeric_limits<std::uint64_t>::max)() - advance) return false;
        currentLineColumns_ += advance;
    }
    return true;
}

bool LargeFileLineScanner::FeedUtf8Byte(std::uint8_t byte)
{
    if (utf8Remaining_ == 0) {
        if (byte <= 0x7FU) {
            utf8CodePoint_ = byte;
            return true;
        }
        if (byte >= 0xC2U && byte <= 0xDFU) {
            utf8Remaining_ = 1;
            utf8CodePoint_ = byte & 0x1FU;
            utf8Minimum_ = 0x80U;
            return true;
        }
        if (byte >= 0xE0U && byte <= 0xEFU) {
            utf8Remaining_ = 2;
            utf8CodePoint_ = byte & 0x0FU;
            utf8Minimum_ = 0x800U;
            return true;
        }
        if (byte >= 0xF0U && byte <= 0xF4U) {
            utf8Remaining_ = 3;
            utf8CodePoint_ = byte & 0x07U;
            utf8Minimum_ = 0x10000U;
            return true;
        }
        return false;
    }

    if ((byte & 0xC0U) != 0x80U) {
        return false;
    }
    utf8CodePoint_ = (utf8CodePoint_ << 6U) | (byte & 0x3FU);
    --utf8Remaining_;
    if (utf8Remaining_ == 0) {
        return utf8CodePoint_ >= utf8Minimum_ && utf8CodePoint_ <= 0x10FFFFU &&
            (utf8CodePoint_ < 0xD800U || utf8CodePoint_ > 0xDFFFU);
    }
    return true;
}

bool LargeFileLineScanner::FeedUtf16Byte(
    std::uint8_t byte,
    std::uint64_t byteOffset,
    std::vector<std::uint64_t>& lineOffsets,
    std::vector<std::uint64_t>* lineCharacterOffsets)
{
    if (!utf16HasLowByte_) {
        utf16HasLowByte_ = true;
        utf16LowByte_ = byte;
        return true;
    }

    const std::uint32_t unit = static_cast<std::uint32_t>(utf16LowByte_) |
        (static_cast<std::uint32_t>(byte) << 8U);
    utf16HasLowByte_ = false;
    if (utf16HasHighSurrogate_) {
        if (unit < 0xDC00U || unit > 0xDFFFU) {
            return false;
        }
        utf16HasHighSurrogate_ = false;
    } else if (unit >= 0xD800U && unit <= 0xDBFFU) {
        utf16HasHighSurrogate_ = true;
    } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
        return false;
    }
    if (characterCount_ == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    ++characterCount_;
    return FeedCharacter(unit, byteOffset + 1U, lineOffsets, lineCharacterOffsets);
}

bool LargeFileLineScanner::Consume(
    const std::uint8_t* bytes,
    std::size_t size,
    std::uint64_t fileOffset,
    std::vector<std::uint64_t>& lineOffsets,
    std::vector<std::uint64_t>* lineCharacterOffsets)
{
    if (!valid_ || bytes == nullptr || fileOffset != nextOffset_ ||
        size > static_cast<std::size_t>((std::numeric_limits<std::uint64_t>::max)() - fileOffset)) {
        valid_ = false;
        return false;
    }

    for (std::size_t index = 0; index < size; ++index) {
        const std::uint64_t offset = fileOffset + static_cast<std::uint64_t>(index);
        if (encoding_ == LargeFileEncoding::Utf8) {
            const int before = utf8Remaining_;
            if (!FeedUtf8Byte(bytes[index])) {
                valid_ = false;
                return false;
            }
            if ((before == 0 && bytes[index] <= 0x7FU) || (before != 0 && utf8Remaining_ == 0)) {
                const std::uint64_t units = utf8CodePoint_ > 0xFFFFU ? 2U : 1U;
                if (characterCount_ > (std::numeric_limits<std::uint64_t>::max)() - units) {
                    valid_ = false;
                    return false;
                }
                characterCount_ += units;
            }
            if ((before == 0 && bytes[index] <= 0x7FU) || (before != 0 && utf8Remaining_ == 0)) {
                if (!FeedCharacter(utf8CodePoint_, offset + 1U, lineOffsets, lineCharacterOffsets)) {
                    valid_ = false;
                    return false;
                }
            }
        } else if (encoding_ == LargeFileEncoding::Utf16Le) {
            if (!FeedUtf16Byte(bytes[index], offset, lineOffsets, lineCharacterOffsets)) {
                valid_ = false;
                return false;
            }
        } else {
            valid_ = false;
            return false;
        }
    }

    nextOffset_ = fileOffset + static_cast<std::uint64_t>(size);
    return true;
}

bool LargeFileLineScanner::Finish(
    std::vector<std::uint64_t>& lineOffsets,
    std::vector<std::uint64_t>* lineCharacterOffsets)
{
    if (!valid_ || utf8Remaining_ != 0 || utf16HasLowByte_ || utf16HasHighSurrogate_) {
        valid_ = false;
        return false;
    }
    if (pendingCr_) {
        ++crCount_;
        pendingCr_ = false;
        maximumLineColumns_ = std::max(maximumLineColumns_, currentLineColumns_);
        currentLineColumns_ = 0;
        return AppendLineOffset(pendingCrEnd_, lineOffsets, lineCharacterOffsets, pendingCrCharacterEnd_);
    }
    maximumLineColumns_ = std::max(maximumLineColumns_, currentLineColumns_);
    return true;
}

bool LargeFileLineScanner::IsValid() const { return valid_; }
std::uint64_t LargeFileLineScanner::NextOffset() const { return nextOffset_; }
std::uint64_t LargeFileLineScanner::CrlfCount() const { return crlfCount_; }
std::uint64_t LargeFileLineScanner::LfCount() const { return lfCount_; }
std::uint64_t LargeFileLineScanner::CrCount() const { return crCount_; }
std::uint64_t LargeFileLineScanner::CharacterCount() const { return characterCount_; }
std::uint64_t LargeFileLineScanner::MaximumLineColumns() const { return maximumLineColumns_; }

} // namespace classic_notepad
