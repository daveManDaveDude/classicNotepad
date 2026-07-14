#include "mac_large_file_view.h"

#include "encoding.h"
#include "large_document.h"
#include "line_endings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr std::size_t kInitialScanBytes = 1024U * 1024U;
constexpr std::size_t kIndexChunkBytes = 8U * 1024U * 1024U;
constexpr std::uint64_t kTargetPieceBytes = 64ULL * 1024ULL;
constexpr std::uint64_t kMaximumRenderedLine = 64ULL * 1024ULL;
constexpr std::uint64_t kClipboardLimit = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kActivationThreshold = 48ULL * 1024ULL * 1024ULL;

std::uint64_t NowMilliseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string Utf8Path(const std::wstring& path)
{
    if (path.empty()) return {};
    NSString* value = [[NSString alloc] initWithBytes:path.data() length:path.size() * sizeof(wchar_t)
        encoding:NSUTF32LittleEndianStringEncoding];
    return value == nil ? std::string() : std::string([value fileSystemRepresentation]);
}

NSString* StringFromUtf16Units(const std::wstring& units)
{
    if (units.empty()) return @"";
    std::vector<unichar> characters;
    characters.reserve(units.size());
    for (wchar_t character : units) characters.push_back(static_cast<unichar>(character));
    return [[NSString alloc] initWithCharacters:characters.data() length:characters.size()];
}

std::wstring Utf16UnitsFromString(NSString* string)
{
    if (string == nil || [string length] == 0) return {};
    std::vector<unichar> characters([string length]);
    [string getCharacters:characters.data() range:NSMakeRange(0, characters.size())];
    std::wstring result;
    result.reserve(characters.size());
    for (unichar character : characters) result.push_back(static_cast<wchar_t>(character));
    return result;
}

std::wstring PlatformWideFromUtf16(const std::wstring& units)
{
    std::wstring result;
    result.reserve(units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        const std::uint32_t first = static_cast<std::uint32_t>(units[i]);
        if (first >= 0xD800U && first <= 0xDBFFU && i + 1 < units.size()) {
            const std::uint32_t second = static_cast<std::uint32_t>(units[i + 1]);
            if (second >= 0xDC00U && second <= 0xDFFFU) {
                result.push_back(static_cast<wchar_t>(0x10000U + ((first - 0xD800U) << 10U) + second - 0xDC00U));
                ++i;
                continue;
            }
        }
        result.push_back(units[i]);
    }
    return result;
}

std::wstring NormalizeInserted(NSString* text)
{
    // AppKit normally supplies LF for Return/paste. Preserve existing CRLF and
    // lone CR while normalising lone LF to the editor's CRLF representation.
    std::wstring source = Utf16UnitsFromString(text);
    std::wstring result;
    result.reserve(source.size() + 16U);
    for (std::size_t index = 0; index < source.size(); ++index) {
        if (source[index] == L'\r') {
            result.push_back(L'\r');
            if (index + 1U < source.size() && source[index + 1U] == L'\n') {
                result.push_back(L'\n');
                ++index;
            }
        } else if (source[index] == L'\n') {
            result.push_back(L'\r');
            result.push_back(L'\n');
        } else {
            result.push_back(source[index]);
        }
    }
    return result;
}

NSCursor* SavingCursor()
{
    static NSCursor* cursor = nil;
    if (cursor == nil) {
        NSImage* image = [NSImage imageWithSystemSymbolName:@"progress.indicator"
                                  accessibilityDescription:@"Saving"];
        cursor = [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(8.0, 8.0)];
    }
    return cursor;
}

std::uint64_t VisualColumnForUnits(const std::wstring& text, std::uint64_t unitCount)
{
    std::uint64_t column = 0;
    const std::uint64_t end = std::min<std::uint64_t>(unitCount, text.size());
    for (std::uint64_t index = 0; index < end; ++index) {
        column += text[static_cast<std::size_t>(index)] == L'\t' ? 8U - (column % 8U) : 1U;
    }
    return column;
}

std::uint64_t UnitOffsetForVisualColumn(const std::wstring& text, std::uint64_t wanted)
{
    std::uint64_t column = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const std::uint64_t advance = text[index] == L'\t' ? 8U - (column % 8U) : 1U;
        if (wanted < column + (advance + 1U) / 2U) return static_cast<std::uint64_t>(index);
        column += advance;
    }
    return static_cast<std::uint64_t>(text.size());
}

bool IsWordUnit(wchar_t character)
{
    return std::iswalnum(static_cast<wint_t>(character)) != 0 || character == L'_';
}
}

@implementation MacLargeFileView {
    std::wstring _path;
    int _fd;
    const std::uint8_t* _mapping;
    std::uint64_t _fileSize;
    classic_notepad::LargeFileEncoding _encoding;
    std::uint64_t _dataOffset;
    std::unique_ptr<classic_notepad::LargeDocument> _document;
    std::unique_ptr<classic_notepad::LargeFileLineScanner> _scanner;
    std::mutex _mutex;
    std::thread _indexThread;
    std::atomic<bool> _cancel;
    std::atomic<bool> _indexingComplete;
    std::atomic<bool> _encodingValid;
    std::atomic<std::uint64_t> _indexedBytes;
    std::atomic<std::uint64_t> _lineCount;
    std::atomic<std::uint64_t> _characterCount;
    std::atomic<std::uint64_t> _maximumLineColumns;
    std::atomic<std::uint64_t> _firstUsableMs;
    std::atomic<std::uint64_t> _firstPaintMs;
    std::atomic<std::uint64_t> _fullIndexMs;
    std::uint64_t _startedMs;
    std::uint64_t _appendedByte;
    std::uint64_t _appendedCharacter;
    std::uint64_t _appendedBreaks;
    std::uint64_t _scannedOffset;
    std::uint64_t _selectionStart;
    std::uint64_t _selectionEnd;
    std::uint64_t _selectionAnchor;
    std::uint64_t _caret;
    std::uint64_t _preferredColumn;
    BOOL _dragging;
    BOOL _wordWrap;
    std::uint64_t _contextWordStart;
    std::uint64_t _contextWordEnd;
    NSFont* _font;
    CGFloat _lineHeight;
    CGFloat _characterWidth;
    NSTimer* _progressTimer;
}

+ (BOOL)shouldUseForPath:(const std::wstring&)path size:(std::uint64_t&)size error:(std::wstring&)error
{
    struct stat info {};
    const std::string native = Utf8Path(path);
    if (native.empty() || stat(native.c_str(), &info) != 0 || info.st_size < 0) {
        error = L"The file size could not be determined.";
        size = 0;
        return NO;
    }
    size = static_cast<std::uint64_t>(info.st_size);
    error.clear();
    return size >= kActivationThreshold;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        _fd = -1;
        _mapping = nullptr;
        _encoding = classic_notepad::LargeFileEncoding::Utf8;
        _font = [[NSFont userFixedPitchFontOfSize:13.0] retain];
        _lineHeight = ceil([_font ascender] - [_font descender] + [_font leading]) + 2.0;
        _characterWidth = ceil([@"M" sizeWithAttributes:@{NSFontAttributeName:_font}].width);
        [self setAccessibilityElement:YES];
        [self setAccessibilityRole:NSAccessibilityTextAreaRole];
        [self setAccessibilityLabel:@"Large document editor"];
    }
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder { [self setNeedsDisplay:YES]; return YES; }
- (BOOL)resignFirstResponder { [self setNeedsDisplay:YES]; return YES; }
- (BOOL)isOpaque { return YES; }

- (void)viewDidMoveToSuperview
{
    [super viewDidMoveToSuperview];
    NSClipView* clip=self.enclosingScrollView.contentView;
    if(clip!=nil){[clip setPostsBoundsChangedNotifications:YES];
        [[NSNotificationCenter defaultCenter] removeObserver:self name:NSViewBoundsDidChangeNotification object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(clipBoundsChanged:) name:NSViewBoundsDidChangeNotification object:clip];}
}

- (void)clipBoundsChanged:(NSNotification*)notification
{
    (void)notification;
    [self setNeedsDisplayInRect:self.visibleRect];
}

- (BOOL)copyBytes:(std::uint64_t)offset length:(std::size_t)length output:(std::vector<std::uint8_t>&)bytes
{
    bytes.clear();
    if (_mapping == nullptr || offset > _fileSize || length > _fileSize - offset) return NO;
    bytes.assign(_mapping + static_cast<std::size_t>(offset), _mapping + static_cast<std::size_t>(offset) + length);
    return YES;
}

- (wchar_t)boundaryCharacter:(std::uint64_t)offset
{
    if (offset >= _fileSize) return 0;
    if (_encoding == classic_notepad::LargeFileEncoding::Utf16Le) {
        if (offset + 1 >= _fileSize) return 0;
        const wchar_t c = static_cast<wchar_t>(_mapping[offset] | (static_cast<std::uint16_t>(_mapping[offset + 1]) << 8U));
        return c == L'\r' || c == L'\n' ? c : wchar_t(1);
    }
    const wchar_t c = static_cast<wchar_t>(_mapping[offset]);
    return c == L'\r' || c == L'\n' ? c : wchar_t(1);
}

- (BOOL)appendPieceToByte:(std::uint64_t)endByte character:(std::uint64_t)endCharacter breaks:(std::uint64_t)breaks error:(std::wstring&)error
{
    if (endByte <= _appendedByte) return YES;
    const std::uint64_t unit = _encoding == classic_notepad::LargeFileEncoding::Utf16Le ? 2 : 1;
    classic_notepad::LargeDocumentOriginalPiece piece{
        _appendedByte, endByte - _appendedByte, endCharacter - _appendedCharacter, breaks - _appendedBreaks,
        [self boundaryCharacter:_appendedByte], [self boundaryCharacter:endByte - unit], true};
    std::vector<classic_notepad::LargeDocumentOriginalPiece> pieces{piece};
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_document) {
        auto model = std::make_unique<classic_notepad::LargeDocument>(_encoding,
            [self](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& bytes) {
                return [self copyBytes:offset length:length output:bytes] == YES;
            });
        if (!model->Initialize(pieces, error)) return NO;
        _document = std::move(model);
    } else if (!_document->AppendOriginalPieces(pieces, error)) return NO;
    _appendedByte = endByte;
    _appendedCharacter = endCharacter;
    _appendedBreaks = breaks;
    _characterCount.store(endCharacter);
    _lineCount.store(breaks + 1);
    return YES;
}

- (BOOL)appendOffsets:(const std::vector<std::uint64_t>&)bytes characters:(const std::vector<std::uint64_t>&)characters before:(std::uint64_t)before tail:(BOOL)tail error:(std::wstring&)error
{
    if (bytes.size() != characters.size()) return NO;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] >= _appendedByte + kTargetPieceBytes && ![self appendPieceToByte:bytes[i] character:characters[i] breaks:before + i + 1 error:error]) return NO;
    }
    if (!bytes.empty() && ![self appendPieceToByte:bytes.back() character:characters.back() breaks:before + bytes.size() error:error]) return NO;
    if (tail && _appendedByte < _fileSize) {
        const auto breaks = _scanner->CrlfCount() + _scanner->LfCount() + _scanner->CrCount();
        if (![self appendPieceToByte:_fileSize character:_scanner->CharacterCount() breaks:breaks error:error]) return NO;
    }
    return YES;
}

- (BOOL)consumeOffset:(std::uint64_t)offset length:(std::size_t)length error:(std::wstring&)error
{
    std::vector<std::uint64_t> bytes, characters;
    const auto before = _scanner->CrlfCount() + _scanner->LfCount() + _scanner->CrCount();
    if (length && !_scanner->Consume(_mapping + static_cast<std::size_t>(offset), length, offset, bytes, &characters)) {
        error = L"The large file contains invalid text encoding.";
        return NO;
    }
    _scannedOffset = offset + length;
    _indexedBytes.store(_scannedOffset);
    _maximumLineColumns.store(_scanner->MaximumLineColumns());
    return [self appendOffsets:bytes characters:characters before:before tail:NO error:error];
}

- (BOOL)openPath:(const std::wstring&)path error:(std::wstring&)error
{
    [self closeDocument];
    _path = path;
    const std::string native = Utf8Path(path);
    _fd = open(native.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat info {};
    if (_fd < 0 || fstat(_fd, &info) != 0 || info.st_size <= 0) { error = L"The large file could not be opened."; [self closeDocument]; return NO; }
    _fileSize = static_cast<std::uint64_t>(info.st_size);
    void* mapped = mmap(nullptr, static_cast<std::size_t>(_fileSize), PROT_READ, MAP_PRIVATE, _fd, 0);
    if (mapped == MAP_FAILED) { _mapping = nullptr; error = L"The large file could not be memory mapped."; [self closeDocument]; return NO; }
    _mapping = static_cast<const std::uint8_t*>(mapped);
    madvise(const_cast<std::uint8_t*>(_mapping), static_cast<std::size_t>(_fileSize), MADV_SEQUENTIAL);
    const auto detected = classic_notepad::DetectLargeFileEncoding(_mapping, static_cast<std::size_t>(std::min<std::uint64_t>(_fileSize, 4)));
    if (detected.encoding == classic_notepad::LargeFileEncoding::Unsupported) { error = L"Large-file mode supports UTF-8/ASCII and UTF-16 LE with BOM."; [self closeDocument]; return NO; }
    _encoding = detected.encoding; _dataOffset = detected.dataOffset; _appendedByte = _scannedOffset = _dataOffset;
    _appendedCharacter = _appendedBreaks = _selectionStart = _selectionEnd = _selectionAnchor = _caret = 0;
    _scanner = std::make_unique<classic_notepad::LargeFileLineScanner>(_encoding, _dataOffset);
    _cancel.store(false); _indexingComplete.store(false); _encodingValid.store(true); _indexedBytes.store(_dataOffset);
    _lineCount.store(1); _characterCount.store(0); _maximumLineColumns.store(0);
    _firstUsableMs.store(0); _firstPaintMs.store(0); _fullIndexMs.store(0); _startedMs = NowMilliseconds();
    const std::size_t initial = static_cast<std::size_t>(std::min<std::uint64_t>(kInitialScanBytes, _fileSize - _dataOffset));
    if (![self consumeOffset:_dataOffset length:initial error:error] || !_document) { [self closeDocument]; return NO; }
    _firstUsableMs.store(NowMilliseconds() - _startedMs);
    [self refreshFrame];
    _progressTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 target:self selector:@selector(indexProgress:) userInfo:nil repeats:YES];
    _indexThread = std::thread([self] {
        std::wstring workerError;
        while (!_cancel.load() && _scannedOffset < _fileSize) {
            const std::uint64_t offset = _scannedOffset;
            const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(kIndexChunkBytes, _fileSize - offset));
            if (![self consumeOffset:offset length:length error:workerError]) { _encodingValid.store(false); break; }
        }
        if (!_cancel.load() && _encodingValid.load()) {
            std::vector<std::uint64_t> bytes, characters;
            const auto before = _scanner->CrlfCount() + _scanner->LfCount() + _scanner->CrCount();
            if (!_scanner->Finish(bytes, &characters) || ![self appendOffsets:bytes characters:characters before:before tail:YES error:workerError]) _encodingValid.store(false);
            else {
                _characterCount.store(_scanner->CharacterCount());
                _lineCount.store(_scanner->CrlfCount() + _scanner->LfCount() + _scanner->CrCount() + 1);
                _maximumLineColumns.store(_scanner->MaximumLineColumns());
                _indexedBytes.store(_fileSize); _indexingComplete.store(true);
            }
        }
        _fullIndexMs.store(NowMilliseconds() - _startedMs);
    });
    return YES;
}

- (void)indexProgress:(NSTimer*)timer
{
    (void)timer;
    [self refreshFrame]; [self setNeedsDisplay:YES];
    [self.changeDelegate largeFileViewDidChange:self];
    if (_indexingComplete.load() || !_encodingValid.load()) { [_progressTimer invalidate]; _progressTimer = nil; }
}

- (void)refreshFrame
{
    const CGFloat width = _wordWrap ? self.enclosingScrollView.contentSize.width
        : std::max<CGFloat>(self.enclosingScrollView.contentSize.width, 12.0 + _maximumLineColumns.load() * _characterWidth);
    const CGFloat height = std::max<CGFloat>(self.enclosingScrollView.contentSize.height, 8.0 + _lineCount.load() * _lineHeight);
    [self setFrameSize:NSMakeSize(width, height)];
}

- (void)closeDocument
{
    [_progressTimer invalidate]; _progressTimer = nil; _cancel.store(true);
    if (_indexThread.joinable()) _indexThread.join();
    { std::lock_guard<std::mutex> lock(_mutex); _document.reset(); }
    _scanner.reset();
    if (_mapping) { munmap(const_cast<std::uint8_t*>(_mapping), static_cast<std::size_t>(_fileSize)); _mapping = nullptr; }
    if (_fd >= 0) { close(_fd); _fd = -1; }
    _fileSize = 0;
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; [self closeDocument]; [_font release]; [super dealloc]; }

- (void)drawRect:(NSRect)dirtyRect
{
    [[NSColor textBackgroundColor] setFill]; NSRectFill(dirtyRect);
    const NSRect visible = [self visibleRect];
    const std::uint64_t firstLine = static_cast<std::uint64_t>(std::max<CGFloat>(0, floor(visible.origin.y / _lineHeight)));
    const std::uint64_t lastLine = std::min<std::uint64_t>(_lineCount.load(), static_cast<std::uint64_t>(ceil(NSMaxY(visible) / _lineHeight)) + 1);
    NSDictionary* attrs = @{NSFontAttributeName:_font, NSForegroundColorAttributeName:[NSColor textColor]};
    const auto selStart = std::min(_selectionStart, _selectionEnd), selEnd = std::max(_selectionStart, _selectionEnd);
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_document) return;
    for (std::uint64_t line = firstLine; line < lastLine; ++line) {
        std::uint64_t start = 0, end = 0, next = 0; std::wstring error;
        if (!_document->LineRange(line, start, end, next, error)) break;
        std::wstring units;
        if (!_document->ReadRange(start, std::min<std::uint64_t>(end - start, kMaximumRenderedLine), units, error)) continue;
        const CGFloat y = 4.0 + line * _lineHeight;
        if (selEnd > start && selStart < end) {
            const auto left = std::max(selStart, start) - start, right = std::min(selEnd, end) - start;
            const auto leftColumn = VisualColumnForUnits(units, left);
            const auto rightColumn = VisualColumnForUnits(units, right);
            [[NSColor selectedTextBackgroundColor] setFill];
            NSRectFill(NSMakeRect(6.0 + leftColumn * _characterWidth, y,
                std::max<CGFloat>(2.0, (rightColumn-leftColumn) * _characterWidth), _lineHeight));
        }
        NSString* lineString = StringFromUtf16Units(units);
        [lineString drawAtPoint:NSMakePoint(6.0, y) withAttributes:attrs];

        // Spell only the bounded visible row. This keeps checker input and
        // underline geometry independent of total document size.
        NSSpellChecker* checker = [NSSpellChecker sharedSpellChecker];
        NSInteger spellingOffset = 0;
        while (spellingOffset < static_cast<NSInteger>(lineString.length)) {
            NSRange misspelling = [checker checkSpellingOfString:lineString
                                                     startingAt:spellingOffset
                                                       language:[checker language]
                                                           wrap:NO
                                         inSpellDocumentWithTag:0
                                                      wordCount:nil];
            if (misspelling.location == NSNotFound) break;
            const auto leftColumn = VisualColumnForUnits(units, misspelling.location);
            const auto rightColumn = VisualColumnForUnits(units, misspelling.location + misspelling.length);
            const CGFloat underlineY = y + _lineHeight - 2.0;
            NSBezierPath* wave = [NSBezierPath bezierPath];
            [wave setLineWidth:1.0];
            BOOL up = YES;
            [wave moveToPoint:NSMakePoint(6.0 + leftColumn * _characterWidth, underlineY)];
            for (CGFloat x = 6.0 + leftColumn * _characterWidth + 2.0;
                 x <= 6.0 + rightColumn * _characterWidth; x += 2.0) {
                [wave lineToPoint:NSMakePoint(x, underlineY + (up ? -1.5 : 1.5))];
                up = !up;
            }
            [[NSColor systemRedColor] setStroke];
            [wave stroke];
            spellingOffset = static_cast<NSInteger>(NSMaxRange(misspelling));
        }
        if ([[self window] firstResponder] == self && _caret >= start && _caret <= end && _selectionStart == _selectionEnd) {
            const auto caretColumn = VisualColumnForUnits(units, _caret - start);
            [[NSColor textColor] setFill]; NSRectFill(NSMakeRect(6.0 + caretColumn*_characterWidth, y, 1.5, _lineHeight));
        }
    }
    std::uint64_t expected = 0;
    _firstPaintMs.compare_exchange_strong(expected,
        std::max<std::uint64_t>(1, NowMilliseconds() - _startedMs));
}

- (BOOL)hitPoint:(NSPoint)point position:(std::uint64_t&)position
{
    const std::uint64_t line = std::min<std::uint64_t>(_lineCount.load()-1, static_cast<std::uint64_t>(std::max<CGFloat>(0, floor(point.y/_lineHeight))));
    const std::uint64_t column = static_cast<std::uint64_t>(std::max<CGFloat>(0, floor((point.x-6.0+_characterWidth/2)/_characterWidth)));
    std::lock_guard<std::mutex> lock(_mutex); std::wstring error; std::uint64_t start=0,end=0,next=0;
    if (!_document || !_document->LineRange(line,start,end,next,error)) return NO;
    std::wstring units;
    if (!_document->ReadRange(start, std::min<std::uint64_t>(end-start, kMaximumRenderedLine), units, error)) return NO;
    position = std::min(end, start + UnitOffsetForVisualColumn(units, column)); return YES;
}

- (void)mouseDown:(NSEvent*)event
{
    [[self window] makeFirstResponder:self]; std::uint64_t position=0;
    if ([self hitPoint:[self convertPoint:event.locationInWindow fromView:nil] position:position]) {
        const BOOL shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
        if (!shift) _selectionAnchor = position;
        _selectionStart = shift ? _selectionAnchor : position; _selectionEnd = _caret = position; _dragging = YES; [self setNeedsDisplay:YES];
    }
}
- (void)mouseDragged:(NSEvent*)event
{
    if (!_dragging) return; std::uint64_t position=0;
    [self autoscroll:event];
    if ([self hitPoint:[self convertPoint:event.locationInWindow fromView:nil] position:position]) {
        _selectionStart=_selectionAnchor; _selectionEnd=_caret=position; [self scrollCaretVisible]; [self changed];
    }
}
- (void)mouseUp:(NSEvent*)event { (void)event; _dragging=NO; [self changed]; }

- (void)moveHorizontal:(BOOL)right byWord:(BOOL)byWord extend:(BOOL)extend
{
    {
        std::lock_guard<std::mutex> lock(_mutex); if (!_document) return;
        std::uint64_t wanted=_caret;
        if (!extend && _selectionStart!=_selectionEnd) {
            wanted=right?std::max(_selectionStart,_selectionEnd):std::min(_selectionStart,_selectionEnd);
        } else if (!byWord) {
            if (right && wanted<_document->CharacterCount()) ++wanted; else if (!right && wanted) --wanted;
        } else {
            std::wstring error; wchar_t character=0;
            if (right) {
                while(wanted<_document->CharacterCount()&&_document->CharacterAt(wanted,character,error)&&IsWordUnit(character))++wanted;
                while(wanted<_document->CharacterCount()&&_document->CharacterAt(wanted,character,error)&&!IsWordUnit(character))++wanted;
            } else {
                while(wanted&&_document->CharacterAt(wanted-1U,character,error)&&!IsWordUnit(character))--wanted;
                while(wanted&&_document->CharacterAt(wanted-1U,character,error)&&IsWordUnit(character))--wanted;
            }
        }
        _caret=wanted; if (extend) {_selectionStart=_selectionAnchor;_selectionEnd=wanted;} else _selectionStart=_selectionEnd=_selectionAnchor=wanted;
    }
    [self scrollCaretVisible]; [self changed];
}
- (void)moveVertical:(NSInteger)delta extend:(BOOL)extend
{
    {
        std::lock_guard<std::mutex> lock(_mutex); if (!_document) return; std::wstring error; classic_notepad::LargeDocumentLineColumn c;
        if (!_document->PositionToLineColumn(_caret,c,error)) return;
        std::uint64_t target=c.line;
        if(delta<0)target=static_cast<std::uint64_t>(-delta)>target?0U:target-static_cast<std::uint64_t>(-delta);
        else target=std::min<std::uint64_t>(_document->LineCount()-1,target+static_cast<std::uint64_t>(delta));
        std::uint64_t start=0,end=0,next=0; if (!_document->LineRange(target,start,end,next,error)) return;
        _caret=std::min(end,start+_preferredColumn); if(extend){_selectionStart=_selectionAnchor;_selectionEnd=_caret;}else _selectionStart=_selectionEnd=_selectionAnchor=_caret;
    }
    [self scrollCaretVisible]; [self changed];
}
- (void)moveEdge:(BOOL)end document:(BOOL)document extend:(BOOL)extend
{
    {
        std::lock_guard<std::mutex> lock(_mutex); if(!_document)return; std::uint64_t wanted=0; std::wstring error;
        if(document) wanted=end?_document->CharacterCount():0; else {classic_notepad::LargeDocumentLineColumn c;std::uint64_t s=0,e=0,n=0;if(!_document->PositionToLineColumn(_caret,c,error)||!_document->LineRange(c.line,s,e,n,error))return;wanted=end?e:s;}
        _caret=wanted;if(extend){_selectionStart=_selectionAnchor;_selectionEnd=wanted;}else _selectionStart=_selectionEnd=_selectionAnchor=wanted;
    }
    [self scrollCaretVisible];[self changed];
}

- (void)keyDown:(NSEvent*)event
{
    const NSEventModifierFlags flags=event.modifierFlags; const BOOL shift=(flags&NSEventModifierFlagShift)!=0; const BOOL command=(flags&NSEventModifierFlagCommand)!=0; const BOOL option=(flags&NSEventModifierFlagOption)!=0;
    NSString* chars=event.charactersIgnoringModifiers; unichar c=chars.length?[chars characterAtIndex:0]:0;
    if(command){ if(c=='a'){[self selectAll:nil];return;} if(c=='c'){[self copy:nil];return;} if(c=='x'){[self cut:nil];return;} if(c=='v'){[self paste:nil];return;} if(c=='z'){[self undo:nil];return;} if(c==NSUpArrowFunctionKey){[self moveEdge:NO document:YES extend:shift];return;} if(c==NSDownArrowFunctionKey){[self moveEdge:YES document:YES extend:shift];return;} return; }
    switch(c){case NSLeftArrowFunctionKey:[self moveHorizontal:NO byWord:option extend:shift];return;case NSRightArrowFunctionKey:[self moveHorizontal:YES byWord:option extend:shift];return;case NSUpArrowFunctionKey:[self moveVertical:-1 extend:shift];return;case NSDownArrowFunctionKey:[self moveVertical:1 extend:shift];return;case NSHomeFunctionKey:[self moveEdge:NO document:NO extend:shift];return;case NSEndFunctionKey:[self moveEdge:YES document:NO extend:shift];return;case NSPageUpFunctionKey:[self moveVertical:-std::max<NSInteger>(1,self.visibleRect.size.height/_lineHeight) extend:shift];return;case NSPageDownFunctionKey:[self moveVertical:std::max<NSInteger>(1,self.visibleRect.size.height/_lineHeight) extend:shift];return;case NSDeleteCharacter:[self deleteBackward:nil];return;case NSDeleteFunctionKey:[self deleteForward:nil];return;case '\r':case '\n':[self insertText:@"\n" replacementRange:NSMakeRange(NSNotFound,0)];return;case '\t':[self insertText:@"\t" replacementRange:NSMakeRange(NSNotFound,0)];return;default:break;}
    if((flags&(NSEventModifierFlagControl|NSEventModifierFlagOption))==0 && event.characters.length) [self insertText:event.characters replacementRange:NSMakeRange(NSNotFound,0)];
}

- (void)insertText:(id)string replacementRange:(NSRange)range { (void)range; std::wstring error; [self replaceSelectionWithWideText:NormalizeInserted([string description]) error:error]; }
- (void)deleteBackward:(id)sender { (void)sender; if(_selectionStart==_selectionEnd&&_caret){_selectionStart=_caret-1;} std::wstring e;[self replaceSelectionWithWideText:L"" error:e]; }
- (void)deleteForward:(id)sender { (void)sender; if(_selectionStart==_selectionEnd&&_caret<[self characterCount])_selectionEnd=_caret+1;std::wstring e;[self replaceSelectionWithWideText:L"" error:e]; }
- (void)selectAll:(id)sender { (void)sender;BOOL selected=NO;{std::lock_guard<std::mutex>lock(_mutex);if(_document){_selectionStart=_selectionAnchor=0;_selectionEnd=_caret=_document->CharacterCount();selected=YES;}}if(selected)[self changed]; }
- (void)copy:(id)sender { (void)sender;std::wstring error,text=[self selectedWideText:error];if(error.empty()){NSPasteboard* p=[NSPasteboard generalPasteboard];[p clearContents];[p setString:StringFromUtf16Units(text) forType:NSPasteboardTypeString];}else{NSBeep();} }
- (void)cut:(id)sender { [self copy:sender]; if(_selectionStart!=_selectionEnd){std::wstring e;[self replaceSelectionWithWideText:L"" error:e];} }
- (void)paste:(id)sender { (void)sender;NSString* text=[[NSPasteboard generalPasteboard]stringForType:NSPasteboardTypeString];if(text){std::wstring e;[self replaceSelectionWithWideText:NormalizeInserted(text) error:e];} }
- (void)undo:(id)sender { (void)sender;BOOL undone=NO;{std::lock_guard<std::mutex>lock(_mutex);if(!_document)return;std::wstring e;std::uint64_t s=_selectionStart,n=_selectionEnd;if(_document->Undo(s,n,e)){_selectionStart=_selectionAnchor=s;_selectionEnd=_caret=n;_lineCount.store(_document->LineCount());_characterCount.store(_document->CharacterCount());undone=YES;}}if(undone){[self scrollCaretVisible];[self changed];} }

- (NSMenu*)menuForEvent:(NSEvent*)event
{
    std::uint64_t position=0;
    if (![self hitPoint:[self convertPoint:event.locationInWindow fromView:nil] position:position]) return nil;
    if (position < std::min(_selectionStart,_selectionEnd) || position > std::max(_selectionStart,_selectionEnd)) {
        _selectionStart=_selectionEnd=_selectionAnchor=_caret=position;
    }
    _contextWordStart=position; _contextWordEnd=position;
    std::wstring word,error;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_document) {
            wchar_t character=0;
            while(_contextWordStart>0 && position-_contextWordStart<256U &&
                  _document->CharacterAt(_contextWordStart-1U,character,error) && IsWordUnit(character))--_contextWordStart;
            while(_contextWordEnd<_document->CharacterCount() && _contextWordEnd-position<256U &&
                  _document->CharacterAt(_contextWordEnd,character,error) && IsWordUnit(character))++_contextWordEnd;
            if(_contextWordEnd>_contextWordStart)_document->ReadRange(_contextWordStart,_contextWordEnd-_contextWordStart,word,error);
        }
    }
    NSMenu* menu=[[NSMenu alloc] initWithTitle:@""];
    if(!word.empty()) {
        NSString* spelling=StringFromUtf16Units(word);
        NSArray<NSString*>* guesses=[[NSSpellChecker sharedSpellChecker]
            guessesForWordRange:NSMakeRange(0,spelling.length) inString:spelling
            language:[[NSSpellChecker sharedSpellChecker] language] inSpellDocumentWithTag:0];
        NSUInteger count=0;
        for(NSString* guess in guesses) {
            if(count++>=5U)break;
            NSMenuItem* item=[menu addItemWithTitle:guess action:@selector(replaceContextSpelling:) keyEquivalent:@""];
            [item setTarget:self]; [item setRepresentedObject:guess];
        }
        if(count)[menu addItem:[NSMenuItem separatorItem]];
    }
    NSMenuItem* undo=[menu addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@""];[undo setTarget:self];
    [menu addItem:[NSMenuItem separatorItem]];
    for(NSArray* specification in @[@[@"Cut",NSStringFromSelector(@selector(cut:))],@[@"Copy",NSStringFromSelector(@selector(copy:))],@[@"Paste",NSStringFromSelector(@selector(paste:))],@[@"Delete",NSStringFromSelector(@selector(deleteForward:))],@[@"Select All",NSStringFromSelector(@selector(selectAll:))]]) {
        NSMenuItem* item=[menu addItemWithTitle:specification[0] action:NSSelectorFromString(specification[1]) keyEquivalent:@""];
        [item setTarget:self];
    }
    return menu;
}

- (void)replaceContextSpelling:(NSMenuItem*)sender
{
    std::wstring error;
    if([self setSelectionStart:_contextWordStart end:_contextWordEnd error:error])
        [self replaceSelectionWithWideText:Utf16UnitsFromString([sender representedObject]) error:error];
}

- (void)scrollCaretVisible
{
    std::lock_guard<std::mutex> lock(_mutex);if(!_document)return;std::wstring error;classic_notepad::LargeDocumentLineColumn c;if(!_document->PositionToLineColumn(_caret,c,error))return;_preferredColumn=c.column;
    [self scrollRectToVisible:NSMakeRect(6+c.column*_characterWidth,4+c.line*_lineHeight,2,_lineHeight)];
    [self setNeedsDisplayInRect:self.visibleRect];
}
- (void)changed { [self refreshFrame];[self setNeedsDisplay:YES];[self.changeDelegate largeFileViewDidChange:self]; }

- (BOOL)replaceSelectionWithWideText:(const std::wstring&)text error:(std::wstring&)error
{
    const auto start=std::min(_selectionStart,_selectionEnd),end=std::max(_selectionStart,_selectionEnd);std::uint64_t as=0,ae=0;
    {std::lock_guard<std::mutex>lock(_mutex);if(!_document||!_document->Replace(start,end,text,_selectionStart,_selectionEnd,as,ae,error))return NO;_lineCount.store(_document->LineCount());_characterCount.store(_document->CharacterCount());}
    _selectionStart=_selectionEnd=_selectionAnchor=_caret=ae;[self scrollCaretVisible];[self changed];return YES;
}
- (BOOL)setSelectionStart:(std::uint64_t)start end:(std::uint64_t)end error:(std::wstring&)error {{std::lock_guard<std::mutex>lock(_mutex);if(!_document||start>_document->CharacterCount()||end>_document->CharacterCount()){error=L"Selection is outside the indexed document.";return NO;}_selectionStart=_selectionAnchor=start;_selectionEnd=_caret=end;}[self scrollCaretVisible];[self changed];return YES;}
- (BOOL)goToLine:(std::uint64_t)line error:(std::wstring&)error {if(!line){error=L"Line numbers start at 1.";return NO;}std::uint64_t p=0;{std::lock_guard<std::mutex>lock(_mutex);if(!_document||!_document->LineStart(line-1,p,error))return NO;}return[self setSelectionStart:p end:p error:error];}
- (BOOL)findText:(const std::wstring&)text matchCase:(BOOL)matchCase wholeWord:(BOOL)wholeWord forward:(BOOL)forward error:(std::wstring&)error {classic_notepad::LargeDocumentMatch m;{std::lock_guard<std::mutex>lock(_mutex);if(!_document||!_document->Find(text,forward?_selectionEnd:_selectionStart,matchCase,wholeWord,forward,m,error))return NO;}return[self setSelectionStart:m.start end:m.start+m.length error:error];}
- (std::uint64_t)replaceAllText:(const std::wstring&)text replacement:(const std::wstring&)replacement matchCase:(BOOL)matchCase wholeWord:(BOOL)wholeWord error:(std::wstring&)error {bool cancelled=false;std::uint64_t count=0;{std::lock_guard<std::mutex>lock(_mutex);if(_document)count=_document->ReplaceAll(text,replacement,matchCase,wholeWord,cancelled,[]{return false;},error);if(_document){_lineCount.store(_document->LineCount());_characterCount.store(_document->CharacterCount());}}if(count)[self changed];return count;}
- (std::wstring)selectedWideText:(std::wstring&)error {const auto s=std::min(_selectionStart,_selectionEnd),e=std::max(_selectionStart,_selectionEnd);if(e-s>kClipboardLimit){error=L"Copy is limited to 16 Mi UTF-16 units in large-file mode.";return{};}return[self boundedTextFrom:s length:e-s error:error];}
- (std::wstring)boundedTextFrom:(std::uint64_t)start length:(std::uint64_t)length error:(std::wstring&)error {std::wstring text;std::lock_guard<std::mutex>lock(_mutex);if(!_document||!_document->ReadRange(start,length,text,error))return{};return text;}

- (BOOL)saveToPath:(const std::wstring&)path error:(std::wstring&)error
{
    [SavingCursor() push];
    [SavingCursor() set];
    [[self window] displayIfNeeded];
    if(!_indexingComplete.load()&&_indexThread.joinable())_indexThread.join();
    if(!_encodingValid.load()||!_indexingComplete.load()){[NSCursor pop];error=L"Large-file indexing did not complete.";return NO;}
    std::unique_ptr<classic_notepad::LargeDocument> snapshot;std::uint64_t generation=0;{std::lock_guard<std::mutex>lock(_mutex);snapshot=_document->Clone();generation=_document->Generation();}
    const std::string destination=Utf8Path(path);std::string pattern=destination+".classic-notepad-save-XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);int output=mkstemp(name.data());if(output<0){[NSCursor pop];error=L"A sibling temporary save file could not be created.";return NO;}
    auto writeAll=[&](const std::uint8_t*data,std::size_t size){while(size){ssize_t n=write(output,data,size);if(n<=0)return false;data+=n;size-=static_cast<std::size_t>(n);}return true;};bool okay=true;
    if(_encoding==classic_notepad::LargeFileEncoding::Utf16Le){const std::uint8_t bom[]={0xff,0xfe};okay=writeAll(bom,2);}else if(_dataOffset==3){const std::uint8_t bom[]={0xef,0xbb,0xbf};okay=writeAll(bom,3);}
    const auto target=_encoding==classic_notepad::LargeFileEncoding::Utf16Le?classic_notepad::TextEncoding::Utf16LeBom:classic_notepad::TextEncoding::Utf8NoBom;
    if(okay)okay=snapshot->StreamText([&](const std::wstring&units){std::vector<std::uint8_t>encoded;std::wstring e;return classic_notepad::EncodeTextChunk(PlatformWideFromUtf16(units),target,false,encoded,e)&&writeAll(encoded.data(),encoded.size());},[]{return false;},error);
    if(okay&&fsync(output)!=0)okay=false;if(close(output)!=0)okay=false;if(okay&&rename(name.data(),destination.c_str())!=0)okay=false;if(!okay){unlink(name.data());if(error.empty())error=L"The complete temporary save could not replace the destination.";[NSCursor pop];return NO;}
    {std::lock_guard<std::mutex>lock(_mutex);if(_document&&_document->Generation()==generation)_document->MarkSaved();}_path=path;[self changed];[NSCursor pop];return YES;
}

- (const std::wstring&)documentPath{return _path;}
- (classic_notepad::LargeFileEncoding)largeEncoding{return _encoding;}
- (BOOL)isModified{std::lock_guard<std::mutex>lock(_mutex);return _document&&_document->IsModified();}
- (BOOL)isIndexingComplete{return _indexingComplete.load();}
- (BOOL)isEncodingValid{return _encodingValid.load();}
- (std::uint64_t)fileSize{return _fileSize;}
- (std::uint64_t)indexedBytes{return _indexedBytes.load();}
- (std::uint64_t)characterCount{std::lock_guard<std::mutex>lock(_mutex);return _document?_document->CharacterCount():0;}
- (std::uint64_t)lineCount{std::lock_guard<std::mutex>lock(_mutex);return _document?_document->LineCount():1;}
- (std::uint64_t)maximumLineColumns{return _maximumLineColumns.load();}
- (std::uint64_t)topVisibleLine{return static_cast<std::uint64_t>(std::max<CGFloat>(0.0,floor(self.visibleRect.origin.y/_lineHeight)));}
- (std::uint64_t)pieceCount{std::lock_guard<std::mutex>lock(_mutex);return _document?_document->PieceCount():0;}
- (std::uint64_t)insertedBufferBytes{std::lock_guard<std::mutex>lock(_mutex);return _document?_document->InsertedBufferBytes():0;}
- (std::uint64_t)firstUsableMilliseconds{return _firstUsableMs.load();}
- (std::uint64_t)firstViewportPaintMilliseconds{return _firstPaintMs.load();}
- (void)markFirstViewportPresented
{
    std::uint64_t expected = 0;
    _firstPaintMs.compare_exchange_strong(expected,
        std::max<std::uint64_t>(1, NowMilliseconds() - _startedMs));
}
- (std::uint64_t)fullIndexMilliseconds{return _fullIndexMs.load();}
- (std::uint64_t)caretPosition{return _caret;}
- (std::uint64_t)selectionStart{return _selectionStart;}
- (std::uint64_t)selectionEnd{return _selectionEnd;}
- (std::uint64_t)caretLine{std::lock_guard<std::mutex>lock(_mutex);classic_notepad::LargeDocumentLineColumn c;std::wstring e;return _document&&_document->PositionToLineColumn(_caret,c,e)?c.line:0;}
- (std::uint64_t)caretColumn{std::lock_guard<std::mutex>lock(_mutex);classic_notepad::LargeDocumentLineColumn c;std::wstring e;return _document&&_document->PositionToLineColumn(_caret,c,e)?c.column:0;}
- (void)setEditorFont:(NSFont*)font{if(font&&font!=_font){[font retain];[_font release];_font=font;}_lineHeight=ceil([_font ascender]-[_font descender]+[_font leading])+2;_characterWidth=ceil([@"M" sizeWithAttributes:@{NSFontAttributeName:_font}].width);[self changed];}
- (void)changeFont:(id)sender{NSFont* converted=[sender convertFont:_font];if(converted)[self setEditorFont:converted];}
- (void)setWordWrapEnabled:(BOOL)enabled{_wordWrap=enabled;[self changed];}
- (BOOL)wordWrapEnabled{return _wordWrap;}
- (id)accessibilityValue { std::wstring e; const auto line=[self caretLine]; std::uint64_t start=0,end=0,next=0; {std::lock_guard<std::mutex>lock(_mutex);if(!_document||!_document->LineRange(line,start,end,next,e))return @"";} return StringFromUtf16Units([self boundedTextFrom:start length:std::min<std::uint64_t>(end-start,4096) error:e]); }

@end
