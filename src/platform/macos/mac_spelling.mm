#include "mac_spelling.h"

#import <AppKit/AppKit.h>

namespace classic_notepad::macos {
namespace {

std::string StringFromNSString(NSString* value)
{
    if (value == nil) {
        return {};
    }

    const char* utf8 = [value UTF8String];
    return utf8 == nullptr ? std::string() : std::string(utf8);
}

NSString* SelectBritishEnglishSpellLanguageObject()
{
    NSSpellChecker* checker = [NSSpellChecker sharedSpellChecker];
    NSArray<NSString*>* languages = [checker availableLanguages];
    NSArray<NSString*>* candidates = @[
        @"en_GB",
        @"en-GB",
        @"en_GB@oxendict",
        @"en-GB-oxendict"
    ];

    for (NSString* candidate in candidates) {
        if ([languages containsObject:candidate]) {
            return candidate;
        }
    }

    for (NSString* language in languages) {
        NSString* normalized = [[language stringByReplacingOccurrencesOfString:@"-" withString:@"_"] lowercaseString];
        if ([normalized isEqualToString:@"en_gb"] || [normalized hasPrefix:@"en_gb."] || [normalized hasPrefix:@"en_gb@"]) {
            return language;
        }
    }

    return nil;
}

} // namespace

std::string SelectBritishEnglishSpellLanguage()
{
    return StringFromNSString(SelectBritishEnglishSpellLanguageObject());
}

MacSpellingConfiguration ConfigureTextViewSpelling(NSTextView* textView)
{
    MacSpellingConfiguration configuration;
    if (textView == nil) {
        configuration.capability = classic_notepad::SpellCapability::MissingBackend;
        return configuration;
    }

    NSString* language = SelectBritishEnglishSpellLanguageObject();
    if (language == nil) {
        [textView setContinuousSpellCheckingEnabled:NO];
        [textView setAutomaticSpellingCorrectionEnabled:NO];
        configuration.capability = classic_notepad::SpellCapability::MissingDictionary;
        return configuration;
    }

    NSSpellChecker* checker = [NSSpellChecker sharedSpellChecker];
    [checker setLanguage:language];

    [textView setContinuousSpellCheckingEnabled:YES];
    [textView setGrammarCheckingEnabled:NO];
    [textView setAutomaticSpellingCorrectionEnabled:NO];
    if ([textView respondsToSelector:@selector(setAutomaticTextCompletionEnabled:)]) {
        [textView setAutomaticTextCompletionEnabled:NO];
    }
    if ([textView respondsToSelector:@selector(setAutomaticQuoteSubstitutionEnabled:)]) {
        [textView setAutomaticQuoteSubstitutionEnabled:NO];
    }
    if ([textView respondsToSelector:@selector(setAutomaticDashSubstitutionEnabled:)]) {
        [textView setAutomaticDashSubstitutionEnabled:NO];
    }
    [textView setRichText:NO];
    [textView setImportsGraphics:NO];

    configuration.capability = classic_notepad::SpellCapability::Available;
    configuration.languageCode = StringFromNSString(language);
    return configuration;
}

} // namespace classic_notepad::macos
