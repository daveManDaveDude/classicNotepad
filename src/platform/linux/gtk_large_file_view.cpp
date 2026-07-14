#include "gtk_large_file_view.h"

#include "encoding.h"
#include "gtk_app.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pango/pangocairo.h>

namespace classic_notepad::linux_ui {
namespace {

constexpr std::size_t kInitialScanBytes = 1024U * 1024U;
constexpr std::size_t kIndexChunkBytes = 8U * 1024U * 1024U;
constexpr std::uint64_t kTargetPieceBytes = 64ULL * 1024ULL;
constexpr std::uint64_t kMaximumRenderedLine = 64ULL * 1024ULL;

std::uint64_t NowMilliseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right)
{
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    return right > maximum - left ? maximum : left + right;
}

std::wstring NormalizeInsertedText(const std::wstring& text)
{
    const std::wstring normalized = classic_notepad::NormalizeLineEndingsForEditor(text);
    if constexpr (sizeof(wchar_t) == 2U) return normalized;
    std::wstring units;
    units.reserve(normalized.size());
    for (wchar_t character : normalized) {
        const std::uint32_t codePoint = static_cast<std::uint32_t>(character);
        if (codePoint > 0xFFFFU) {
            const std::uint32_t adjusted = codePoint - 0x10000U;
            units.push_back(static_cast<wchar_t>(0xD800U + (adjusted >> 10U)));
            units.push_back(static_cast<wchar_t>(0xDC00U + (adjusted & 0x3FFU)));
        } else {
            units.push_back(character);
        }
    }
    return units;
}

std::wstring PlatformWideFromUtf16Units(const std::wstring& units)
{
    if constexpr (sizeof(wchar_t) == 2U) return units;
    std::wstring text;
    text.reserve(units.size());
    for (std::size_t index = 0; index < units.size(); ++index) {
        const std::uint32_t first = static_cast<std::uint32_t>(units[index]);
        if (first >= 0xD800U && first <= 0xDBFFU && index + 1U < units.size()) {
            const std::uint32_t second = static_cast<std::uint32_t>(units[index + 1U]);
            if (second >= 0xDC00U && second <= 0xDFFFU) {
                text.push_back(static_cast<wchar_t>(0x10000U + ((first - 0xD800U) << 10U) + second - 0xDC00U));
                ++index;
                continue;
            }
        }
        text.push_back(units[index]);
    }
    return text;
}

} // namespace

GtkLargeFileView::GtkLargeFileView() = default;

GtkLargeFileView::~GtkLargeFileView()
{
    Close();
}

bool GtkLargeFileView::ShouldUse(const std::wstring& path, std::uint64_t& size, std::wstring& errorMessage)
{
    const std::string utf8Path = Utf8FromWide(path);
    struct stat info {};
    if (utf8Path.empty() || stat(utf8Path.c_str(), &info) != 0 || info.st_size < 0) {
        errorMessage = L"The file size could not be determined.";
        size = 0;
        return false;
    }
    size = static_cast<std::uint64_t>(info.st_size);
    errorMessage.clear();
    return size >= kActivationThresholdBytes;
}

bool GtkLargeFileView::Open(const std::wstring& path, std::wstring& errorMessage)
{
    Close();
    path_ = path;
    const std::string utf8Path = Utf8FromWide(path);
    fileDescriptor_ = open(utf8Path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fileDescriptor_ < 0) {
        errorMessage = L"The large file could not be opened.";
        return false;
    }
    struct stat info {};
    if (fstat(fileDescriptor_, &info) != 0 || info.st_size < 0) {
        errorMessage = L"The large file size could not be determined.";
        Close();
        return false;
    }
    fileSize_ = static_cast<std::uint64_t>(info.st_size);
    if (fileSize_ == 0 || fileSize_ > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        errorMessage = L"The large file cannot be mapped in this Linux process.";
        Close();
        return false;
    }
    void* mapped = mmap(nullptr, static_cast<std::size_t>(fileSize_), PROT_READ, MAP_PRIVATE, fileDescriptor_, 0);
    if (mapped == MAP_FAILED) {
        errorMessage = L"The large file could not be memory mapped.";
        mapping_ = nullptr;
        Close();
        return false;
    }
    mapping_ = static_cast<const std::uint8_t*>(mapped);
    madvise(const_cast<std::uint8_t*>(mapping_), static_cast<std::size_t>(fileSize_), MADV_SEQUENTIAL);
    const LargeFileEncodingInfo detected = DetectLargeFileEncoding(mapping_, static_cast<std::size_t>(std::min<std::uint64_t>(fileSize_, 4U)));
    if (detected.encoding == LargeFileEncoding::Unsupported || detected.dataOffset > fileSize_) {
        errorMessage = L"Large-file mode supports UTF-8/ASCII and UTF-16 LE with BOM.";
        Close();
        return false;
    }
    encoding_ = detected.encoding;
    dataOffset_ = detected.dataOffset;
    appendedByte_ = dataOffset_;
    scannedOffset_ = dataOffset_;
    scanner_ = std::make_unique<LargeFileLineScanner>(encoding_, dataOffset_);
    startedMilliseconds_ = NowMilliseconds();
    cancel_.store(false);
    indexingComplete_.store(false);
    encodingValid_.store(true);
    indexedBytes_.store(dataOffset_);

    root_ = gtk_grid_new();
    drawingArea_ = gtk_drawing_area_new();
    gtk_widget_set_focusable(drawingArea_, TRUE);
    gtk_widget_set_focus_on_click(drawingArea_, TRUE);
    gtk_widget_set_hexpand(drawingArea_, TRUE);
    gtk_widget_set_vexpand(drawingArea_, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawingArea_), Draw, this, nullptr);
    verticalAdjustment_ = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 1, 1, 10, 10));
    horizontalAdjustment_ = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 1, 16, 200, 200));
    verticalScrollbar_ = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, verticalAdjustment_);
    horizontalScrollbar_ = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, horizontalAdjustment_);
    gtk_grid_attach(GTK_GRID(root_), drawingArea_, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(root_), verticalScrollbar_, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(root_), horizontalScrollbar_, 0, 1, 1, 1);
    g_signal_connect(verticalAdjustment_, "value-changed", G_CALLBACK(AdjustmentChanged), this);
    g_signal_connect(horizontalAdjustment_, "value-changed", G_CALLBACK(AdjustmentChanged), this);

    GtkEventController* key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(KeyPressed), this);
    g_signal_connect(key, "key-released", G_CALLBACK(KeyReleased), this);
    gtk_widget_add_controller(drawingArea_, key);
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(ClickPressed), this);
    gtk_widget_add_controller(drawingArea_, GTK_EVENT_CONTROLLER(click));
    GtkGesture* contextClick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(contextClick), GDK_BUTTON_SECONDARY);
    g_signal_connect(contextClick, "released", G_CALLBACK(ContextPressed), this);
    gtk_widget_add_controller(drawingArea_, GTK_EVENT_CONTROLLER(contextClick));
    GtkGesture* drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(DragBegin), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(DragUpdate), this);
    gtk_widget_add_controller(drawingArea_, GTK_EVENT_CONTROLLER(drag));

    const std::size_t initial = static_cast<std::size_t>(std::min<std::uint64_t>(kInitialScanBytes, fileSize_ - dataOffset_));
    if (!ConsumeChunk(dataOffset_, initial, errorMessage)) {
        Close();
        return false;
    }
    if (document_ == nullptr) {
        errorMessage = L"The first large-file viewport could not be indexed.";
        Close();
        return false;
    }
    firstUsableMilliseconds_.store(NowMilliseconds() - startedMilliseconds_);
    RefreshMetrics();
    RefreshAdjustments();
    progressSource_ = g_timeout_add(33, ProgressTick, this);
    indexThread_ = std::thread(&GtkLargeFileView::IndexWorker, this);
    return true;
}

void GtkLargeFileView::Close()
{
    cancel_.store(true);
    if (indexThread_.joinable()) indexThread_.join();
    if (progressSource_ != 0) {
        g_source_remove(progressSource_);
        progressSource_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        document_.reset();
    }
    scanner_.reset();
    if (mapping_ != nullptr) {
        munmap(const_cast<std::uint8_t*>(mapping_), static_cast<std::size_t>(fileSize_));
        mapping_ = nullptr;
    }
    if (fileDescriptor_ >= 0) {
        close(fileDescriptor_);
        fileDescriptor_ = -1;
    }
    root_ = drawingArea_ = verticalScrollbar_ = horizontalScrollbar_ = nullptr;
    verticalAdjustment_ = horizontalAdjustment_ = nullptr;
    fileSize_ = 0;
}

GtkWidget* GtkLargeFileView::Widget() const { return root_; }
GtkWidget* GtkLargeFileView::DrawingArea() const { return drawingArea_; }
const std::wstring& GtkLargeFileView::Path() const { return path_; }
LargeFileEncoding GtkLargeFileView::Encoding() const { return encoding_; }

GtkLargeFileView::State GtkLargeFileView::GetState() const
{
    State state;
    state.active = mapping_ != nullptr;
    state.indexingComplete = indexingComplete_.load();
    state.encodingValid = encodingValid_.load();
    state.fileSize = fileSize_;
    state.indexedBytes = indexedBytes_.load();
    state.characterCount = characterCount_.load();
    state.lineCount = lineCount_.load();
    state.maximumLineColumns = maximumLineColumns_.load();
    state.selectionStart = selectionStart_;
    state.selectionEnd = selectionEnd_;
    state.caretPosition = caretPosition_;
    state.topLine = verticalAdjustment_ == nullptr ? 0 : static_cast<std::uint64_t>(gtk_adjustment_get_value(verticalAdjustment_));
    state.firstUsableMilliseconds = firstUsableMilliseconds_.load();
    state.fullIndexMilliseconds = fullIndexMilliseconds_.load();
    std::unique_lock<std::mutex> lock(documentMutex_);
    if (document_ != nullptr) {
        state.editable = true;
        state.modified = document_->IsModified();
        state.characterCount = document_->CharacterCount();
        state.lineCount = document_->LineCount();
        state.pieceCount = document_->PieceCount();
        state.insertedBufferBytes = document_->InsertedBufferBytes();
        LargeDocumentLineColumn coordinate;
        std::wstring ignored;
        if (document_->PositionToLineColumn(caretPosition_, coordinate, ignored)) {
            state.caretLine = coordinate.line;
            state.caretColumn = coordinate.column;
        }
    }
    return state;
}

void GtkLargeFileView::SetChangedCallback(std::function<void()> callback) { changedCallback_ = std::move(callback); }
void GtkLargeFileView::SetContextMenuCallback(std::function<void(double, double, std::uint64_t)> callback)
{
    contextMenuCallback_ = std::move(callback);
}

void GtkLargeFileView::SetSpellCheckCallback(
    std::function<std::vector<classic_notepad::SpellIssue>(const std::wstring&)> callback)
{
    spellCheckCallback_ = std::move(callback);
    QueueDraw();
}

void GtkLargeFileView::InvalidateSpelling()
{
    spellCache_.clear();
    QueueDraw();
}
void GtkLargeFileView::SetDarkMode(bool darkMode) { darkMode_ = darkMode; QueueDraw(); }
void GtkLargeFileView::Focus() { if (drawingArea_ != nullptr) gtk_widget_grab_focus(drawingArea_); }
void GtkLargeFileView::QueueDraw() { if (drawingArea_ != nullptr) gtk_widget_queue_draw(drawingArea_); }

void GtkLargeFileView::SetFont(const std::wstring& description)
{
    fontDescription_ = description;
    RefreshMetrics();
    RefreshAdjustments();
    QueueDraw();
}

bool GtkLargeFileView::CopyBytes(std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) const
{
    bytes.clear();
    if (mapping_ == nullptr || offset > fileSize_ || static_cast<std::uint64_t>(length) > fileSize_ - offset) return false;
    bytes.assign(mapping_ + static_cast<std::size_t>(offset), mapping_ + static_cast<std::size_t>(offset) + length);
    return true;
}

wchar_t GtkLargeFileView::BoundaryCharacter(std::uint64_t offset) const
{
    if (offset >= fileSize_) return 0;
    if (encoding_ == LargeFileEncoding::Utf16Le) {
        if (offset + 1U >= fileSize_) return 0;
        const wchar_t value = static_cast<wchar_t>(mapping_[offset] | (static_cast<std::uint16_t>(mapping_[offset + 1U]) << 8U));
        return value == L'\r' || value == L'\n' ? value : wchar_t(1);
    }
    const std::uint8_t value = mapping_[offset];
    return value == '\r' || value == '\n' ? static_cast<wchar_t>(value) : wchar_t(1);
}

bool GtkLargeFileView::AppendPiece(
    std::uint64_t endByte, std::uint64_t endCharacter, std::uint64_t totalBreaks, std::wstring& errorMessage)
{
    if (endByte <= appendedByte_) return true;
    if (endCharacter < appendedCharacter_ || totalBreaks < appendedBreaks_) return false;
    const std::uint64_t unit = encoding_ == LargeFileEncoding::Utf16Le ? 2U : 1U;
    LargeDocumentOriginalPiece piece{
        appendedByte_, endByte - appendedByte_, endCharacter - appendedCharacter_, totalBreaks - appendedBreaks_,
        BoundaryCharacter(appendedByte_), BoundaryCharacter(endByte - unit), true};
    std::vector<LargeDocumentOriginalPiece> pieces{piece};
    std::unique_lock<std::mutex> lock(documentMutex_);
    if (document_ == nullptr) {
        auto model = std::make_unique<LargeDocument>(encoding_,
            [this](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
                return CopyBytes(offset, length, bytes);
            });
        if (!model->Initialize(pieces, errorMessage)) return false;
        document_ = std::move(model);
    } else if (!document_->AppendOriginalPieces(pieces, errorMessage)) {
        return false;
    }
    appendedByte_ = endByte;
    appendedCharacter_ = endCharacter;
    appendedBreaks_ = totalBreaks;
    characterCount_.store(endCharacter);
    lineCount_.store(totalBreaks + 1U);
    return true;
}

bool GtkLargeFileView::AppendIndexedPieces(
    const std::vector<std::uint64_t>& byteOffsets,
    const std::vector<std::uint64_t>& characterOffsets,
    std::uint64_t breaksBeforeBatch,
    bool appendTail,
    std::wstring& errorMessage)
{
    if (byteOffsets.size() != characterOffsets.size()) return false;
    for (std::size_t index = 0; index < byteOffsets.size(); ++index) {
        if (byteOffsets[index] >= SaturatingAdd(appendedByte_, kTargetPieceBytes)) {
            const std::uint64_t breaks = breaksBeforeBatch + static_cast<std::uint64_t>(index) + 1U;
            if (!AppendPiece(byteOffsets[index], characterOffsets[index], breaks, errorMessage)) return false;
        }
    }
    if (!byteOffsets.empty()) {
        const std::uint64_t breaks = breaksBeforeBatch + static_cast<std::uint64_t>(byteOffsets.size());
        if (!AppendPiece(byteOffsets.back(), characterOffsets.back(), breaks, errorMessage)) return false;
    }
    if (appendTail && appendedByte_ < fileSize_) {
        const std::uint64_t breaks = scanner_->CrlfCount() + scanner_->LfCount() + scanner_->CrCount();
        if (!AppendPiece(fileSize_, scanner_->CharacterCount(), breaks, errorMessage)) return false;
    }
    return true;
}

bool GtkLargeFileView::ConsumeChunk(std::uint64_t offset, std::size_t length, std::wstring& errorMessage)
{
    std::vector<std::uint64_t> bytes;
    std::vector<std::uint64_t> characters;
    const std::uint64_t breaksBefore = scanner_->CrlfCount() + scanner_->LfCount() + scanner_->CrCount();
    if (length != 0 && !scanner_->Consume(mapping_ + static_cast<std::size_t>(offset), length, offset, bytes, &characters)) {
        errorMessage = L"The large file contains invalid text encoding.";
        return false;
    }
    scannedOffset_ = offset + length;
    indexedBytes_.store(scannedOffset_);
    maximumLineColumns_.store(scanner_->MaximumLineColumns());
    return AppendIndexedPieces(bytes, characters, breaksBefore, false, errorMessage);
}

void GtkLargeFileView::IndexWorker()
{
    std::wstring error;
    while (!cancel_.load() && scannedOffset_ < fileSize_) {
        const std::uint64_t chunkOffset = scannedOffset_;
        const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(kIndexChunkBytes, fileSize_ - chunkOffset));
        if (!ConsumeChunk(chunkOffset, length, error)) {
            encodingValid_.store(false);
            break;
        }
        const long pageSize = std::max<long>(1, sysconf(_SC_PAGESIZE));
        const std::uint64_t aligned = chunkOffset - chunkOffset % static_cast<std::uint64_t>(pageSize);
        const std::uint64_t end = chunkOffset + length;
        madvise(const_cast<std::uint8_t*>(mapping_) + static_cast<std::size_t>(aligned),
            static_cast<std::size_t>(end - aligned), MADV_DONTNEED);
    }
    if (!cancel_.load() && encodingValid_.load()) {
        std::vector<std::uint64_t> bytes;
        std::vector<std::uint64_t> characters;
        const std::uint64_t breaksBefore = scanner_->CrlfCount() + scanner_->LfCount() + scanner_->CrCount();
        if (!scanner_->Finish(bytes, &characters) || !AppendIndexedPieces(bytes, characters, breaksBefore, true, error)) {
            encodingValid_.store(false);
        } else {
            characterCount_.store(scanner_->CharacterCount());
            lineCount_.store(scanner_->CrlfCount() + scanner_->LfCount() + scanner_->CrCount() + 1U);
            maximumLineColumns_.store(scanner_->MaximumLineColumns());
            indexedBytes_.store(fileSize_);
            indexingComplete_.store(true);
        }
    }
    fullIndexMilliseconds_.store(NowMilliseconds() - startedMilliseconds_);
}

void GtkLargeFileView::RefreshMetrics()
{
    if (drawingArea_ == nullptr) return;
    PangoLayout* layout = gtk_widget_create_pango_layout(drawingArea_, "M");
    pango_layout_set_width(layout, -1);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    const std::string font = Utf8FromWide(fontDescription_);
    PangoFontDescription* description = pango_font_description_from_string(font.c_str());
    pango_layout_set_font_description(layout, description);
    int width = 0;
    int height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    characterWidth_ = std::max(1, width);
    lineHeight_ = std::max(1, height + 2);
    pango_font_description_free(description);
    g_object_unref(layout);
}

void GtkLargeFileView::RefreshAdjustments()
{
    if (verticalAdjustment_ == nullptr || horizontalAdjustment_ == nullptr || drawingArea_ == nullptr) return;
    const int height = std::max(1, gtk_widget_get_height(drawingArea_));
    const int width = std::max(1, gtk_widget_get_width(drawingArea_));
    const double visibleLines = std::max(1, height / std::max(1, lineHeight_));
    const double lines = static_cast<double>(std::max<std::uint64_t>(1U, lineCount_.load()));
    gtk_adjustment_configure(verticalAdjustment_, std::min(gtk_adjustment_get_value(verticalAdjustment_), std::max(0.0, lines - visibleLines)),
        0.0, lines, 1.0, visibleLines, visibleLines);
    const double contentWidth = static_cast<double>(maximumLineColumns_.load()) * characterWidth_ + 8.0;
    gtk_adjustment_configure(horizontalAdjustment_, std::min(gtk_adjustment_get_value(horizontalAdjustment_), std::max(0.0, contentWidth - width)),
        0.0, std::max<double>(width, contentWidth), characterWidth_, std::max(1, width - characterWidth_), width);
}

void GtkLargeFileView::NotifyChanged()
{
    RefreshAdjustments();
    QueueDraw();
    if (changedCallback_) changedCallback_();
}

gboolean GtkLargeFileView::ProgressTick(gpointer userData)
{
    auto* view = static_cast<GtkLargeFileView*>(userData);
    view->NotifyChanged();
    if (view->indexingComplete_.load() || !view->encodingValid_.load() || view->cancel_.load()) {
        view->progressSource_ = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void GtkLargeFileView::Draw(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer userData)
{
    static_cast<GtkLargeFileView*>(userData)->Paint(cr, width, height);
}

void GtkLargeFileView::Paint(cairo_t* cr, int width, int height)
{
    if (lineHeight_ < 8 || characterWidth_ < 4) {
        RefreshMetrics();
        RefreshAdjustments();
    }
    cairo_set_source_rgb(cr, darkMode_ ? 0.075 : 1.0, darkMode_ ? 0.075 : 1.0, darkMode_ ? 0.075 : 1.0);
    cairo_paint(cr);
    const std::uint64_t top = verticalAdjustment_ == nullptr ? 0 : static_cast<std::uint64_t>(gtk_adjustment_get_value(verticalAdjustment_));
    const double horizontal = horizontalAdjustment_ == nullptr ? 0 : gtk_adjustment_get_value(horizontalAdjustment_);
    const int visible = std::max(1, height / std::max(1, lineHeight_) + 2);
    PangoLayout* layout = gtk_widget_create_pango_layout(drawingArea_, nullptr);
    pango_layout_set_width(layout, -1);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    const std::string font = Utf8FromWide(fontDescription_);
    PangoFontDescription* description = pango_font_description_from_string(font.c_str());
    pango_layout_set_font_description(layout, description);
    for (int row = 0; row < visible; ++row) {
        const std::uint64_t line = top + static_cast<std::uint64_t>(row);
        std::uint64_t start = 0, end = 0, next = 0;
        std::wstring text;
        std::wstring error;
        {
            std::lock_guard<std::mutex> lock(documentMutex_);
            if (document_ == nullptr || line >= document_->LineCount() ||
                !document_->LineRange(line, start, end, next, error) ||
                !document_->ReadRange(start, std::min<std::uint64_t>(end - start, kMaximumRenderedLine), text, error)) continue;
        }
        std::wstring expanded;
        std::uint64_t column = 0;
        for (wchar_t ch : text) {
            if (ch == L'\t') {
                const std::uint64_t spaces = 8U - column % 8U;
                expanded.append(static_cast<std::size_t>(spaces), L' ');
                column += spaces;
            } else {
                expanded.push_back(ch);
                ++column;
            }
        }
        const std::wstring displayText = PlatformWideFromUtf16Units(expanded);
        const std::string utf8 = Utf8FromWide(displayText);
        pango_layout_set_text(layout, utf8.c_str(), static_cast<int>(utf8.size()));
        const double y = static_cast<double>(row * lineHeight_);
        const std::uint64_t selectedStart = std::min(selectionStart_, selectionEnd_);
        const std::uint64_t selectedEnd = std::max(selectionStart_, selectionEnd_);
        if (selectedEnd > start && selectedStart < end) {
            const double left = 4.0 + (std::max(selectedStart, start) - start) * characterWidth_ - horizontal;
            const double right = 4.0 + (std::min(selectedEnd, end) - start) * characterWidth_ - horizontal;
            cairo_set_source_rgb(cr, darkMode_ ? 0.18 : 0.70, darkMode_ ? 0.36 : 0.82, darkMode_ ? 0.62 : 1.0);
            cairo_rectangle(cr, left, y, std::max(1.0, right - left), lineHeight_);
            cairo_fill(cr);
        }
        cairo_set_source_rgb(cr, darkMode_ ? 0.93 : 0.05, darkMode_ ? 0.93 : 0.05, darkMode_ ? 0.93 : 0.05);
        cairo_move_to(cr, 4.0 - horizontal, y + 1.0);
        pango_cairo_show_layout(cr, layout);
        if (spellCheckCallback_) {
            auto cached = std::find_if(spellCache_.begin(), spellCache_.end(), [&](const SpellCacheEntry& entry) {
                return entry.line == line && entry.text == displayText;
            });
            if (cached == spellCache_.end()) {
                if (spellCache_.size() >= 128U) spellCache_.erase(spellCache_.begin());
                spellCache_.push_back({ line, displayText, spellCheckCallback_(displayText) });
                cached = std::prev(spellCache_.end());
            }
            for (const SpellIssue& issue : cached->issues) {
                if (issue.lengthUtf16 == 0U || issue.startUtf16 >= expanded.size()) continue;
                const double left = 4.0 + static_cast<double>(issue.startUtf16) * characterWidth_ - horizontal;
                const double right = 4.0 + static_cast<double>(std::min<std::size_t>(
                    expanded.size(), issue.startUtf16 + issue.lengthUtf16)) * characterWidth_ - horizontal;
                if (right <= 0.0 || left >= width) continue;
                const double underlineY = y + lineHeight_ - 2.0;
                cairo_set_source_rgb(cr, 0.88, 0.08, 0.08);
                cairo_set_line_width(cr, 1.0);
                cairo_move_to(cr, left, underlineY);
                bool down = true;
                for (double underlineX = left + 2.0; underlineX < right; underlineX += 2.0) {
                    cairo_line_to(cr, underlineX, underlineY + (down ? 1.5 : -1.5));
                    down = !down;
                }
                cairo_line_to(cr, right, underlineY);
                cairo_stroke(cr);
            }
        }
        if (gtk_widget_has_focus(drawingArea_) && caretPosition_ >= start && caretPosition_ <= end) {
            const double caretX = 4.0 + (caretPosition_ - start) * characterWidth_ - horizontal;
            cairo_set_source_rgb(cr, darkMode_ ? 1.0 : 0.0, darkMode_ ? 1.0 : 0.0, darkMode_ ? 1.0 : 0.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, caretX, y + 1.0);
            cairo_line_to(cr, caretX, y + lineHeight_ - 1.0);
            cairo_stroke(cr);
        }
    }
    pango_font_description_free(description);
    g_object_unref(layout);
}

bool GtkLargeFileView::HitTest(double x, double y, std::uint64_t& position, std::wstring& errorMessage)
{
    const std::uint64_t top = static_cast<std::uint64_t>(gtk_adjustment_get_value(verticalAdjustment_));
    const std::uint64_t line = top + static_cast<std::uint64_t>(std::max(0.0, y) / lineHeight_);
    std::uint64_t start = 0, end = 0, next = 0;
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (document_ == nullptr || line >= document_->LineCount() || !document_->LineRange(line, start, end, next, errorMessage)) return false;
    const double documentX = x + gtk_adjustment_get_value(horizontalAdjustment_) - 4.0;
    const std::uint64_t column = documentX <= 0 ? 0 : static_cast<std::uint64_t>(std::llround(documentX / characterWidth_));
    position = std::min(end, SaturatingAdd(start, column));
    return true;
}

void GtkLargeFileView::ClickPressed(GtkGestureClick*, int, double x, double y, gpointer userData)
{
    auto* view = static_cast<GtkLargeFileView*>(userData);
    view->Focus();
    std::uint64_t position = 0;
    std::wstring error;
    if (view->HitTest(x, y, position, error)) {
        view->selectionStart_ = view->selectionEnd_ = view->selectionAnchor_ = view->caretPosition_ = position;
        view->EnsureCaretVisible();
        view->NotifyChanged();
    }
}

void GtkLargeFileView::ContextPressed(GtkGestureClick* gesture, int, double x, double y, gpointer userData)
{
    auto* view = static_cast<GtkLargeFileView*>(userData);
    view->Focus();
    std::uint64_t position = view->caretPosition_;
    std::wstring error;
    if (view->HitTest(x, y, position, error)) {
        const std::uint64_t selectedStart = std::min(view->selectionStart_, view->selectionEnd_);
        const std::uint64_t selectedEnd = std::max(view->selectionStart_, view->selectionEnd_);
        if (position < selectedStart || position > selectedEnd || selectedStart == selectedEnd) {
            view->selectionStart_ = view->selectionEnd_ = view->selectionAnchor_ = view->caretPosition_ = position;
            view->NotifyChanged();
        }
        view->ShowContextMenu(x, y, position);
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }
}

void GtkLargeFileView::ShowContextMenu(double x, double y, std::uint64_t position)
{
    if (contextMenuCallback_) contextMenuCallback_(x, y, position);
}

void GtkLargeFileView::ShowCaretContextMenu()
{
    std::wstring error;
    LargeDocumentLineColumn coordinate;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || !document_->PositionToLineColumn(caretPosition_, coordinate, error)) return;
    }
    const double x = 4.0 + static_cast<double>(coordinate.column) * characterWidth_ -
        gtk_adjustment_get_value(horizontalAdjustment_);
    const double y = (static_cast<double>(coordinate.line) - gtk_adjustment_get_value(verticalAdjustment_) + 1.0) *
        lineHeight_;
    ShowContextMenu(std::max(0.0, x), std::max(0.0, y), caretPosition_);
}

void GtkLargeFileView::DragBegin(GtkGestureDrag*, double x, double y, gpointer userData)
{
    auto* view = static_cast<GtkLargeFileView*>(userData);
    view->dragging_ = true;
    view->dragStartX_ = x;
    view->dragStartY_ = y;
    std::uint64_t position = 0;
    std::wstring error;
    if (view->HitTest(x, y, position, error)) view->selectionAnchor_ = position;
}

void GtkLargeFileView::DragUpdate(GtkGestureDrag*, double x, double y, gpointer userData)
{
    auto* view = static_cast<GtkLargeFileView*>(userData);
    std::uint64_t position = 0;
    std::wstring error;
    if (view->HitTest(view->dragStartX_ + x, view->dragStartY_ + y, position, error)) {
        view->selectionStart_ = view->selectionAnchor_;
        view->selectionEnd_ = view->caretPosition_ = position;
        view->EnsureCaretVisible();
        view->NotifyChanged();
    }
}

void GtkLargeFileView::AdjustmentChanged(GtkAdjustment*, gpointer userData)
{
    static_cast<GtkLargeFileView*>(userData)->QueueDraw();
}

bool GtkLargeFileView::CurrentLineRange(std::uint64_t& start, std::uint64_t& end, std::uint64_t& next, std::wstring& errorMessage) const
{
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (document_ == nullptr) return false;
    LargeDocumentLineColumn coordinate;
    if (!document_->PositionToLineColumn(caretPosition_, coordinate, errorMessage)) return false;
    return document_->LineRange(coordinate.line, start, end, next, errorMessage);
}

void GtkLargeFileView::MoveHorizontal(bool right, bool extend)
{
    std::uint64_t wanted = caretPosition_;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) return;
        if (!extend && selectionStart_ != selectionEnd_) wanted = right ? std::max(selectionStart_, selectionEnd_) : std::min(selectionStart_, selectionEnd_);
        else if (right && wanted < document_->CharacterCount()) ++wanted;
        else if (!right && wanted > 0) --wanted;
        std::wstring ignored;
        while (wanted > 0 && wanted < document_->CharacterCount() && !document_->IsCharacterBoundary(wanted, ignored)) wanted += right ? 1 : -1;
    }
    if (extend) { selectionStart_ = selectionAnchor_; selectionEnd_ = wanted; }
    else selectionStart_ = selectionEnd_ = selectionAnchor_ = wanted;
    caretPosition_ = wanted;
    EnsureCaretVisible(); NotifyChanged();
}

void GtkLargeFileView::MoveVertical(int lines, bool extend)
{
    std::wstring error;
    LargeDocumentLineColumn coordinate;
    std::unique_lock<std::mutex> lock(documentMutex_);
    if (document_ == nullptr || !document_->PositionToLineColumn(caretPosition_, coordinate, error)) return;
    const std::uint64_t target = lines < 0 && static_cast<std::uint64_t>(-lines) > coordinate.line ? 0 :
        std::min<std::uint64_t>(document_->LineCount() - 1U, coordinate.line + lines);
    std::uint64_t start = 0, end = 0, next = 0;
    if (!document_->LineRange(target, start, end, next, error)) return;
    const std::uint64_t wanted = std::min(end, start + preferredColumn_);
    caretPosition_ = wanted;
    if (extend) { selectionStart_ = selectionAnchor_; selectionEnd_ = wanted; }
    else selectionStart_ = selectionEnd_ = selectionAnchor_ = wanted;
    lock.unlock();
    EnsureCaretVisible(); NotifyChanged();
}

void GtkLargeFileView::MoveLineEdge(bool end, bool document, bool extend)
{
    std::uint64_t wanted = 0;
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) return;
        if (document) wanted = end ? document_->CharacterCount() : 0;
        else {
            LargeDocumentLineColumn coordinate;
            std::uint64_t start = 0, lineEnd = 0, next = 0;
            if (!document_->PositionToLineColumn(caretPosition_, coordinate, error) ||
                !document_->LineRange(coordinate.line, start, lineEnd, next, error)) return;
            wanted = end ? lineEnd : start;
        }
    }
    caretPosition_ = wanted;
    if (extend) { selectionStart_ = selectionAnchor_; selectionEnd_ = wanted; }
    else selectionStart_ = selectionEnd_ = selectionAnchor_ = wanted;
    EnsureCaretVisible(); NotifyChanged();
}

gboolean GtkLargeFileView::KeyPressed(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer userData)
{
    auto* view = static_cast<GtkLargeFileView*>(userData);
    const bool shift = (state & GDK_SHIFT_MASK) != 0;
    const bool control = (state & GDK_CONTROL_MASK) != 0;
    if (keyval == GDK_KEY_Menu || (keyval == GDK_KEY_F10 && shift)) return TRUE;
    switch (keyval) {
    case GDK_KEY_Left: view->MoveHorizontal(false, shift); return TRUE;
    case GDK_KEY_Right: view->MoveHorizontal(true, shift); return TRUE;
    case GDK_KEY_Up: view->MoveVertical(-1, shift); return TRUE;
    case GDK_KEY_Down: view->MoveVertical(1, shift); return TRUE;
    case GDK_KEY_Home: view->MoveLineEdge(false, control, shift); return TRUE;
    case GDK_KEY_End: view->MoveLineEdge(true, control, shift); return TRUE;
    case GDK_KEY_Page_Up: view->MoveVertical(-std::max(1, gtk_widget_get_height(view->drawingArea_) / view->lineHeight_), shift); return TRUE;
    case GDK_KEY_Page_Down: view->MoveVertical(std::max(1, gtk_widget_get_height(view->drawingArea_) / view->lineHeight_), shift); return TRUE;
    case GDK_KEY_BackSpace:
        if (view->selectionStart_ == view->selectionEnd_ && view->caretPosition_ > 0) view->selectionStart_ = view->caretPosition_ - 1U;
        { std::wstring error; view->DeleteSelection(error); }
        return TRUE;
    case GDK_KEY_Delete:
        if (view->selectionStart_ == view->selectionEnd_) view->selectionEnd_ = view->caretPosition_ + 1U;
        { std::wstring error; view->DeleteSelection(error); }
        return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        { std::wstring error; view->ReplaceSelection(L"\r\n", error); }
        return TRUE;
    case GDK_KEY_Tab:
        { std::wstring error; view->ReplaceSelection(L"\t", error); }
        return TRUE;
    default: break;
    }
    if (control && (keyval == GDK_KEY_a || keyval == GDK_KEY_A)) { view->SelectAll(); return TRUE; }
    if (control && (keyval == GDK_KEY_z || keyval == GDK_KEY_Z)) { std::wstring error; view->Undo(error); return TRUE; }
    if (!control && (state & (GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) == 0) {
        const gunichar unicode = gdk_keyval_to_unicode(keyval);
        if (unicode >= 0x20U && g_unichar_isprint(unicode)) {
            char utf8[8]{};
            const int length = g_unichar_to_utf8(unicode, utf8);
            std::wstring error;
            view->ReplaceSelection(WideFromUtf8(std::string(utf8, static_cast<std::size_t>(length))), error);
            return TRUE;
        }
    }
    return FALSE;
}

void GtkLargeFileView::KeyReleased(
    GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer userData)
{
    auto* view = static_cast<GtkLargeFileView*>(userData);
    if (keyval == GDK_KEY_Menu || (keyval == GDK_KEY_F10 && (state & GDK_SHIFT_MASK) != 0)) {
        view->ShowCaretContextMenu();
    }
}

void GtkLargeFileView::EnsureCaretVisible()
{
    std::wstring error;
    LargeDocumentLineColumn coordinate;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || !document_->PositionToLineColumn(caretPosition_, coordinate, error)) return;
    }
    preferredColumn_ = coordinate.column;
    const double visible = std::max(1, gtk_widget_get_height(drawingArea_) / std::max(1, lineHeight_));
    double top = gtk_adjustment_get_value(verticalAdjustment_);
    if (coordinate.line < static_cast<std::uint64_t>(top)) top = static_cast<double>(coordinate.line);
    else if (coordinate.line >= static_cast<std::uint64_t>(top + visible)) top = static_cast<double>(coordinate.line) - visible + 1.0;
    gtk_adjustment_set_value(verticalAdjustment_, top);
}

bool GtkLargeFileView::SetSelection(std::uint64_t start, std::uint64_t end, std::wstring& errorMessage)
{
    std::unique_lock<std::mutex> lock(documentMutex_);
    if (document_ == nullptr || start > document_->CharacterCount() || end > document_->CharacterCount() ||
        !document_->IsCharacterBoundary(start, errorMessage) || !document_->IsCharacterBoundary(end, errorMessage)) return false;
    selectionStart_ = selectionAnchor_ = start;
    selectionEnd_ = caretPosition_ = end;
    lock.unlock();
    EnsureCaretVisible(); NotifyChanged();
    return true;
}

bool GtkLargeFileView::GetSelectedText(std::wstring& text, std::wstring& errorMessage) const
{
    const std::uint64_t start = std::min(selectionStart_, selectionEnd_);
    const std::uint64_t end = std::max(selectionStart_, selectionEnd_);
    if (end - start > kClipboardCharacterLimit) {
        errorMessage = L"Large-file clipboard selections are limited to 16 Mi UTF-16 units.";
        return false;
    }
    std::unique_lock<std::mutex> lock(documentMutex_);
    if (document_ == nullptr || !document_->ReadRange(start, end - start, text, errorMessage)) return false;
    text = PlatformWideFromUtf16Units(text);
    return true;
}

bool GtkLargeFileView::ReplaceSelection(const std::wstring& text, std::wstring& errorMessage)
{
    const std::wstring normalized = NormalizeInsertedText(text);
    const std::uint64_t start = std::min(selectionStart_, selectionEnd_);
    const std::uint64_t end = std::max(selectionStart_, selectionEnd_);
    std::uint64_t afterStart = 0, afterEnd = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || !document_->Replace(start, end, normalized, selectionStart_, selectionEnd_, afterStart, afterEnd, errorMessage)) return false;
        characterCount_.store(document_->CharacterCount());
        lineCount_.store(document_->LineCount());
    }
    selectionStart_ = selectionEnd_ = selectionAnchor_ = caretPosition_ = afterEnd;
    EnsureCaretVisible(); NotifyChanged();
    return true;
}

bool GtkLargeFileView::DeleteSelection(std::wstring& errorMessage)
{
    return ReplaceSelection(L"", errorMessage);
}

bool GtkLargeFileView::Undo(std::wstring& errorMessage)
{
    std::uint64_t start = selectionStart_, end = selectionEnd_;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || !document_->Undo(start, end, errorMessage)) return false;
        characterCount_.store(document_->CharacterCount()); lineCount_.store(document_->LineCount());
    }
    selectionStart_ = selectionAnchor_ = start; selectionEnd_ = caretPosition_ = end;
    EnsureCaretVisible(); NotifyChanged(); return true;
}

void GtkLargeFileView::SelectAll()
{
    std::unique_lock<std::mutex> lock(documentMutex_);
    if (document_ == nullptr) return;
    selectionStart_ = selectionAnchor_ = 0; selectionEnd_ = caretPosition_ = document_->CharacterCount();
    lock.unlock(); NotifyChanged();
}

bool GtkLargeFileView::GoToLine(std::uint64_t oneBasedLine, std::wstring& errorMessage)
{
    if (oneBasedLine == 0) { errorMessage = L"Line numbers start at 1."; return false; }
    std::uint64_t position = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || !document_->LineStart(oneBasedLine - 1U, position, errorMessage)) return false;
    }
    return SetSelection(position, position, errorMessage);
}

bool GtkLargeFileView::Find(const std::wstring& text, bool matchCase, bool wholeWord, bool forward, std::wstring& errorMessage)
{
    LargeDocumentMatch match;
    const std::wstring needle = NormalizeInsertedText(text);
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr || !document_->Find(needle, forward ? selectionEnd_ : selectionStart_, matchCase, wholeWord, forward, match, errorMessage)) return false;
    }
    return SetSelection(match.start, match.start + match.length, errorMessage);
}

bool GtkLargeFileView::ReadRange(std::uint64_t start, std::uint64_t length, std::wstring& text, std::wstring& errorMessage) const
{
    std::lock_guard<std::mutex> lock(documentMutex_);
    if (document_ == nullptr || !document_->ReadRange(start, length, text, errorMessage)) return false;
    text = PlatformWideFromUtf16Units(text);
    return true;
}

std::uint64_t GtkLargeFileView::ReplaceAll(
    const std::wstring& text, const std::wstring& replacement,
    bool matchCase, bool wholeWord, std::wstring& errorMessage)
{
    bool cancelled = false;
    std::uint64_t count = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) return 0;
        count = document_->ReplaceAll(NormalizeInsertedText(text), NormalizeInsertedText(replacement), matchCase, wholeWord,
            cancelled, [] { return false; }, errorMessage);
        characterCount_.store(document_->CharacterCount());
        lineCount_.store(document_->LineCount());
    }
    if (count != 0) {
        selectionStart_ = selectionEnd_ = selectionAnchor_ = caretPosition_ = 0;
        NotifyChanged();
    }
    return count;
}

bool GtkLargeFileView::SaveTo(const std::wstring& path, std::wstring& errorMessage)
{
    if (!indexingComplete_.load()) {
        if (indexThread_.joinable()) indexThread_.join();
        if (!encodingValid_.load()) {
            errorMessage = L"The large file contains invalid or unsupported encoded text.";
            return false;
        }
        if (!indexingComplete_.load()) {
            errorMessage = L"Large-file preparation did not complete, so the file was not saved.";
            return false;
        }
    }
    std::unique_ptr<LargeDocument> snapshot;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ == nullptr) return false;
        snapshot = document_->Clone();
        generation = document_->Generation();
    }
    const std::string destination = Utf8FromWide(path);
    if (destination.empty()) { errorMessage = L"The save path is empty."; return false; }
    if (g_file_test(destination.c_str(), G_FILE_TEST_IS_DIR)) {
        errorMessage = L"Please choose a file name inside the destination folder.";
        return false;
    }
    std::string temporary = destination + ".classic-notepad-save-XXXXXX";
    std::vector<char> name(temporary.begin(), temporary.end());
    name.push_back('\0');
    const int output = mkstemp(name.data());
    if (output < 0) { errorMessage = L"A sibling temporary save file could not be created."; return false; }
    bool okay = true;
    auto writeAll = [&](const std::uint8_t* data, std::size_t size) {
        std::size_t written = 0;
        while (written < size) {
            const ssize_t amount = write(output, data + written, size - written);
            if (amount <= 0) return false;
            written += static_cast<std::size_t>(amount);
        }
        return true;
    };
    if (encoding_ == LargeFileEncoding::Utf16Le) {
        const std::uint8_t bom[] = {0xFFU, 0xFEU};
        okay = writeAll(bom, sizeof(bom));
    } else if (dataOffset_ == 3U) {
        const std::uint8_t bom[] = {0xEFU, 0xBBU, 0xBFU};
        okay = writeAll(bom, sizeof(bom));
    }
    bool pendingCr = false;
    std::wstring normalized;
    const TextEncoding targetEncoding = encoding_ == LargeFileEncoding::Utf16Le
        ? TextEncoding::Utf16LeBom : TextEncoding::Utf8NoBom;
    if (okay) {
        okay = snapshot->StreamText([&](const std::wstring& chunk) {
            normalized.clear();
            normalized.reserve(chunk.size() + 32U);
            for (wchar_t character : chunk) {
                if (pendingCr) {
                    normalized += L"\r\n";
                    pendingCr = false;
                    if (character == L'\n') continue;
                }
                if (character == L'\r') pendingCr = true;
                else if (character == L'\n') normalized += L"\r\n";
                else normalized.push_back(character);
            }
            std::vector<std::uint8_t> encoded;
            std::wstring encodeError;
            if (!EncodeTextChunk(PlatformWideFromUtf16Units(normalized), targetEncoding, false, encoded, encodeError) ||
                !writeAll(encoded.data(), encoded.size())) return false;
            return true;
        }, [] { return false; }, errorMessage);
    }
    if (okay && pendingCr) {
        const std::wstring ending = L"\r\n";
        std::vector<std::uint8_t> encoded;
        okay = EncodeTextChunk(ending, targetEncoding, false, encoded, errorMessage) && writeAll(encoded.data(), encoded.size());
    }
    int failureError = 0;
    if (okay && fsync(output) != 0) {
        failureError = errno;
        okay = false;
    }
    if (close(output) != 0) {
        if (failureError == 0) failureError = errno;
        okay = false;
    }
    if (okay) {
        bool renamed = false;
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (rename(name.data(), destination.c_str()) == 0) {
                renamed = true;
                break;
            }
            failureError = errno;
            if (failureError != EACCES && failureError != EBUSY && failureError != EPERM && failureError != ETXTBSY) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        okay = renamed;
    }
    if (!okay) {
        for (int attempt = 0; attempt < 20 && unlink(name.data()) != 0; ++attempt) {
            if (errno == ENOENT) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (errorMessage.empty()) {
            errorMessage = L"The complete temporary save could not replace the destination";
            if (failureError != 0) {
                errorMessage += L": ";
                errorMessage += WideFromUtf8(std::strerror(failureError));
            }
            errorMessage += L".";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(documentMutex_);
        if (document_ != nullptr && document_->Generation() == generation) document_->MarkSaved();
    }
    path_ = path;
    NotifyChanged();
    return true;
}

} // namespace classic_notepad::linux_ui
