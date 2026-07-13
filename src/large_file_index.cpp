#include "large_file_index.h"

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

LargeFileLineScanner::LargeFileLineScanner(LargeFileEncoding encoding, std::uint64_t dataOffset)
    : encoding_(encoding)
    , nextOffset_(dataOffset)
{
    valid_ = encoding != LargeFileEncoding::Unsupported;
}

bool LargeFileLineScanner::AppendLineOffset(
    std::uint64_t offset,
    std::vector<std::uint64_t>& lineOffsets,
    std::vector<std::uint64_t>* lineCharacterOffsets)
{
    if (!lineOffsets.empty() && offset <= lineOffsets.back()) {
        valid_ = false;
        return false;
    }
    lineOffsets.push_back(offset);
    if (lineCharacterOffsets != nullptr) {
        if (!lineCharacterOffsets->empty() && characterCount_ < lineCharacterOffsets->back()) {
            valid_ = false;
            return false;
        }
        lineCharacterOffsets->push_back(characterCount_);
    }
    return true;
}

bool LargeFileLineScanner::FeedCharacter(
    std::uint32_t character,
    std::uint64_t characterEnd,
    std::vector<std::uint64_t>& lineOffsets,
    std::vector<std::uint64_t>* lineCharacterOffsets)
{
    if (pendingCr_) {
        if (character == static_cast<std::uint32_t>('\n')) {
            ++crlfCount_;
            pendingCr_ = false;
            return AppendLineOffset(characterEnd, lineOffsets, lineCharacterOffsets);
        }
        ++crCount_;
        if (!AppendLineOffset(pendingCrEnd_, lineOffsets, lineCharacterOffsets)) {
            return false;
        }
        pendingCr_ = false;
    }

    if (character == static_cast<std::uint32_t>('\r')) {
        pendingCr_ = true;
        pendingCrEnd_ = characterEnd;
    } else if (character == static_cast<std::uint32_t>('\n')) {
        ++lfCount_;
        return AppendLineOffset(characterEnd, lineOffsets, lineCharacterOffsets);
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
            // A CR/LF byte can only be a complete UTF-8 character.
            if (bytes[index] == static_cast<std::uint8_t>('\r') || bytes[index] == static_cast<std::uint8_t>('\n')) {
                if (!FeedCharacter(bytes[index], offset + 1U, lineOffsets, lineCharacterOffsets)) {
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
        return AppendLineOffset(pendingCrEnd_, lineOffsets, lineCharacterOffsets);
    }
    return true;
}

bool LargeFileLineScanner::IsValid() const { return valid_; }
std::uint64_t LargeFileLineScanner::NextOffset() const { return nextOffset_; }
std::uint64_t LargeFileLineScanner::CrlfCount() const { return crlfCount_; }
std::uint64_t LargeFileLineScanner::LfCount() const { return lfCount_; }
std::uint64_t LargeFileLineScanner::CrCount() const { return crCount_; }
std::uint64_t LargeFileLineScanner::CharacterCount() const { return characterCount_; }

} // namespace classic_notepad
