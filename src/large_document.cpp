#include "large_document.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <utility>

namespace classic_notepad {
namespace {

constexpr std::uint64_t kReadChunkCharacters = 64U * 1024U;
constexpr std::size_t kReadChunkBytes = 64U * 1024U;

bool AddWouldOverflow(std::uint64_t left, std::uint64_t right)
{
    return right > (std::numeric_limits<std::uint64_t>::max)() - left;
}

bool IsHighSurrogate(wchar_t character)
{
    return character >= 0xD800 && character <= 0xDBFF;
}

bool IsLowSurrogate(wchar_t character)
{
    return character >= 0xDC00 && character <= 0xDFFF;
}

} // namespace

struct LargeDocument::Piece {
    enum class Kind { Original, Inserted };
    Kind kind = Kind::Original;
    std::uint64_t offset = 0; // File byte offset or inserted-buffer UTF-16 offset.
    std::uint64_t length = 0; // Bytes for Original, UTF-16 code units for Inserted.
    std::uint64_t characters = 0;
    std::size_t bufferIndex = 0;
};

struct LargeDocument::Node {
    explicit Node(Piece value, std::uint64_t priorityValue)
        : piece(std::move(value)), priority(priorityValue) {}

    Piece piece;
    std::uint64_t priority = 0;
    std::uint64_t characters = 0;
    NodePtr left;
    NodePtr right;
};

struct LargeDocument::UndoRecord {
    std::uint64_t start = 0;
    std::uint64_t insertedLength = 0;
    std::vector<Piece> removed;
    std::uint64_t selectionBeforeStart = 0;
    std::uint64_t selectionBeforeEnd = 0;
    std::uint64_t memoryBytes = 0;
};

LargeDocument::LargeDocument(LargeFileEncoding encoding, LargeDocumentByteReader reader)
    : encoding_(encoding)
    , reader_(std::move(reader))
{
}

LargeDocument::~LargeDocument() = default;

std::uint64_t LargeDocument::CharacterLength(const NodePtr& node)
{
    return node == nullptr ? 0 : node->characters;
}

std::uint64_t LargeDocument::PieceLength(const Piece& piece)
{
    return piece.characters;
}

void LargeDocument::Update(Node* node)
{
    if (node == nullptr) {
        return;
    }
    const std::uint64_t left = CharacterLength(node->left);
    const std::uint64_t self = PieceLength(node->piece);
    const std::uint64_t right = CharacterLength(node->right);
    if (AddWouldOverflow(left, self) || AddWouldOverflow(left + self, right)) {
        node->characters = (std::numeric_limits<std::uint64_t>::max)();
    } else {
        node->characters = left + self + right;
    }
}

LargeDocument::NodePtr LargeDocument::Merge(NodePtr left, NodePtr right)
{
    if (left == nullptr) {
        return right;
    }
    if (right == nullptr) {
        return left;
    }
    if (left->priority < right->priority) {
        left->right = Merge(std::move(left->right), std::move(right));
        Update(left.get());
        return left;
    }
    right->left = Merge(std::move(left), std::move(right->left));
    Update(right.get());
    return right;
}

LargeDocument::NodePtr LargeDocument::MakeNode(Piece piece)
{
    randomState_ ^= randomState_ << 7U;
    randomState_ ^= randomState_ >> 9U;
    auto node = std::make_unique<Node>(std::move(piece), randomState_);
    Update(node.get());
    return node;
}

LargeDocument::NodePtr LargeDocument::BuildTree(std::vector<Piece> pieces)
{
    NodePtr tree;
    for (Piece& piece : pieces) {
        if (PieceLength(piece) != 0U) {
            tree = Merge(std::move(tree), MakeNode(std::move(piece)));
        }
    }
    return tree;
}

void LargeDocument::CollectPieces(NodePtr root, std::vector<Piece>& pieces)
{
    if (root == nullptr) {
        return;
    }
    CollectPieces(std::move(root->left), pieces);
    pieces.push_back(std::move(root->piece));
    CollectPieces(std::move(root->right), pieces);
}

void LargeDocument::CountNodes(const NodePtr& node, std::uint64_t& count)
{
    if (node == nullptr || count == (std::numeric_limits<std::uint64_t>::max)()) {
        return;
    }
    CountNodes(node->left, count);
    if (count != (std::numeric_limits<std::uint64_t>::max)()) {
        ++count;
    }
    CountNodes(node->right, count);
}

bool LargeDocument::Initialize(
    const std::vector<LargeDocumentOriginalPiece>& originalPieces,
    std::wstring& errorMessage)
{
    errorMessage.clear();
    Clear();
    std::vector<Piece> pieces;
    pieces.reserve(originalPieces.size());
    std::uint64_t previousEnd = 0;
    for (const LargeDocumentOriginalPiece& descriptor : originalPieces) {
        if (descriptor.byteLength == 0U && descriptor.characterLength == 0U) {
            continue;
        }
        if (descriptor.byteLength == 0U || AddWouldOverflow(descriptor.byteOffset, descriptor.byteLength) ||
            descriptor.byteOffset < previousEnd) {
            errorMessage = L"The large-document source pieces are invalid.";
            Clear();
            return false;
        }
        previousEnd = descriptor.byteOffset + descriptor.byteLength;
        Piece piece;
        piece.kind = Piece::Kind::Original;
        piece.offset = descriptor.byteOffset;
        piece.length = descriptor.byteLength;
        piece.characters = descriptor.characterLength;
        pieces.push_back(piece);
    }
    root_ = BuildTree(std::move(pieces));
    modified_ = false;
    ++generation_;
    return true;
}

void LargeDocument::Clear()
{
    root_.reset();
    insertedBuffers_.clear();
    undo_.clear();
    insertedBytes_ = 0;
    undoBytes_ = 0;
    modified_ = false;
    ++generation_;
}

std::uint64_t LargeDocument::CharacterCount() const { return CharacterLength(root_); }
std::uint64_t LargeDocument::InsertedBufferBytes() const { return insertedBytes_; }
bool LargeDocument::IsModified() const { return modified_; }
std::uint64_t LargeDocument::Generation() const { return generation_; }
bool LargeDocument::CanUndo() const { return !undo_.empty(); }
std::uint64_t LargeDocument::UndoBytes() const { return undoBytes_; }

std::uint64_t LargeDocument::PieceCount() const
{
    std::uint64_t count = 0;
    CountNodes(root_, count);
    return count;
}

bool LargeDocument::ValidateRange(std::uint64_t start, std::uint64_t end, std::wstring& errorMessage) const
{
    const std::uint64_t total = CharacterCount();
    if (start > end || end > total) {
        errorMessage = L"The requested large-document range is outside the document.";
        return false;
    }
    return true;
}

bool LargeDocument::OriginalCharacterToByte(
    const Piece& piece,
    std::uint64_t characterOffset,
    std::uint64_t& byteOffset,
    std::wstring& errorMessage) const
{
    if (piece.kind != Piece::Kind::Original || characterOffset > piece.characters) {
        errorMessage = L"The requested source coordinate is invalid.";
        return false;
    }
    if (characterOffset == 0U) {
        byteOffset = piece.offset;
        return true;
    }
    if (characterOffset == piece.characters) {
        byteOffset = piece.offset + piece.length;
        return true;
    }

    std::uint64_t cursor = piece.offset;
    const std::uint64_t end = piece.offset + piece.length;
    std::uint64_t count = 0;
    std::vector<std::uint8_t> bytes;
    int utf8Remaining = 0;
    std::uint32_t utf8CodePoint = 0;
    std::uint8_t utf16Low = 0;
    bool utf16HasLow = false;
    while (cursor < end) {
        const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(kReadChunkBytes, end - cursor));
        if (!reader_(cursor, requested, bytes) || bytes.size() != requested) {
            errorMessage = L"The mapped source could not be read.";
            return false;
        }
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const std::uint64_t offset = cursor + static_cast<std::uint64_t>(index);
            if (encoding_ == LargeFileEncoding::Utf16Le) {
                if (!utf16HasLow) {
                    utf16Low = bytes[index];
                    utf16HasLow = true;
                    continue;
                }
                utf16HasLow = false;
                if (count == characterOffset) {
                    byteOffset = offset - 1U;
                    return true;
                }
                ++count;
                if (count == characterOffset) {
                    byteOffset = offset + 1U;
                    return true;
                }
                continue;
            }
            const std::uint8_t byte = bytes[index];
            if (utf8Remaining == 0) {
                if (byte <= 0x7FU) {
                    if (count == characterOffset) { byteOffset = offset; return true; }
                    ++count;
                    if (count == characterOffset) { byteOffset = offset + 1U; return true; }
                } else if (byte >= 0xC2U && byte <= 0xDFU) {
                    utf8Remaining = 1; utf8CodePoint = byte & 0x1FU;
                    if (count == characterOffset) { byteOffset = offset; return true; }
                } else if (byte >= 0xE0U && byte <= 0xEFU) {
                    utf8Remaining = 2; utf8CodePoint = byte & 0x0FU;
                    if (count == characterOffset) { byteOffset = offset; return true; }
                } else if (byte >= 0xF0U && byte <= 0xF4U) {
                    utf8Remaining = 3; utf8CodePoint = byte & 0x07U;
                    if (count == characterOffset) { byteOffset = offset; return true; }
                } else {
                    errorMessage = L"The mapped source contains invalid UTF-8.";
                    return false;
                }
            } else {
                if ((byte & 0xC0U) != 0x80U) { errorMessage = L"The mapped source contains invalid UTF-8."; return false; }
                utf8CodePoint = (utf8CodePoint << 6U) | (byte & 0x3FU);
                --utf8Remaining;
                if (utf8Remaining == 0) {
                    count += utf8CodePoint > 0xFFFFU ? 2U : 1U;
                    if (count >= characterOffset) { byteOffset = offset + 1U; return true; }
                }
            }
        }
        cursor += requested;
    }
    errorMessage = L"The mapped source ended at an invalid character boundary.";
    return false;
}

bool LargeDocument::SplitPiece(
    const Piece& piece,
    std::uint64_t position,
    Piece& left,
    Piece& right,
    std::wstring& errorMessage) const
{
    if (position == 0U || position >= piece.characters) {
        errorMessage = L"The requested piece split is invalid.";
        return false;
    }
    left = piece;
    right = piece;
    left.characters = position;
    right.characters = piece.characters - position;
    if (piece.kind == Piece::Kind::Inserted) {
        left.length = position;
        right.offset += position;
        right.length = piece.length - position;
        return true;
    }
    std::uint64_t byte = 0;
    if (!OriginalCharacterToByte(piece, position, byte, errorMessage)) {
        return false;
    }
    left.length = byte - piece.offset;
    right.offset = byte;
    right.length = piece.offset + piece.length - byte;
    return left.length != 0U && right.length != 0U;
}

bool LargeDocument::Split(NodePtr root, std::uint64_t position, NodePtr& left, NodePtr& right, std::wstring& errorMessage)
{
    if (root == nullptr) {
        left.reset(); right.reset(); return position == 0U;
    }
    const std::uint64_t leftLength = CharacterLength(root->left);
    const std::uint64_t ownLength = PieceLength(root->piece);
    if (position < leftLength) {
        NodePtr splitLeft;
        if (!Split(std::move(root->left), position, splitLeft, root->left, errorMessage)) return false;
        Update(root.get());
        left = std::move(splitLeft);
        right = std::move(root);
        return true;
    }
    if (position > leftLength + ownLength) {
        NodePtr splitRight;
        if (!Split(std::move(root->right), position - leftLength - ownLength, root->right, splitRight, errorMessage)) return false;
        Update(root.get());
        left = std::move(root);
        right = std::move(splitRight);
        return true;
    }
    const std::uint64_t inPiece = position - leftLength;
    NodePtr nodeLeft = std::move(root->left);
    NodePtr nodeRight = std::move(root->right);
    if (inPiece == 0U) {
        root->left.reset();
        root->right = std::move(nodeRight);
        Update(root.get());
        left = std::move(nodeLeft);
        right = std::move(root);
        return true;
    }
    if (inPiece == ownLength) {
        root->left = std::move(nodeLeft);
        root->right.reset();
        Update(root.get());
        left = std::move(root);
        right = std::move(nodeRight);
        return true;
    }
    Piece first;
    Piece second;
    if (!SplitPiece(root->piece, inPiece, first, second, errorMessage)) return false;
    left = Merge(std::move(nodeLeft), MakeNode(std::move(first)));
    right = Merge(MakeNode(std::move(second)), std::move(nodeRight));
    return true;
}

bool LargeDocument::Locate(std::uint64_t position, const Node*& node, std::uint64_t& offset) const
{
    if (position >= CharacterCount()) return false;
    const Node* current = root_.get();
    std::uint64_t remaining = position;
    while (current != nullptr) {
        const std::uint64_t left = CharacterLength(current->left);
        const std::uint64_t own = PieceLength(current->piece);
        if (remaining < left) { current = current->left.get(); continue; }
        if (remaining < left + own) { node = current; offset = remaining - left; return true; }
        remaining -= left + own;
        current = current->right.get();
    }
    return false;
}

bool LargeDocument::DecodeOriginal(
    const Piece& piece,
    std::uint64_t startCharacter,
    std::uint64_t maxCharacters,
    std::wstring& text,
    std::wstring& errorMessage) const
{
    std::uint64_t startByte = 0;
    if (!OriginalCharacterToByte(piece, startCharacter, startByte, errorMessage)) return false;
    const std::uint64_t end = piece.offset + piece.length;
    std::uint64_t cursor = startByte;
    std::uint64_t emitted = 0;
    std::vector<std::uint8_t> bytes;
    int remaining = 0;
    std::uint32_t codePoint = 0;
    std::uint8_t low = 0;
    bool hasLow = false;
    while (cursor < end && emitted < maxCharacters) {
        const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(kReadChunkBytes, end - cursor));
        if (!reader_(cursor, requested, bytes) || bytes.size() != requested) { errorMessage = L"The mapped source could not be read."; return false; }
        for (std::size_t index = 0; index < bytes.size() && emitted < maxCharacters; ++index) {
            if (encoding_ == LargeFileEncoding::Utf16Le) {
                if (!hasLow) { low = bytes[index]; hasLow = true; continue; }
                hasLow = false;
                text.push_back(static_cast<wchar_t>(static_cast<std::uint16_t>(low) | (static_cast<std::uint16_t>(bytes[index]) << 8U)));
                ++emitted;
                continue;
            }
            const std::uint8_t byte = bytes[index];
            if (remaining == 0) {
                if (byte <= 0x7FU) { text.push_back(static_cast<wchar_t>(byte)); ++emitted; }
                else if (byte >= 0xC2U && byte <= 0xDFU) { remaining = 1; codePoint = byte & 0x1FU; }
                else if (byte >= 0xE0U && byte <= 0xEFU) { remaining = 2; codePoint = byte & 0x0FU; }
                else if (byte >= 0xF0U && byte <= 0xF4U) { remaining = 3; codePoint = byte & 0x07U; }
                else { errorMessage = L"The mapped source contains invalid UTF-8."; return false; }
            } else {
                if ((byte & 0xC0U) != 0x80U) { errorMessage = L"The mapped source contains invalid UTF-8."; return false; }
                codePoint = (codePoint << 6U) | (byte & 0x3FU);
                if (--remaining == 0) {
                    if (codePoint <= 0xFFFFU) { text.push_back(static_cast<wchar_t>(codePoint)); ++emitted; }
                    else {
                        const std::uint32_t adjusted = codePoint - 0x10000U;
                        if (emitted + 2U > maxCharacters) break;
                        text.push_back(static_cast<wchar_t>(0xD800U + (adjusted >> 10U)));
                        text.push_back(static_cast<wchar_t>(0xDC00U + (adjusted & 0x3FFU)));
                        emitted += 2U;
                    }
                }
            }
        }
        cursor += requested;
    }
    if (remaining != 0 || hasLow) { errorMessage = L"The mapped source ended at an invalid character boundary."; return false; }
    return true;
}

bool LargeDocument::ReadPiece(
    const Piece& piece,
    std::uint64_t start,
    std::uint64_t length,
    std::wstring& text,
    std::wstring& errorMessage) const
{
    if (start > piece.characters || length > piece.characters - start) {
        errorMessage = L"The requested piece range is invalid.";
        return false;
    }
    if (piece.kind == Piece::Kind::Inserted) {
        if (piece.bufferIndex >= insertedBuffers_.size()) { errorMessage = L"The inserted text buffer is unavailable."; return false; }
        const std::wstring& buffer = insertedBuffers_[piece.bufferIndex];
        if (piece.offset > buffer.size() || piece.length > buffer.size() - piece.offset) { errorMessage = L"The inserted text range is invalid."; return false; }
        text.append(buffer, static_cast<std::size_t>(piece.offset + start), static_cast<std::size_t>(length));
        return true;
    }
    return DecodeOriginal(piece, start, length, text, errorMessage);
}

bool LargeDocument::ReadRange(
    std::uint64_t start,
    std::uint64_t maxCharacters,
    std::wstring& text,
    std::wstring& errorMessage) const
{
    text.clear(); errorMessage.clear();
    if (start > CharacterCount()) { errorMessage = L"The requested large-document range is outside the document."; return false; }
    const std::uint64_t remaining = CharacterCount() - start;
    const std::uint64_t wanted = std::min(maxCharacters, remaining);
    if (wanted == 0U) return true;
    if (wanted > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) { errorMessage = L"The requested text range is too large."; return false; }
    text.reserve(static_cast<std::size_t>(wanted));
    std::uint64_t position = start;
    while (position < start + wanted) {
        const Node* node = nullptr; std::uint64_t offset = 0;
        if (!Locate(position, node, offset)) { errorMessage = L"The document piece table is inconsistent."; return false; }
        const std::uint64_t amount = std::min(wanted - (position - start), node->piece.characters - offset);
        if (!ReadPiece(node->piece, offset, amount, text, errorMessage)) return false;
        position += amount;
    }
    return true;
}

bool LargeDocument::CharacterAt(std::uint64_t position, wchar_t& character, std::wstring& errorMessage) const
{
    std::wstring text;
    if (!ReadRange(position, 1U, text, errorMessage) || text.empty()) return false;
    character = text.front(); return true;
}

void LargeDocument::SetUndoByteLimit(std::uint64_t bytes)
{
    undoByteLimit_ = bytes;
    TrimUndo();
}

void LargeDocument::TrimUndo()
{
    while (!undo_.empty() && undoBytes_ > undoByteLimit_) {
        undoBytes_ -= undo_.front().memoryBytes;
        undo_.erase(undo_.begin());
    }
}

bool LargeDocument::Replace(
    std::uint64_t start,
    std::uint64_t end,
    const std::wstring& replacement,
    std::uint64_t selectionBeforeStart,
    std::uint64_t selectionBeforeEnd,
    std::uint64_t& selectionAfterStart,
    std::uint64_t& selectionAfterEnd,
    std::wstring& errorMessage)
{
    errorMessage.clear();
    if (!ValidateRange(start, end, errorMessage)) return false;
    if (replacement.size() > static_cast<std::size_t>((std::numeric_limits<std::uint64_t>::max)()) ||
        AddWouldOverflow(CharacterCount() - (end - start), static_cast<std::uint64_t>(replacement.size()))) {
        errorMessage = L"The edit would exceed the supported 64-bit document size.";
        return false;
    }
    NodePtr before; NodePtr rest;
    if (!Split(std::move(root_), start, before, rest, errorMessage)) { root_ = Merge(std::move(before), std::move(rest)); return false; }
    NodePtr removed; NodePtr after;
    if (!Split(std::move(rest), end - start, removed, after, errorMessage)) { root_ = Merge(std::move(before), std::move(after)); return false; }
    UndoRecord record;
    record.start = start;
    record.insertedLength = static_cast<std::uint64_t>(replacement.size());
    record.selectionBeforeStart = selectionBeforeStart;
    record.selectionBeforeEnd = selectionBeforeEnd;
    CollectPieces(std::move(removed), record.removed);
    for (const Piece& piece : record.removed) {
        if (piece.kind == Piece::Kind::Inserted && !AddWouldOverflow(record.memoryBytes, piece.length * sizeof(wchar_t))) {
            record.memoryBytes += piece.length * sizeof(wchar_t);
        }
    }
    std::vector<Piece> inserted;
    if (!replacement.empty()) {
        if (replacement.size() > (std::numeric_limits<std::uint64_t>::max)() / sizeof(wchar_t) ||
            AddWouldOverflow(insertedBytes_, static_cast<std::uint64_t>(replacement.size() * sizeof(wchar_t)))) {
            root_ = Merge(Merge(std::move(before), BuildTree(std::move(record.removed))), std::move(after));
            errorMessage = L"The inserted-text buffer would exceed the supported size.";
            return false;
        }
        insertedBuffers_.push_back(replacement);
        Piece piece;
        piece.kind = Piece::Kind::Inserted;
        piece.bufferIndex = insertedBuffers_.size() - 1U;
        piece.length = static_cast<std::uint64_t>(replacement.size());
        piece.characters = piece.length;
        inserted.push_back(piece);
        insertedBytes_ += piece.length * sizeof(wchar_t);
        record.memoryBytes += piece.length * sizeof(wchar_t);
    }
    root_ = Merge(Merge(std::move(before), BuildTree(std::move(inserted))), std::move(after));
    undoBytes_ += record.memoryBytes;
    undo_.push_back(std::move(record));
    TrimUndo();
    selectionAfterStart = start + static_cast<std::uint64_t>(replacement.size());
    selectionAfterEnd = selectionAfterStart;
    modified_ = true;
    ++generation_;
    return true;
}

bool LargeDocument::Undo(std::uint64_t& selectionStart, std::uint64_t& selectionEnd, std::wstring& errorMessage)
{
    errorMessage.clear();
    if (undo_.empty()) { errorMessage = L"There is nothing to undo."; return false; }
    UndoRecord record = std::move(undo_.back());
    undo_.pop_back();
    undoBytes_ -= record.memoryBytes;
    NodePtr before; NodePtr discard;
    if (!Split(std::move(root_), record.start, before, discard, errorMessage)) { root_ = Merge(std::move(before), std::move(discard)); return false; }
    NodePtr inserted; NodePtr after;
    if (!Split(std::move(discard), record.insertedLength, inserted, after, errorMessage)) { root_ = Merge(std::move(before), std::move(after)); return false; }
    root_ = Merge(Merge(std::move(before), BuildTree(std::move(record.removed))), std::move(after));
    selectionStart = record.selectionBeforeStart;
    selectionEnd = record.selectionBeforeEnd;
    modified_ = true;
    ++generation_;
    return true;
}

bool LargeDocument::IsWordCharacter(wchar_t character)
{
    return std::iswalnum(static_cast<wint_t>(character)) != 0 || character == L'_';
}

bool LargeDocument::MatchWholeWord(std::uint64_t start, std::uint64_t length, std::wstring& errorMessage) const
{
    wchar_t character = 0;
    if (start > 0U && CharacterAt(start - 1U, character, errorMessage) && IsWordCharacter(character)) return false;
    if (start + length < CharacterCount() && CharacterAt(start + length, character, errorMessage) && IsWordCharacter(character)) return false;
    return errorMessage.empty();
}

bool LargeDocument::Equals(const std::wstring& left, const std::wstring& right, bool matchCase)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (matchCase ? left[index] != right[index] : std::towlower(left[index]) != std::towlower(right[index])) return false;
    }
    return true;
}

bool LargeDocument::Find(
    const std::wstring& needle,
    std::uint64_t start,
    bool matchCase,
    bool wholeWord,
    bool forward,
    LargeDocumentMatch& match,
    std::wstring& errorMessage) const
{
    errorMessage.clear(); match = {};
    if (needle.empty() || start > CharacterCount()) { errorMessage = L"The search text is invalid."; return false; }
    const std::uint64_t total = CharacterCount();
    LargeDocumentMatch last{}; bool found = false;
    const std::uint64_t begin = forward ? start : 0U;
    std::uint64_t position = 0;
    std::wstring carry;
    const bool completed = StreamText([&](const std::wstring& chunk) {
        if (chunk.empty()) return true;
        std::wstring searchable = carry + chunk;
        const std::uint64_t searchableStart = position - static_cast<std::uint64_t>(carry.size());
        for (std::size_t index = 0; index + needle.size() <= searchable.size(); ++index) {
            const std::uint64_t candidate = searchableStart + static_cast<std::uint64_t>(index);
            if (candidate < begin || candidate + needle.size() > total) continue;
            bool matches = true;
            for (std::size_t needleIndex = 0; needleIndex < needle.size(); ++needleIndex) {
                const wchar_t left = searchable[index + needleIndex];
                const wchar_t right = needle[needleIndex];
                if (matchCase ? left != right : std::towlower(left) != std::towlower(right)) {
                    matches = false;
                    break;
                }
            }
            if (!matches) continue;
            if (wholeWord && !MatchWholeWord(candidate, static_cast<std::uint64_t>(needle.size()), errorMessage)) return false;
            if (forward) { match = { candidate, static_cast<std::uint64_t>(needle.size()) }; found = true; return false; }
            if (candidate + needle.size() <= start) { last = { candidate, static_cast<std::uint64_t>(needle.size()) }; found = true; }
        }
        const std::size_t carryLength = needle.size() > 1U ? std::min(needle.size() - 1U, searchable.size()) : 0U;
        carry.assign(searchable, searchable.size() - carryLength, carryLength);
        position += static_cast<std::uint64_t>(chunk.size());
        return true;
    }, {}, errorMessage);
    if (!completed && found) return true;
    if (!completed && !errorMessage.empty()) return false;
    if (found) { match = last; return true; }
    return false;
}

std::uint64_t LargeDocument::ReplaceAll(
    const std::wstring& needle,
    const std::wstring& replacement,
    bool matchCase,
    bool wholeWord,
    bool& cancelled,
    const std::function<bool()>& shouldCancel,
    std::wstring& errorMessage)
{
    cancelled = false; errorMessage.clear();
    if (needle.empty()) { errorMessage = L"The replacement text is invalid."; return 0; }
    std::uint64_t count = 0; std::uint64_t position = 0;
    while (position <= CharacterCount()) {
        if (shouldCancel && shouldCancel()) { cancelled = true; break; }
        LargeDocumentMatch match;
        if (!Find(needle, position, matchCase, wholeWord, true, match, errorMessage)) {
            if (!errorMessage.empty()) return count;
            break;
        }
        std::uint64_t afterStart = 0, afterEnd = 0;
        if (!Replace(match.start, match.start + match.length, replacement, match.start, match.start + match.length,
                afterStart, afterEnd, errorMessage)) return count;
        ++count;
        position = afterEnd;
    }
    return count;
}

bool LargeDocument::StreamText(
    const std::function<bool(const std::wstring&)>& visitor,
    const std::function<bool()>& shouldCancel,
    std::wstring& errorMessage) const
{
    errorMessage.clear();
    bool succeeded = true;
    const auto streamPiece = [this, &visitor, &shouldCancel, &errorMessage, &succeeded](const Piece& piece) {
        if (!succeeded) return;
        if (shouldCancel && shouldCancel()) {
            errorMessage = L"The operation was cancelled.";
            succeeded = false;
            return;
        }
        if (piece.kind == Piece::Kind::Inserted) {
            if (piece.bufferIndex >= insertedBuffers_.size()) {
                errorMessage = L"The inserted text buffer is unavailable.";
                succeeded = false;
                return;
            }
            const std::wstring& buffer = insertedBuffers_[piece.bufferIndex];
            for (std::uint64_t offset = 0; offset < piece.characters && succeeded;) {
                const std::uint64_t amount = std::min<std::uint64_t>(kReadChunkCharacters, piece.characters - offset);
                const std::wstring chunk = buffer.substr(
                    static_cast<std::size_t>(piece.offset + offset), static_cast<std::size_t>(amount));
                succeeded = visitor(chunk);
                offset += amount;
            }
            return;
        }
        // Source pieces are deliberately bounded to approximately 8 MiB by the
        // Windows index builder. Decode one piece, then release it before the
        // next one: this keeps save memory independent of document size and
        // avoids repeated source rescans for every output chunk.
        std::wstring decoded;
        if (!DecodeOriginal(piece, 0, piece.characters, decoded, errorMessage)) {
            succeeded = false;
            return;
        }
        for (std::size_t offset = 0; offset < decoded.size() && succeeded;) {
            const std::size_t amount = std::min<std::size_t>(static_cast<std::size_t>(kReadChunkCharacters), decoded.size() - offset);
            succeeded = visitor(decoded.substr(offset, amount));
            offset += amount;
        }
    };
    const auto visit = [&streamPiece, &succeeded](const auto& self, const Node* node) -> void {
        if (node == nullptr || !succeeded) return;
        self(self, node->left.get());
        streamPiece(node->piece);
        self(self, node->right.get());
    };
    visit(visit, root_.get());
    return succeeded;
}

} // namespace classic_notepad
