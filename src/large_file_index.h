#pragma once

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
        std::vector<std::uint64_t>* lineCharacterOffsets);

    LargeFileEncoding encoding_;
    std::uint64_t nextOffset_;
    bool valid_ = true;
    bool pendingCr_ = false;
    std::uint64_t pendingCrEnd_ = 0;
    std::uint64_t crlfCount_ = 0;
    std::uint64_t lfCount_ = 0;
    std::uint64_t crCount_ = 0;
    std::uint64_t characterCount_ = 0;

    int utf8Remaining_ = 0;
    std::uint32_t utf8CodePoint_ = 0;
    std::uint32_t utf8Minimum_ = 0;
    bool utf16HasLowByte_ = false;
    std::uint8_t utf16LowByte_ = 0;
    bool utf16HasHighSurrogate_ = false;
};

} // namespace classic_notepad
