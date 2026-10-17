#include "large_document.h"
#include "inserted_text_validation.h"
#include "text_search_semantics.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace classic_notepad {
namespace {

constexpr std::uint64_t kReadChunkCharacters = 64U * 1024U;
constexpr std::size_t kReadChunkBytes = 64U * 1024U;
constexpr std::uint64_t kCheckpointCharacters = 4U * 1024U;
// Source checkpoint storage is reserved once and capped. This keeps the
// mutable cache owner's retained allocation stable after an undo record has
// charged for it, including when another snapshot reads the shared source.
constexpr std::size_t kMaximumSourceCheckpoints = 17U;
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

struct LargeDocument::InsertedStorageLedger {
    std::atomic<std::uint64_t> liveBytes{0};

    bool Add(std::uint64_t bytes) noexcept
    {
        std::uint64_t current = liveBytes.load(std::memory_order_relaxed);
        for (;;) {
            if (AddWouldOverflow(current, bytes)) return false;
            if (liveBytes.compare_exchange_weak(current, current + bytes,
                    std::memory_order_relaxed, std::memory_order_relaxed)) return true;
        }
    }

    void Remove(std::uint64_t bytes) noexcept
    {
        liveBytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
};

struct LargeDocument::InsertedBuffer {
    InsertedBuffer(std::wstring value, std::shared_ptr<InsertedStorageLedger> storageLedger)
        : text(std::move(value))
        , ledger(std::move(storageLedger))
    {
        if (text.capacity() > (std::numeric_limits<std::uint64_t>::max)() / sizeof(wchar_t)) {
            throw std::length_error("inserted chunk capacity exceeds accounting range");
        }
        storageBytes = static_cast<std::uint64_t>(text.capacity()) * sizeof(wchar_t);
        if (!ledger->Add(storageBytes)) {
            throw std::length_error("inserted storage accounting exceeds supported range");
        }
        registered = true;
    }

    ~InsertedBuffer()
    {
        if (registered) ledger->Remove(storageBytes);
    }

    const std::wstring text;
    std::shared_ptr<InsertedStorageLedger> ledger;
    std::uint64_t storageBytes = 0;
    bool registered = false;
};

struct LargeDocument::Piece {
    enum class Kind { Original, Inserted };
    Kind kind = Kind::Original;
    std::uint64_t offset = 0; // File byte offset or inserted-buffer UTF-16 offset.
    std::uint64_t length = 0; // Bytes for Original, UTF-16 code units for Inserted.
    std::uint64_t characters = 0;
    std::shared_ptr<const InsertedBuffer> inserted;
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
    , insertedStorageLedger_(std::make_shared<InsertedStorageLedger>())
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
    node->summary = CombineLargeTextSummaries(
        CombineLargeTextSummaries(SummaryOf(node->left), node->piece.summary), SummaryOf(node->right));
}

LargeDocument::NodePtr LargeDocument::Merge(NodePtr left, NodePtr right, std::uint64_t& randomState)
{
    if (left == nullptr) {
        return right;
    }
    if (right == nullptr) {
        return left;
    }
    if (left->priority < right->priority) {
        NodePtr mergedRight = Merge(left->right, std::move(right), randomState);
        return CloneWithChildren(left, left->left, std::move(mergedRight));
    }
    NodePtr mergedLeft = Merge(std::move(left), right->left, randomState);
    return CloneWithChildren(right, std::move(mergedLeft), right->right);
}

LargeDocument::NodePtr LargeDocument::MakeNode(Piece piece, std::uint64_t& randomState)
{
    randomState ^= randomState << 7U;
    randomState ^= randomState >> 9U;
    auto node = std::make_shared<Node>(std::move(piece), randomState);
    Update(node.get());
    return node;
}

LargeDocument::NodePtr LargeDocument::CloneWithChildren(const NodePtr& node, NodePtr left, NodePtr right)
{
    auto copy = std::make_shared<Node>(node->piece, node->priority);
    copy->left = std::move(left);
    copy->right = std::move(right);
    Update(copy.get());
    return copy;
}

LargeDocument::NodePtr LargeDocument::BuildTree(std::vector<Piece> pieces, std::uint64_t& randomState)
{
    NodePtr tree;
    for (Piece& piece : pieces) {
        if (PieceLength(piece) != 0U) {
            tree = Merge(std::move(tree), MakeNode(std::move(piece), randomState), randomState);
        }
    }
    return tree;
}

void LargeDocument::CollectPieces(const NodePtr& root, std::vector<Piece>& pieces)
{
    if (root == nullptr) {
        return;
    }
    CollectPieces(root->left, pieces);
    pieces.push_back(root->piece);
    CollectPieces(root->right, pieces);
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

std::unique_ptr<LargeDocument> LargeDocument::Clone() const
{
    auto copy = std::make_unique<LargeDocument>(encoding_, reader_);
    // Nodes are immutable after publication and edit operations path-copy the
    // changed paths, so the snapshot can share the root directly.
    copy->root_ = root_;
    copy->insertedStorageLedger_ = insertedStorageLedger_;
    // Each history entry is an immutable shared record; compounds and their
    // removed-piece descriptors stay shared while each snapshot edits history.
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
        piece.source->checkpoints.reserve(kMaximumSourceCheckpoints);
        piece.source->checkpoints.push_back({0, 0});
        if (descriptor.summaryKnown) {
            piece.summary.characters = descriptor.characterLength;
            piece.summary.lineBreaks = descriptor.logicalLineBreaks;
            piece.summary.empty = descriptor.characterLength == 0U;
            piece.summary.first = descriptor.firstCharacter;
            piece.summary.last = descriptor.lastCharacter;
            if (descriptor.displayedCharacterCountKnown) {
                piece.summary.displayedCharacters = descriptor.displayedCharacterCount;
                piece.summary.displayedCharactersKnown = true;
            }
            if (descriptor.widthSummaryKnown) {
                piece.summary.width = descriptor.widthSummary;
            }
            if (!descriptor.widthSummaryKnown) {
                TextSummary streamed;
                if (!StreamOriginal(piece, [&streamed](const std::wstring& chunk) {
                        streamed = CombineLargeTextSummaries(streamed, SummarizeLargeText(chunk));
                        return true;
                    }, {}, errorMessage)) {
                    return false;
                }
                piece.summary.width = streamed.width;
                if (!descriptor.displayedCharacterCountKnown) {
                    piece.summary.displayedCharacters = streamed.displayedCharacters;
                    piece.summary.displayedCharactersKnown = streamed.displayedCharactersKnown;
                }
            }
        } else {
            TextSummary summary;
            if (!StreamOriginal(piece, [&summary](const std::wstring& chunk) {
                    summary = CombineLargeTextSummaries(summary, SummarizeLargeText(chunk));
                    return true;
                }, {}, errorMessage)) {
                return false;
            }
            piece.summary = summary;
        }
        pieces.push_back(piece);
    }
    std::uint64_t stagedRandomState = randomState_;
    NodePtr appended = BuildTree(std::move(pieces), stagedRandomState);
    root_ = Merge(root_, std::move(appended), stagedRandomState);
    randomState_ = stagedRandomState;
    sourceHighWater_ = previousEnd;
    ++generation_;
    return true;
}

void LargeDocument::Clear()
{
    root_.reset();
    undo_.clear();
    undoBytes_ = 0;
    sourceHighWater_ = 0;
    stateId_ = nextStateId_++;
    savedStateId_ = stateId_;
    ++generation_;
}

std::uint64_t LargeDocument::CharacterCount() const { return CharacterLength(root_); }
std::uint64_t LargeDocument::DisplayedCharacterCount() const
{
    return root_ == nullptr || !root_->summary.displayedCharactersKnown
        ? 0U : root_->summary.displayedCharacters;
}
bool LargeDocument::HasExactDisplayedCharacterCount() const
{
    return root_ == nullptr || root_->summary.displayedCharactersKnown;
}
std::uint64_t LargeDocument::InsertedBufferBytes() const
{
    return insertedStorageLedger_->liveBytes.load(std::memory_order_relaxed);
}
bool LargeDocument::IsModified() const { return stateId_ != savedStateId_; }
std::uint64_t LargeDocument::Generation() const { return generation_; }
std::uint64_t LargeDocument::SavedState() const { return savedStateId_; }
void LargeDocument::MarkSaved() { savedStateId_ = stateId_; }
std::uint64_t LargeDocument::LineCount() const
{
    return root_ == nullptr ? 1U : root_->summary.lineBreaks + 1U;
}
std::uint64_t LargeDocument::MaximumLineColumns() const
{
    return root_ == nullptr ? 0U : root_->summary.width.maximumLineColumns;
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
    summary = SummarizeLargeText(text);
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
        summary = CombineLargeTextSummaries(summary, SummaryOf(node->left));
        remaining -= leftLength;
        const std::uint64_t amount = std::min(remaining, node->piece.characters);
        if (amount == node->piece.characters) {
            summary = CombineLargeTextSummaries(summary, node->piece.summary);
        } else {
            TextSummary partial;
            if (!SummarizePiece(node->piece, 0, amount, partial, errorMessage)) return false;
            summary = CombineLargeTextSummaries(summary, partial);
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
    coordinate.column = position >= lineStart ? position - lineStart : 0U;
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
    std::wstring& errorMessage,
    std::uint64_t* codeUnitsIntoScalar,
    const std::function<bool()>& shouldCancel) const
{
    if (shouldCancel && shouldCancel()) {
        errorMessage = L"The operation was cancelled.";
        return false;
    }
    if (codeUnitsIntoScalar != nullptr) *codeUnitsIntoScalar = 0U;
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
    if (piece.source->checkpoints.empty()) {
        errorMessage = L"The original piece has no source checkpoint data.";
        return false;
    }
    const auto checkpointAfterTarget = std::upper_bound(
        piece.source->checkpoints.begin(), piece.source->checkpoints.end(), target,
        [](std::uint64_t character, const Piece::SourceData::Checkpoint& candidate) {
            return character < candidate.character;
        });
    if (checkpointAfterTarget == piece.source->checkpoints.begin()) {
        errorMessage = L"The requested source coordinate has no preceding checkpoint.";
        return false;
    }
    const Piece::SourceData::Checkpoint checkpoint = *std::prev(checkpointAfterTarget);
    std::uint64_t cursor = piece.source->byteOffset + checkpoint.byte;
    const std::uint64_t end = piece.source->byteOffset + piece.source->byteLength;
    std::uint64_t count = checkpoint.character;
    if (count == target) {
        byteOffset = cursor;
        return true;
    }
    std::vector<std::uint8_t> bytes;
    int utf8Remaining = 0;
    std::uint32_t utf8CodePoint = 0;
    std::uint32_t utf8Minimum = 0;
    std::uint64_t utf8Start = cursor;
    const auto rememberCheckpoint = [&](std::uint64_t character, std::uint64_t absoluteByte) {
        const Piece::SourceData::Checkpoint& last = piece.source->checkpoints.back();
        if (character > last.character && character - last.character >= kCheckpointCharacters) {
            if (piece.source->checkpoints.size() == kMaximumSourceCheckpoints) {
                // Keep the source origin plus the most recent bounded window.
                // Erasing does not allocate, and the sorted checkpoint order
                // remains valid for upper_bound below.
                piece.source->checkpoints.erase(piece.source->checkpoints.begin() + 1);
            }
            piece.source->checkpoints.push_back({character, absoluteByte - piece.source->byteOffset});
        }
    };
    while (cursor < end) {
        if (shouldCancel && shouldCancel()) {
            errorMessage = L"The operation was cancelled.";
            return false;
        }
        const std::uint64_t remainingCharacters = target - count;
        const std::uint64_t maximumTranslationBytes = remainingCharacters >
                ((std::numeric_limits<std::uint64_t>::max)() - 1U) / 3U
            ? (std::numeric_limits<std::uint64_t>::max)()
            : remainingCharacters * 3U + 1U;
        const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(
            std::min<std::uint64_t>(kReadChunkBytes, end - cursor), maximumTranslationBytes));
        const bool readSucceeded = reader_(cursor, requested, bytes);
        if (shouldCancel && shouldCancel()) {
            errorMessage = L"The operation was cancelled.";
            return false;
        }
        if (!readSucceeded || bytes.size() != requested) {
            errorMessage = L"The mapped source could not be read.";
            return false;
        }
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const std::uint64_t offset = cursor + static_cast<std::uint64_t>(index);
            const std::uint8_t byte = bytes[index];
            if (utf8Remaining == 0) {
                if (byte <= 0x7FU) {
                    ++count;
                    rememberCheckpoint(count, offset + 1U);
                    if (count == target) { byteOffset = offset + 1U; return true; }
                } else if (byte >= 0xC2U && byte <= 0xDFU) {
                    utf8Start = offset; utf8Remaining = 1; utf8CodePoint = byte & 0x1FU; utf8Minimum = 0x80U;
                } else if (byte >= 0xE0U && byte <= 0xEFU) {
                    utf8Start = offset; utf8Remaining = 2; utf8CodePoint = byte & 0x0FU; utf8Minimum = 0x800U;
                } else if (byte >= 0xF0U && byte <= 0xF4U) {
                    utf8Start = offset; utf8Remaining = 3; utf8CodePoint = byte & 0x07U; utf8Minimum = 0x10000U;
                } else {
                    errorMessage = L"The mapped source contains invalid UTF-8.";
                    return false;
                }
            } else {
                if ((byte & 0xC0U) != 0x80U) { errorMessage = L"The mapped source contains invalid UTF-8."; return false; }
                utf8CodePoint = (utf8CodePoint << 6U) | (byte & 0x3FU);
                --utf8Remaining;
                if (utf8Remaining == 0) {
                    if (utf8CodePoint < utf8Minimum || utf8CodePoint > 0x10FFFFU ||
                        (utf8CodePoint >= 0xD800U && utf8CodePoint <= 0xDFFFU)) {
                        errorMessage = L"The mapped source contains invalid UTF-8.";
                        return false;
                    }
                    const std::uint64_t units = utf8CodePoint > 0xFFFFU ? 2U : 1U;
                    if (count < target && target < count + units) {
                        if (codeUnitsIntoScalar == nullptr) {
                            errorMessage = L"A caret or edit boundary cannot split a surrogate pair.";
                            return false;
                        }
                        byteOffset = utf8Start;
                        *codeUnitsIntoScalar = target - count;
                    }
                    count += units;
                    rememberCheckpoint(count, offset + 1U);
                    if (codeUnitsIntoScalar != nullptr && *codeUnitsIntoScalar != 0U) {
                        return true;
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
        if (piece.inserted == nullptr || piece.offset > piece.inserted->text.size() ||
            piece.length > piece.inserted->text.size() - piece.offset) {
            errorMessage = L"The inserted text range is invalid.";
            return false;
        }
        left.length = position;
        right.offset += position;
        right.length = piece.length - position;
    } else {
        std::uint64_t byte = 0;
        std::uint64_t codeUnitsIntoScalar = 0U;
        if (!OriginalCharacterToByte(piece, position, byte, errorMessage, &codeUnitsIntoScalar)) {
            return false;
        }
        if (codeUnitsIntoScalar != 0U) {
            errorMessage = L"A caret or edit boundary cannot split a surrogate pair.";
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

bool LargeDocument::Split(
    const NodePtr& root,
    std::uint64_t position,
    NodePtr& left,
    NodePtr& right,
    std::uint64_t& randomState,
    std::wstring& errorMessage) const
{
    if (root == nullptr) {
        if (position != 0U) return false;
        left.reset();
        right.reset();
        return true;
    }
    const std::uint64_t leftLength = CharacterLength(root->left);
    const std::uint64_t ownLength = PieceLength(root->piece);
    if (position < leftLength) {
        NodePtr splitLeft;
        NodePtr splitRight;
        if (!Split(root->left, position, splitLeft, splitRight, randomState, errorMessage)) return false;
        NodePtr candidateRight = CloneWithChildren(root, std::move(splitRight), root->right);
        left = std::move(splitLeft);
        right = std::move(candidateRight);
        return true;
    }
    if (position > leftLength + ownLength) {
        NodePtr splitRight;
        NodePtr splitLeft;
        if (!Split(root->right, position - leftLength - ownLength, splitLeft, splitRight,
                randomState, errorMessage)) return false;
        NodePtr candidateLeft = CloneWithChildren(root, root->left, std::move(splitLeft));
        left = std::move(candidateLeft);
        right = std::move(splitRight);
        return true;
    }
    const std::uint64_t inPiece = position - leftLength;
    if (inPiece == 0U) {
        NodePtr candidateRight = CloneWithChildren(root, nullptr, root->right);
        left = root->left;
        right = std::move(candidateRight);
        return true;
    }
    if (inPiece == ownLength) {
        NodePtr candidateLeft = CloneWithChildren(root, root->left, nullptr);
        left = std::move(candidateLeft);
        right = root->right;
        return true;
    }
    Piece first;
    Piece second;
    if (!SplitPiece(root->piece, inPiece, first, second, errorMessage)) return false;
    NodePtr candidateLeft = Merge(root->left, MakeNode(std::move(first), randomState), randomState);
    NodePtr candidateRight = Merge(MakeNode(std::move(second), randomState), root->right, randomState);
    left = std::move(candidateLeft);
    right = std::move(candidateRight);
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
    std::uint64_t skippedCodeUnits = 0U;
    if (!OriginalCharacterToByte(piece, startCharacter, startByte, errorMessage, &skippedCodeUnits)) return false;
    const std::uint64_t end = piece.offset + piece.length;
    std::uint64_t cursor = startByte;
    std::uint64_t emitted = 0;
    std::vector<std::uint8_t> bytes;
    int remaining = 0;
    std::uint32_t codePoint = 0;
    std::uint32_t minimumCodePoint = 0;
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
                else if (byte >= 0xC2U && byte <= 0xDFU) { remaining = 1; codePoint = byte & 0x1FU; minimumCodePoint = 0x80U; }
                else if (byte >= 0xE0U && byte <= 0xEFU) { remaining = 2; codePoint = byte & 0x0FU; minimumCodePoint = 0x800U; }
                else if (byte >= 0xF0U && byte <= 0xF4U) { remaining = 3; codePoint = byte & 0x07U; minimumCodePoint = 0x10000U; }
                else { errorMessage = L"The mapped source contains invalid UTF-8."; return false; }
            } else {
                if ((byte & 0xC0U) != 0x80U) { errorMessage = L"The mapped source contains invalid UTF-8."; return false; }
                codePoint = (codePoint << 6U) | (byte & 0x3FU);
                if (--remaining == 0) {
                    if (codePoint < minimumCodePoint || codePoint > 0x10FFFFU ||
                        (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
                        errorMessage = L"The mapped source contains invalid UTF-8.";
                        return false;
                    }
                    if (codePoint <= 0xFFFFU) {
                        if (skippedCodeUnits != 0U) {
                            errorMessage = L"The mapped source has an invalid UTF-16 coordinate.";
                            return false;
                        }
                        text.push_back(static_cast<wchar_t>(codePoint));
                        ++emitted;
                    } else {
                        const std::uint32_t adjusted = codePoint - 0x10000U;
                        const wchar_t high = static_cast<wchar_t>(0xD800U + (adjusted >> 10U));
                        const wchar_t lowSurrogate = static_cast<wchar_t>(0xDC00U + (adjusted & 0x3FFU));
                        if (skippedCodeUnits == 0U && emitted < maxCharacters) {
                            text.push_back(high);
                            ++emitted;
                        }
                        if (skippedCodeUnits <= 1U && emitted < maxCharacters) {
                            text.push_back(lowSurrogate);
                            ++emitted;
                        }
                        skippedCodeUnits = 0U;
                    }
                }
            }
        }
        cursor += requested;
    }
    if (remaining != 0 || hasLow) { errorMessage = L"The mapped source ended at an invalid character boundary."; return false; }
    return true;
}

bool LargeDocument::StreamOriginal(
    const Piece& piece,
    const std::function<bool(const std::wstring&)>& visitor,
    const std::function<bool()>& shouldCancel,
    std::wstring& errorMessage) const
{
    if (shouldCancel && shouldCancel()) {
        errorMessage = L"The operation was cancelled.";
        return false;
    }
    if (piece.kind != Piece::Kind::Original || piece.source == nullptr ||
        AddWouldOverflow(piece.offset, piece.length)) {
        errorMessage = L"The original document piece is invalid.";
        return false;
    }
    if (piece.characters == 0U) return true;

    std::uint64_t cursor = 0;
    if (!OriginalCharacterToByte(piece, 0U, cursor, errorMessage, nullptr, shouldCancel)) return false;
    const std::uint64_t end = piece.offset + piece.length;
    if (cursor < piece.offset || cursor > end) {
        errorMessage = L"The requested source coordinate is outside its piece.";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    std::wstring decoded;
    decoded.reserve(static_cast<std::size_t>(kReadChunkCharacters) + 1U);
    std::uint64_t emitted = 0;
    bool hasPendingHighSurrogate = false;
    wchar_t pendingHighSurrogate = 0;

    const auto flush = [&]() {
        if (decoded.empty()) return true;
        if (shouldCancel && shouldCancel()) {
            errorMessage = L"The operation was cancelled.";
            return false;
        }
        const bool keepGoing = visitor(decoded);
        decoded.clear();
        if (!keepGoing) return false;
        if (shouldCancel && shouldCancel()) {
            errorMessage = L"The operation was cancelled.";
            return false;
        }
        return true;
    };
    const auto appendUnits = [&](wchar_t first, wchar_t second, bool isPair) {
        const std::size_t unitCount = isPair ? 2U : 1U;
        if (!decoded.empty() && decoded.size() + unitCount > static_cast<std::size_t>(kReadChunkCharacters) + 1U) {
            if (!flush()) return false;
        }
        decoded.push_back(first);
        if (isPair) decoded.push_back(second);
        if (decoded.size() >= static_cast<std::size_t>(kReadChunkCharacters)) return flush();
        return true;
    };
    const auto appendCodeUnit = [&](wchar_t character) {
        if (hasPendingHighSurrogate) {
            if (IsLowSurrogate(character)) {
                hasPendingHighSurrogate = false;
                return appendUnits(pendingHighSurrogate, character, true);
            }
            const wchar_t unmatchedHigh = pendingHighSurrogate;
            hasPendingHighSurrogate = false;
            if (!appendUnits(unmatchedHigh, 0, false)) return false;
        }
        if (IsHighSurrogate(character)) {
            pendingHighSurrogate = character;
            hasPendingHighSurrogate = true;
            return true;
        }
        return appendUnits(character, 0, false);
    };

    int utf8Remaining = 0;
    std::uint32_t utf8CodePoint = 0;
    std::uint32_t utf8Minimum = 0;
    std::uint8_t utf16LowByte = 0;
    bool hasUtf16LowByte = false;
    while (cursor < end && emitted < piece.characters) {
        if (shouldCancel && shouldCancel()) {
            errorMessage = L"The operation was cancelled.";
            return false;
        }
        const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(
            kReadChunkBytes, end - cursor));
        const bool readSucceeded = reader_(cursor, requested, bytes);
        if (shouldCancel && shouldCancel()) {
            errorMessage = L"The operation was cancelled.";
            return false;
        }
        if (!readSucceeded || bytes.size() != requested) {
            errorMessage = L"The mapped source could not be read.";
            return false;
        }

        for (std::size_t index = 0; index < bytes.size() && emitted < piece.characters; ++index) {
            if (encoding_ == LargeFileEncoding::Utf16Le) {
                if (!hasUtf16LowByte) {
                    utf16LowByte = bytes[index];
                    hasUtf16LowByte = true;
                    continue;
                }
                hasUtf16LowByte = false;
                const auto unit = static_cast<std::uint16_t>(utf16LowByte) |
                    (static_cast<std::uint16_t>(bytes[index]) << 8U);
                if (!appendCodeUnit(static_cast<wchar_t>(unit))) return false;
                ++emitted;
                continue;
            }

            const std::uint8_t byte = bytes[index];
            if (utf8Remaining == 0) {
                if (byte <= 0x7FU) {
                    if (!appendCodeUnit(static_cast<wchar_t>(byte))) return false;
                    ++emitted;
                } else if (byte >= 0xC2U && byte <= 0xDFU) {
                    utf8Remaining = 1;
                    utf8CodePoint = byte & 0x1FU;
                    utf8Minimum = 0x80U;
                } else if (byte >= 0xE0U && byte <= 0xEFU) {
                    utf8Remaining = 2;
                    utf8CodePoint = byte & 0x0FU;
                    utf8Minimum = 0x800U;
                } else if (byte >= 0xF0U && byte <= 0xF4U) {
                    utf8Remaining = 3;
                    utf8CodePoint = byte & 0x07U;
                    utf8Minimum = 0x10000U;
                } else {
                    errorMessage = L"The mapped source contains invalid UTF-8.";
                    return false;
                }
                continue;
            }

            if ((byte & 0xC0U) != 0x80U) {
                errorMessage = L"The mapped source contains invalid UTF-8.";
                return false;
            }
            utf8CodePoint = (utf8CodePoint << 6U) | (byte & 0x3FU);
            if (--utf8Remaining != 0) continue;
            if (utf8CodePoint < utf8Minimum || utf8CodePoint > 0x10FFFFU ||
                (utf8CodePoint >= 0xD800U && utf8CodePoint <= 0xDFFFU)) {
                errorMessage = L"The mapped source contains invalid UTF-8.";
                return false;
            }
            if (utf8CodePoint <= 0xFFFFU) {
                if (!appendCodeUnit(static_cast<wchar_t>(utf8CodePoint))) return false;
                ++emitted;
            } else {
                if (piece.characters - emitted < 2U) {
                    errorMessage = L"The mapped source ended at an invalid character boundary.";
                    return false;
                }
                const std::uint32_t adjusted = utf8CodePoint - 0x10000U;
                const wchar_t high = static_cast<wchar_t>(0xD800U + (adjusted >> 10U));
                const wchar_t low = static_cast<wchar_t>(0xDC00U + (adjusted & 0x3FFU));
                if (!appendCodeUnit(high) || !appendCodeUnit(low)) return false;
                emitted += 2U;
            }
        }
        cursor += requested;
    }

    if (utf8Remaining != 0 || hasUtf16LowByte || emitted != piece.characters) {
        errorMessage = L"The mapped source ended at an invalid character boundary.";
        return false;
    }
    if (hasPendingHighSurrogate) {
        if (!appendUnits(pendingHighSurrogate, 0, false)) return false;
        hasPendingHighSurrogate = false;
    }
    return flush();
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
        if (piece.inserted == nullptr) { errorMessage = L"The inserted text buffer is unavailable."; return false; }
        const std::wstring& buffer = piece.inserted->text;
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
        undoBytes_ -= undo_.front()->memoryBytes;
        undo_.pop_front();
    }
}

bool LargeDocument::PrepareReplace(
    const NodePtr& root,
    std::uint64_t start,
    std::uint64_t end,
    const std::wstring& replacement,
    std::uint64_t& randomState,
    NodePtr& candidateRoot,
    UndoRecord& record,
    std::wstring& errorMessage) const
{
    const std::uint64_t total = CharacterLength(root);
    if (start > end || end > total) {
        errorMessage = L"The requested large-document range is outside the document.";
        return false;
    }
    if (replacement.size() > static_cast<std::size_t>((std::numeric_limits<std::uint64_t>::max)()) ||
        AddWouldOverflow(total - (end - start), static_cast<std::uint64_t>(replacement.size()))) {
        errorMessage = L"The edit would exceed the supported 64-bit document size.";
        return false;
    }

    NodePtr before;
    NodePtr rest;
    if (!Split(root, start, before, rest, randomState, errorMessage)) return false;
    NodePtr removed;
    NodePtr after;
    if (!Split(rest, end - start, removed, after, randomState, errorMessage)) return false;

    record.start = start;
    record.insertedLength = static_cast<std::uint64_t>(replacement.size());
    CollectPieces(removed, record.removed);
    const auto addCharge = [&record, &errorMessage](std::uint64_t bytes) {
        if (AddWouldOverflow(record.memoryBytes, bytes)) {
            errorMessage = L"The undo record would exceed the supported size.";
            return false;
        }
        record.memoryBytes += bytes;
        return true;
    };
    const auto addArrayCharge = [&addCharge, &errorMessage](std::size_t count, std::size_t elementSize) {
        if (elementSize != 0U && count > (std::numeric_limits<std::uint64_t>::max)() / elementSize) {
            errorMessage = L"The undo record would exceed the supported size.";
            return false;
        }
        return addCharge(static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(elementSize));
    };

    // UndoBytes is a conservative history-retention estimate: charge the
    // action/descriptor storage, a modeled node per retained or inserted
    // piece, and each immutable inserted allocation once. It is not RSS.
    if (!addCharge(sizeof(UndoRecord)) || !addArrayCharge(record.removed.capacity(), sizeof(Piece)) ||
        !addArrayCharge(record.removed.size(), sizeof(Node))) return false;
    std::vector<const InsertedBuffer*> retainedBuffers;
    retainedBuffers.reserve(record.removed.size());
    for (const Piece& piece : record.removed) {
        if (piece.kind == Piece::Kind::Inserted && piece.inserted != nullptr) {
            retainedBuffers.push_back(piece.inserted.get());
        }
    }
    std::sort(retainedBuffers.begin(), retainedBuffers.end(), std::less<const InsertedBuffer*>{});
    retainedBuffers.erase(std::unique(retainedBuffers.begin(), retainedBuffers.end()), retainedBuffers.end());
    for (const InsertedBuffer* buffer : retainedBuffers) {
        if (!addCharge(buffer->storageBytes)) return false;
    }

    std::vector<const Piece::SourceData*> retainedSources;
    retainedSources.reserve(record.removed.size());
    for (const Piece& piece : record.removed) {
        if (piece.kind == Piece::Kind::Original && piece.source != nullptr) {
            retainedSources.push_back(piece.source.get());
        }
    }
    std::sort(retainedSources.begin(), retainedSources.end(), std::less<const Piece::SourceData*>{});
    retainedSources.erase(std::unique(retainedSources.begin(), retainedSources.end()), retainedSources.end());
    for (const Piece::SourceData* source : retainedSources) {
        // SourceData contains its mutex and checkpoint vector descriptor; its
        // retained vector allocation is charged separately. The cache is
        // capped at the reserved capacity, so later snapshot reads cannot
        // grow this allocation beyond the recorded charge.
        std::size_t checkpointCapacity = 0U;
        {
            std::lock_guard<std::mutex> checkpointLock(source->mutex);
            checkpointCapacity = source->checkpoints.capacity();
        }
        if (!addCharge(sizeof(Piece::SourceData)) ||
            !addArrayCharge(checkpointCapacity, sizeof(Piece::SourceData::Checkpoint))) return false;
    }

    std::vector<Piece> inserted;
    for (std::size_t offset = 0; offset < replacement.size();) {
        std::size_t amount = std::min(kInsertedChunkCharacters, replacement.size() - offset);
        if (offset + amount < replacement.size() && IsHighSurrogate(replacement[offset + amount - 1U])) --amount;
        if (amount == 0U || amount > (std::numeric_limits<std::uint64_t>::max)() / sizeof(wchar_t)) {
            errorMessage = L"The inserted-text buffer would exceed the supported size.";
            return false;
        }
        std::shared_ptr<const InsertedBuffer> buffer = std::make_shared<InsertedBuffer>(
            replacement.substr(offset, amount), insertedStorageLedger_);
        if (!addCharge(buffer->storageBytes)) return false;
        Piece piece;
        piece.kind = Piece::Kind::Inserted;
        piece.inserted = std::move(buffer);
        piece.length = static_cast<std::uint64_t>(amount);
        piece.characters = piece.length;
        piece.summary = SummarizeLargeText(piece.inserted->text);
        inserted.push_back(std::move(piece));
        offset += amount;
    }

    if (!addArrayCharge(inserted.capacity(), sizeof(Piece)) ||
        !addArrayCharge(inserted.size(), sizeof(Node))) return false;

    NodePtr insertedTree = BuildTree(std::move(inserted), randomState);
    NodePtr candidate = Merge(Merge(std::move(before), std::move(insertedTree), randomState),
        std::move(after), randomState);
    candidateRoot = std::move(candidate);
    return true;
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
    try {
        if (!ValidateRange(start, end, errorMessage)) return false;
        if (!IsCharacterBoundary(start, errorMessage) || !IsCharacterBoundary(end, errorMessage)) return false;
        if (!IsValidInsertedText(replacement)) {
            errorMessage = L"Inserted text contains an unmatched Unicode surrogate.";
            return false;
        }

        std::uint64_t stagedRandomState = randomState_;
        NodePtr candidateRoot;
        UndoRecord record;
        if (!PrepareReplace(root_, start, end, replacement,
                stagedRandomState, candidateRoot, record, errorMessage)) return false;
        record.selectionBeforeStart = selectionBeforeStart;
        record.selectionBeforeEnd = selectionBeforeEnd;
        record.stateBefore = stateId_;
        record.stateAfter = nextStateId_;

        const bool coalesceTyping = !undo_.empty() && record.removed.empty() && !replacement.empty() &&
            selectionBeforeStart == selectionBeforeEnd && selectionBeforeStart == start &&
            undo_.back()->compound.empty() && undo_.back()->removed.empty() && undo_.back()->insertedLength != 0U &&
            undo_.back()->start + undo_.back()->insertedLength == start &&
            undo_.back()->stateAfter == record.stateBefore && record.stateBefore != savedStateId_;
        std::uint64_t previousCoalescedCharge = 0U;
        std::uint64_t committedCharge = record.memoryBytes;
        std::shared_ptr<const UndoRecord> committedRecord;
        if (coalesceTyping) {
            const std::shared_ptr<const UndoRecord>& previous = undo_.back();
            if (AddWouldOverflow(previous->memoryBytes, record.memoryBytes) ||
                AddWouldOverflow(previous->insertedLength, record.insertedLength)) {
                errorMessage = L"The undo record would exceed the supported size.";
                return false;
            }
            auto combined = std::make_shared<UndoRecord>(*previous);
            combined->insertedLength += record.insertedLength;
            combined->memoryBytes += record.memoryBytes;
            combined->stateAfter = record.stateAfter;
            previousCoalescedCharge = previous->memoryBytes;
            committedCharge = combined->memoryBytes;
            committedRecord = std::move(combined);
        } else {
            committedRecord = std::make_shared<UndoRecord>(std::move(record));
        }

        // Allocate the deque node before publishing the edit. No fallible
        // work remains after the tree root is committed.
        if (!coalesceTyping) undo_.push_back(committedRecord);

        root_ = std::move(candidateRoot);
        randomState_ = stagedRandomState;
        stateId_ = nextStateId_++;
        if (coalesceTyping) {
            undo_.back() = std::move(committedRecord);
            undoBytes_ -= previousCoalescedCharge;
        }
        if (committedCharge > undoByteLimit_) {
            undo_.clear();
            undoBytes_ = 0U;
        } else {
            const std::uint64_t maximumPreviousCharge = undoByteLimit_ - committedCharge;
            while (!undo_.empty() && undoBytes_ > maximumPreviousCharge) {
                undoBytes_ -= undo_.front()->memoryBytes;
                undo_.pop_front();
            }
            undoBytes_ += committedCharge;
        }
        selectionAfterStart = start + static_cast<std::uint64_t>(replacement.size());
        selectionAfterEnd = selectionAfterStart;
        ++generation_;
        return true;
    } catch (const std::bad_alloc&) {
        errorMessage = L"There is not enough memory to complete the edit.";
        return false;
    } catch (const std::length_error&) {
        errorMessage = L"The edit exceeds the supported in-memory size.";
        return false;
    }
}

bool LargeDocument::Undo(std::uint64_t& selectionStart, std::uint64_t& selectionEnd, std::wstring& errorMessage)
{
    errorMessage.clear();
    if (undo_.empty()) { errorMessage = L"There is nothing to undo."; return false; }

    try {
        const std::shared_ptr<const UndoRecord> recordOwner = undo_.back();
        const UndoRecord& record = *recordOwner;
        NodePtr candidateRoot = root_;
        std::uint64_t stagedRandomState = randomState_;
        const auto apply = [this, &candidateRoot, &stagedRandomState, &errorMessage](const UndoRecord& item) {
            NodePtr before;
            NodePtr discard;
            if (!Split(candidateRoot, item.start, before, discard, stagedRandomState, errorMessage)) return false;
            NodePtr inserted;
            NodePtr after;
            if (!Split(discard, item.insertedLength, inserted, after, stagedRandomState, errorMessage)) return false;
            std::vector<Piece> restorePieces(item.removed);
            NodePtr restored = BuildTree(std::move(restorePieces), stagedRandomState);
            candidateRoot = Merge(Merge(std::move(before), std::move(restored), stagedRandomState),
                std::move(after), stagedRandomState);
            return true;
        };

        if (!record.compound.empty()) {
            for (auto iterator = record.compound.rbegin(); iterator != record.compound.rend(); ++iterator) {
                if (!apply(*iterator)) {
                    if (errorMessage.empty()) errorMessage = L"The undo history does not match the document.";
                    return false;
                }
            }
        } else if (!apply(record)) {
            if (errorMessage.empty()) errorMessage = L"The undo history does not match the document.";
            return false;
        }

        const std::uint64_t restoredSelectionStart = record.selectionBeforeStart;
        const std::uint64_t restoredSelectionEnd = record.selectionBeforeEnd;
        const std::uint64_t restoredState = record.stateBefore;
        const std::uint64_t restoredUndoBytes = record.memoryBytes;
        root_ = std::move(candidateRoot);
        randomState_ = stagedRandomState;
        undo_.pop_back();
        undoBytes_ -= restoredUndoBytes;
        stateId_ = restoredState;
        ++generation_;
        selectionStart = restoredSelectionStart;
        selectionEnd = restoredSelectionEnd;
        return true;
    } catch (const std::bad_alloc&) {
        errorMessage = L"There is not enough memory to complete Undo.";
        return false;
    } catch (const std::length_error&) {
        errorMessage = L"The undo operation exceeds the supported in-memory size.";
        return false;
    }
}

bool LargeDocument::IsWordCharacter(wchar_t character)
{
    return IsSearchWordUnit(character);
}

bool LargeDocument::MatchWholeWord(std::uint64_t start, std::uint64_t length, std::wstring& errorMessage) const
{
    if (start > CharacterCount() || length > CharacterCount() - start) {
        errorMessage = L"The matched range is outside the large document.";
        return false;
    }
    const std::uint64_t end = start + length;
    if (!IsCharacterBoundary(start, errorMessage) || !IsCharacterBoundary(end, errorMessage)) return false;
    if (length == 0U) return true;
    wchar_t first = 0;
    wchar_t last = 0;
    if (!CharacterAt(start, first, errorMessage) ||
        !CharacterAt(end - 1U, last, errorMessage)) return false;
    wchar_t before = 0;
    wchar_t after = 0;
    if (start > 0U && !CharacterAt(start - 1U, before, errorMessage)) return false;
    if (end < CharacterCount() && !CharacterAt(end, after, errorMessage)) return false;
    const bool leadingBoundary = start == 0U || !IsWordCharacter(before) || !IsWordCharacter(first);
    const bool trailingBoundary = end == CharacterCount() || !IsWordCharacter(last) || !IsWordCharacter(after);
    return errorMessage.empty() && leadingBoundary && trailingBoundary;
}

bool LargeDocument::Equals(const std::wstring& left, const std::wstring& right, bool matchCase)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!SearchCodeUnitsEqual(left[index], right[index], matchCase)) return false;
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
        return SearchCodeUnitsEqual(left, right, matchCase);
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
            // Most search terms can only match at scalar boundaries based on
            // their own endpoints. Read adjacent source units only when the
            // term begins with a low surrogate or ends with a high surrogate;
            // this keeps ordinary ASCII scans from adding source reads that
            // change cancellation and read-failure behavior.
            const wchar_t firstNeedleUnit = needle.front();
            const wchar_t lastNeedleUnit = needle.back();
            const bool startsWithLowSurrogate = firstNeedleUnit >= 0xDC00 && firstNeedleUnit <= 0xDFFF;
            const bool endsWithHighSurrogate = lastNeedleUnit >= 0xD800 && lastNeedleUnit <= 0xDBFF;
            bool scalarSafe = candidate + needle.size() <= total;
            if (inDirection && scalarSafe && startsWithLowSurrogate && candidate != 0U) {
                scalarSafe = IsCharacterBoundary(candidate, errorMessage);
            }
            if (inDirection && scalarSafe && endsWithHighSurrogate && candidate + needle.size() < total) {
                scalarSafe = IsCharacterBoundary(candidate + static_cast<std::uint64_t>(needle.size()), errorMessage);
            }
            if (!errorMessage.empty()) return false;
            if (inDirection && scalarSafe &&
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
    if (!completed) {
        if (!errorMessage.empty()) return false;
        if (shouldCancel && shouldCancel()) {
            errorMessage = L"The operation was cancelled.";
            return false;
        }
        // Forward search deliberately stops the stream as soon as its first
        // match is found. Backward search never stops intentionally: an
        // incomplete scan cannot establish that its last candidate is final.
        if (forward && found) return true;
        return false;
    }
    if (found) { match = last; return true; }
    return false;
}

bool LargeDocument::MatchesAt(
    std::uint64_t start,
    const std::wstring& needle,
    bool matchCase,
    bool wholeWord,
    std::wstring& errorMessage) const
{
    errorMessage.clear();
    if (needle.empty() || start > CharacterCount() ||
        static_cast<std::uint64_t>(needle.size()) > CharacterCount() - start) return false;
    const std::uint64_t end = start + static_cast<std::uint64_t>(needle.size());
    if (!IsCharacterBoundary(start, errorMessage) || !IsCharacterBoundary(end, errorMessage)) return false;
    std::wstring candidate;
    if (!ReadRange(start, static_cast<std::uint64_t>(needle.size()), candidate, errorMessage)) return false;
    if (!SearchTextMatchesAt(candidate, 0U, needle, matchCase, false)) return false;
    return !wholeWord || MatchWholeWord(start, static_cast<std::uint64_t>(needle.size()), errorMessage);
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
    try {
        std::vector<std::size_t> prefix(needle.size(), 0U);
        const auto equalUnit = [matchCase](wchar_t left, wchar_t right) {
            return SearchCodeUnitsEqual(left, right, matchCase);
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
                    const bool scalarSafe = IsCharacterBoundary(start, errorMessage) &&
                        IsCharacterBoundary(start + static_cast<std::uint64_t>(needle.size()), errorMessage);
                    if (!errorMessage.empty()) return false;
                    if (start >= lastEnd && scalarSafe && (!wholeWord || MatchWholeWord(start,
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
        if (shouldCancel && shouldCancel()) {
            cancelled = true;
            return 0;
        }

        if (!IsValidInsertedText(replacement)) {
            errorMessage = L"Inserted text contains an unmatched Unicode surrogate.";
            return 0;
        }
        for (const LargeDocumentMatch& match : matches) {
            if (!IsCharacterBoundary(match.start, errorMessage) ||
                !IsCharacterBoundary(match.start + match.length, errorMessage)) return 0;
        }

        std::uint64_t stagedRandomState = randomState_;
        std::uint64_t stagedStateId = stateId_;
        std::uint64_t stagedNextStateId = nextStateId_;
        std::uint64_t operationUndoBytes = 0;
        NodePtr candidateRoot = root_;
        std::vector<UndoRecord> records;
        records.reserve(matches.size());
        for (auto iterator = matches.rbegin(); iterator != matches.rend(); ++iterator) {
            if (shouldCancel && shouldCancel()) {
                cancelled = true;
                return 0;
            }
            UndoRecord record;
            if (!PrepareReplace(candidateRoot, iterator->start, iterator->start + iterator->length,
                    replacement, stagedRandomState, candidateRoot, record, errorMessage)) return 0;
            if (shouldCancel && shouldCancel()) {
                cancelled = true;
                return 0;
            }
            record.selectionBeforeStart = iterator->start;
            record.selectionBeforeEnd = iterator->start + iterator->length;
            record.stateBefore = stagedStateId;
            record.stateAfter = stagedNextStateId;
            stagedStateId = stagedNextStateId++;
            if (AddWouldOverflow(operationUndoBytes, record.memoryBytes)) {
                errorMessage = L"The undo history would exceed the supported size.";
                return 0;
            }
            operationUndoBytes += record.memoryBytes;
            records.push_back(std::move(record));
        }

        UndoRecord committedRecord;
        if (records.size() == 1U) {
            committedRecord = std::move(records.front());
        } else {
            committedRecord.selectionBeforeStart = 0;
            committedRecord.selectionBeforeEnd = 0;
            committedRecord.stateBefore = stateId_;
            committedRecord.stateAfter = stagedStateId;
            if (records.capacity() > (std::numeric_limits<std::uint64_t>::max)() / sizeof(UndoRecord) ||
                AddWouldOverflow(operationUndoBytes,
                    static_cast<std::uint64_t>(records.capacity()) * sizeof(UndoRecord)) ||
                AddWouldOverflow(operationUndoBytes +
                    static_cast<std::uint64_t>(records.capacity()) * sizeof(UndoRecord), sizeof(UndoRecord))) {
                errorMessage = L"The undo history would exceed the supported size.";
                return 0;
            }
            committedRecord.memoryBytes = operationUndoBytes +
                static_cast<std::uint64_t>(records.capacity()) * sizeof(UndoRecord) + sizeof(UndoRecord);
            committedRecord.compound = std::move(records);
        }
        if (shouldCancel && shouldCancel()) {
            cancelled = true;
            return 0;
        }
        const std::uint64_t committedCharge = committedRecord.memoryBytes;
        const std::shared_ptr<const UndoRecord> recordOwner =
            std::make_shared<UndoRecord>(std::move(committedRecord));
        // Stage deque growth before publishing the compound edit. Everything
        // after this insertion is nonthrowing, so cancellation/failure cannot
        // expose a partial Replace All or its history record.
        undo_.push_back(recordOwner);
        root_ = std::move(candidateRoot);
        randomState_ = stagedRandomState;
        stateId_ = stagedStateId;
        nextStateId_ = stagedNextStateId;
        if (committedCharge > undoByteLimit_) {
            undo_.clear();
            undoBytes_ = 0U;
        } else {
            const std::uint64_t maximumPreviousCharge = undoByteLimit_ - committedCharge;
            while (!undo_.empty() && undoBytes_ > maximumPreviousCharge) {
                undoBytes_ -= undo_.front()->memoryBytes;
                undo_.pop_front();
            }
            undoBytes_ += committedCharge;
        }
        ++generation_;
        return static_cast<std::uint64_t>(matches.size());
    } catch (const std::bad_alloc&) {
        errorMessage = L"There is not enough memory to complete Replace All.";
        return 0;
    } catch (const std::length_error&) {
        errorMessage = L"Replace All exceeds the supported in-memory size.";
        return 0;
    }
}

bool LargeDocument::StreamText(
    const std::function<bool(const std::wstring&)>& visitor,
    const std::function<bool()>& shouldCancel,
    std::wstring& errorMessage) const
{
    errorMessage.clear();
    bool succeeded = true;
    std::wstring outputChunk;
    outputChunk.reserve(static_cast<std::size_t>(kReadChunkCharacters) + 1U);
    bool hasPendingHighSurrogate = false;
    const auto flushOutput = [&]() {
        if (outputChunk.empty() || !succeeded) return;
        if (shouldCancel && shouldCancel()) {
            errorMessage = L"The operation was cancelled.";
            succeeded = false;
            return;
        }
        succeeded = visitor(outputChunk);
        outputChunk.clear();
    };
    const auto appendScalarSafe = [&](const std::wstring& chunk) {
        for (wchar_t character : chunk) {
            if (hasPendingHighSurrogate) {
                if (!IsLowSurrogate(character)) {
                    errorMessage = L"The document contains an unmatched Unicode surrogate.";
                    succeeded = false;
                    return;
                }
                outputChunk.push_back(character);
                hasPendingHighSurrogate = false;
            } else if (IsHighSurrogate(character)) {
                outputChunk.push_back(character);
                hasPendingHighSurrogate = true;
            } else if (IsLowSurrogate(character)) {
                errorMessage = L"The document contains an unmatched Unicode surrogate.";
                succeeded = false;
                return;
            } else {
                outputChunk.push_back(character);
            }
            // A high surrogate and its low surrogate are appended as one pair,
            // so this flush can never split a scalar across visitor calls.
            if (outputChunk.size() >= static_cast<std::size_t>(kReadChunkCharacters) &&
                !hasPendingHighSurrogate) {
                flushOutput();
                if (!succeeded) return;
            }
        }
    };
    const auto streamPiece = [this, &shouldCancel, &errorMessage, &succeeded, &appendScalarSafe](const Piece& piece) {
        if (!succeeded) return;
        if (shouldCancel && shouldCancel()) {
            errorMessage = L"The operation was cancelled.";
            succeeded = false;
            return;
        }
        if (piece.kind == Piece::Kind::Inserted) {
            if (piece.inserted == nullptr) {
                errorMessage = L"The inserted text buffer is unavailable.";
                succeeded = false;
                return;
            }
            const std::wstring& buffer = piece.inserted->text;
            if (piece.offset > buffer.size() || piece.characters > buffer.size() - piece.offset) {
                errorMessage = L"The inserted text range is invalid.";
                succeeded = false;
                return;
            }
            for (std::uint64_t offset = 0; offset < piece.characters && succeeded;) {
                if (shouldCancel && shouldCancel()) {
                    errorMessage = L"The operation was cancelled.";
                    succeeded = false;
                    return;
                }
                const std::uint64_t amount = std::min<std::uint64_t>(kReadChunkCharacters, piece.characters - offset);
                const std::wstring chunk = buffer.substr(
                    static_cast<std::size_t>(piece.offset + offset), static_cast<std::size_t>(amount));
                appendScalarSafe(chunk);
                offset += amount;
            }
            return;
        }
        if (!StreamOriginal(piece, [&](const std::wstring& decoded) {
                appendScalarSafe(decoded);
                return succeeded;
            }, shouldCancel, errorMessage)) {
            succeeded = false;
        }
    };
    const auto visit = [&streamPiece, &succeeded](const auto& self, const Node* node) -> void {
        if (node == nullptr || !succeeded) return;
        self(self, node->left.get());
        streamPiece(node->piece);
        self(self, node->right.get());
    };
    visit(visit, root_.get());
    if (succeeded && hasPendingHighSurrogate) {
        errorMessage = L"The document contains an unmatched Unicode surrogate.";
        return false;
    }
    flushOutput();
    return succeeded;
}

} // namespace classic_notepad
