#include "mac_app.h"

#include "document.h"
#include "file_io.h"
#include "app_version.h"
#include "mac_appearance.h"
#include "mac_automation.h"
#include "mac_spelling.h"
#include "text_metadata.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

constexpr CGFloat kInitialWindowWidth = 760.0;
constexpr CGFloat kInitialWindowHeight = 520.0;

enum class DirtyPromptResult {
    Save,
    DontSave,
    Cancel
};

static NSString* NSStringFromWide(const std::wstring& text)
{
    if (text.empty()) {
        return @"";
    }

    static_assert(sizeof(wchar_t) == 4, "macOS wchar_t is expected to be UTF-32.");
    return [[NSString alloc]
        initWithBytes:text.data()
        length:text.size() * sizeof(wchar_t)
        encoding:NSUTF32LittleEndianStringEncoding];
}

static NSString* NSStringFromUtf8(const std::string& text)
{
    return [[NSString alloc] initWithUTF8String:text.c_str()];
}

static std::wstring WideFromNSString(NSString* text)
{
    if (text == nil) {
        return {};
    }

    static_assert(sizeof(wchar_t) == 4, "macOS wchar_t is expected to be UTF-32.");
    NSData* data = [text dataUsingEncoding:NSUTF32LittleEndianStringEncoding allowLossyConversion:NO];
    if (data == nil) {
        return {};
    }

    return std::wstring(
        static_cast<const wchar_t*>([data bytes]),
        static_cast<std::size_t>([data length] / sizeof(wchar_t)));
}

static NSString* WindowTitle(const Document& document)
{
    std::wstring title;
    if (document.IsModified()) {
        title += L"*";
    }

    title += document.DisplayName();
    title += L" - Classic Notepad";
    return NSStringFromWide(title);
}

static void ShowError(NSWindow* parent, NSString* message)
{
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleCritical];
    [alert setMessageText:@"Classic Notepad"];
    [alert setInformativeText:message == nil ? @"" : message];
    if (parent != nil) {
        [alert beginSheetModalForWindow:parent completionHandler:nil];
    } else {
        [alert runModal];
    }
}

static std::wstring EditorTextFromTextView(NSTextView* textView)
{
    return WideFromNSString([textView string]);
}

static bool PathExists(const std::wstring& path)
{
    std::error_code error;
    return std::filesystem::exists(std::filesystem::path(path), error);
}

static std::wstring PathFromUrl(NSURL* url)
{
    return url == nil ? std::wstring() : WideFromNSString([url path]);
}

static NSImage* LoadApplicationIcon()
{
    NSString* iconPath = [[NSBundle mainBundle] pathForResource:@"classic_notepad" ofType:@"icns"];
    if (iconPath == nil) {
        return nil;
    }

    return [[NSImage alloc] initWithContentsOfFile:iconPath];
}

static NSImage* LoadMenuIcon()
{
    NSImage* icon = LoadApplicationIcon();
    if (icon != nil) {
        [icon setSize:NSMakeSize(16.0, 16.0)];
    }
    return icon;
}

static std::string Utf8FromWide(const std::wstring& text)
{
    NSMutableString* string = [NSMutableString stringWithString:NSStringFromWide(text)];
    const char* utf8 = [string UTF8String];
    return utf8 == nullptr ? std::string() : std::string(utf8);
}

static std::wstring WideFromUtf8(const std::string& text)
{
    NSString* string = [[NSString alloc] initWithUTF8String:text.c_str()];
    return WideFromNSString(string);
}

static std::wstring LowercaseCopy(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return text;
}

static bool IsWordCharacter(wchar_t character)
{
    return std::iswalnum(character) != 0 || character == L'_';
}

static bool HasWordBoundaryAt(const std::wstring& text, std::size_t position)
{
    if (position > text.size()) {
        return false;
    }

    const bool leftWord = position > 0U && IsWordCharacter(text[position - 1U]);
    const bool rightWord = position < text.size() && IsWordCharacter(text[position]);
    return leftWord != rightWord;
}

static bool IsWholeWordMatch(const std::wstring& text, std::size_t position, std::size_t length)
{
    return HasWordBoundaryAt(text, position) && HasWordBoundaryAt(text, position + length);
}

static std::size_t CountLines(const std::wstring& text)
{
    return 1U + static_cast<std::size_t>(std::count(text.begin(), text.end(), L'\n'));
}

static std::wstring BuildPrintSinkText(
    const std::wstring& fontDescription,
    const classic_notepad::macos::MacAutomationPageMargins& margins,
    const std::wstring& text)
{
    std::wstring output = L"Classic Notepad Print Sink\n";
    output += L"Platform: macos\n";
    output += L"Pages: 1\n";
    output += L"Font: ";
    output += fontDescription;
    output += L"\nMargins (thousandths inch): ";
    output += std::to_wstring(margins.left);
    output += L",";
    output += std::to_wstring(margins.top);
    output += L",";
    output += std::to_wstring(margins.right);
    output += L",";
    output += std::to_wstring(margins.bottom);
    output += L"\n--- Page 1 ---\n";
    output += text;
    if (!output.empty() && output.back() != L'\n') {
        output += L"\n";
    }
    return output;
}

static int ThousandthsFromPoints(CGFloat points)
{
    return static_cast<int>(std::lround((points * 1000.0) / 72.0));
}

static CGFloat PointsFromThousandths(int thousandths)
{
    return static_cast<CGFloat>((static_cast<double>(thousandths) * 72.0) / 1000.0);
}

@interface ClassicNotepadMacDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTextViewDelegate, NSMenuItemValidation>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSTextView* textView;
@property(nonatomic, strong) NSTextField* statusField;
- (instancetype)initWithInitialPath:(const std::wstring&)initialPath automationMode:(BOOL)automationMode automationVisible:(BOOL)automationVisible;
- (void)createMenus;
- (void)createWindow;
- (void)openInitialPath;
- (BOOL)loadPath:(const std::wstring&)path;
- (void)updateWindowTitle;
- (void)newDocument:(id)sender;
- (void)openDocument:(id)sender;
- (void)saveDocument:(id)sender;
- (void)saveDocumentAs:(id)sender;
- (void)pageSetupDocument:(id)sender;
- (void)printDocument:(id)sender;
- (void)exitApplication:(id)sender;
- (BOOL)save;
- (BOOL)saveAs;
- (BOOL)confirmContinueAfterDirtyDocument;
- (DirtyPromptResult)showDirtyPrompt;
- (BOOL)confirmCreateMissingPath:(const std::wstring&)path;
- (void)resetUntitledDocument;
- (void)setEditorText:(const std::wstring&)text markModified:(BOOL)modified;
- (void)insertTimeDate:(id)sender;
- (void)findText:(id)sender;
- (void)findNextText:(id)sender;
- (void)replaceText:(id)sender;
- (void)goToLine:(id)sender;
- (void)toggleWordWrap:(id)sender;
- (void)chooseFont:(id)sender;
- (void)toggleStatusBar:(id)sender;
- (void)setAppearanceSystem:(id)sender;
- (void)setAppearanceLight:(id)sender;
- (void)setAppearanceDark:(id)sender;
- (void)updateStatusText;
- (bool)wordWrapEnabled;
- (bool)statusBarVisible;
- (std::wstring)fontDescription;
- (classic_notepad::macos::MacAutomationPageMargins)pageMargins;
- (void)setPageMarginsForAutomation:(const classic_notepad::macos::MacAutomationPageMargins&)margins;
- (void)syncPageMarginsFromPrintInfo;
- (void)syncPrintInfoFromPageMargins;
- (classic_notepad::macos::MacSpellingConfiguration)spellingConfiguration;
@end

@implementation ClassicNotepadMacDelegate {
    Document _document;
    std::wstring _initialPath;
    bool _automationMode;
    bool _automationVisible;
    bool _wordWrap;
    bool _statusBarVisible;
    std::wstring _findText;
    std::wstring _fontDescription;
    classic_notepad::macos::MacAutomationPageMargins _pageMargins;
    NSPrintInfo* _printInfo;
    classic_notepad::macos::MacAppearanceConfiguration _appearanceConfiguration;
    classic_notepad::macos::MacSpellingConfiguration _spellingConfiguration;
    classic_notepad::AppearanceTheme _appearanceTheme;
}

- (instancetype)initWithInitialPath:(const std::wstring&)initialPath automationMode:(BOOL)automationMode automationVisible:(BOOL)automationVisible
{
    self = [super init];
    if (self != nil) {
        _initialPath = initialPath;
        _document.ResetUntitled();
        _automationMode = automationMode;
        _automationVisible = automationVisible;
        _wordWrap = false;
        _statusBarVisible = true;
        _fontDescription = L"Menlo 13";
        _printInfo = [[NSPrintInfo sharedPrintInfo] copy];
        [self syncPageMarginsFromPrintInfo];
        _appearanceTheme = classic_notepad::macos::AppearanceThemeFromEnvironment();
    }
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    (void)notification;

    if ([NSWindow respondsToSelector:@selector(setAllowsAutomaticWindowTabbing:)]) {
        [NSWindow setAllowsAutomaticWindowTabbing:NO];
    }

    NSImage* appIcon = LoadApplicationIcon();
    if (appIcon != nil) {
        [NSApp setApplicationIconImage:appIcon];
    }

    _appearanceConfiguration = classic_notepad::macos::ApplyApplicationAppearance(NSApp, _appearanceTheme);

    [self createMenus];
    [self createWindow];

    if (!_initialPath.empty()) {
        if (_automationMode && !PathExists(_initialPath)) {
            _document.ResetNewFile(_initialPath);
            [self setEditorText:L"" markModified:NO];
        } else {
            [self openInitialPath];
        }
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
    (void)sender;
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender
{
    (void)sender;
    return [self confirmContinueAfterDirtyDocument] ? NSTerminateNow : NSTerminateCancel;
}

- (BOOL)windowShouldClose:(id)sender
{
    (void)sender;
    return [self confirmContinueAfterDirtyDocument];
}

- (void)textDidChange:(NSNotification*)notification
{
    (void)notification;
    _document.SetModified(true);
    [self updateWindowTitle];
    [self updateStatusText];
}

- (BOOL)application:(NSApplication*)application openFile:(NSString*)filename
{
    (void)application;
    if (![self confirmContinueAfterDirtyDocument]) {
        return NO;
    }

    return [self loadPath:WideFromNSString(filename)];
}

- (void)createMenus
{
    NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@""];

    NSMenuItem* appItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    [mainMenu addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"Classic Notepad"];
    [appItem setSubmenu:appMenu];
    NSMenuItem* aboutAppItem = [appMenu addItemWithTitle:@"About Classic Notepad" action:@selector(showAbout:) keyEquivalent:@""];
    [aboutAppItem setTarget:self];
    [aboutAppItem setImage:LoadMenuIcon()];
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* quitItem = [appMenu addItemWithTitle:@"Quit Classic Notepad" action:@selector(exitApplication:) keyEquivalent:@"q"];
    [quitItem setTarget:self];

    NSMenuItem* fileItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    [mainMenu addItem:fileItem];
    NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [fileItem setSubmenu:fileMenu];
    NSMenuItem* newItem = [fileMenu addItemWithTitle:@"New" action:@selector(newDocument:) keyEquivalent:@"n"];
    [newItem setTarget:self];
    NSMenuItem* openItem = [fileMenu addItemWithTitle:@"Open..." action:@selector(openDocument:) keyEquivalent:@"o"];
    [openItem setTarget:self];
    NSMenuItem* saveItem = [fileMenu addItemWithTitle:@"Save" action:@selector(saveDocument:) keyEquivalent:@"s"];
    [saveItem setTarget:self];
    NSMenuItem* saveAsItem = [fileMenu addItemWithTitle:@"Save As..." action:@selector(saveDocumentAs:) keyEquivalent:@"S"];
    [saveAsItem setTarget:self];
    [saveAsItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* pageSetupItem = [fileMenu addItemWithTitle:@"Page Setup..." action:@selector(pageSetupDocument:) keyEquivalent:@""];
    [pageSetupItem setTarget:self];
    NSMenuItem* printItem = [fileMenu addItemWithTitle:@"Print..." action:@selector(printDocument:) keyEquivalent:@"p"];
    [printItem setTarget:self];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [fileMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    NSMenuItem* exitItem = [fileMenu addItemWithTitle:@"Exit" action:@selector(exitApplication:) keyEquivalent:@""];
    [exitItem setTarget:self];

    NSMenuItem* editItem = [[NSMenuItem alloc] initWithTitle:@"Edit" action:nil keyEquivalent:@""];
    [mainMenu addItem:editItem];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editItem setSubmenu:editMenu];
    [editMenu addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    [editMenu addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"Delete" action:@selector(delete:) keyEquivalent:@""];
    [editMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* findItem = [editMenu addItemWithTitle:@"Find..." action:@selector(findText:) keyEquivalent:@"f"];
    [findItem setTarget:self];
    NSMenuItem* findNextItem = [editMenu addItemWithTitle:@"Find Next" action:@selector(findNextText:) keyEquivalent:@"g"];
    [findNextItem setTarget:self];
    NSMenuItem* replaceItem = [editMenu addItemWithTitle:@"Replace..." action:@selector(replaceText:) keyEquivalent:@"h"];
    [replaceItem setTarget:self];
    NSMenuItem* goToItem = [editMenu addItemWithTitle:@"Go To..." action:@selector(goToLine:) keyEquivalent:@"l"];
    [goToItem setTarget:self];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    NSMenuItem* timeDateItem = [editMenu addItemWithTitle:@"Time/Date" action:@selector(insertTimeDate:) keyEquivalent:@""];
    [timeDateItem setTarget:self];

    NSMenuItem* formatItem = [[NSMenuItem alloc] initWithTitle:@"Format" action:nil keyEquivalent:@""];
    [mainMenu addItem:formatItem];
    NSMenu* formatMenu = [[NSMenu alloc] initWithTitle:@"Format"];
    [formatItem setSubmenu:formatMenu];
    NSMenuItem* wordWrapItem = [formatMenu addItemWithTitle:@"Word Wrap" action:@selector(toggleWordWrap:) keyEquivalent:@""];
    [wordWrapItem setTarget:self];
    NSMenuItem* fontItem = [formatMenu addItemWithTitle:@"Font..." action:@selector(chooseFont:) keyEquivalent:@""];
    [fontItem setTarget:self];

    NSMenuItem* viewItem = [[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""];
    [mainMenu addItem:viewItem];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    [viewItem setSubmenu:viewMenu];
    NSMenuItem* statusItem = [viewMenu addItemWithTitle:@"Status Bar" action:@selector(toggleStatusBar:) keyEquivalent:@""];
    [statusItem setTarget:self];
    NSMenu* appearanceMenu = [[NSMenu alloc] initWithTitle:@"Appearance"];
    NSMenuItem* appearanceItem = [[NSMenuItem alloc] initWithTitle:@"Appearance" action:nil keyEquivalent:@""];
    [appearanceItem setSubmenu:appearanceMenu];
    [viewMenu addItem:appearanceItem];
    NSMenuItem* systemAppearanceItem = [appearanceMenu addItemWithTitle:@"System" action:@selector(setAppearanceSystem:) keyEquivalent:@""];
    [systemAppearanceItem setTarget:self];
    NSMenuItem* lightAppearanceItem = [appearanceMenu addItemWithTitle:@"Light" action:@selector(setAppearanceLight:) keyEquivalent:@""];
    [lightAppearanceItem setTarget:self];
    NSMenuItem* darkAppearanceItem = [appearanceMenu addItemWithTitle:@"Dark" action:@selector(setAppearanceDark:) keyEquivalent:@""];
    [darkAppearanceItem setTarget:self];

    NSMenuItem* helpItem = [[NSMenuItem alloc] initWithTitle:@"Help" action:nil keyEquivalent:@""];
    [mainMenu addItem:helpItem];
    NSMenu* helpMenu = [[NSMenu alloc] initWithTitle:@"Help"];
    [helpItem setSubmenu:helpMenu];
    NSMenuItem* aboutHelpItem = [helpMenu addItemWithTitle:@"About Classic Notepad" action:@selector(showAbout:) keyEquivalent:@""];
    [aboutHelpItem setTarget:self];
    [aboutHelpItem setImage:LoadMenuIcon()];

    [NSApp setMainMenu:mainMenu];
}

- (BOOL)validateMenuItem:(NSMenuItem*)menuItem
{
    const SEL action = [menuItem action];
    if (action == @selector(toggleWordWrap:)) {
        [menuItem setState:_wordWrap ? NSControlStateValueOn : NSControlStateValueOff];
        return YES;
    }
    if (action == @selector(toggleStatusBar:)) {
        [menuItem setState:_statusBarVisible ? NSControlStateValueOn : NSControlStateValueOff];
        return YES;
    }
    if (action == @selector(pageSetupDocument:) || action == @selector(printDocument:)) {
        return !_automationMode;
    }
    if (action == @selector(setAppearanceSystem:)) {
        [menuItem setState:_appearanceTheme == classic_notepad::AppearanceTheme::System ? NSControlStateValueOn : NSControlStateValueOff];
        return YES;
    }
    if (action == @selector(setAppearanceLight:)) {
        [menuItem setState:_appearanceTheme == classic_notepad::AppearanceTheme::Light ? NSControlStateValueOn : NSControlStateValueOff];
        return YES;
    }
    if (action == @selector(setAppearanceDark:)) {
        [menuItem setState:_appearanceTheme == classic_notepad::AppearanceTheme::Dark ? NSControlStateValueOn : NSControlStateValueOff];
        return YES;
    }
    return YES;
}

- (void)createWindow
{
    NSRect frame = NSMakeRect(0, 0, kInitialWindowWidth, kInitialWindowHeight);
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                   NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered
        defer:NO];

    [self.window setDelegate:self];
    [self.window center];

    NSView* contentView = [self.window contentView];
    const CGFloat statusHeight = 24.0;
    NSRect contentBounds = [contentView bounds];
    NSScrollView* scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(
        0,
        statusHeight,
        contentBounds.size.width,
        contentBounds.size.height - statusHeight)];
    [scrollView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [scrollView setBorderType:NSNoBorder];
    [scrollView setHasVerticalScroller:YES];
    [scrollView setHasHorizontalScroller:YES];
    [scrollView setAutohidesScrollers:NO];

    NSSize contentSize = [scrollView contentSize];
    self.textView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, contentSize.width, contentSize.height)];
    [self.textView setDelegate:self];
    [self.textView setMinSize:NSMakeSize(0.0, contentSize.height)];
    [self.textView setMaxSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)];
    [self.textView setVerticallyResizable:YES];
    [self.textView setHorizontallyResizable:YES];
    [self.textView setAutoresizingMask:NSViewWidthSizable];
    [[self.textView textContainer] setContainerSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)];
    [[self.textView textContainer] setWidthTracksTextView:NO];
    [self.textView setRichText:NO];
    [self.textView setImportsGraphics:NO];
    [self.textView setUsesFindBar:YES];
    [self.textView setFont:[NSFont userFixedPitchFontOfSize:13.0]];

    classic_notepad::macos::ConfigurePlainTextViewAppearance(self.textView);
    _spellingConfiguration = classic_notepad::macos::ConfigureTextViewSpelling(self.textView);
    _appearanceConfiguration = classic_notepad::macos::ApplyWindowAppearance(self.window, _appearanceTheme);

    [scrollView setDocumentView:self.textView];
    [contentView addSubview:scrollView];

    self.statusField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, contentBounds.size.width, statusHeight)];
    [self.statusField setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
    [self.statusField setEditable:NO];
    [self.statusField setSelectable:NO];
    [self.statusField setBezeled:NO];
    [self.statusField setDrawsBackground:YES];
    [self.statusField setBackgroundColor:[NSColor windowBackgroundColor]];
    [self.statusField setTextColor:[NSColor labelColor]];
    [self.statusField setFont:[NSFont systemFontOfSize:12.0]];
    [contentView addSubview:self.statusField];

    [self setWordWrap:_wordWrap];
    [self setStatusBarVisible:_statusBarVisible];
    [self updateWindowTitle];
    [self updateStatusText];
    if (!_automationMode || _automationVisible) {
        [self.window makeKeyAndOrderFront:nil];
    }
}

- (void)openInitialPath
{
    if (PathExists(_initialPath)) {
        [self loadPath:_initialPath];
        return;
    }

    if ([self confirmCreateMissingPath:_initialPath]) {
        _document.ResetNewFile(_initialPath);
        [self setEditorText:L"" markModified:NO];
    }
}

- (BOOL)loadPath:(const std::wstring&)path
{
    std::wstring editorText;
    std::wstring errorMessage;
    if (!_document.Load(path, editorText, errorMessage)) {
        ShowError(self.window, NSStringFromWide(errorMessage));
        return NO;
    }

    [self setEditorText:editorText markModified:NO];
    return YES;
}

- (void)updateWindowTitle
{
    [self.window setTitle:WindowTitle(_document)];
    [self.window setDocumentEdited:_document.IsModified()];
}

- (void)newDocument:(id)sender
{
    (void)sender;
    if (![self confirmContinueAfterDirtyDocument]) {
        return;
    }

    [self resetUntitledDocument];
}

- (void)openDocument:(id)sender
{
    (void)sender;
    if (![self confirmContinueAfterDirtyDocument]) {
        return;
    }

    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];

    if ([panel runModal] != NSModalResponseOK) {
        return;
    }

    [self loadPath:PathFromUrl([panel URL])];
}

- (void)saveDocument:(id)sender
{
    (void)sender;
    [self save];
}

- (void)saveDocumentAs:(id)sender
{
    (void)sender;
    [self saveAs];
}

- (void)pageSetupDocument:(id)sender
{
    (void)sender;
    if (_automationMode) {
        return;
    }

    NSPageLayout* pageLayout = [NSPageLayout pageLayout];
    const NSInteger result = [pageLayout runModalWithPrintInfo:_printInfo];
    if (result == NSModalResponseOK) {
        [self syncPageMarginsFromPrintInfo];
    }
}

- (void)printDocument:(id)sender
{
    (void)sender;
    if (_automationMode) {
        return;
    }

    NSPrintOperation* operation = [NSPrintOperation printOperationWithView:self.textView printInfo:_printInfo];
    [operation setJobTitle:@"Classic Notepad"];
    [operation runOperationModalForWindow:self.window
                                 delegate:nil
                           didRunSelector:nil
                              contextInfo:nil];
    [self syncPageMarginsFromPrintInfo];
}

- (void)exitApplication:(id)sender
{
    (void)sender;
    [NSApp terminate:self];
}

- (void)showAbout:(id)sender
{
    (void)sender;

    NSAlert* alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleInformational];
    [alert setMessageText:@"Classic Notepad"];

    std::wstring text = CLASSIC_NOTEPAD_VERSION_DISPLAY_W;
    text += L"\n\n";
    text += L"Native AppKit build with classic menus, local files, find/replace, Go To, word wrap, font selection, status metadata, page setup, and printing.";
    text += L"\n\n";
    text += L"No tabs, cloud features, telemetry, or modern editor extras.";
    [alert setInformativeText:NSStringFromWide(text)];

    NSImage* icon = LoadApplicationIcon();
    if (icon == nil) {
        icon = [NSApp applicationIconImage];
    }
    if (icon != nil) {
        [alert setIcon:icon];
    }

    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
}

- (BOOL)save
{
    if (!_document.HasPath()) {
        return [self saveAs];
    }

    std::wstring errorMessage;
    if (!_document.Save(EditorTextFromTextView(self.textView), errorMessage)) {
        ShowError(self.window, NSStringFromWide(errorMessage));
        return NO;
    }

    [self updateWindowTitle];
    return YES;
}

- (BOOL)saveAs
{
    NSSavePanel* panel = [NSSavePanel savePanel];
    [panel setCanCreateDirectories:YES];
    [panel setNameFieldStringValue:NSStringFromWide(_document.DisplayName())];

    if ([panel runModal] != NSModalResponseOK) {
        return NO;
    }

    std::wstring errorMessage;
    if (!_document.SaveAs(PathFromUrl([panel URL]), EditorTextFromTextView(self.textView), errorMessage)) {
        ShowError(self.window, NSStringFromWide(errorMessage));
        return NO;
    }

    [self updateWindowTitle];
    return YES;
}

- (BOOL)saveAsPath:(const std::wstring&)path errorMessage:(std::wstring&)errorMessage
{
    if (!_document.SaveAs(path, EditorTextFromTextView(self.textView), errorMessage)) {
        return NO;
    }

    [self updateWindowTitle];
    [self updateStatusText];
    return YES;
}

- (BOOL)confirmContinueAfterDirtyDocument
{
    if (!_document.IsModified()) {
        return YES;
    }

    switch ([self showDirtyPrompt]) {
    case DirtyPromptResult::Save:
        return [self save];
    case DirtyPromptResult::DontSave:
        _document.SetModified(false);
        [self updateWindowTitle];
        return YES;
    case DirtyPromptResult::Cancel:
    default:
        return NO;
    }
}

- (DirtyPromptResult)showDirtyPrompt
{
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:[NSString stringWithFormat:@"Do you want to save changes to %@?", NSStringFromWide(_document.DisplayName())]];
    [alert setInformativeText:@"Your changes will be lost if you don't save them."];
    [alert addButtonWithTitle:@"Save"];
    [alert addButtonWithTitle:@"Don't Save"];
    [alert addButtonWithTitle:@"Cancel"];

    const NSModalResponse response = [alert runModal];
    if (response == NSAlertFirstButtonReturn) {
        return DirtyPromptResult::Save;
    }
    if (response == NSAlertSecondButtonReturn) {
        return DirtyPromptResult::DontSave;
    }
    return DirtyPromptResult::Cancel;
}

- (BOOL)confirmCreateMissingPath:(const std::wstring&)path
{
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"File not found"];
    [alert setInformativeText:[NSString stringWithFormat:@"Cannot find %@. Do you want to create a new file?", NSStringFromWide(path)]];
    [alert addButtonWithTitle:@"Create"];
    [alert addButtonWithTitle:@"Cancel"];
    return [alert runModal] == NSAlertFirstButtonReturn;
}

- (void)resetUntitledDocument
{
    _document.ResetUntitled();
    [self setEditorText:L"" markModified:NO];
}

- (void)setEditorText:(const std::wstring&)text markModified:(BOOL)modified
{
    [self.textView setString:NSStringFromWide(text)];
    _document.SetModified(modified);
    [self updateWindowTitle];
    [self updateStatusText];
}

- (void)insertTimeDate:(id)sender
{
    (void)sender;
    [self insertTextAtSelection:[self buildTimeDateText]];
}

- (void)findText:(id)sender
{
    (void)sender;
    [self.textView performTextFinderAction:@(NSTextFinderActionShowFindInterface)];
}

- (void)findNextText:(id)sender
{
    (void)sender;
    [self.textView performTextFinderAction:@(NSTextFinderActionNextMatch)];
}

- (void)replaceText:(id)sender
{
    (void)sender;
    [self.textView performTextFinderAction:@(NSTextFinderActionShowReplaceInterface)];
}

- (void)goToLine:(id)sender
{
    (void)sender;
}

- (void)toggleWordWrap:(id)sender
{
    (void)sender;
    [self setWordWrap:!_wordWrap];
}

- (void)chooseFont:(id)sender
{
    (void)sender;
    [[NSFontManager sharedFontManager] orderFrontFontPanel:self];
}

- (void)toggleStatusBar:(id)sender
{
    (void)sender;
    [self setStatusBarVisible:!_statusBarVisible];
}

- (void)setAppearanceSystem:(id)sender
{
    (void)sender;
    [self setAppearanceThemeForAutomation:classic_notepad::AppearanceTheme::System];
}

- (void)setAppearanceLight:(id)sender
{
    (void)sender;
    [self setAppearanceThemeForAutomation:classic_notepad::AppearanceTheme::Light];
}

- (void)setAppearanceDark:(id)sender
{
    (void)sender;
    [self setAppearanceThemeForAutomation:classic_notepad::AppearanceTheme::Dark];
}

- (void)updateStatusText
{
    if (self.statusField == nil) {
        return;
    }

    [self.statusField setStringValue:NSStringFromWide([self buildStatusText])];
}

- (std::wstring)getText
{
    return EditorTextFromTextView(self.textView);
}

- (void)setTextForAutomation:(const std::wstring&)text
{
    [self setEditorText:text markModified:YES];
}

- (void)insertTextAtSelection:(const std::wstring&)text
{
    [self.textView insertText:NSStringFromWide(text) replacementRange:[self.textView selectedRange]];
    _document.SetModified(true);
    [self updateWindowTitle];
    [self updateStatusText];
}

- (classic_notepad::macos::MacAutomationSelection)getSelectionForAutomation
{
    const NSRange range = [self.textView selectedRange];
    return {static_cast<std::size_t>(range.location), static_cast<std::size_t>(range.location + range.length)};
}

- (void)setSelectionStart:(std::size_t)start end:(std::size_t)end
{
    const std::wstring text = [self getText];
    start = std::min(start, text.size());
    end = std::min(end, text.size());
    if (end < start) {
        end = start;
    }
    [self.textView setSelectedRange:NSMakeRange(start, end - start)];
    [self.textView scrollRangeToVisible:[self.textView selectedRange]];
    [self updateStatusText];
}

- (void)deleteSelectionForAutomation
{
    [self.textView insertText:@"" replacementRange:[self.textView selectedRange]];
    _document.SetModified(true);
    [self updateWindowTitle];
    [self updateStatusText];
}

- (void)deleteForwardForAutomation
{
    NSRange range = [self.textView selectedRange];
    const std::wstring text = [self getText];
    if (range.length == 0 && range.location < text.size()) {
        range.length = 1;
    }
    [self.textView insertText:@"" replacementRange:range];
    _document.SetModified(true);
    [self updateWindowTitle];
    [self updateStatusText];
}

- (bool)findText:(const std::wstring&)needle matchCase:(bool)matchCase wholeWord:(bool)wholeWord searchDown:(bool)searchDown
{
    if (needle.empty()) {
        return false;
    }

    _findText = needle;
    return [self findStoredTextMatchCase:matchCase wholeWord:wholeWord searchDown:searchDown];
}

- (bool)findStoredTextMatchCase:(bool)matchCase wholeWord:(bool)wholeWord searchDown:(bool)searchDown
{
    if (_findText.empty()) {
        return false;
    }

    const std::wstring source = [self getText];
    const std::wstring haystack = matchCase ? source : LowercaseCopy(source);
    const std::wstring needle = matchCase ? _findText : LowercaseCopy(_findText);
    const NSRange selection = [self.textView selectedRange];

    if (searchDown) {
        std::size_t start = static_cast<std::size_t>(selection.location + selection.length);
        for (;;) {
            const std::size_t found = haystack.find(needle, start);
            if (found == std::wstring::npos) {
                return false;
            }
            if (!wholeWord || IsWholeWordMatch(source, found, needle.size())) {
                [self setSelectionStart:found end:found + needle.size()];
                return true;
            }
            start = found + 1U;
        }
    }

    std::size_t start = static_cast<std::size_t>(selection.location);
    while (start <= haystack.size()) {
        const std::size_t found = haystack.rfind(needle, start == 0U ? 0U : start - 1U);
        if (found == std::wstring::npos) {
            return false;
        }
        if (!wholeWord || IsWholeWordMatch(source, found, needle.size())) {
            [self setSelectionStart:found end:found + needle.size()];
            return true;
        }
        if (found == 0U) {
            return false;
        }
        start = found;
    }

    return false;
}

- (bool)replaceText:(const std::wstring&)needle replacement:(const std::wstring&)replacement matchCase:(bool)matchCase wholeWord:(bool)wholeWord searchDown:(bool)searchDown
{
    if (![self findText:needle matchCase:matchCase wholeWord:wholeWord searchDown:searchDown]) {
        return false;
    }

    [self insertTextAtSelection:replacement];
    return true;
}

- (std::size_t)replaceAllText:(const std::wstring&)needle replacement:(const std::wstring&)replacement matchCase:(bool)matchCase wholeWord:(bool)wholeWord
{
    if (needle.empty()) {
        return 0;
    }

    const std::wstring source = [self getText];
    const std::wstring haystack = matchCase ? source : LowercaseCopy(source);
    const std::wstring searchNeedle = matchCase ? needle : LowercaseCopy(needle);
    std::wstring result;
    std::size_t count = 0;
    std::size_t cursor = 0;

    while (cursor < source.size()) {
        const std::size_t found = haystack.find(searchNeedle, cursor);
        if (found == std::wstring::npos) {
            result.append(source.substr(cursor));
            break;
        }

        if (wholeWord && !IsWholeWordMatch(source, found, needle.size())) {
            result.append(source.substr(cursor, found - cursor + 1U));
            cursor = found + 1U;
            continue;
        }

        result.append(source.substr(cursor, found - cursor));
        result.append(replacement);
        cursor = found + needle.size();
        ++count;
    }

    if (count > 0U) {
        [self setTextForAutomation:result];
    }
    return count;
}

- (bool)goToLineNumber:(int)lineNumber errorMessage:(std::wstring&)errorMessage
{
    const std::wstring text = [self getText];
    const std::size_t lineCount = CountLines(text);
    if (lineNumber < 1 || static_cast<std::size_t>(lineNumber) > lineCount) {
        errorMessage = L"Line number must be between 1 and ";
        errorMessage += std::to_wstring(lineCount);
        return false;
    }

    std::size_t position = 0;
    for (int line = 1; line < lineNumber && position < text.size(); ++position) {
        if (text[position] == L'\n') {
            ++line;
        }
    }

    [self setSelectionStart:position end:position];
    return true;
}

- (std::wstring)buildTimeDateText
{
    std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_r(&now, &localTime);
    wchar_t buffer[128]{};
    std::wcsftime(buffer, std::size(buffer), L"%H:%M %d/%m/%Y", &localTime);
    return buffer;
}

- (void)setWordWrap:(bool)enabled
{
    _wordWrap = enabled;
    NSTextContainer* container = [self.textView textContainer];
    if (enabled) {
        [self.textView setHorizontallyResizable:NO];
        [self.textView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [container setContainerSize:NSMakeSize([[self.textView enclosingScrollView] contentSize].width, CGFLOAT_MAX)];
        [container setWidthTracksTextView:YES];
        [[self.textView enclosingScrollView] setHasHorizontalScroller:NO];
    } else {
        [self.textView setHorizontallyResizable:YES];
        [self.textView setAutoresizingMask:NSViewWidthSizable];
        [container setContainerSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)];
        [container setWidthTracksTextView:NO];
        [[self.textView enclosingScrollView] setHasHorizontalScroller:YES];
    }
    [[NSApp mainMenu] update];
}

- (void)setStatusBarVisible:(bool)visible
{
    _statusBarVisible = visible;
    if (self.statusField == nil) {
        return;
    }

    [self.statusField setHidden:!visible];
    NSScrollView* scrollView = [self.textView enclosingScrollView];
    NSRect contentBounds = [[self.window contentView] bounds];
    const CGFloat statusHeight = visible ? 24.0 : 0.0;
    [scrollView setFrame:NSMakeRect(0, statusHeight, contentBounds.size.width, contentBounds.size.height - statusHeight)];
    [[NSApp mainMenu] update];
}

- (bool)setFontDescription:(const std::wstring&)font errorMessage:(std::wstring&)errorMessage
{
    (void)errorMessage;
    if (font.empty()) {
        errorMessage = L"Font description cannot be empty.";
        return false;
    }
    _fontDescription = font;
    [self.textView setFont:[NSFont userFixedPitchFontOfSize:13.0]];
    return true;
}

- (std::wstring)buildStatusText
{
    const std::wstring text = [self getText];
    const classic_notepad::macos::MacAutomationSelection selection = [self getSelectionForAutomation];
    std::size_t line = 1;
    std::size_t column = 1;
    const std::size_t caret = std::min(selection.start, text.size());
    for (std::size_t index = 0; index < caret; ++index) {
        if (text[index] == L'\n') {
            ++line;
            column = 1;
        } else if (text[index] != L'\r') {
            ++column;
        }
    }

    std::wstring status = L"Ln ";
    status += std::to_wstring(line);
    status += L", Col ";
    status += std::to_wstring(column);
    status += L"  ";
    status += classic_notepad::FormatCharacterCount(classic_notepad::CountStatusCharacters(text));
    status += L"  ";
    status += classic_notepad::FormatLineEnding(_document.LineEnding());
    status += L"  ";
    status += classic_notepad::FormatEncoding(_document.Encoding());
    return status;
}

- (classic_notepad::macos::MacAutomationDocumentMetadata)getMetadataForAutomation
{
    classic_notepad::macos::MacAutomationDocumentMetadata metadata;
    metadata.path = _document.Path();
    metadata.displayName = _document.DisplayName();
    metadata.hasPath = _document.HasPath();
    metadata.encoding = classic_notepad::FormatEncoding(_document.Encoding());
    metadata.lineEnding = classic_notepad::FormatLineEnding(_document.LineEnding());
    metadata.saveLineEnding = classic_notepad::FormatLineEnding(_document.SaveLineEnding());
    return metadata;
}

- (bool)printToTestSink:(const std::wstring&)path errorMessage:(std::wstring&)errorMessage
{
    std::vector<std::uint8_t> bytes;
    const std::wstring sinkText = BuildPrintSinkText(_fontDescription, _pageMargins, [self getText]);
    if (!classic_notepad::EncodeTextBytes(sinkText, classic_notepad::TextEncoding::Utf8NoBom, bytes, errorMessage)) {
        return false;
    }
    return classic_notepad::WriteFileBytesAtomically(path, bytes, errorMessage);
}

- (void)setAppearanceThemeForAutomation:(classic_notepad::AppearanceTheme)theme
{
    _appearanceTheme = theme;
    _appearanceConfiguration = classic_notepad::macos::ApplyApplicationAppearance(NSApp, _appearanceTheme);
    _appearanceConfiguration = classic_notepad::macos::ApplyWindowAppearance(self.window, _appearanceTheme);
    classic_notepad::macos::ConfigurePlainTextViewAppearance(self.textView);
    [[NSApp mainMenu] update];
}

- (classic_notepad::macos::MacAutomationAppearance)getAppearanceForAutomation
{
    classic_notepad::macos::MacAutomationAppearance appearance;
    appearance.theme = _appearanceTheme;
    appearance.darkMode = _appearanceConfiguration.darkMode;
    appearance.highContrast = false;
    appearance.effectiveAppearance = _appearanceConfiguration.darkMode ? L"dark" : L"light";
    return appearance;
}

- (std::vector<classic_notepad::macos::MacAutomationSpellingIssue>)checkSpellingText:(const std::wstring&)text
{
    std::vector<classic_notepad::macos::MacAutomationSpellingIssue> issues;
    if (_spellingConfiguration.capability != classic_notepad::SpellCapability::Available) {
        return issues;
    }

    NSString* string = NSStringFromWide(text);
    NSSpellChecker* checker = [NSSpellChecker sharedSpellChecker];
    NSInteger offset = 0;
    while (offset < static_cast<NSInteger>([string length])) {
        const NSRange range = [checker checkSpellingOfString:string startingAt:offset language:NSStringFromUtf8(_spellingConfiguration.languageCode) wrap:NO inSpellDocumentWithTag:0 wordCount:nil];
        if (range.location == NSNotFound) {
            break;
        }
        classic_notepad::macos::MacAutomationSpellingIssue issue;
        issue.start = range.location;
        issue.length = range.length;
        issue.action = L"flag";
        issues.push_back(issue);
        offset = static_cast<NSInteger>(range.location + std::max<NSUInteger>(range.length, 1));
    }
    return issues;
}

- (std::vector<std::wstring>)suggestSpellingForWord:(const std::wstring&)word limit:(std::size_t)limit
{
    std::vector<std::wstring> suggestions;
    if (_spellingConfiguration.capability != classic_notepad::SpellCapability::Available) {
        return suggestions;
    }

    NSArray<NSString*>* guesses = [[NSSpellChecker sharedSpellChecker]
        guessesForWordRange:NSMakeRange(0, [NSStringFromWide(word) length])
        inString:NSStringFromWide(word)
        language:NSStringFromUtf8(_spellingConfiguration.languageCode)
        inSpellDocumentWithTag:0];
    for (NSString* guess in guesses) {
        if (suggestions.size() >= limit) {
            break;
        }
        suggestions.push_back(WideFromNSString(guess));
    }
    return suggestions;
}

- (bool)wordWrapEnabled
{
    return _wordWrap;
}

- (bool)statusBarVisible
{
    return _statusBarVisible;
}

- (std::wstring)fontDescription
{
    return _fontDescription;
}

- (classic_notepad::macos::MacAutomationPageMargins)pageMargins
{
    return _pageMargins;
}

- (void)setPageMarginsForAutomation:(const classic_notepad::macos::MacAutomationPageMargins&)margins
{
    _pageMargins = margins;
    [self syncPrintInfoFromPageMargins];
}

- (void)syncPageMarginsFromPrintInfo
{
    if (_printInfo == nil) {
        return;
    }

    _pageMargins.left = ThousandthsFromPoints([_printInfo leftMargin]);
    _pageMargins.top = ThousandthsFromPoints([_printInfo topMargin]);
    _pageMargins.right = ThousandthsFromPoints([_printInfo rightMargin]);
    _pageMargins.bottom = ThousandthsFromPoints([_printInfo bottomMargin]);
}

- (void)syncPrintInfoFromPageMargins
{
    if (_printInfo == nil) {
        return;
    }

    [_printInfo setLeftMargin:PointsFromThousandths(_pageMargins.left)];
    [_printInfo setTopMargin:PointsFromThousandths(_pageMargins.top)];
    [_printInfo setRightMargin:PointsFromThousandths(_pageMargins.right)];
    [_printInfo setBottomMargin:PointsFromThousandths(_pageMargins.bottom)];
}

- (classic_notepad::macos::MacSpellingConfiguration)spellingConfiguration
{
    return _spellingConfiguration;
}

@end

class MacAutomationDelegateHost final : public classic_notepad::macos::MacAutomationHost {
public:
    explicit MacAutomationDelegateHost(ClassicNotepadMacDelegate* delegate)
        : delegate_(delegate)
    {
    }

    void PumpEvents() override
    {
        for (;;) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event == nil) {
                break;
            }
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
    }

    void NewDocument() override { [delegate_ resetUntitledDocument]; }
    bool OpenFile(const std::wstring& path, std::wstring& errorMessage) override
    {
        (void)errorMessage;
        return [delegate_ loadPath:path] == YES;
    }
    bool Save(std::wstring& errorMessage) override
    {
        if ([delegate_ save]) {
            return true;
        }
        errorMessage = L"Save failed.";
        return false;
    }
    bool SaveAs(const std::wstring& path, std::wstring& errorMessage) override
    {
        return [delegate_ saveAsPath:path errorMessage:errorMessage] == YES;
    }
    void SetText(const std::wstring& text) override { [delegate_ setTextForAutomation:text]; }
    void InsertText(const std::wstring& text) override { [delegate_ insertTextAtSelection:text]; }
    std::wstring GetText() const override { return [delegate_ getText]; }
    std::wstring GetTitle() const override { return WideFromNSString([delegate_.window title]); }
    bool IsModified() const override { return [delegate_.window isDocumentEdited] == YES; }
    classic_notepad::macos::MacAutomationDocumentMetadata GetDocumentMetadata() const override
    {
        return [delegate_ getMetadataForAutomation];
    }
    std::wstring GetStatusText() const override { return [delegate_ buildStatusText]; }
    classic_notepad::macos::MacAutomationSelection GetSelection() const override
    {
        return [delegate_ getSelectionForAutomation];
    }
    void SetSelection(std::size_t start, std::size_t end) override { [delegate_ setSelectionStart:start end:end]; }
    void SelectAll() override { [delegate_.textView selectAll:nil]; }
    void Undo() override { [[delegate_.textView undoManager] undo]; }
    void DeleteSelection() override { [delegate_ deleteSelectionForAutomation]; }
    void DeleteForward() override { [delegate_ deleteForwardForAutomation]; }
    bool Find(const std::wstring& text, bool matchCase, bool wholeWord, bool searchDown) override
    {
        return [delegate_ findText:text matchCase:matchCase wholeWord:wholeWord searchDown:searchDown];
    }
    bool FindNext(bool matchCase, bool wholeWord, bool searchDown) override
    {
        return [delegate_ findStoredTextMatchCase:matchCase wholeWord:wholeWord searchDown:searchDown];
    }
    bool Replace(
        const std::wstring& text,
        const std::wstring& replacement,
        bool matchCase,
        bool wholeWord,
        bool searchDown) override
    {
        return [delegate_ replaceText:text replacement:replacement matchCase:matchCase wholeWord:wholeWord searchDown:searchDown];
    }
    std::size_t ReplaceAll(const std::wstring& text, const std::wstring& replacement, bool matchCase, bool wholeWord) override
    {
        return [delegate_ replaceAllText:text replacement:replacement matchCase:matchCase wholeWord:wholeWord];
    }
    bool GoToLine(int lineNumber, std::wstring& errorMessage) override
    {
        return [delegate_ goToLineNumber:lineNumber errorMessage:errorMessage];
    }
    void InsertTimeDate() override { [delegate_ insertTextAtSelection:[delegate_ buildTimeDateText]]; }
    void SetWordWrap(bool enabled) override { [delegate_ setWordWrap:enabled]; }
    bool GetWordWrap() const override { return [delegate_ wordWrapEnabled]; }
    void SetStatusBarVisible(bool visible) override { [delegate_ setStatusBarVisible:visible]; }
    bool GetStatusBarVisible() const override { return [delegate_ statusBarVisible]; }
    bool SetFont(const std::wstring& font, std::wstring& errorMessage) override
    {
        return [delegate_ setFontDescription:font errorMessage:errorMessage];
    }
    std::wstring GetFont() const override { return [delegate_ fontDescription]; }
    classic_notepad::macos::MacAutomationPageMargins GetPageMargins() const override { return [delegate_ pageMargins]; }
    bool SetPageMargins(const classic_notepad::macos::MacAutomationPageMargins& margins, std::wstring& errorMessage) override
    {
        (void)errorMessage;
        [delegate_ setPageMarginsForAutomation:margins];
        return true;
    }
    bool PrintToTestSink(const std::wstring& path, std::wstring& errorMessage) const override
    {
        return [delegate_ printToTestSink:path errorMessage:errorMessage];
    }
    classic_notepad::macos::MacAutomationAppearance GetAppearance() const override
    {
        return [delegate_ getAppearanceForAutomation];
    }
    void SetAppearanceTheme(classic_notepad::AppearanceTheme theme) override
    {
        [delegate_ setAppearanceThemeForAutomation:theme];
    }
    classic_notepad::SpellCapability SpellCheckCapability() const override { return [delegate_ spellingConfiguration].capability; }
    std::wstring SpellCheckLanguage() const override { return WideFromUtf8([delegate_ spellingConfiguration].languageCode); }
    std::vector<classic_notepad::macos::MacAutomationSpellingIssue> CheckSpelling(const std::wstring& text) const override
    {
        return [delegate_ checkSpellingText:text];
    }
    std::vector<std::wstring> SuggestSpelling(const std::wstring& word, std::size_t limit) const override
    {
        return [delegate_ suggestSpellingForWord:word limit:limit];
    }
    bool IgnoreSpelling(const std::wstring& word, std::wstring& errorMessage) override
    {
        (void)errorMessage;
        [[NSSpellChecker sharedSpellChecker] ignoreWord:NSStringFromWide(word) inSpellDocumentWithTag:0];
        return true;
    }
    bool AddSpelling(const std::wstring& word, bool dryRun, std::wstring& errorMessage) override
    {
        (void)errorMessage;
        if (!dryRun) {
            [[NSSpellChecker sharedSpellChecker] learnWord:NSStringFromWide(word)];
        }
        return true;
    }

private:
    ClassicNotepadMacDelegate* delegate_;
};

class HeadlessMacAutomationHost final : public classic_notepad::macos::MacAutomationHost {
public:
    explicit HeadlessMacAutomationHost(std::wstring initialPath)
    {
        document_.ResetUntitled();
        appearance_.theme = classic_notepad::macos::AppearanceThemeFromEnvironment();
        ApplyTheme(appearance_.theme);
        spellingCapability_ = classic_notepad::SpellCapability::MissingBackend;
        if (!initialPath.empty()) {
            std::wstring error;
            if (!PathExists(initialPath)) {
                document_.ResetNewFile(initialPath);
            } else {
                OpenFile(initialPath, error);
            }
        }
    }

    void PumpEvents() override {}
    void NewDocument() override
    {
        PushUndo();
        document_.ResetUntitled();
        text_.clear();
        selection_ = {};
    }
    bool OpenFile(const std::wstring& path, std::wstring& errorMessage) override
    {
        std::wstring editorText;
        if (!document_.Load(path, editorText, errorMessage)) {
            return false;
        }
        text_ = editorText;
        selection_ = {};
        undoText_.reset();
        return true;
    }
    bool Save(std::wstring& errorMessage) override { return document_.Save(text_, errorMessage); }
    bool SaveAs(const std::wstring& path, std::wstring& errorMessage) override { return document_.SaveAs(path, text_, errorMessage); }
    void SetText(const std::wstring& text) override
    {
        PushUndo();
        text_ = text;
        selection_ = {0, 0};
        document_.SetModified(true);
    }
    void InsertText(const std::wstring& text) override
    {
        PushUndo();
        const std::size_t start = std::min(selection_.start, text_.size());
        const std::size_t end = std::min(selection_.end, text_.size());
        text_.replace(start, end - start, text);
        selection_ = {start + text.size(), start + text.size()};
        document_.SetModified(true);
    }
    std::wstring GetText() const override { return text_; }
    std::wstring GetTitle() const override { return WideFromNSString(WindowTitle(document_)); }
    bool IsModified() const override { return document_.IsModified(); }
    classic_notepad::macos::MacAutomationDocumentMetadata GetDocumentMetadata() const override
    {
        classic_notepad::macos::MacAutomationDocumentMetadata metadata;
        metadata.path = document_.Path();
        metadata.displayName = document_.DisplayName();
        metadata.hasPath = document_.HasPath();
        metadata.encoding = classic_notepad::FormatEncoding(document_.Encoding());
        metadata.lineEnding = classic_notepad::FormatLineEnding(document_.LineEnding());
        metadata.saveLineEnding = classic_notepad::FormatLineEnding(document_.SaveLineEnding());
        return metadata;
    }
    std::wstring GetStatusText() const override
    {
        std::size_t line = 1;
        std::size_t column = 1;
        const std::size_t caret = std::min(selection_.start, text_.size());
        for (std::size_t index = 0; index < caret; ++index) {
            if (text_[index] == L'\n') {
                ++line;
                column = 1;
            } else if (text_[index] != L'\r') {
                ++column;
            }
        }
        std::wstring status = L"Ln " + std::to_wstring(line) + L", Col " + std::to_wstring(column) + L"  ";
        status += classic_notepad::FormatCharacterCount(classic_notepad::CountStatusCharacters(text_));
        status += L"  ";
        status += classic_notepad::FormatLineEnding(document_.LineEnding());
        status += L"  ";
        status += classic_notepad::FormatEncoding(document_.Encoding());
        return status;
    }
    classic_notepad::macos::MacAutomationSelection GetSelection() const override { return selection_; }
    void SetSelection(std::size_t start, std::size_t end) override
    {
        start = std::min(start, text_.size());
        end = std::min(end, text_.size());
        if (end < start) {
            end = start;
        }
        selection_ = {start, end};
    }
    void SelectAll() override { selection_ = {0, text_.size()}; }
    void Undo() override
    {
        if (!undoText_.has_value()) {
            return;
        }
        text_ = *undoText_;
        undoText_.reset();
        selection_ = {text_.size(), text_.size()};
        document_.SetModified(true);
    }
    void DeleteSelection() override
    {
        if (selection_.end <= selection_.start) {
            return;
        }
        PushUndo();
        text_.erase(selection_.start, selection_.end - selection_.start);
        selection_.end = selection_.start;
        document_.SetModified(true);
    }
    void DeleteForward() override
    {
        if (selection_.end > selection_.start) {
            DeleteSelection();
            return;
        }
        if (selection_.start >= text_.size()) {
            return;
        }
        PushUndo();
        text_.erase(selection_.start, 1);
        document_.SetModified(true);
    }
    bool Find(const std::wstring& text, bool matchCase, bool wholeWord, bool searchDown) override
    {
        findText_ = text;
        selection_ = searchDown ? classic_notepad::macos::MacAutomationSelection{0, 0}
            : classic_notepad::macos::MacAutomationSelection{text_.size(), text_.size()};
        return FindNext(matchCase, wholeWord, searchDown);
    }
    bool FindNext(bool matchCase, bool wholeWord, bool searchDown) override
    {
        if (findText_.empty()) {
            return false;
        }
        const std::wstring haystack = matchCase ? text_ : LowercaseCopy(text_);
        const std::wstring needle = matchCase ? findText_ : LowercaseCopy(findText_);
        if (searchDown) {
            std::size_t start = selection_.end;
            for (;;) {
                const std::size_t found = haystack.find(needle, start);
                if (found == std::wstring::npos) {
                    return false;
                }
                if (!wholeWord || IsWholeWordMatch(text_, found, needle.size())) {
                    selection_ = {found, found + needle.size()};
                    return true;
                }
                start = found + 1U;
            }
        }

        std::size_t start = selection_.start;
        while (start <= haystack.size()) {
            const std::size_t found = haystack.rfind(needle, start == 0U ? 0U : start - 1U);
            if (found == std::wstring::npos) {
                return false;
            }
            if (!wholeWord || IsWholeWordMatch(text_, found, needle.size())) {
                selection_ = {found, found + needle.size()};
                return true;
            }
            if (found == 0U) {
                return false;
            }
            start = found;
        }
        return false;
    }
    bool Replace(const std::wstring& text, const std::wstring& replacement, bool matchCase, bool wholeWord, bool searchDown) override
    {
        bool selectedMatch = false;
        if (selection_.end > selection_.start && selection_.end <= text_.size()) {
            const std::wstring selected = text_.substr(selection_.start, selection_.end - selection_.start);
            selectedMatch = matchCase ? selected == text : LowercaseCopy(selected) == LowercaseCopy(text);
            if (selectedMatch && wholeWord) {
                selectedMatch = IsWholeWordMatch(text_, selection_.start, text.size());
            }
        }
        if (!selectedMatch && !Find(text, matchCase, wholeWord, searchDown)) {
            return false;
        }
        InsertText(replacement);
        return true;
    }
    std::size_t ReplaceAll(const std::wstring& text, const std::wstring& replacement, bool matchCase, bool wholeWord) override
    {
        if (text.empty()) {
            return 0;
        }
        const std::wstring haystack = matchCase ? text_ : LowercaseCopy(text_);
        const std::wstring needle = matchCase ? text : LowercaseCopy(text);
        std::wstring result;
        std::size_t count = 0;
        std::size_t cursor = 0;
        while (cursor < text_.size()) {
            const std::size_t found = haystack.find(needle, cursor);
            if (found == std::wstring::npos) {
                result += text_.substr(cursor);
                break;
            }
            if (wholeWord && !IsWholeWordMatch(text_, found, text.size())) {
                result += text_.substr(cursor, found - cursor + 1U);
                cursor = found + 1U;
                continue;
            }
            result += text_.substr(cursor, found - cursor);
            result += replacement;
            cursor = found + text.size();
            ++count;
        }
        if (count > 0U) {
            PushUndo();
            text_ = result;
            selection_ = {text_.size(), text_.size()};
            document_.SetModified(true);
        }
        return count;
    }
    bool GoToLine(int lineNumber, std::wstring& errorMessage) override
    {
        const std::size_t lineCount = CountLines(text_);
        if (lineNumber < 1 || static_cast<std::size_t>(lineNumber) > lineCount) {
            errorMessage = L"Line number must be between 1 and " + std::to_wstring(lineCount) + L".";
            return false;
        }
        std::size_t position = 0;
        for (int line = 1; line < lineNumber && position < text_.size(); ++position) {
            if (text_[position] == L'\n') {
                ++line;
            }
        }
        selection_ = {position, position};
        return true;
    }
    void InsertTimeDate() override { InsertText(BuildTimeDate()); }
    void SetWordWrap(bool enabled) override { wordWrap_ = enabled; }
    bool GetWordWrap() const override { return wordWrap_; }
    void SetStatusBarVisible(bool visible) override { statusBarVisible_ = visible; }
    bool GetStatusBarVisible() const override { return statusBarVisible_; }
    bool SetFont(const std::wstring& font, std::wstring& errorMessage) override
    {
        if (font.empty()) {
            errorMessage = L"Font description cannot be empty.";
            return false;
        }
        fontDescription_ = font;
        return true;
    }
    std::wstring GetFont() const override { return fontDescription_; }
    classic_notepad::macos::MacAutomationPageMargins GetPageMargins() const override { return margins_; }
    bool SetPageMargins(const classic_notepad::macos::MacAutomationPageMargins& margins, std::wstring& errorMessage) override
    {
        (void)errorMessage;
        margins_ = margins;
        return true;
    }
    bool PrintToTestSink(const std::wstring& path, std::wstring& errorMessage) const override
    {
        std::vector<std::uint8_t> bytes;
        if (!classic_notepad::EncodeTextBytes(BuildPrintSinkText(fontDescription_, margins_, text_), classic_notepad::TextEncoding::Utf8NoBom, bytes, errorMessage)) {
            return false;
        }
        return classic_notepad::WriteFileBytesAtomically(path, bytes, errorMessage);
    }
    classic_notepad::macos::MacAutomationAppearance GetAppearance() const override { return appearance_; }
    void SetAppearanceTheme(classic_notepad::AppearanceTheme theme) override { ApplyTheme(theme); }
    classic_notepad::SpellCapability SpellCheckCapability() const override { return spellingCapability_; }
    std::wstring SpellCheckLanguage() const override { return WideFromUtf8(spellingLanguage_); }
    std::vector<classic_notepad::macos::MacAutomationSpellingIssue> CheckSpelling(const std::wstring& text) const override
    {
        (void)text;
        return {};
    }
    std::vector<std::wstring> SuggestSpelling(const std::wstring& word, std::size_t limit) const override
    {
        (void)word;
        (void)limit;
        return {};
    }
    bool IgnoreSpelling(const std::wstring& word, std::wstring& errorMessage) override
    {
        (void)errorMessage;
        (void)word;
        return true;
    }
    bool AddSpelling(const std::wstring& word, bool dryRun, std::wstring& errorMessage) override
    {
        (void)errorMessage;
        (void)word;
        (void)dryRun;
        return true;
    }

private:
    void PushUndo() { undoText_ = text_; }
    void ApplyTheme(classic_notepad::AppearanceTheme theme)
    {
        appearance_.theme = theme;
        appearance_.darkMode = theme == classic_notepad::AppearanceTheme::Dark;
        appearance_.highContrast = false;
        appearance_.effectiveAppearance = appearance_.darkMode ? L"dark" : L"light";
    }
    std::wstring BuildTimeDate() const
    {
        std::time_t now = std::time(nullptr);
        std::tm localTime{};
        localtime_r(&now, &localTime);
        wchar_t buffer[128]{};
        std::wcsftime(buffer, std::size(buffer), L"%H:%M %d/%m/%Y", &localTime);
        return buffer;
    }

    Document document_;
    std::wstring text_;
    classic_notepad::macos::MacAutomationSelection selection_;
    std::optional<std::wstring> undoText_;
    std::wstring findText_;
    bool wordWrap_ = false;
    bool statusBarVisible_ = true;
    std::wstring fontDescription_ = L"Menlo 13";
    classic_notepad::macos::MacAutomationPageMargins margins_;
    classic_notepad::macos::MacAutomationAppearance appearance_;
    classic_notepad::SpellCapability spellingCapability_ = classic_notepad::SpellCapability::MissingDictionary;
    std::string spellingLanguage_;
};

namespace classic_notepad::macos {

int ClassicNotepadMacApp::Run(int argc, char* argv[])
{
    bool automationMode = false;
    bool automationVisible = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            continue;
        }
        const std::string argument = argv[index];
        if (argument == "--automation") {
            automationMode = true;
        } else if (argument == "--automation-visible") {
            automationVisible = true;
        } else if (initialPath_.empty()) {
            initialPath_ = std::filesystem::path(argument).wstring();
        }
    }

    @autoreleasepool {
        if (automationMode && !automationVisible) {
            HeadlessMacAutomationHost host(initialPath_);
            MacAutomationController controller(host);
            return controller.Run();
        }

        NSApplication* application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];

        ClassicNotepadMacDelegate* delegate = [[ClassicNotepadMacDelegate alloc]
            initWithInitialPath:initialPath_
                 automationMode:automationMode
              automationVisible:automationVisible];
        [application setDelegate:delegate];
        if (automationMode) {
            [application finishLaunching];
            MacAutomationDelegateHost host(delegate);
            MacAutomationController controller(host);
            return controller.Run();
        }

        [application activateIgnoringOtherApps:YES];
        [application run];
    }

    return 0;
}

} // namespace classic_notepad::macos
