#pragma once

#import <AppKit/AppKit.h>

#include "large_file_index.h"

#include <cstdint>
#include <string>

@class MacLargeFileView;

@protocol MacLargeFileViewDelegate <NSObject>
- (void)largeFileViewDidChange:(MacLargeFileView*)view;
@end

@interface MacLargeFileView : NSView

// The macOS target is intentionally built without ARC. The owning app delegate
// retains the view, so this back-reference must remain non-retaining.
@property(nonatomic, assign) id<MacLargeFileViewDelegate> changeDelegate;

+ (BOOL)shouldUseForPath:(const std::wstring&)path size:(std::uint64_t&)size error:(std::wstring&)errorMessage;
- (BOOL)openPath:(const std::wstring&)path error:(std::wstring&)errorMessage;
- (void)closeDocument;
- (const std::wstring&)documentPath;
- (classic_notepad::LargeFileEncoding)largeEncoding;
- (BOOL)isModified;
- (BOOL)isIndexingComplete;
- (BOOL)isEncodingValid;
- (std::uint64_t)fileSize;
- (std::uint64_t)indexedBytes;
- (std::uint64_t)characterCount;
- (std::uint64_t)lineCount;
- (std::uint64_t)maximumLineColumns;
- (std::uint64_t)topVisibleLine;
- (std::uint64_t)pieceCount;
- (std::uint64_t)insertedBufferBytes;
- (std::uint64_t)firstUsableMilliseconds;
- (std::uint64_t)firstViewportPaintMilliseconds;
- (void)markFirstViewportPresented;
- (std::uint64_t)fullIndexMilliseconds;
- (std::uint64_t)caretPosition;
- (std::uint64_t)selectionStart;
- (std::uint64_t)selectionEnd;
- (std::uint64_t)caretLine;
- (std::uint64_t)caretColumn;
- (BOOL)replaceSelectionWithWideText:(const std::wstring&)text error:(std::wstring&)errorMessage;
- (BOOL)setSelectionStart:(std::uint64_t)start end:(std::uint64_t)end error:(std::wstring&)errorMessage;
- (BOOL)goToLine:(std::uint64_t)oneBasedLine error:(std::wstring&)errorMessage;
- (BOOL)findText:(const std::wstring&)text matchCase:(BOOL)matchCase wholeWord:(BOOL)wholeWord forward:(BOOL)forward error:(std::wstring&)errorMessage;
- (std::uint64_t)replaceAllText:(const std::wstring&)text replacement:(const std::wstring&)replacement matchCase:(BOOL)matchCase wholeWord:(BOOL)wholeWord error:(std::wstring&)errorMessage;
- (BOOL)saveToPath:(const std::wstring&)path error:(std::wstring&)errorMessage;
- (std::wstring)selectedWideText:(std::wstring&)errorMessage;
- (std::wstring)boundedTextFrom:(std::uint64_t)start length:(std::uint64_t)length error:(std::wstring&)errorMessage;
- (void)setEditorFont:(NSFont*)font;
- (void)setWordWrapEnabled:(BOOL)enabled;
- (BOOL)wordWrapEnabled;
- (void)undo:(id)sender;

@end
