#pragma once

#include "large_document.h"
#include "large_file_index.h"
#include "spelling.h"

#include <gtk/gtk.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace classic_notepad::linux_ui {

class GtkLargeFileView {
public:
    static constexpr std::uint64_t kActivationThresholdBytes = 48ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t kClipboardCharacterLimit = 16ULL * 1024ULL * 1024ULL;

    struct State {
        bool active = false;
        bool editable = false;
        bool indexingComplete = false;
        bool encodingValid = true;
        bool modified = false;
        std::uint64_t fileSize = 0;
        std::uint64_t indexedBytes = 0;
        std::uint64_t characterCount = 0;
        std::uint64_t lineCount = 1;
        std::uint64_t maximumLineColumns = 0;
        std::uint64_t selectionStart = 0;
        std::uint64_t selectionEnd = 0;
        std::uint64_t caretPosition = 0;
        std::uint64_t caretLine = 0;
        std::uint64_t caretColumn = 0;
        std::uint64_t topLine = 0;
        std::uint64_t pieceCount = 0;
        std::uint64_t insertedBufferBytes = 0;
        std::uint64_t firstUsableMilliseconds = 0;
        std::uint64_t fullIndexMilliseconds = 0;
    };

    GtkLargeFileView();
    ~GtkLargeFileView();

    GtkLargeFileView(const GtkLargeFileView&) = delete;
    GtkLargeFileView& operator=(const GtkLargeFileView&) = delete;

    static bool ShouldUse(const std::wstring& path, std::uint64_t& size, std::wstring& errorMessage);
    bool Open(const std::wstring& path, std::wstring& errorMessage);
    void Close();

    GtkWidget* Widget() const;
    GtkWidget* DrawingArea() const;
    const std::wstring& Path() const;
    LargeFileEncoding Encoding() const;
    State GetState() const;

    void SetChangedCallback(std::function<void()> callback);
    void SetContextMenuCallback(std::function<void(double, double, std::uint64_t)> callback);
    void SetSpellCheckCallback(std::function<std::vector<classic_notepad::SpellIssue>(const std::wstring&)> callback);
    void InvalidateSpelling();
    void SetFont(const std::wstring& description);
    void SetDarkMode(bool darkMode);
    void Focus();
    void QueueDraw();

    bool SetSelection(std::uint64_t start, std::uint64_t end, std::wstring& errorMessage);
    bool GetSelectedText(std::wstring& text, std::wstring& errorMessage) const;
    bool ReplaceSelection(const std::wstring& text, std::wstring& errorMessage);
    bool DeleteSelection(std::wstring& errorMessage);
    bool Undo(std::wstring& errorMessage);
    void SelectAll();
    bool GoToLine(std::uint64_t oneBasedLine, std::wstring& errorMessage);
    bool Find(const std::wstring& text, bool matchCase, bool wholeWord, bool forward, std::wstring& errorMessage);
    bool ReadRange(std::uint64_t start, std::uint64_t length, std::wstring& text, std::wstring& errorMessage) const;
    std::uint64_t ReplaceAll(const std::wstring& text, const std::wstring& replacement,
        bool matchCase, bool wholeWord, std::wstring& errorMessage);
    bool SaveTo(const std::wstring& path, std::wstring& errorMessage);

private:
    static void Draw(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer userData);
    static gboolean KeyPressed(GtkEventControllerKey*, guint keyval, guint keycode, GdkModifierType state, gpointer userData);
    static void KeyReleased(GtkEventControllerKey*, guint keyval, guint keycode, GdkModifierType state, gpointer userData);
    static void ClickPressed(GtkGestureClick*, int presses, double x, double y, gpointer userData);
    static void ContextPressed(GtkGestureClick*, int presses, double x, double y, gpointer userData);
    static void DragBegin(GtkGestureDrag*, double x, double y, gpointer userData);
    static void DragUpdate(GtkGestureDrag*, double x, double y, gpointer userData);
    static void AdjustmentChanged(GtkAdjustment*, gpointer userData);
    static gboolean ProgressTick(gpointer userData);

    void IndexWorker();
    bool ConsumeChunk(std::uint64_t offset, std::size_t length, std::wstring& errorMessage);
    bool AppendIndexedPieces(
        const std::vector<std::uint64_t>& byteOffsets,
        const std::vector<std::uint64_t>& characterOffsets,
        std::uint64_t breaksBeforeBatch,
        bool appendTail,
        std::wstring& errorMessage);
    bool AppendPiece(std::uint64_t endByte, std::uint64_t endCharacter, std::uint64_t totalBreaks, std::wstring& errorMessage);
    bool CopyBytes(std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) const;
    wchar_t BoundaryCharacter(std::uint64_t offset) const;
    void RefreshMetrics();
    void RefreshAdjustments();
    void NotifyChanged();
    void EnsureCaretVisible();
    bool HitTest(double x, double y, std::uint64_t& position, std::wstring& errorMessage);
    void ShowContextMenu(double x, double y, std::uint64_t position);
    void ShowCaretContextMenu();
    void MoveHorizontal(bool right, bool extend);
    void MoveVertical(int lines, bool extend);
    void MoveLineEdge(bool end, bool document, bool extend);
    void Paint(cairo_t* cr, int width, int height);
    bool CurrentLineRange(std::uint64_t& start, std::uint64_t& end, std::uint64_t& next, std::wstring& errorMessage) const;

    std::wstring path_;
    int fileDescriptor_ = -1;
    const std::uint8_t* mapping_ = nullptr;
    std::uint64_t fileSize_ = 0;
    LargeFileEncoding encoding_ = LargeFileEncoding::Utf8;
    std::uint64_t dataOffset_ = 0;

    GtkWidget* root_ = nullptr;
    GtkWidget* drawingArea_ = nullptr;
    GtkWidget* verticalScrollbar_ = nullptr;
    GtkWidget* horizontalScrollbar_ = nullptr;
    GtkAdjustment* verticalAdjustment_ = nullptr;
    GtkAdjustment* horizontalAdjustment_ = nullptr;
    guint progressSource_ = 0;

    std::wstring fontDescription_ = L"Monospace 11";
    bool darkMode_ = false;
    int lineHeight_ = 20;
    int characterWidth_ = 9;

    mutable std::mutex documentMutex_;
    std::unique_ptr<LargeDocument> document_;
    std::unique_ptr<LargeFileLineScanner> scanner_;
    std::uint64_t appendedByte_ = 0;
    std::uint64_t appendedCharacter_ = 0;
    std::uint64_t appendedBreaks_ = 0;
    std::uint64_t scannedOffset_ = 0;

    std::thread indexThread_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> indexingComplete_{false};
    std::atomic<bool> encodingValid_{true};
    std::atomic<std::uint64_t> indexedBytes_{0};
    std::atomic<std::uint64_t> characterCount_{0};
    std::atomic<std::uint64_t> lineCount_{1};
    std::atomic<std::uint64_t> maximumLineColumns_{0};
    std::atomic<std::uint64_t> firstUsableMilliseconds_{0};
    std::atomic<std::uint64_t> fullIndexMilliseconds_{0};
    std::uint64_t startedMilliseconds_ = 0;

    std::uint64_t selectionStart_ = 0;
    std::uint64_t selectionEnd_ = 0;
    std::uint64_t selectionAnchor_ = 0;
    std::uint64_t caretPosition_ = 0;
    std::uint64_t preferredColumn_ = 0;
    bool dragging_ = false;
    double dragStartX_ = 0;
    double dragStartY_ = 0;

    std::function<void()> changedCallback_;
    std::function<void(double, double, std::uint64_t)> contextMenuCallback_;
    std::function<std::vector<classic_notepad::SpellIssue>(const std::wstring&)> spellCheckCallback_;
    struct SpellCacheEntry {
        std::uint64_t line = 0;
        std::wstring text;
        std::vector<classic_notepad::SpellIssue> issues;
    };
    std::vector<SpellCacheEntry> spellCache_;
};

} // namespace classic_notepad::linux_ui
