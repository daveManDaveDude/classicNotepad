#include "large_document.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <mutex>
#include <utility>

namespace classic_notepad {
namespace {

constexpr std::uint64_t kReadChunkCharacters = 64U * 1024U;
constexpr std::size_t kReadChunkBytes = 64U * 1024U;
constexpr std::uint64_t kCheckpointCharacters = 64U * 1024U;
constexpr std::size_t kInsertedChunkCharacters = 64U * 1024U;

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

struct LargeDocument::TextSummary {
    std::uint64_t characters = 0;
    std::uint64_t lineBreaks = 0;
    bool empty = true;
    wchar_t first = 0;
    wchar_t last = 0;
};

struct LargeDocument::Piece {
    enum class Kind { Original, Inserted };
    Kind kind = Kind::Original;
    std::uint64_t offset = 0; // File byte offset or inserted-buffer UTF-16 offset.
    std::uint64_t length = 0; // Bytes for Original, UTF-16 code units for Inserted.
    std::uint64_t characters = 0;
    std::size_t bufferIndex = 0;
    std::uint64_t sourceCharacterOffset = 0;
    struct SourceData {
        struct Checkpoint {
            std::uint64_t character = 0;
            std::uint64_t byte = 0;
        };
        std::uint64_t byteOffset = 0;
        std::uint64_t byteLength = 0;
        std::uint64_t characters = 0;
        mutable std::mutex mutex;
        mutable std::vector<Checkpoint> checkpoints;
    };
    std::shared_ptr<SourceData> source;
    TextSummary summary;
};

namespace {

LargeDocument::TextSummary CombineSummary(
    const LargeDocument::TextSummary& left,
    const LargeDocument::TextSummary& right)
{
    if (left.empty) return right;
    if (right.empty) return left;
    LargeDocument::TextSummary combined;
    combined.empty = false;
    combined.first = left.first;
    combined.last = right.last;
    combined.characters = left.characters + right.characters;
    combined.lineBreaks = left.lineBreaks + right.lineBreaks;
    if (left.last == L'\r' && right.first == L'\n') {
        --combined.lineBreaks;
    }
    return combined;
}

LargeDocument::TextSummary SummarizeText(const std::wstring& text)
{
    LargeDocument::TextSummary summary;
    summary.characters = static_cast<std::uint64_t>(text.size());
    summary.empty = text.empty();
    if (text.empty()) return summary;
    summary.first = text.front();
    summary.last = text.back();
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            ++summary.lineBreaks;
            if (index + 1U < text.size() && text[index + 1U] == L'\n') ++index;
        } else if (text[index] == L'\n') {
            ++summary.lineBreaks;
        }
    }
    return summary;
}

} // namespace

struct LargeDocument::Node {
    explicit Node(Piece value, std::uint64_t priorityValue)
        : piece(std::move(value)), priority(priorityValue) {}

    Piece piece;
    std::uint64_t priority = 0;
    std::uint64_t characters = 0;
    TextSummary summary;
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
    std::uint64_t stateBefore = 0;
    std::uint64_t stateAfter = 0;
    std::vector<UndoRecord> compound;
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

LargeDocument::TextSummary LargeDocument::SummaryOf(const NodePtr& node)
{
    return node == nullptr ? TextSummary{} : node->summary;
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
    node->summary = CombineSummary(CombineSummary(SummaryOf(node->left), node->piece.summary), SummaryOf(node->right));
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

LargeDocument::NodePtr LargeDocument::CloneNode(const NodePtr& node)
{
    if (node == nullptr) return nullptr;
    auto copy = std::make_unique<Node>(node->piece, node->priority);
    copy->left = CloneNode(node->left);
    copy->right = CloneNode(node->right);
    copy->characters = node->characters;
    copy->summary = node->summary;
    return copy;
}

std::unique_ptr<LargeDocument> LargeDocument::Clone() const
{
    auto copy = std::make_unique<LargeDocument>(encoding_, reader_);
    copy->root_ = CloneNode(root_);
    copy->insertedBuffers_ = insertedBuffers_;
    copy->insertedBytes_ = insertedBytes_;
    copy->undo_ = undo_;
    copy->undoBytes_ = undoBytes_;
    copy->undoByteLimit_ = undoByteLimit_;
    copy->generation_ = generation_;
    copy->stateId_ = stateId_;
    copy->savedStateId_ = savedStateId_;
    copy->nextStateId_ = nextStateId_;
    copy->randomState_ = randomState_;
    copy->sourceHighWater_ = sourceHighWater_;
    return copy;
}

bool LargeDocument::Initialize(
    const std::vector<LargeDocumentOriginalPiece>& originalPieces,
    std::wstring& errorMessage)
{
    errorMessage.clear();
    Clear();
    if (!AppendOriginalPieces(originalPieces, errorMessage)) {
        Clear();
        return false;
    }
    stateId_ = nextStateId_++;
    savedStateId_ = stateId_;
    ++generation_;
    return true;
}

bool LargeDocument::AppendOriginalPieces(
    const std::vector<LargeDocumentOriginalPiece>& originalPieces,
    std::wstring& errorMessage)
{
    errorMessage.clear();
    std::vector<Piece> pieces;
    pieces.reserve(originalPieces.size());
    std::uint64_t previousEnd = sourceHighWater_;
    for (const LargeDocumentOriginalPiece& descriptor : originalPieces) {
        if (descriptor.byteLength == 0U && descriptor.characterLength == 0U) {
            continue;
        }
        if (descriptor.byteLength == 0U || AddWouldOverflow(descriptor.byteOffset, descriptor.byteLength) ||
            descriptor.byteOffset < previousEnd) {
            errorMessage = L"The large-document source pieces are invalid.";
            return false;
        }
        previousEnd = descriptor.byteOffset + descriptor.byteLength;
        Piece piece;
        piece.kind = Piece::Kind::Original;
        piece.offset = descriptor.byteOffset;
        piece.length = descriptor.byteLength;
        piece.characters = descriptor.characterLength;
        piece.source = std::make_shared<Piece::SourceData>();
        piece.source->byteOffset = descriptor.byteOffset;
        piece.source->byteLength = descriptor.byteLength;
        piece.source->characters = descriptor.characterLength;
        piece.source->checkpoints.push_back({0, descriptor.byteOffset});
        if (descriptor.summaryKnown) {
            piece.summary.characters = descriptor.characterLength;
            piece.summary.lineBreaks = descriptor.logicalLineBreaks;
            piece.summary.empty = descriptor.characterLength == 0U;
            piece.summary.first = descriptor.firstCharacter;
            piece.summary.last = descriptor.lastCharacter;
        } else {
            std::wstring decoded;
            if (!DecodeOriginal(piece, 0, piece.characters, decoded, errorMessage)) {
                return false;
            }
            piece.summary = SummarizeText(decoded);
        }
        pieces.push_back(piece);
    }
    root_ = Merge(std::move(root_), BuildTree(std::move(pieces)));
    sourceHighWater_ = previousEnd;
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
    sourceHighWater_ = 0;
    stateId_ = nextStateId_++;
    savedStateId_ = stateId_;
    ++generation_;
}

std::uint64_t LargeDocument::CharacterCount() const { return CharacterLength(root_); }
std::uint64_t LargeDocument::InsertedBufferBytes() const { return insertedBytes_; }
bool LargeDocument::IsModified() const { return stateId_ != savedStateId_; }
std::uint64_t LargeDocument::Generation() const { return generation_; }
std::uint64_t LargeDocument::SavedState() const { return savedStateId_; }
void LargeDocument::MarkSaved() { savedStateId_ = stateId_; }
std::uint64_t LargeDocument::LineCount() const
{
    return root_ == nullptr ? 1U : root_->summary.lineBreaks + 1U;
}
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

bool LargeDocument::SummarizePiece(
    const Piece& piece, std::uint64_t start, std::uint64_t length,
    TextSummary& summary, std::wstring& errorMessage) const
{
    std::wstring text;
    if (!ReadPiece(piece, start, length, text, errorMessage)) return false;
    summary = SummarizeText(text);
    return true;
}

bool LargeDocument::SummarizePrefix(
    std::uint64_t length, TextSummary& summary, std::wstring& errorMessage) const
{
    summary = {};
    if (length > CharacterCount()) {
        errorMessage = L"The requested prefix is outside the document.";
        return false;
    }
    const Node* node = root_.get();
    std::uint64_t remaining = length;
    while (node != nullptr && remaining != 0U) {
        const std::uint64_t leftLength = CharacterLength(node->left);
        if (remaining <= leftLength) {
            node = node->left.get();
            continue;
        }
        summary = CombineSummary(summary, SummaryOf(node->left));
        remaining -= leftLength;
        const std::uint64_t amount = std::min(remaining, node->piece.characters);
        if (amount == node->piece.characters) {
            summary = CombineSummary(summary, node->piece.summary);
        } else {
            TextSummary partial;
            if (!SummarizePiece(node->piece, 0, amount, partial, errorMessage)) return false;
            summary = CombineSummary(summary, partial);
        }
        remaining -= amount;
        if (amount != node->piece.characters) break;
        node = node->right.get();
    }
    return remaining == 0U;
}

bool LargeDocument::IsCharacterBoundary(std::uint64_t position, std::wstring& errorMessage) const
{
    errorMessage.clear();
    if (position > CharacterCount()) {
        errorMessage = L"The caret is outside the document.";
        return false;
    }
    if (position == 0U || position == CharacterCount()) return true;
    wchar_t before = 0;
    wchar_t after = 0;
    if (!CharacterAt(position - 1U, before, errorMessage) || !CharacterAt(position, after, errorMessage)) return false;
    if (IsHighSurrogate(before) && IsLowSurrogate(after)) {
        errorMessage = L"A caret or edit boundary cannot split a surrogate pair.";
        return false;
    }
    return true;
}

bool LargeDocument::PositionToLineColumn(
    std::uint64_t position, LargeDocumentLineColumn& coordinate, std::wstring& errorMessage) const
{
    if (!IsCharacterBoundary(position, errorMessage)) return false;
    TextSummary prefix;
    if (!SummarizePrefix(position, prefix, errorMessage)) return false;
    coordinate.line = prefix.lineBreaks;
    std::uint64_t lineStart = 0;
    if (!LineStart(coordinate.line, lineStart, errorMessage)) return false;
    coordinate.column = position - lineStart;
    return true;
}

bool LargeDocument::LineStart(
    std::uint64_t zeroBasedLine, std::uint64_t& position, std::wstring& errorMessage) const
{
    errorMessage.clear();
    if (zeroBasedLine >= LineCount()) {
        errorMessage = L"The requested line is outside the document.";
        return false;
    }
    if (zeroBasedLine == 0U) { position = 0; return true; }
    std::uint64_t remainingBreaks = zeroBasedLine;
    std::uint64_t absolute = 0;
    bool previousWasCr = false;
    bool found = false;
    const auto skipSummary = [&](const TextSummary& summary) {
        if (summary.empty) return false;
        std::uint64_t effective = summary.lineBreaks;
        if (previousWasCr && summary.first == L'\n' && effective != 0U) --effective;
        if (remainingBreaks <= effective) return false;
        remainingBreaks -= effective;
        absolute += summary.characters;
        previousWasCr = summary.last == L'\r';
        return true;
    };
    const auto visitPiece = [&](const Piece& piece) {
        if (skipSummary(piece.summary)) return;
        constexpr std::uint64_t chunkSize = 64U * 1024U;
        for (std::uint64_t offset = 0; offset < piece.characters && !found;) {
            const std::uint64_t amount = std::min(chunkSize, piece.characters - offset);
            std::wstring text;
            if (!ReadPiece(piece, offset, amount, text, errorMessage)) return;
            for (std::size_t index = 0; index < text.size(); ++index) {
                const wchar_t character = text[index];
                if (character == L'\n' && previousWasCr) {
                    previousWasCr = false;
                    continue;
                }
                if (character == L'\r' || character == L'\n') {
                    if (--remainingBreaks == 0U) {
                        position = absolute + static_cast<std::uint64_t>(index) + 1U;
                        found = true;
                        return;
                    }
                    previousWasCr = character == L'\r';
                } else {
                    previousWasCr = false;
                }
            }
            absolute += amount;
            offset += amount;
        }
    };
    const auto visit = [&](const auto& self, const Node* node) -> void {
        if (node == nullptr || found || !errorMessage.empty()) return;
        if (skipSummary(node->summary)) return;
        self(self, node->left.get());
        if (found || !errorMessage.empty()) return;
        visitPiece(node->piece);
        if (found || !errorMessage.empty()) return;
        self(self, node->right.get());
    };
    visit(visit, root_.get());
    if (!found || !errorMessage.empty()) {
        if (errorMessage.empty()) errorMessage = L"The requested line could not be located.";
        return false;
    }
    wchar_t before = 0;
    wchar_t after = 0;
    if (position < CharacterCount() && CharacterAt(position - 1U, before, errorMessage) &&
        CharacterAt(position, after, errorMessage) && before == L'\r' && after == L'\n') ++position;
    return errorMessage.empty();
}

bool LargeDocument::LineRange(
    std::uint64_t zeroBasedLine, std::uint64_t& start, std::uint64_t& contentEnd,
    std::uint64_t& nextLineStart, std::wstring& errorMessage) const
{
    if (!LineStart(zeroBasedLine, start, errorMessage)) return false;
    if (zeroBasedLine + 1U < LineCount()) {
        if (!LineStart(zeroBasedLine + 1U, nextLineStart, errorMessage)) return false;
    } else {
        nextLineStart = CharacterCount();
    }
    contentEnd = nextLineStart;
    wchar_t character = 0;
    if (contentEnd > start && CharacterAt(contentEnd - 1U, character, errorMessage) && character == L'\n') --contentEnd;
    if (contentEnd > start && CharacterAt(contentEnd - 1U, character, errorMessage) && character == L'\r') --contentEnd;
    return errorMessage.empty();
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
    if (piece.source == nullptr || AddWouldOverflow(piece.sourceCharacterOffset, characterOffset)) {
        errorMessage = L"The original piece has no source checkpoint data.";
        return false;
    }
    const std::uint64_t target = piece.sourceCharacterOffset + characterOffset;
    if (target > piece.source->characters) {
        errorMessage = L"The requested source coordinate is invalid.";
        return false;
    }
    if (encoding_ == LargeFileEncoding::Utf16Le) {
        if (target > ((std::numeric_limits<std::uint64_t>::max)() - piece.source->byteOffset) / 2U) {
            errorMessage = L"The source coordinate exceeds the 64-bit byte range.";
            return false;
        }
        byteOffset = piece.source->byteOffset + target * 2U;
        return true;
    }

    std::lock_guard<std::mutex> checkpointLock(piece.source->mutex);
    Piece::SourceData::Checkpoint checkpoint = piece.source->checkpoints.front();
    for (const auto& candidate : piece.source->checkpoints) {
        if (candidate.character > target) break;
        checkpoint = candidate;
    }
    std::uint64_t cursor = checkpoint.byte;
    const std::uint64_t end = piece.source->byteOffset + piece.source->byteLength;
    std::uint64_t count = checkpoint.character;
    if (count == target) {
        byteOffset = cursor;
        return true;
    }
    std::vector<std::uint8_t> bytes;
    int utf8Remaining = 0;
    std::uint32_t utf8CodePoint = 0;
    while (cursor < end) {
        const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(kReadChunkBytes, end - cursor));
        if (!reader_(cursor, requested, bytes) || bytes.size() != requested) {
            errorMessage = L"The mapped source could not be read.";
            return false;
        }
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const std::uint64_t offset = cursor + static_cast<std::uint64_t>(index);
            const std::uint8_t byte = bytes[index];
            if (utf8Remaining == 0) {
                if (byte <= 0x7FU) {
                    ++count;
                    if (count == target) { byteOffset = offset + 1U; return true; }
                } else if (byte >= 0xC2U && byte <= 0xDFU) {
                    utf8Remaining = 1; utf8CodePoint = byte & 0x1FU;
                } else if (byte >= 0xE0U && byte <= 0xEFU) {
                    utf8Remaining = 2; utf8CodePoint = byte & 0x0FU;
                } else if (byte >= 0xF0U && byte <= 0xF4U) {
                    utf8Remaining = 3; utf8CodePoint = byte & 0x07U;
                } else {
                    errorMessage = L"The mapped source contains invalid UTF-8.";
                    return false;
                }
            } else {
                if ((byte & 0xC0U) != 0x80U) { errorMessage = L"The mapped source contains invalid UTF-8."; return false; }
                utf8CodePoint = (utf8CodePoint << 6U) | (byte & 0x3FU);
                --utf8Remaining;
                if (utf8Remaining == 0) {
                    const std::uint64_t units = utf8CodePoint > 0xFFFFU ? 2U : 1U;
                    if (count < target && target < count + units) {
                        errorMessage = L"A caret or edit boundary cannot split a surrogate pair.";
                        return false;
                    }
                    count += units;
                    if (piece.source->checkpoints.empty() ||
                        count - piece.source->checkpoints.back().character >= kCheckpointCharacters) {
                        piece.source->checkpoints.push_back({count, offset + 1U});
                    }
                    if (count == target) { byteOffset = offset + 1U; return true; }
                }
            }
        }
        cursor += requested;
    }
    if (count == target) {
        byteOffset = end;
        return true;
    }
    errorMessage = L"The mapped source ended before the requested character boundary.";
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
    } else {
        std::uint64_t byte = 0;
        if (!OriginalCharacterToByte(piece, position, byte, errorMessage)) {
            return false;
        }
        left.length = byte - piece.offset;
        right.offset = byte;
        right.length = piece.offset + piece.length - byte;
        right.sourceCharacterOffset += position;
        if (left.length == 0U || right.length == 0U) return false;
    }
    if (!SummarizePiece(left, 0, left.characters, left.summary, errorMessage) ||
        !SummarizePiece(right, 0, right.characters, right.summary, errorMessage)) return false;
    return true;
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
    if (!IsCharacterBoundary(start, errorMessage) || !IsCharacterBoundary(end, errorMessage)) return false;
    for (std::size_t index = 0; index < replacement.size(); ++index) {
        if (IsHighSurrogate(replacement[index])) {
            if (index + 1U >= replacement.size() || !IsLowSurrogate(replacement[index + 1U])) {
                errorMessage = L"Inserted text contains an unmatched Unicode surrogate.";
                return false;
            }
            ++index;
        } else if (IsLowSurrogate(replacement[index])) {
            errorMessage = L"Inserted text contains an unmatched Unicode surrogate.";
            return false;
        }
    }
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
    record.stateBefore = stateId_;
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
        for (std::size_t offset = 0; offset < replacement.size();) {
            std::size_t amount = std::min(kInsertedChunkCharacters, replacement.size() - offset);
            if (offset + amount < replacement.size() && IsHighSurrogate(replacement[offset + amount - 1U])) --amount;
            insertedBuffers_.push_back(replacement.substr(offset, amount));
            Piece piece;
            piece.kind = Piece::Kind::Inserted;
            piece.bufferIndex = insertedBuffers_.size() - 1U;
            piece.length = static_cast<std::uint64_t>(amount);
            piece.characters = piece.length;
            piece.summary = SummarizeText(insertedBuffers_.back());
            inserted.push_back(std::move(piece));
            insertedBytes_ += static_cast<std::uint64_t>(amount) * sizeof(wchar_t);
            record.memoryBytes += static_cast<std::uint64_t>(amount) * sizeof(wchar_t);
            offset += amount;
        }
    }
    root_ = Merge(Merge(std::move(before), BuildTree(std::move(inserted))), std::move(after));
    stateId_ = nextStateId_++;
    record.stateAfter = stateId_;
    undoBytes_ += record.memoryBytes;
    const bool coalesceTyping = !undo_.empty() && record.removed.empty() && !replacement.empty() &&
        selectionBeforeStart == selectionBeforeEnd && selectionBeforeStart == start &&
        undo_.back().compound.empty() && undo_.back().removed.empty() && undo_.back().insertedLength != 0U &&
        undo_.back().start + undo_.back().insertedLength == start &&
        undo_.back().stateAfter == record.stateBefore && record.stateBefore != savedStateId_;
    if (coalesceTyping) {
        undo_.back().insertedLength += record.insertedLength;
        undo_.back().memoryBytes += record.memoryBytes;
        undo_.back().stateAfter = record.stateAfter;
    } else {
        undo_.push_back(std::move(record));
    }
    TrimUndo();
    selectionAfterStart = start + static_cast<std::uint64_t>(replacement.size());
    selectionAfterEnd = selectionAfterStart;
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
    const auto apply = [this, &errorMessage](UndoRecord& item) {
        NodePtr before; NodePtr discard;
        if (!Split(std::move(root_), item.start, before, discard, errorMessage)) {
            root_ = Merge(std::move(before), std::move(discard));
            return false;
        }
        NodePtr inserted; NodePtr after;
        if (!Split(std::move(discard), item.insertedLength, inserted, after, errorMessage)) {
            root_ = Merge(std::move(before), std::move(after));
            return false;
        }
        root_ = Merge(Merge(std::move(before), BuildTree(std::move(item.removed))), std::move(after));
        return true;
    };
    if (!record.compound.empty()) {
        for (auto iterator = record.compound.rbegin(); iterator != record.compound.rend(); ++iterator) {
            if (!apply(*iterator)) return false;
        }
    } else if (!apply(record)) {
        return false;
    }
    selectionStart = record.selectionBeforeStart;
    selectionEnd = record.selectionBeforeEnd;
    stateId_ = record.stateBefore;
    ++generation_;
    return true;
}

bool LargeDocument::IsWordCharacter(wchar_t character)
{
    return std::iswalnum(static_cast<wint_t>(character)) != 0 || character == L'_';
}

bool LargeDocument::MatchWholeWord(std::uint64_t start, std::uint64_t length, std::wstring& errorMessage) const
{
    if (start > CharacterCount() || length > CharacterCount() - start) {
        errorMessage = L"The matched range is outside the large document.";
        return false;
    }
    const std::uint64_t end = start + length;
    wchar_t character = 0;
    if (start > 0U && CharacterAt(start - 1U, character, errorMessage) && IsWordCharacter(character)) return false;
    if (end < CharacterCount() && CharacterAt(end, character, errorMessage) && IsWordCharacter(character)) return false;
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
    std::wstring& errorMessage,
    const std::function<bool()>& shouldCancel,
    const std::function<void(std::uint64_t)>& onProgress) const
{
    errorMessage.clear(); match = {};
    if (needle.empty() || start > CharacterCount()) { errorMessage = L"The search text is invalid."; return false; }
    const std::uint64_t total = CharacterCount();
    LargeDocumentMatch last{};
    bool found = false;
    const std::uint64_t begin = forward ? start : 0U;
    const auto equalUnit = [matchCase](wchar_t left, wchar_t right) {
        return matchCase ? left == right : std::towlower(left) == std::towlower(right);
    };
    std::vector<std::size_t> prefix(needle.size(), 0U);
    for (std::size_t index = 1U, matchedPrefix = 0U; index < needle.size(); ++index) {
        while (matchedPrefix != 0U && !equalUnit(needle[index], needle[matchedPrefix])) {
            matchedPrefix = prefix[matchedPrefix - 1U];
        }
        if (equalUnit(needle[index], needle[matchedPrefix])) ++matchedPrefix;
        prefix[index] = matchedPrefix;
    }
    std::uint64_t streamed = 0;
    std::size_t matched = 0;
    const bool completed = StreamText([&](const std::wstring& chunk) {
        for (wchar_t character : chunk) {
            while (matched != 0U && !equalUnit(character, needle[matched])) matched = prefix[matched - 1U];
            if (equalUnit(character, needle[matched])) ++matched;
            ++streamed;
            if (matched != needle.size()) continue;
            const std::uint64_t candidate = streamed - static_cast<std::uint64_t>(needle.size());
            const bool inDirection = forward ? candidate >= begin : streamed <= start;
            if (inDirection && candidate + needle.size() <= total &&
                (!wholeWord || MatchWholeWord(candidate, static_cast<std::uint64_t>(needle.size()), errorMessage))) {
                if (forward) {
                    match = {candidate, static_cast<std::uint64_t>(needle.size())};
                    found = true;
                    return false;
                }
                last = {candidate, static_cast<std::uint64_t>(needle.size())};
                found = true;
            }
            if (!errorMessage.empty()) return false;
            matched = prefix[matched - 1U];
        }
        if (onProgress) onProgress(streamed);
        return !(shouldCancel && shouldCancel());
    }, shouldCancel, errorMessage);
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
    std::wstring& errorMessage,
    const std::function<void(std::uint64_t)>& onProgress)
{
    cancelled = false; errorMessage.clear();
    if (needle.empty()) { errorMessage = L"The replacement text is invalid."; return 0; }
    std::vector<std::size_t> prefix(needle.size(), 0U);
    const auto equalUnit = [matchCase](wchar_t left, wchar_t right) {
        return matchCase ? left == right : std::towlower(left) == std::towlower(right);
    };
    for (std::size_t index = 1U, matched = 0U; index < needle.size(); ++index) {
        while (matched != 0U && !equalUnit(needle[index], needle[matched])) matched = prefix[matched - 1U];
        if (equalUnit(needle[index], needle[matched])) ++matched;
        prefix[index] = matched;
    }
    std::vector<LargeDocumentMatch> matches;
    std::uint64_t streamed = 0;
    std::uint64_t lastEnd = 0;
    std::size_t matched = 0;
    if (!StreamText([&](const std::wstring& chunk) {
        for (wchar_t character : chunk) {
            while (matched != 0U && !equalUnit(character, needle[matched])) matched = prefix[matched - 1U];
            if (equalUnit(character, needle[matched])) ++matched;
            ++streamed;
            if (matched == needle.size()) {
                const std::uint64_t start = streamed - static_cast<std::uint64_t>(needle.size());
                if (start >= lastEnd && (!wholeWord || MatchWholeWord(start,
                        static_cast<std::uint64_t>(needle.size()), errorMessage))) {
                    matches.push_back({start, static_cast<std::uint64_t>(needle.size())});
                    lastEnd = streamed;
                }
                matched = prefix[matched - 1U];
            }
            if (!errorMessage.empty()) return false;
        }
        if (onProgress) onProgress(streamed);
        return !(shouldCancel && shouldCancel());
    }, shouldCancel, errorMessage)) {
        if (shouldCancel && shouldCancel()) {
            cancelled = true;
            errorMessage.clear();
        }
        return 0;
    }
    if (matches.empty()) return 0;
    const std::size_t undoStart = undo_.size();
    const std::uint64_t stateBefore = stateId_;
    for (auto iterator = matches.rbegin(); iterator != matches.rend(); ++iterator) {
        std::uint64_t afterStart = 0, afterEnd = 0;
        if (!Replace(iterator->start, iterator->start + iterator->length, replacement,
                iterator->start, iterator->start + iterator->length, afterStart, afterEnd, errorMessage)) {
            return static_cast<std::uint64_t>(matches.rend() - iterator - 1);
        }
    }
    const std::uint64_t count = static_cast<std::uint64_t>(matches.size());
    if (undo_.size() > undoStart + 1U) {
        UndoRecord group;
        group.selectionBeforeStart = 0;
        group.selectionBeforeEnd = 0;
        group.stateBefore = stateBefore;
        group.stateAfter = stateId_;
        group.compound.reserve(undo_.size() - undoStart);
        for (std::size_t index = undoStart; index < undo_.size(); ++index) {
            group.memoryBytes += undo_[index].memoryBytes;
            group.compound.push_back(std::move(undo_[index]));
        }
        undo_.erase(undo_.begin() + static_cast<std::ptrdiff_t>(undoStart), undo_.end());
        undo_.push_back(std::move(group));
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
        // Source pieces are deliberately bounded to approximately 64 KiB by the
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
