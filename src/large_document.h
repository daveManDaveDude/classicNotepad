#pragma once

#include "large_file_index.h"
#include "large_text_summary.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace classic_notepad {

// A bounded byte reader.  Implementations must never return data outside the
// requested range.  The Windows implementation maps only the requested view;
// portable tests use an in-memory reader.
using LargeDocumentByteReader = std::function<bool(
    std::uint64_t offset,
    std::size_t length,
    std::vector<std::uint8_t>& bytes)>;

struct LargeDocumentOriginalPiece {
    std::uint64_t byteOffset = 0;
    std::uint64_t byteLength = 0;
    std::uint64_t characterLength = 0; // UTF-16 code units (the Win32 coordinate system).
    // Counted within this descriptor. Adjacent descriptors may end/start with
    // CR/LF; the tree combines that pair as one logical line break.
    std::uint64_t logicalLineBreaks = 0;
    wchar_t firstCharacter = 0;
    wchar_t lastCharacter = 0;
    bool summaryKnown = false;
    LargeFileWidthSummary widthSummary{};
    bool widthSummaryKnown = false;
    // Exact count using status display semantics (surrogate pair=1, CRLF=1).
    std::uint64_t displayedCharacterCount = 0;
    bool displayedCharacterCountKnown = false;
};

struct LargeDocumentLineColumn {
    std::uint64_t line = 0;
    std::uint64_t column = 0;
};

struct LargeDocumentMatch {
    std::uint64_t start = 0;
    std::uint64_t length = 0;
};

// A scalable editable sequence. The original file is immutable and is read
// through byte ranges; inserted text lives in immutable bounded chunks shared
// by pieces, undo records, and snapshots. The tree aggregates logical UTF-16
// positions, so edits do not copy the unchanged suffix of a large document.
class LargeDocument {
public:
    static constexpr std::uint64_t kDefaultUndoByteLimit = 64ULL * 1024ULL * 1024ULL;

    using TextSummary = LargeTextSummary;

    LargeDocument(LargeFileEncoding encoding, LargeDocumentByteReader reader);
    ~LargeDocument();

    LargeDocument(const LargeDocument&) = delete;
    LargeDocument& operator=(const LargeDocument&) = delete;

    bool Initialize(
        const std::vector<LargeDocumentOriginalPiece>& originalPieces,
        std::wstring& errorMessage);
    bool AppendOriginalPieces(
        const std::vector<LargeDocumentOriginalPiece>& originalPieces,
        std::wstring& errorMessage);
    void Clear();

    std::uint64_t CharacterCount() const;
    std::uint64_t DisplayedCharacterCount() const;
    bool HasExactDisplayedCharacterCount() const;
    std::uint64_t LineCount() const;
    std::uint64_t MaximumLineColumns() const;
    std::uint64_t PieceCount() const;
    // Bytes of live immutable inserted-chunk capacity retained by this
    // document's snapshot family. This counts each owner once, including
    // chunks kept only by Undo history or another shared snapshot; it is a
    // model-storage measure, not allocator-exact process memory.
    std::uint64_t InsertedBufferBytes() const;
    bool IsModified() const;
    std::uint64_t Generation() const;
    std::uint64_t SavedState() const;
    void MarkSaved();

    bool IsCharacterBoundary(std::uint64_t position, std::wstring& errorMessage) const;
    bool PositionToLineColumn(
        std::uint64_t position,
        LargeDocumentLineColumn& coordinate,
        std::wstring& errorMessage) const;
    bool LineStart(
        std::uint64_t zeroBasedLine,
        std::uint64_t& position,
        std::wstring& errorMessage) const;
    bool LineRange(
        std::uint64_t zeroBasedLine,
        std::uint64_t& start,
        std::uint64_t& contentEnd,
        std::uint64_t& nextLineStart,
        std::wstring& errorMessage) const;

    // Reads no more than maxCharacters.  This is intentionally bounded and
    // never materializes the full document.
    bool ReadRange(
        std::uint64_t start,
        std::uint64_t maxCharacters,
        std::wstring& text,
        std::wstring& errorMessage) const;
    bool CharacterAt(std::uint64_t position, wchar_t& character, std::wstring& errorMessage) const;

    bool Replace(
        std::uint64_t start,
        std::uint64_t end,
        const std::wstring& replacement,
        std::uint64_t selectionBeforeStart,
        std::uint64_t selectionBeforeEnd,
        std::uint64_t& selectionAfterStart,
        std::uint64_t& selectionAfterEnd,
        std::wstring& errorMessage);
    bool Undo(
        std::uint64_t& selectionStart,
        std::uint64_t& selectionEnd,
        std::wstring& errorMessage);
    bool CanUndo() const;
    void SetUndoByteLimit(std::uint64_t bytes);
    // Conservative logical history-retention charge, separate from the live
    // owner-capacity counter above (the estimates can overlap). This is not
    // an allocator-exact RSS measurement.
    std::uint64_t UndoBytes() const;

    bool Find(
        const std::wstring& needle,
        std::uint64_t start,
        bool matchCase,
        bool wholeWord,
        bool forward,
        LargeDocumentMatch& match,
        std::wstring& errorMessage,
        const std::function<bool()>& shouldCancel = {},
        const std::function<void(std::uint64_t)>& onProgress = {}) const;
    bool MatchesAt(
        std::uint64_t start,
        const std::wstring& needle,
        bool matchCase,
        bool wholeWord,
        std::wstring& errorMessage) const;
    std::uint64_t ReplaceAll(
        const std::wstring& needle,
        const std::wstring& replacement,
        bool matchCase,
        bool wholeWord,
        bool& cancelled,
        const std::function<bool()>& shouldCancel,
        std::wstring& errorMessage,
        const std::function<void(std::uint64_t)>& onProgress = {});

    // Visits text in fixed-size chunks.  The callback returning false stops
    // streaming without treating it as a document error.
    bool StreamText(
        const std::function<bool(const std::wstring&)>& visitor,
        const std::function<bool()>& shouldCancel,
        std::wstring& errorMessage) const;
    std::unique_ptr<LargeDocument> Clone() const;

private:
    struct InsertedStorageLedger;
    struct InsertedBuffer;
    struct Piece;
    struct Node;
    struct UndoRecord;

    using NodePtr = std::shared_ptr<Node>;

    static std::uint64_t CharacterLength(const NodePtr& node);
    static TextSummary SummaryOf(const NodePtr& node);
    static std::uint64_t PieceLength(const Piece& piece);
    static void Update(Node* node);
    static NodePtr Merge(NodePtr left, NodePtr right, std::uint64_t& randomState);
    bool Split(const NodePtr& root, std::uint64_t position, NodePtr& left, NodePtr& right,
        std::uint64_t& randomState, std::wstring& errorMessage) const;
    bool SplitPiece(const Piece& piece, std::uint64_t position, Piece& left, Piece& right, std::wstring& errorMessage) const;
    static NodePtr MakeNode(Piece piece, std::uint64_t& randomState);
    static NodePtr CloneWithChildren(const NodePtr& node, NodePtr left, NodePtr right);
    static NodePtr BuildTree(std::vector<Piece> pieces, std::uint64_t& randomState);
    static void CollectPieces(const NodePtr& root, std::vector<Piece>& pieces);
    static void CountNodes(const NodePtr& node, std::uint64_t& count);
    bool Locate(std::uint64_t position, const Node*& node, std::uint64_t& offset) const;
    bool ReadPiece(
        const Piece& piece,
        std::uint64_t start,
        std::uint64_t length,
        std::wstring& text,
        std::wstring& errorMessage) const;
    bool OriginalCharacterToByte(
        const Piece& piece,
        std::uint64_t characterOffset,
        std::uint64_t& byteOffset,
        std::wstring& errorMessage,
        std::uint64_t* codeUnitsIntoScalar = nullptr,
        const std::function<bool()>& shouldCancel = {}) const;
    bool StreamOriginal(
        const Piece& piece,
        const std::function<bool(const std::wstring&)>& visitor,
        const std::function<bool()>& shouldCancel,
        std::wstring& errorMessage) const;
    bool DecodeOriginal(
        const Piece& piece,
        std::uint64_t startCharacter,
        std::uint64_t maxCharacters,
        std::wstring& text,
        std::wstring& errorMessage) const;
    bool ValidateRange(std::uint64_t start, std::uint64_t end, std::wstring& errorMessage) const;
    bool PrepareReplace(
        const NodePtr& root,
        std::uint64_t start,
        std::uint64_t end,
        const std::wstring& replacement,
        std::uint64_t& randomState,
        NodePtr& candidateRoot,
        UndoRecord& record,
        std::wstring& errorMessage) const;
    bool SummarizePrefix(std::uint64_t length, TextSummary& summary, std::wstring& errorMessage) const;
    bool SummarizePiece(
        const Piece& piece,
        std::uint64_t start,
        std::uint64_t length,
        TextSummary& summary,
        std::wstring& errorMessage) const;
    static bool IsWordCharacter(wchar_t character);
    bool MatchWholeWord(std::uint64_t start, std::uint64_t length, std::wstring& errorMessage) const;
    static bool Equals(const std::wstring& left, const std::wstring& right, bool matchCase);
    void TrimUndo();

    LargeFileEncoding encoding_;
    LargeDocumentByteReader reader_;
    NodePtr root_;
    std::shared_ptr<InsertedStorageLedger> insertedStorageLedger_;
    std::deque<std::shared_ptr<const UndoRecord>> undo_;
    std::uint64_t undoBytes_ = 0;
    std::uint64_t undoByteLimit_ = kDefaultUndoByteLimit;
    std::uint64_t generation_ = 1;
    std::uint64_t stateId_ = 1;
    std::uint64_t savedStateId_ = 1;
    std::uint64_t nextStateId_ = 2;
    std::uint64_t randomState_ = 0x9E3779B97F4A7C15ULL;
    std::uint64_t sourceHighWater_ = 0;
};

} // namespace classic_notepad
