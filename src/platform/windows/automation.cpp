#include "automation.h"

#include "app.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

enum class JsonValueType {
    Null,
    String,
    Number,
    Boolean
};

struct JsonValue {
    JsonValueType type = JsonValueType::Null;
    std::wstring stringValue;
    long long numberValue = 0;
    bool booleanValue = false;
};

using JsonObject = std::map<std::string, JsonValue>;

long long ElapsedMicroseconds(std::chrono::steady_clock::time_point started)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
}

bool ReadUtf16LeFile(const std::wstring& path, std::wstring& text, std::wstring& errorMessage)
{
    static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 automation files require 16-bit wchar_t.");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errorMessage = L"The UTF-16 input file could not be opened.";
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0 || (fileSize.QuadPart % 2) != 0 ||
        fileSize.QuadPart > 1024LL * 1024LL * 1024LL) {
        CloseHandle(file);
        errorMessage = L"The UTF-16 input file must have an even byte length no larger than 1 GiB.";
        return false;
    }

    try {
        text.assign(static_cast<std::size_t>(fileSize.QuadPart / 2), L'\0');
    } catch (...) {
        CloseHandle(file);
        errorMessage = L"The UTF-16 input file could not be allocated.";
        return false;
    }

    const std::size_t byteCount = text.size() * sizeof(wchar_t);
    std::size_t offset = 0;
    while (offset < byteCount) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(byteCount - offset, 4U * 1024U * 1024U));
        DWORD read = 0;
        if (!ReadFile(file, reinterpret_cast<char*>(text.data()) + offset, request, &read, nullptr) || read != request) {
            CloseHandle(file);
            text.clear();
            errorMessage = L"The UTF-16 input file could not be read completely.";
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

bool WriteUtf16LeFile(const std::wstring& path, const std::wstring& text, std::wstring& errorMessage)
{
    static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 automation files require 16-bit wchar_t.");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errorMessage = L"The UTF-16 output file could not be created.";
        return false;
    }

    const std::size_t byteCount = text.size() * sizeof(wchar_t);
    std::size_t offset = 0;
    while (offset < byteCount) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(byteCount - offset, 4U * 1024U * 1024U));
        DWORD written = 0;
        if (!WriteFile(file, reinterpret_cast<const char*>(text.data()) + offset, request, &written, nullptr) ||
            written != request) {
            CloseHandle(file);
            errorMessage = L"The UTF-16 output file could not be written completely.";
            return false;
        }
        offset += written;
    }
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!flushed) {
        errorMessage = L"The UTF-16 output file could not be flushed.";
        return false;
    }
    return true;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }

    int wideLength = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (wideLength <= 0) {
        wideLength = MultiByteToWideChar(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0);
    }

    if (wideLength <= 0) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        wide.data(),
        wideLength);
    return wide;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }

    const int byteLength = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (byteLength <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(byteLength), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        utf8.data(),
        byteLength,
        nullptr,
        nullptr);
    return utf8;
}

std::string NarrowAscii(const std::wstring& text)
{
    std::string narrowed;
    narrowed.reserve(text.size());
    for (wchar_t character : text) {
        narrowed.push_back(character <= 0x7f ? static_cast<char>(character) : '?');
    }
    return narrowed;
}

void AppendUtf8Chunk(std::wstring& target, std::string& chunk)
{
    if (!chunk.empty()) {
        target += Utf8ToWide(chunk);
        chunk.clear();
    }
}

bool IsHexDigit(char character)
{
    return std::isxdigit(static_cast<unsigned char>(character)) != 0;
}

int HexValue(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }

    if (character >= 'a' && character <= 'f') {
        return 10 + character - 'a';
    }

    if (character >= 'A' && character <= 'F') {
        return 10 + character - 'A';
    }

    return 0;
}

std::optional<std::uint16_t> ParseHexQuad(const std::string& text, std::size_t position)
{
    if (position + 4U > text.size()) {
        return std::nullopt;
    }

    std::uint16_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        const char character = text[position + index];
        if (!IsHexDigit(character)) {
            return std::nullopt;
        }

        value = static_cast<std::uint16_t>((value << 4) | HexValue(character));
    }

    return value;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& text)
        : text_(text)
    {
    }

    bool ParseObject(JsonObject& object, std::string& error)
    {
        object.clear();
        SkipWhitespace();
        if (!Consume('{')) {
            error = "Expected JSON object.";
            return false;
        }

        SkipWhitespace();
        if (Consume('}')) {
            SkipWhitespace();
            return position_ == text_.size();
        }

        for (;;) {
            std::wstring key;
            if (!ParseString(key)) {
                error = "Expected object key.";
                return false;
            }

            SkipWhitespace();
            if (!Consume(':')) {
                error = "Expected ':' after object key.";
                return false;
            }

            JsonValue value;
            if (!ParseValue(value)) {
                error = "Expected object value.";
                return false;
            }

            object[NarrowAscii(key)] = std::move(value);

            SkipWhitespace();
            if (Consume('}')) {
                SkipWhitespace();
                if (position_ != text_.size()) {
                    error = "Unexpected text after JSON object.";
                    return false;
                }

                return true;
            }

            if (!Consume(',')) {
                error = "Expected ',' between object members.";
                return false;
            }
        }
    }

private:
    void SkipWhitespace()
    {
        while (position_ < text_.size() &&
            std::isspace(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }
    }

    bool Consume(char expected)
    {
        SkipWhitespace();
        if (position_ >= text_.size() || text_[position_] != expected) {
            return false;
        }

        ++position_;
        return true;
    }

    bool ParseValue(JsonValue& value)
    {
        SkipWhitespace();
        if (position_ >= text_.size()) {
            return false;
        }

        if (text_[position_] == '"') {
            value.type = JsonValueType::String;
            return ParseString(value.stringValue);
        }

        if (Matches("true")) {
            value.type = JsonValueType::Boolean;
            value.booleanValue = true;
            position_ += 4U;
            return true;
        }

        if (Matches("false")) {
            value.type = JsonValueType::Boolean;
            value.booleanValue = false;
            position_ += 5U;
            return true;
        }

        if (Matches("null")) {
            value.type = JsonValueType::Null;
            position_ += 4U;
            return true;
        }

        return ParseNumber(value);
    }

    bool ParseString(std::wstring& value)
    {
        SkipWhitespace();
        if (position_ >= text_.size() || text_[position_] != '"') {
            return false;
        }

        ++position_;
        value.clear();
        std::string utf8Chunk;

        while (position_ < text_.size()) {
            const char character = text_[position_++];
            if (character == '"') {
                AppendUtf8Chunk(value, utf8Chunk);
                return true;
            }

            if (static_cast<unsigned char>(character) < 0x20U) {
                return false;
            }

            if (character != '\\') {
                utf8Chunk.push_back(character);
                continue;
            }

            if (position_ >= text_.size()) {
                return false;
            }

            const char escaped = text_[position_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                utf8Chunk.push_back(escaped);
                break;
            case 'b':
                utf8Chunk.push_back('\b');
                break;
            case 'f':
                utf8Chunk.push_back('\f');
                break;
            case 'n':
                utf8Chunk.push_back('\n');
                break;
            case 'r':
                utf8Chunk.push_back('\r');
                break;
            case 't':
                utf8Chunk.push_back('\t');
                break;
            case 'u': {
                AppendUtf8Chunk(value, utf8Chunk);
                const std::optional<std::uint16_t> codeUnit = ParseHexQuad(text_, position_);
                if (!codeUnit.has_value()) {
                    return false;
                }

                value.push_back(static_cast<wchar_t>(*codeUnit));
                position_ += 4U;
                break;
            }
            default:
                return false;
            }
        }

        return false;
    }

    bool ParseNumber(JsonValue& value)
    {
        SkipWhitespace();
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') {
            ++position_;
        }

        const std::size_t digitStart = position_;
        while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }

        if (digitStart == position_) {
            return false;
        }

        const std::string numberText = text_.substr(start, position_ - start);
        long long parsed = 0;
        const auto result = std::from_chars(numberText.data(), numberText.data() + numberText.size(), parsed);
        if (result.ec != std::errc()) {
            return false;
        }

        value.type = JsonValueType::Number;
        value.numberValue = parsed;
        return true;
    }

    bool Matches(const char* literal) const
    {
        const std::size_t length = std::char_traits<char>::length(literal);
        return position_ + length <= text_.size() && text_.compare(position_, length, literal) == 0;
    }

    const std::string& text_;
    std::size_t position_ = 0;
};

std::string EscapeJsonString(const std::wstring& text)
{
    std::ostringstream output;
    output << '"';

    for (wchar_t character : text) {
        switch (character) {
        case L'"':
            output << "\\\"";
            break;
        case L'\\':
            output << "\\\\";
            break;
        case L'\b':
            output << "\\b";
            break;
        case L'\f':
            output << "\\f";
            break;
        case L'\n':
            output << "\\n";
            break;
        case L'\r':
            output << "\\r";
            break;
        case L'\t':
            output << "\\t";
            break;
        default:
            if (character >= 0x20 && character <= 0x7e) {
                output << static_cast<char>(character);
            } else {
                output << "\\u";
                const unsigned int value = static_cast<unsigned int>(character) & 0xffffU;
                constexpr char digits[] = "0123456789abcdef";
                output << digits[(value >> 12) & 0xfU]
                       << digits[(value >> 8) & 0xfU]
                       << digits[(value >> 4) & 0xfU]
                       << digits[value & 0xfU];
            }
            break;
        }
    }

    output << '"';
    return output.str();
}

std::optional<long long> GetNumber(const JsonObject& object, const std::string& key)
{
    const auto it = object.find(key);
    if (it == object.end() || it->second.type != JsonValueType::Number) {
        return std::nullopt;
    }

    return it->second.numberValue;
}

std::optional<std::wstring> GetString(const JsonObject& object, const std::string& key)
{
    const auto it = object.find(key);
    if (it == object.end() || it->second.type != JsonValueType::String) {
        return std::nullopt;
    }

    return it->second.stringValue;
}

bool GetBool(const JsonObject& object, const std::string& key, bool defaultValue)
{
    const auto it = object.find(key);
    if (it == object.end() || it->second.type != JsonValueType::Boolean) {
        return defaultValue;
    }

    return it->second.booleanValue;
}

bool RequireString(const JsonObject& object, const std::string& key, std::wstring& value, std::wstring& error)
{
    const std::optional<std::wstring> stringValue = GetString(object, key);
    if (!stringValue.has_value()) {
        error = L"Missing string field: ";
        error += Utf8ToWide(key);
        return false;
    }

    value = *stringValue;
    return true;
}

bool RequireNumber(const JsonObject& object, const std::string& key, long long& value, std::wstring& error)
{
    const std::optional<long long> numberValue = GetNumber(object, key);
    if (!numberValue.has_value()) {
        error = L"Missing numeric field: ";
        error += Utf8ToWide(key);
        return false;
    }

    value = *numberValue;
    return true;
}

class ResponseWriter {
public:
    ResponseWriter(long long id, bool ok)
    {
        stream_ << "{\"id\":" << id << ",\"ok\":" << (ok ? "true" : "false");
    }

    void AddString(const char* key, const std::wstring& value)
    {
        stream_ << ",\"" << key << "\":" << EscapeJsonString(value);
    }

    void AddBool(const char* key, bool value)
    {
        stream_ << ",\"" << key << "\":" << (value ? "true" : "false");
    }

    void AddNumber(const char* key, long long value)
    {
        stream_ << ",\"" << key << "\":" << value;
    }

    void AddRaw(const char* key, const std::string& value)
    {
        stream_ << ",\"" << key << "\":" << value;
    }

    std::string Finish()
    {
        stream_ << "}";
        return stream_.str();
    }

private:
    std::ostringstream stream_;
};

const char* SpellOperationStatusName(SpellOperationStatus status)
{
    switch (status) {
    case SpellOperationStatus::Success: return "success";
    case SpellOperationStatus::Unavailable: return "unavailable";
    case SpellOperationStatus::Failure: return "failure";
    }
    return "failure";
}

std::optional<SpellOperationStatus> ParseSpellOperationStatus(const std::wstring& value)
{
    if (value == L"success") return SpellOperationStatus::Success;
    if (value == L"unavailable") return SpellOperationStatus::Unavailable;
    if (value == L"failure") return SpellOperationStatus::Failure;
    return std::nullopt;
}

void AddSpellOperationFields(ResponseWriter& response, SpellOperationStatus status, HRESULT hresult)
{
    response.AddString("status", Utf8ToWide(SpellOperationStatusName(status)));
    response.AddNumber("hresult", static_cast<long long>(static_cast<std::uint32_t>(hresult)));
}

std::string BuildErrorResponse(long long id, const std::wstring& message)
{
    ResponseWriter response(id, false);
    response.AddString("error", message);
    return response.Finish();
}

std::string BuildMetadataObject(const ClassicNotepadApp::AutomationDocumentMetadata& metadata)
{
    std::ostringstream output;
    output << "{"
           << "\"path\":" << EscapeJsonString(metadata.path)
           << ",\"displayName\":" << EscapeJsonString(metadata.displayName)
           << ",\"hasPath\":" << (metadata.hasPath ? "true" : "false")
           << ",\"encoding\":" << EscapeJsonString(metadata.encoding)
           << ",\"lineEnding\":" << EscapeJsonString(metadata.lineEnding)
           << ",\"saveLineEnding\":" << EscapeJsonString(metadata.saveLineEnding)
           << "}";
    return output.str();
}

std::string BuildSelectionObject(const ClassicNotepadApp::AutomationSelection& selection)
{
    std::ostringstream output;
    output << "{\"start\":" << selection.start << ",\"end\":" << selection.end << "}";
    return output.str();
}

std::string BuildLargeFileStateObject(const ClassicNotepadApp::AutomationLargeFileState& state)
{
    std::ostringstream output;
    output << "{"
           << "\"active\":" << (state.active ? "true" : "false")
           << ",\"readOnly\":" << (state.readOnly ? "true" : "false")
           << ",\"editable\":" << (state.editable ? "true" : "false")
           << ",\"modified\":" << (state.modified ? "true" : "false")
           << ",\"indexingComplete\":" << (state.indexingComplete ? "true" : "false")
           << ",\"globalScrollReady\":" << (state.globalScrollReady ? "true" : "false")
           << ",\"encodingValid\":" << (state.encodingValid ? "true" : "false")
           << ",\"fileSize\":" << state.fileSize
           << ",\"indexedBytes\":" << state.indexedBytes
           << ",\"indexedLines\":" << state.indexedLines
           << ",\"totalLines\":" << state.totalLines
           << ",\"firstUsableMilliseconds\":" << state.firstUsableMilliseconds
           << ",\"firstViewportPaintMilliseconds\":" << state.firstViewportPaintMilliseconds
           << ",\"fullIndexMilliseconds\":" << state.fullIndexMilliseconds
           << ",\"characterCount\":" << state.characterCount
           << ",\"caret\":" << state.caret
           << ",\"selectionStart\":" << state.selectionStart
           << ",\"selectionEnd\":" << state.selectionEnd
           << ",\"topLine\":" << state.topLine
           << ",\"maximumTopLine\":" << state.maximumTopLine
           << ",\"verticalScrollRangeMaximum\":" << state.verticalScrollRangeMaximum
           << ",\"verticalScrollPosition\":" << state.verticalScrollPosition
           << ",\"verticalScrollPage\":" << state.verticalScrollPage
           << ",\"visibleLineCount\":" << state.visibleLineCount
           << ",\"generation\":" << state.generation
           << ",\"pieceCount\":" << state.pieceCount
           << ",\"insertedBufferBytes\":" << state.insertedBufferBytes
           << ",\"saveProgressBytes\":" << state.saveProgressBytes
           << ",\"saveInProgress\":" << (state.saveInProgress ? "true" : "false")
           << ",\"paintGeneration\":" << state.paintGeneration
           << ",\"callbackAllocationFailures\":" << state.callbackAllocationFailures
           << ",\"viewportGeneration\":" << state.viewportGeneration
           << ",\"wordWrap\":" << (state.wordWrap ? "true" : "false")
           << ",\"viewportVisualRows\":" << state.viewportVisualRows
           << ",\"wrappedContinuationRows\":" << state.wrappedContinuationRows
           << ",\"viewportCacheRebuilds\":" << state.viewportCacheRebuilds
           << ",\"viewportCacheHits\":" << state.viewportCacheHits
           << ",\"viewportLastBuildDecodedCharacters\":" << state.viewportLastBuildDecodedCharacters
           << ",\"viewportLastBuildMeasuredCharacters\":" << state.viewportLastBuildMeasuredCharacters
           << ",\"viewportLastLogicalAnchor\":" << state.viewportLastLogicalAnchor
           << ",\"viewportLastSegmentStart\":" << state.viewportLastSegmentStart
           << ",\"viewportLastSegmentEnd\":" << state.viewportLastSegmentEnd
           << ",\"spellingIssueCount\":" << state.spellingIssueCount
           << ",\"maximumLineColumns\":" << state.maximumLineColumns
           << ",\"horizontalOffset\":" << state.horizontalOffset
           << ",\"horizontalMaximumOffset\":" << state.horizontalMaximumOffset
           << ",\"viewportWidth\":" << state.viewportWidth
           << ",\"viewportHeight\":" << state.viewportHeight
           << ",\"caretRectangle\":{"
           << "\"left\":" << state.caretRectangle.left
           << ",\"top\":" << state.caretRectangle.top
           << ",\"right\":" << state.caretRectangle.right
           << ",\"bottom\":" << state.caretRectangle.bottom << "}"
           << ",\"operationInProgress\":" << (state.operationInProgress ? "true" : "false")
           << ",\"operationCompleted\":" << (state.operationCompleted ? "true" : "false")
           << ",\"operationSucceeded\":" << (state.operationSucceeded ? "true" : "false")
           << ",\"operationName\":" << EscapeJsonString(state.operationName)
           << ",\"operationError\":" << EscapeJsonString(state.operationError)
           << ",\"operationTargetPath\":" << EscapeJsonString(state.operationTargetPath)
           << ",\"saveFailureStage\":" << EscapeJsonString(classic_notepad::SaveFailureStageName(state.saveDiagnostics.failureStage))
           << ",\"saveWin32Error\":" << state.saveDiagnostics.win32Error
           << ",\"saveOutputComplete\":" << (state.saveDiagnostics.outputComplete ? "true" : "false")
           << ",\"saveCommitted\":" << (state.saveDiagnostics.committed ? "true" : "false")
           << ",\"saveRecoveryPath\":" << EscapeJsonString(state.saveDiagnostics.recoveryPath)
           << ",\"operationGeneration\":" << state.operationGeneration
           << ",\"operationToken\":" << state.operationToken
           << ",\"operationProgress\":" << state.operationProgress
           << ",\"operationResultCount\":" << state.operationResultCount
           << "}";
    return output.str();
}

std::string BuildEditorWidthStateObject(const ClassicNotepadApp::AutomationEditorWidthState& state)
{
    std::ostringstream output;
    output << "{"
           << "\"generation\":" << state.generation
           << ",\"editDisplayLineCount\":" << state.editDisplayLineCount
           << ",\"pendingLines\":" << state.pendingLines
           << ",\"measuredDisplayLines\":" << state.measuredDisplayLines
           << ",\"measuredTextUnits\":" << state.measuredTextUnits
           << ",\"measurementBatches\":" << state.measurementBatches
           << ",\"lastBatchDiscoveredDisplayLines\":" << state.lastBatchDiscoveredDisplayLines
           << ",\"lastBatchPublishedDisplayLines\":" << state.lastBatchPublishedDisplayLines
           << ",\"lastBatchDiscoveredTextUnits\":" << state.lastBatchDiscoveredTextUnits
           << ",\"lastBatchMeasuredTextUnits\":" << state.lastBatchMeasuredTextUnits
           << ",\"maximumBatchDiscoveredDisplayLines\":" << state.maximumBatchDiscoveredDisplayLines
           << ",\"maximumBatchPublishedDisplayLines\":" << state.maximumBatchPublishedDisplayLines
           << ",\"maximumBatchTextUnits\":" << state.maximumBatchTextUnits
           << ",\"localEditMeasuredDisplayLines\":" << state.localEditMeasuredDisplayLines
           << ",\"localEditMeasuredTextUnits\":" << state.localEditMeasuredTextUnits
           << ",\"conservativeLineEstimates\":" << state.conservativeLineEstimates
           << ",\"maximumEditDisplayLineLength\":" << state.maximumEditDisplayLineLength
           << ",\"documentWidth\":" << state.documentWidth
           << ",\"viewportWidth\":" << state.viewportWidth
           << ",\"viewportHeight\":" << state.viewportHeight
           << ",\"documentHorizontalRangeMaximumOffset\":" << state.documentHorizontalRangeMaximumOffset
           << ",\"horizontalOffset\":" << state.horizontalOffset
           << ",\"horizontalMaximumOffset\":" << state.horizontalMaximumOffset
           << ",\"nativeHorizontalOffset\":" << state.nativeHorizontalOffset
           << ",\"nativeHorizontalMaximumOffset\":" << state.nativeHorizontalMaximumOffset
           << ",\"nativeHorizontalScrollMinimum\":" << state.nativeHorizontalScrollMinimum
           << ",\"nativeHorizontalScrollMaximum\":" << state.nativeHorizontalScrollMaximum
           << ",\"nativeHorizontalScrollPage\":" << state.nativeHorizontalScrollPage
           << ",\"fontMaximumAdvance\":" << state.fontMaximumAdvance
           << ",\"editTextLimit\":" << state.editTextLimit
           << ",\"windowDpi\":" << state.windowDpi
           << ",\"caretX\":" << state.caretX
           << ",\"caretY\":" << state.caretY
           << ",\"caretPositionAvailable\":" << (state.caretPositionAvailable ? "true" : "false")
           << ",\"caretDisplayLine\":" << state.caretDisplayLine
           << ",\"charPositionCharacterIndex\":" << state.charPositionCharacterIndex
           << ",\"charPositionX\":" << state.charPositionX
           << ",\"charPositionY\":" << state.charPositionY
           << ",\"charPositionAvailable\":" << (state.charPositionAvailable ? "true" : "false")
           << ",\"hitTestCharacterLowWord\":" << state.hitTestCharacterLowWord
           << ",\"hitTestLineHighWord\":" << state.hitTestLineHighWord
           << ",\"hitTestAvailable\":" << (state.hitTestAvailable ? "true" : "false")
           << ",\"customHorizontalOffset\":" << state.customHorizontalOffset
           << ",\"customHorizontalOffsetPixels\":" << state.customHorizontalOffsetPixels
           << ",\"horizontalScrollUnit\":" << state.horizontalScrollUnit
           << ",\"horizontalAnchorPixels\":" << state.horizontalAnchorPixels
           << ",\"verticalFirstVisibleLine\":" << state.verticalFirstVisibleLine
           << ",\"verticalTopContentPosition\":" << state.verticalTopContentPosition
           << ",\"verticalPage\":" << state.verticalPage
           << ",\"verticalMaximumOffset\":" << state.verticalMaximumOffset
           << ",\"verticalLineHeight\":" << state.verticalLineHeight
           << ",\"pastePostCaretFirstVisibleLine\":" << state.pastePostCaretFirstVisibleLine
           << ",\"pastePostCaretTopContentPosition\":" << state.pastePostCaretTopContentPosition
           << ",\"pastePostCaretHorizontalOffset\":" << state.pastePostCaretHorizontalOffset
           << ",\"nativeHorizontalScrollInfoAvailable\":" <<
                (state.nativeHorizontalScrollInfoAvailable ? "true" : "false")
           << ",\"measurementPending\":" << (state.measurementPending ? "true" : "false")
           << ",\"horizontalScrollBarVisible\":" << (state.horizontalScrollBarVisible ? "true" : "false")
           << ",\"verticalScrollBarVisible\":" << (state.verticalScrollBarVisible ? "true" : "false")
           << "}";
    return output.str();
}

std::string BuildLargeFileVisibleWindowsObject(const std::vector<LargeFileVisibleTextWindow>& windows)
{
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < windows.size(); ++index) {
        if (index != 0U) output << ',';
        output << "{\"sessionId\":" << windows[index].revision.session
               << ",\"contentRevision\":" << windows[index].revision.content
               << ",\"layoutRevision\":" << windows[index].revision.layout
               << ",\"start\":" << windows[index].absoluteStart
               << ",\"eligibleStart\":" << windows[index].eligibleStart
               << ",\"eligibleEnd\":" << windows[index].eligibleEnd
               << ",\"text\":" << EscapeJsonString(windows[index].text) << '}';
    }
    output << ']';
    return output.str();
}

std::string BuildMarginsObject(const RECT& margins)
{
    std::ostringstream output;
    output << "{"
           << "\"left\":" << margins.left
           << ",\"top\":" << margins.top
           << ",\"right\":" << margins.right
           << ",\"bottom\":" << margins.bottom
           << "}";
    return output.str();
}

const wchar_t* CorrectiveActionName(CORRECTIVE_ACTION action)
{
    switch (action) {
    case CORRECTIVE_ACTION_GET_SUGGESTIONS:
        return L"suggestions";
    case CORRECTIVE_ACTION_REPLACE:
        return L"replace";
    case CORRECTIVE_ACTION_DELETE:
        return L"delete";
    case CORRECTIVE_ACTION_NONE:
    default:
        return L"none";
    }
}

std::string BuildStringArray(const std::vector<std::wstring>& values)
{
    std::ostringstream output;
    output << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0U) {
            output << ",";
        }
        output << EscapeJsonString(values[index]);
    }
    output << "]";
    return output.str();
}

std::string BuildSpellingErrorsArray(
    const std::vector<SpellingErrorRange>& errors,
    const std::wstring& text)
{
    std::ostringstream output;
    output << "[";
    for (std::size_t index = 0; index < errors.size(); ++index) {
        const SpellingErrorRange& error = errors[index];
        if (index > 0U) {
            output << ",";
        }

        std::wstring misspelling;
        const std::size_t start = static_cast<std::size_t>(error.start);
        const std::size_t length = static_cast<std::size_t>(error.length);
        if (start <= text.size() && length <= text.size() - start) {
            misspelling = text.substr(start, length);
        }

        output << "{"
               << "\"start\":" << error.start
               << ",\"length\":" << error.length
               << ",\"text\":" << EscapeJsonString(misspelling)
               << ",\"replacement\":" << EscapeJsonString(error.replacement)
               << ",\"action\":" << EscapeJsonString(CorrectiveActionName(error.action))
               << "}";
    }
    output << "]";
    return output.str();
}

std::string BuildSpellingAnnotationsArray(const classic_notepad::SpellingAnnotationBatch& batch)
{
    std::ostringstream output;
    output << "[";
    for (std::size_t index = 0U; index < batch.annotations.size(); ++index) {
        if (index != 0U) output << ",";
        const classic_notepad::SpellingAnnotation& annotation = batch.annotations[index];
        output << "{\"start\":" << annotation.start
               << ",\"length\":" << annotation.length << "}";
    }
    output << "]";
    return output.str();
}

std::wstring JsonAppearanceThemeName(classic_notepad::AppearanceTheme theme)
{
    return Utf8ToWide(classic_notepad::AppearanceThemeName(theme));
}

std::string BuildAppearanceObject(const ClassicNotepadApp& app)
{
    std::ostringstream output;
    output << "{"
           << "\"theme\":" << EscapeJsonString(JsonAppearanceThemeName(app.AutomationAppearanceTheme()))
           << ",\"effectiveAppearance\":" << EscapeJsonString(app.AutomationDarkModeEnabled() ? L"dark" : L"light")
           << ",\"darkMode\":" << (app.AutomationDarkModeEnabled() ? "true" : "false")
           << ",\"highContrast\":" << (app.AutomationHighContrastThemeActive() ? "true" : "false")
           << "}";
    return output.str();
}

std::string BuildCapabilitiesObject(const ClassicNotepadApp& app)
{
    std::ostringstream output;
    output << "{"
           << "\"platform\":\"windows\""
           << ",\"nativeUi\":\"win32\""
           << ",\"largeFileMode\":true"
           << ",\"printing\":true"
           << ",\"pageSetup\":true"
           << ",\"fontChooser\":true"
           << ",\"spellCheck\":" << (app.AutomationSpellCheckAvailable() ? "true" : "false")
           << ",\"darkMode\":" << (app.AutomationDarkModeEnabled() ? "true" : "false")
           << ",\"appearanceTheme\":" << EscapeJsonString(JsonAppearanceThemeName(app.AutomationAppearanceTheme()))
           << ",\"effectiveAppearance\":" << EscapeJsonString(app.AutomationDarkModeEnabled() ? L"dark" : L"light")
           << ",\"highContrast\":" << (app.AutomationHighContrastThemeActive() ? "true" : "false")
           << "}";
    return output.str();
}

DWORD ClampSelectionValue(long long value)
{
    if (value <= 0) {
        return 0;
    }

    return static_cast<DWORD>(std::min<long long>(value, static_cast<long long>(0x7fffffff)));
}

std::wstring SelectedText(const ClassicNotepadApp& app)
{
    const ClassicNotepadApp::AutomationSelection selection = app.AutomationGetSelection();
    const ClassicNotepadApp::AutomationLargeFileState large = app.AutomationGetLargeFileState();
    if (large.active) {
        std::wstring text;
        std::wstring error;
        const std::uint64_t start = selection.start;
        const std::uint64_t end = selection.end;
        if (end >= start && app.AutomationGetLargeFileTextRange(start, end - start, text, error)) {
            return text;
        }
        return {};
    }
    const std::wstring text = app.AutomationGetText();
    const std::size_t start = std::min<std::size_t>(selection.start, text.size());
    const std::size_t end = std::min<std::size_t>(selection.end, text.size());
    return start < end ? text.substr(start, end - start) : std::wstring();
}

std::string HandleCommand(
    ClassicNotepadApp& app,
    std::wstring& testClipboard,
    const JsonObject& request,
    bool& shouldClose)
{
    const long long id = GetNumber(request, "id").value_or(0);
    const std::optional<std::wstring> command = GetString(request, "command");
    if (!command.has_value()) {
        return BuildErrorResponse(id, L"Missing string field: command");
    }

    const std::wstring& name = *command;
    std::wstring errorMessage;

    if (name == L"getCapabilities") {
        ResponseWriter response(id, true);
        response.AddRaw("capabilities", BuildCapabilitiesObject(app));
        return response.Finish();
    }

    if (name == L"getAppearance") {
        ResponseWriter response(id, true);
        response.AddRaw("appearance", BuildAppearanceObject(app));
        return response.Finish();
    }

    if (name == L"setAppearanceTheme") {
        std::wstring themeText;
        if (!RequireString(request, "theme", themeText, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        const std::optional<classic_notepad::AppearanceTheme> theme =
            classic_notepad::TryParseAppearanceTheme(WideToUtf8(themeText));
        if (!theme.has_value()) {
            return BuildErrorResponse(id, L"Appearance theme must be system, light, or dark.");
        }

        app.AutomationSetAppearanceTheme(*theme);
        ResponseWriter response(id, true);
        response.AddRaw("appearance", BuildAppearanceObject(app));
        return response.Finish();
    }

    if (name == L"newDocument") {
        app.AutomationNewDocument();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"requestNewTransition") {
        app.AutomationRequestNewTransition(GetBool(request, "saveChanges", true));
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"openFile") {
        std::wstring path;
        if (!RequireString(request, "path", path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        std::uint32_t ansiCodePageOverride = 0U;
        const auto ansiCodePage = GetNumber(request, "ansiCodePage");
        if (ansiCodePage.has_value()) {
            if (*ansiCodePage <= 0 || *ansiCodePage > 0xFFFFFFFFLL) {
                return BuildErrorResponse(id, L"ansiCodePage must be a positive 32-bit code page number.");
            }
            ansiCodePageOverride = static_cast<std::uint32_t>(*ansiCodePage);
        }

        if (!app.AutomationOpenFile(path, errorMessage, ansiCodePageOverride)) {
            return BuildErrorResponse(id, errorMessage);
        }

        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getLargeFileState") {
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"getEditorWidthState") {
        ResponseWriter response(id, true);
        response.AddRaw("editorWidth", BuildEditorWidthStateObject(app.AutomationGetEditorWidthState()));
        return response.Finish();
    }

    if (name == L"resizeEditorViewport") {
        const auto width = GetNumber(request, "width");
        const auto height = GetNumber(request, "height");
        if (!width.has_value() || !height.has_value() ||
            !app.AutomationSetEditorViewportSize(static_cast<int>(*width), static_cast<int>(*height))) {
            return BuildErrorResponse(id, L"The ordinary editor viewport could not be resized.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("editorWidth", BuildEditorWidthStateObject(app.AutomationGetEditorWidthState()));
        return response.Finish();
    }

    if (name == L"sendEditorHorizontalScroll") {
        const auto scrollCommand = GetNumber(request, "scrollCommand");
        const auto trackPosition = GetNumber(request, "trackPosition").value_or(0);
        if (!scrollCommand.has_value() || *scrollCommand < 0 || *scrollCommand > SB_ENDSCROLL ||
            trackPosition < 0 || trackPosition > (std::numeric_limits<int>::max)() ||
            !app.AutomationSendEditorHorizontalScroll(static_cast<UINT>(*scrollCommand), static_cast<int>(trackPosition))) {
            return BuildErrorResponse(id, L"Ordinary-editor horizontal scroll input could not be delivered.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("editorWidth", BuildEditorWidthStateObject(app.AutomationGetEditorWidthState()));
        return response.Finish();
    }

    if (name == L"sendEditorVerticalScroll") {
        const auto scrollCommand = GetNumber(request, "scrollCommand");
        const auto trackPosition = GetNumber(request, "trackPosition").value_or(0);
        if (!scrollCommand.has_value() || *scrollCommand < 0 || *scrollCommand > SB_ENDSCROLL ||
            trackPosition < 0 || trackPosition > (std::numeric_limits<int>::max)() ||
            !app.AutomationSendEditorVerticalScroll(static_cast<UINT>(*scrollCommand), static_cast<int>(trackPosition))) {
            return BuildErrorResponse(id, L"Ordinary-editor vertical scroll input could not be delivered.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("editorWidth", BuildEditorWidthStateObject(app.AutomationGetEditorWidthState()));
        return response.Finish();
    }

    if (name == L"sendEditorKey") {
        const auto key = GetNumber(request, "key");
        if (!key.has_value() || *key < 0 || *key > 0xFF ||
            !app.AutomationSendEditorKey(static_cast<UINT>(*key),
                GetBool(request, "shift", false), GetBool(request, "control", false))) {
            return BuildErrorResponse(id, L"Ordinary-editor key input could not be delivered.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("selection", BuildSelectionObject(app.AutomationGetSelection()));
        return response.Finish();
    }

    if (name == L"clickEditorClientPoint") {
        const auto x = GetNumber(request, "x");
        const auto y = GetNumber(request, "y");
        if (!x.has_value() || !y.has_value() ||
            *x < (std::numeric_limits<int>::min)() || *x > (std::numeric_limits<int>::max)() ||
            *y < (std::numeric_limits<int>::min)() || *y > (std::numeric_limits<int>::max)() ||
            !app.AutomationClickEditorClientPoint(static_cast<int>(*x), static_cast<int>(*y))) {
            return BuildErrorResponse(id, L"Native editor click input could not be delivered inside the client area.");
        }
        const ClassicNotepadApp::AutomationSelection selection = app.AutomationGetSelection();
        ResponseWriter response(id, true);
        response.AddRaw("selection", BuildSelectionObject(selection));
        return response.Finish();
    }

    if (name == L"focusEditor") {
        ResponseWriter response(id, true);
        response.AddBool("focused", app.AutomationFocusEditor());
        return response.Finish();
    }

    if (name == L"scrollEditorByUnits") {
        const auto horizontalUnits = GetNumber(request, "horizontalUnits").value_or(0);
        const auto verticalLines = GetNumber(request, "verticalLines").value_or(0);
        if (horizontalUnits < (std::numeric_limits<int>::min)() ||
            horizontalUnits > (std::numeric_limits<int>::max)() ||
            verticalLines < (std::numeric_limits<int>::min)() ||
            verticalLines > (std::numeric_limits<int>::max)() ||
            !app.AutomationSendEditorLineScroll(static_cast<int>(horizontalUnits), static_cast<int>(verticalLines))) {
            return BuildErrorResponse(id, L"Ordinary-editor line scroll input could not be delivered.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("editorWidth", BuildEditorWidthStateObject(app.AutomationGetEditorWidthState()));
        return response.Finish();
    }

    if (name == L"getLargeFileLine") {
        const std::optional<long long> line = GetNumber(request, "line");
        if (!line.has_value() || *line <= 0) {
            return BuildErrorResponse(id, L"getLargeFileLine requires a positive line number.");
        }
        std::wstring text;
        if (!app.AutomationGetLargeFileLine(static_cast<std::uint64_t>(*line), text, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        ResponseWriter response(id, true);
        response.AddString("text", text);
        return response.Finish();
    }

    if (name == L"getLargeFileVisibleText") {
        std::uint64_t rangeStart = 0;
        std::wstring text;
        if (!app.AutomationGetLargeFileVisibleText(rangeStart, text, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        ResponseWriter response(id, true);
        response.AddNumber("start", static_cast<long long>(rangeStart));
        response.AddString("text", text);
        return response.Finish();
    }

    if (name == L"getLargeFileVisibleWindows") {
        std::vector<LargeFileVisibleTextWindow> windows;
        if (!app.AutomationGetLargeFileVisibleWindows(windows, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        ResponseWriter response(id, true);
        response.AddRaw("windows", BuildLargeFileVisibleWindowsObject(windows));
        return response.Finish();
    }

    if (name == L"getEditorVisibleSpellWindows") {
        std::vector<classic_notepad::SpellingTextWindow> windows;
        if (!app.AutomationGetEditorVisibleSpellingWindows(windows, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        ResponseWriter response(id, true);
        response.AddRaw("windows", BuildLargeFileVisibleWindowsObject(windows));
        return response.Finish();
    }

    if (name == L"resolveSpellingContextTarget") {
        const bool keyboardInvocation = GetBool(request, "keyboard", false);
        POINT point{};
        if (!keyboardInvocation) {
            const auto x = GetNumber(request, "x");
            const auto y = GetNumber(request, "y");
            if (!x.has_value() || !y.has_value() ||
                *x < (std::numeric_limits<int>::min)() || *x > (std::numeric_limits<int>::max)() ||
                *y < (std::numeric_limits<int>::min)() || *y > (std::numeric_limits<int>::max)()) {
                return BuildErrorResponse(id, L"resolveSpellingContextTarget requires integer client coordinates or keyboard=true.");
            }
            point = { static_cast<LONG>(*x), static_cast<LONG>(*y) };
        }
        classic_notepad::SpellingContextTarget target;
        const bool found = app.AutomationResolveSpellingContextTarget(point, keyboardInvocation, target);
        ResponseWriter response(id, true);
        response.AddBool("found", found);
        if (found) {
            response.AddNumber("sessionId", static_cast<long long>(target.revision.session));
            response.AddNumber("contentRevision", static_cast<long long>(target.revision.content));
            response.AddNumber("layoutRevision", static_cast<long long>(target.revision.layout));
            response.AddNumber("start", static_cast<long long>(target.start));
            response.AddNumber("length", static_cast<long long>(target.length));
            response.AddString("text", target.text);
        }
        return response.Finish();
    }

    if (name == L"getVisibleTextPositionPoint") {
        const auto position = GetNumber(request, "position");
        if (!position.has_value() || *position < 0) {
            return BuildErrorResponse(id, L"getVisibleTextPositionPoint requires a non-negative UTF-16 position.");
        }
        POINT point{};
        if (!app.AutomationGetTextPositionPoint(static_cast<std::uint64_t>(*position), point)) {
            return BuildErrorResponse(id, L"The requested text position is not visible or cannot be mapped.");
        }
        ResponseWriter response(id, true);
        response.AddNumber("x", point.x);
        response.AddNumber("y", point.y);
        return response.Finish();
    }

    if (name == L"sendEditorContextMenu") {
        const bool keyboardInvocation = GetBool(request, "keyboard", false);
        const auto x = GetNumber(request, "x").value_or(0);
        const auto y = GetNumber(request, "y").value_or(0);
        if (x < (std::numeric_limits<int>::min)() || x > (std::numeric_limits<int>::max)() ||
            y < (std::numeric_limits<int>::min)() || y > (std::numeric_limits<int>::max)() ||
            !app.AutomationSendEditorContextMenu(static_cast<int>(x), static_cast<int>(y), keyboardInvocation)) {
            return BuildErrorResponse(id, L"The editor context menu could not be opened and dismissed.");
        }
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getLargeFileRange") {
        const std::optional<long long> start = GetNumber(request, "start");
        const std::optional<long long> length = GetNumber(request, "length");
        if (!start.has_value() || !length.has_value() || *start < 0 || *length < 0) {
            return BuildErrorResponse(id, L"getLargeFileRange requires non-negative start and length values.");
        }
        std::wstring text;
        if (!app.AutomationGetLargeFileTextRange(
                static_cast<std::uint64_t>(*start), static_cast<std::uint64_t>(*length), text, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        ResponseWriter response(id, true);
        response.AddString("text", text);
        return response.Finish();
    }

    if (name == L"dragEditor") {
        const auto startX = GetNumber(request, "startX");
        const auto startY = GetNumber(request, "startY");
        const auto endX = GetNumber(request, "endX");
        const auto endY = GetNumber(request, "endY");
        if (!startX.has_value() || !startY.has_value() || !endX.has_value() || !endY.has_value() ||
            !app.AutomationSendEditorDrag(static_cast<int>(*startX), static_cast<int>(*startY),
                static_cast<int>(*endX), static_cast<int>(*endY))) {
            return BuildErrorResponse(id, L"Ordinary-editor drag input could not be delivered.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("selection", BuildSelectionObject(app.AutomationGetSelection()));
        return response.Finish();
    }

    if (name == L"sendLargeFileClick") {
        const auto x = GetNumber(request, "x");
        const auto y = GetNumber(request, "y");
        if (!x.has_value() || !y.has_value() ||
            !app.AutomationSendLargeFileClick(static_cast<int>(*x), static_cast<int>(*y), GetBool(request, "shift", false))) {
            return BuildErrorResponse(id, L"Large-file click input could not be delivered.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"sendLargeFileDrag") {
        const auto startX = GetNumber(request, "startX");
        const auto startY = GetNumber(request, "startY");
        const auto endX = GetNumber(request, "endX");
        const auto endY = GetNumber(request, "endY");
        if (!startX.has_value() || !startY.has_value() || !endX.has_value() || !endY.has_value() ||
            !app.AutomationSendLargeFileDrag(static_cast<int>(*startX), static_cast<int>(*startY),
                static_cast<int>(*endX), static_cast<int>(*endY))) {
            return BuildErrorResponse(id, L"Large-file drag input could not be delivered.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"sendLargeFileKey") {
        const auto key = GetNumber(request, "key");
        if (!key.has_value() || *key < 0 || *key > 0xffff ||
            !app.AutomationSendLargeFileKey(static_cast<UINT>(*key),
                GetBool(request, "shift", false), GetBool(request, "control", false))) {
            return BuildErrorResponse(id, L"Large-file key input could not be delivered.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"sendLargeFileVerticalScroll") {
        const auto scrollCommand = GetNumber(request, "scrollCommand");
        const auto trackPosition = GetNumber(request, "trackPosition").value_or(0);
        if (!scrollCommand.has_value() || *scrollCommand < 0 || *scrollCommand > SB_ENDSCROLL ||
            trackPosition < 0 || trackPosition > (std::numeric_limits<int>::max)() ||
            !app.AutomationSendLargeFileVerticalScroll(
                static_cast<UINT>(*scrollCommand), static_cast<int>(trackPosition))) {
            return BuildErrorResponse(id, L"Large-file vertical scroll input could not be delivered.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"sendLargeFileWheel") {
        const auto delta = GetNumber(request, "delta");
        if (!delta.has_value() || *delta < -32768 || *delta > 32767 ||
            !app.AutomationSendLargeFileWheel(static_cast<int>(*delta))) {
            return BuildErrorResponse(id, L"Large-file mouse-wheel input could not be delivered.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"sendLargeFileHorizontalScroll") {
        const auto scrollCommand = GetNumber(request, "scrollCommand");
        if (!scrollCommand.has_value() || *scrollCommand < 0 || *scrollCommand > SB_ENDSCROLL ||
            !app.AutomationSendLargeFileHorizontalScroll(static_cast<UINT>(*scrollCommand))) {
            return BuildErrorResponse(id, L"Large-file horizontal scroll input could not be delivered.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"resizeLargeFileViewport") {
        const auto width = GetNumber(request, "width");
        const auto height = GetNumber(request, "height");
        if (!width.has_value() || !height.has_value() || *width <= 0 || *height <= 0 ||
            *width > 16384 || *height > 16384 ||
            !app.AutomationSetLargeFileViewportSize(static_cast<int>(*width), static_cast<int>(*height))) {
            return BuildErrorResponse(id, L"The large-file viewport could not be resized.");
        }
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"sendLargeFileText") {
        std::wstring text;
        if (!RequireString(request, "text", text, errorMessage) || !app.AutomationSendLargeFileText(text)) {
            return BuildErrorResponse(id, errorMessage.empty() ? L"Large-file text input could not be delivered." : errorMessage);
        }
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"startLargeFileFind") {
        std::wstring text;
        if (!RequireString(request, "text", text, errorMessage) ||
            !app.AutomationStartLargeFileFind(text, GetBool(request, "matchCase", false),
                GetBool(request, "wholeWord", false), GetBool(request, "forward", true), errorMessage)) {
            return BuildErrorResponse(id, errorMessage.empty() ? L"The asynchronous search could not start." : errorMessage);
        }
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"startLargeFileReplaceAll") {
        std::wstring text;
        std::wstring replacement;
        if (!RequireString(request, "text", text, errorMessage) ||
            !RequireString(request, "replacement", replacement, errorMessage) ||
            !app.AutomationStartLargeFileReplaceAll(text, replacement,
                GetBool(request, "matchCase", false), GetBool(request, "wholeWord", false), errorMessage)) {
            return BuildErrorResponse(id, errorMessage.empty() ? L"The asynchronous Replace All could not start." : errorMessage);
        }
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"cancelLargeFileOperation") {
        app.AutomationCancelLargeFileOperation();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"startLargeFileSave") {
        std::wstring path;
        if (!RequireString(request, "path", path, errorMessage) ||
            !app.AutomationStartLargeFileSave(path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage.empty() ? L"The asynchronous save could not start." : errorMessage);
        }
        ResponseWriter response(id, true);
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"save") {
        if (!app.AutomationSave(errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        return ResponseWriter(id, true).Finish();
    }

    if (name == L"saveAs") {
        std::wstring path;
        if (!RequireString(request, "path", path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        if (!app.AutomationSaveAs(path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        return ResponseWriter(id, true).Finish();
    }

    if (name == L"setText") {
        std::wstring text;
        if (!RequireString(request, "text", text, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        app.AutomationSetText(text);
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"setTextFromUtf16File") {
        std::wstring path;
        std::wstring text;
        if (!RequireString(request, "path", path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        const auto fileReadStarted = std::chrono::steady_clock::now();
        if (!ReadUtf16LeFile(path, text, errorMessage)) return BuildErrorResponse(id, errorMessage);
        const long long fileReadMicroseconds = ElapsedMicroseconds(fileReadStarted);
        const long long units = static_cast<long long>(text.size());
        const auto setTextStarted = std::chrono::steady_clock::now();
        app.AutomationSetText(text);
        ResponseWriter response(id, true);
        response.AddNumber("sourceUnits", units);
        response.AddNumber("sourceFileReadMicroseconds", fileReadMicroseconds);
        response.AddNumber("setTextMicroseconds", ElapsedMicroseconds(setTextStarted));
        return response.Finish();
    }

    if (name == L"getText") {
        ResponseWriter response(id, true);
        response.AddString("text", app.AutomationGetText());
        return response.Finish();
    }

    if (name == L"writeCurrentTextUtf16File") {
        std::wstring path;
        if (!RequireString(request, "path", path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        const auto exportStarted = std::chrono::steady_clock::now();
        const std::wstring text = app.AutomationGetText();
        if (!WriteUtf16LeFile(path, text, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        ResponseWriter response(id, true);
        response.AddNumber("storedUnits", static_cast<long long>(text.size()));
        response.AddNumber("exportOperationMicroseconds", ElapsedMicroseconds(exportStarted));
        return response.Finish();
    }

    if (name == L"insertText") {
        std::wstring text;
        if (!RequireString(request, "text", text, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        app.AutomationInsertText(text);
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"typeText") {
        std::wstring text;
        if (!RequireString(request, "text", text, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        app.AutomationTypeText(text);
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"pressInsert") {
        app.AutomationPressInsert();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getTitle") {
        ResponseWriter response(id, true);
        response.AddString("title", app.AutomationGetTitle());
        return response.Finish();
    }

    if (name == L"isModified") {
        ResponseWriter response(id, true);
        response.AddBool("modified", app.AutomationIsModified());
        return response.Finish();
    }

    if (name == L"getDocumentMetadata") {
        ResponseWriter response(id, true);
        response.AddRaw("metadata", BuildMetadataObject(app.AutomationGetDocumentMetadata()));
        return response.Finish();
    }

    if (name == L"getStatusText") {
        ResponseWriter response(id, true);
        response.AddString("statusText", app.AutomationGetStatusText());
        return response.Finish();
    }

    if (name == L"getSelection") {
        ResponseWriter response(id, true);
        response.AddRaw("selection", BuildSelectionObject(app.AutomationGetSelection()));
        return response.Finish();
    }

    if (name == L"setSelection") {
        long long start = 0;
        long long end = 0;
        if (!RequireNumber(request, "start", start, errorMessage) ||
            !RequireNumber(request, "end", end, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        app.AutomationSetSelection(ClampSelectionValue(start), ClampSelectionValue(end));
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"selectAll") {
        app.AutomationSelectAll();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"undo") {
        app.AutomationUndo();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"cut") {
        testClipboard = SelectedText(app);
        app.AutomationDeleteSelection();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"copy") {
        testClipboard = SelectedText(app);
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"paste") {
        app.AutomationInsertText(testClipboard);
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"pasteThroughProductionHandler") {
        std::wstring text;
        if (!RequireString(request, "text", text, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        app.AutomationPasteTextThroughProductionHandler(text);
        ResponseWriter response(id, true);
        response.AddRaw("selection", BuildSelectionObject(app.AutomationGetSelection()));
        response.AddRaw("editorWidth", BuildEditorWidthStateObject(app.AutomationGetEditorWidthState()));
        response.AddString("outcome", app.AutomationGetLastPasteOutcome());
        response.AddBool("modified", app.AutomationIsModified());
        const ClassicNotepadApp::AutomationClipboardDiagnostics clipboard = app.AutomationGetLastClipboardDiagnostics();
        std::ostringstream clipboardDiagnostics;
        clipboardDiagnostics << "{"
            << "\"openCalls\":" << clipboard.openCalls
            << ",\"closeCalls\":" << clipboard.closeCalls
            << ",\"unicodeAvailabilityChecks\":" << clipboard.unicodeAvailabilityChecks
            << ",\"ansiAvailabilityChecks\":" << clipboard.ansiAvailabilityChecks
            << ",\"unicodeGetDataCalls\":" << clipboard.unicodeGetDataCalls
            << ",\"ansiGetDataCalls\":" << clipboard.ansiGetDataCalls
            << ",\"sizeCalls\":" << clipboard.sizeCalls
            << ",\"lockCalls\":" << clipboard.lockCalls
            << ",\"unlockCalls\":" << clipboard.unlockCalls
            << ",\"conversionCalls\":" << clipboard.conversionCalls
            << ",\"lastAnsiSourceLength\":" << clipboard.lastAnsiSourceLength
            << "}";
        response.AddRaw("clipboardDiagnostics", clipboardDiagnostics.str());
        response.AddNumber("nativeNotification", app.AutomationGetLastPasteNativeNotification());
        return response.Finish();
    }

    if (name == L"setPrintFault") {
        std::wstring stage;
        if (!RequireString(request, "stage", stage, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        if (!app.AutomationSetPrintFaultStage(stage)) {
            return BuildErrorResponse(id, L"Unsupported print fault stage or automation mode is unavailable.");
        }
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getPrintDiagnostics") {
        const ClassicNotepadApp::AutomationPrintDiagnostics diagnostics = app.AutomationGetPrintDiagnostics();
        ResponseWriter response(id, true);
        response.AddBool("succeeded", diagnostics.succeeded);
        response.AddBool("faultInjected", diagnostics.faultInjected);
        response.AddString("injectedStage", diagnostics.injectedStage);
        response.AddString("failureStage", diagnostics.failureStage);
        response.AddString("errorMessage", diagnostics.errorMessage);
        response.AddBool("abortDocCalled", diagnostics.abortDocCalled);
        response.AddBool("abortDocSucceeded", diagnostics.abortDocSucceeded);
        return response.Finish();
    }

    if (name == L"pasteThroughProductionHandlerFromUtf16File") {
        std::wstring path;
        std::wstring text;
        if (!RequireString(request, "path", path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        const auto fileReadStarted = std::chrono::steady_clock::now();
        if (!ReadUtf16LeFile(path, text, errorMessage)) return BuildErrorResponse(id, errorMessage);
        const long long fileReadMicroseconds = ElapsedMicroseconds(fileReadStarted);
        const long long units = static_cast<long long>(text.size());
        const auto helperStarted = std::chrono::steady_clock::now();
        app.AutomationPasteTextThroughProductionHandler(text);
        const long long helperMicroseconds = ElapsedMicroseconds(helperStarted);
        const ClassicNotepadApp::AutomationPasteTiming pasteTiming = app.AutomationGetLastPasteTiming();
        const auto widthStateStarted = std::chrono::steady_clock::now();
        const ClassicNotepadApp::AutomationEditorWidthState widthState = app.AutomationGetEditorWidthState();
        const long long widthStateMicroseconds = ElapsedMicroseconds(widthStateStarted);

        ResponseWriter response(id, true);
        response.AddNumber("sourceUnits", units);
        response.AddNumber("sourceFileReadMicroseconds", fileReadMicroseconds);
        response.AddNumber("providerSetupMicroseconds", pasteTiming.providerSetupMicroseconds);
        response.AddNumber("productionDispatchMicroseconds", pasteTiming.productionDispatchMicroseconds);
        response.AddNumber("providerCleanupMicroseconds", pasteTiming.providerCleanupMicroseconds);
        response.AddNumber("pasteHelperWallMicroseconds", helperMicroseconds);
        response.AddNumber("initialWidthStateReadMicroseconds", widthStateMicroseconds);
        response.AddRaw("selection", BuildSelectionObject(app.AutomationGetSelection()));
        response.AddRaw("editorWidth", BuildEditorWidthStateObject(widthState));
        response.AddString("outcome", app.AutomationGetLastPasteOutcome());
        response.AddBool("modified", app.AutomationIsModified());
        response.AddNumber("nativeNotification", app.AutomationGetLastPasteNativeNotification());
        const ClassicNotepadApp::AutomationClipboardDiagnostics clipboard = app.AutomationGetLastClipboardDiagnostics();
        std::ostringstream clipboardDiagnostics;
        clipboardDiagnostics << "{"
            << "\"openCalls\":" << clipboard.openCalls
            << ",\"closeCalls\":" << clipboard.closeCalls
            << ",\"unicodeGetDataCalls\":" << clipboard.unicodeGetDataCalls
            << ",\"lockCalls\":" << clipboard.lockCalls
            << ",\"unlockCalls\":" << clipboard.unlockCalls
            << "}";
        response.AddRaw("clipboardDiagnostics", clipboardDiagnostics.str());
        return response.Finish();
    }

    if (name == L"setPasteFault") {
        std::wstring stage;
        if (!RequireString(request, "stage", stage, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        if (!app.AutomationSetPasteFaultStage(stage)) {
            return BuildErrorResponse(id, L"Unsupported paste fault stage.");
        }
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"setEditorTextLimit") {
        long long limit = 0;
        if (!RequireNumber(request, "limit", limit, errorMessage) || limit < 0 ||
            static_cast<unsigned long long>(limit) > (std::numeric_limits<DWORD>::max)()) {
            return BuildErrorResponse(id, L"The editor text limit must be a non-negative 32-bit value.");
        }
        ResponseWriter response(id, true);
        response.AddNumber("limit", app.AutomationSetEditorTextLimit(static_cast<DWORD>(limit)));
        return response.Finish();
    }

    if (name == L"probeNativeReplace") {
        std::wstring text;
        std::wstring replacement;
        long long start = 0;
        long long end = 0;
        long long limit = 0;
        if (!RequireString(request, "text", text, errorMessage) ||
            !RequireString(request, "replacement", replacement, errorMessage) ||
            !RequireNumber(request, "start", start, errorMessage) ||
            !RequireNumber(request, "end", end, errorMessage) ||
            !RequireNumber(request, "limit", limit, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        if (start < 0 || end < 0 || limit < 0 ||
            static_cast<unsigned long long>(start) > (std::numeric_limits<DWORD>::max)() ||
            static_cast<unsigned long long>(end) > (std::numeric_limits<DWORD>::max)() ||
            static_cast<unsigned long long>(limit) > (std::numeric_limits<DWORD>::max)()) {
            return BuildErrorResponse(id, L"Native probe positions and limit must be non-negative 32-bit values.");
        }
        const auto result = app.AutomationProbeNativeReplace(
            text,
            static_cast<DWORD>(start),
            static_cast<DWORD>(end),
            replacement,
            static_cast<DWORD>(limit));
        ResponseWriter response(id, true);
        response.AddString("textBefore", result.textBefore);
        response.AddString("textAfter", result.textAfter);
        response.AddNumber("textLimit", result.textLimit);
        response.AddNumber("notificationMask", result.notificationMask);
        return response.Finish();
    }

    if (name == L"deleteSelection") {
        app.AutomationDeleteSelection();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"delete") {
        app.AutomationDelete();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"find" || name == L"findNext") {
        std::wstring text;
        const bool matchCase = GetBool(request, "matchCase", false);
        const bool wholeWord = GetBool(request, "wholeWord", false);
        const bool searchDown = GetBool(request, "searchDown", true);
        bool found = false;

        if (name == L"find") {
            if (!RequireString(request, "text", text, errorMessage)) {
                return BuildErrorResponse(id, errorMessage);
            }

            found = app.AutomationFind(text, matchCase, wholeWord, searchDown);
        } else {
            found = app.AutomationFindNext(matchCase, wholeWord, searchDown);
        }

        ResponseWriter response(id, true);
        response.AddBool("found", found);
        response.AddRaw("selection", BuildSelectionObject(app.AutomationGetSelection()));
        return response.Finish();
    }

    if (name == L"productionFindReplace") {
        std::wstring action;
        std::wstring text;
        std::wstring replacement;
        if (!RequireString(request, "action", action, errorMessage) ||
            !RequireString(request, "text", text, errorMessage) ||
            !RequireString(request, "replacement", replacement, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        if (!app.AutomationDispatchFindReplace(action, text, replacement,
                GetBool(request, "matchCase", false), GetBool(request, "wholeWord", false),
                GetBool(request, "searchDown", true), errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        ResponseWriter response(id, true);
        response.AddRaw("selection", BuildSelectionObject(app.AutomationGetSelection()));
        response.AddRaw("largeFile", BuildLargeFileStateObject(app.AutomationGetLargeFileState()));
        return response.Finish();
    }

    if (name == L"replace") {
        std::wstring text;
        std::wstring replacement;
        if (!RequireString(request, "text", text, errorMessage) ||
            !RequireString(request, "replacement", replacement, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        const bool replaced = app.AutomationReplace(
            text,
            replacement,
            GetBool(request, "matchCase", false),
            GetBool(request, "wholeWord", false),
            GetBool(request, "searchDown", true));
        ResponseWriter response(id, true);
        response.AddBool("replaced", replaced);
        response.AddRaw("selection", BuildSelectionObject(app.AutomationGetSelection()));
        return response.Finish();
    }

    if (name == L"replaceAll") {
        std::wstring text;
        std::wstring replacement;
        if (!RequireString(request, "text", text, errorMessage) ||
            !RequireString(request, "replacement", replacement, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        const std::size_t count = app.AutomationReplaceAll(
            text,
            replacement,
            GetBool(request, "matchCase", false),
            GetBool(request, "wholeWord", false));
        ResponseWriter response(id, true);
        response.AddNumber("count", static_cast<long long>(count));
        return response.Finish();
    }

    if (name == L"goToLine") {
        long long lineNumber = 0;
        if (!RequireNumber(request, "line", lineNumber, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        if (lineNumber < 0) {
            return BuildErrorResponse(id, L"Line number must be positive.");
        }
        if (!app.AutomationGoToLine(static_cast<std::uint64_t>(lineNumber), errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        ResponseWriter response(id, true);
        response.AddRaw("selection", BuildSelectionObject(app.AutomationGetSelection()));
        return response.Finish();
    }

    if (name == L"probeGoToDialogDefault") {
        if (app.AutomationGetLargeFileState().active) {
            return BuildErrorResponse(id, L"The Go To dialog probe is for the ordinary native editor.");
        }
        ResponseWriter response(id, true);
        response.AddString("initialLine", app.AutomationProbeGoToDialogDefault());
        return response.Finish();
    }

    if (name == L"insertTimeDate") {
        app.AutomationInsertTimeDate();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"setWordWrap") {
        app.AutomationSetWordWrap(GetBool(request, "enabled", false));
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getWordWrap") {
        ResponseWriter response(id, true);
        response.AddBool("enabled", app.AutomationGetWordWrap());
        return response.Finish();
    }

    if (name == L"invokeWordWrapMenuCommand") {
        app.AutomationInvokeWordWrapMenuCommand();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getWordWrapMenuState") {
        bool enabled = false;
        bool checked = false;
        if (!app.AutomationGetWordWrapMenuState(enabled, checked)) {
            return BuildErrorResponse(id, L"The Word Wrap menu item is unavailable.");
        }
        ResponseWriter response(id, true);
        response.AddBool("enabled", enabled);
        response.AddBool("checked", checked);
        response.AddBool("activeEditorFocused", app.AutomationActiveEditorHasFocus());
        response.AddBool("nativeEditorFocused", app.AutomationNativeEditorHasFocus());
        response.AddBool("largeFileActive", app.AutomationGetLargeFileState().active);
        return response.Finish();
    }

    if (name == L"setStatusBarVisible") {
        app.AutomationSetStatusBarVisible(GetBool(request, "visible", true));
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getStatusBarVisible") {
        ResponseWriter response(id, true);
        response.AddBool("visible", app.AutomationGetStatusBarVisible());
        return response.Finish();
    }

    if (name == L"setFont") {
        std::wstring font;
        if (!RequireString(request, "font", font, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        if (!app.AutomationSetFont(font, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getFont") {
        ResponseWriter response(id, true);
        response.AddString("font", app.AutomationGetFont());
        return response.Finish();
    }

    if (name == L"pageSetup") {
        RECT margins = app.AutomationGetPageMarginsThousandths();
        const std::optional<long long> left = GetNumber(request, "left");
        const std::optional<long long> top = GetNumber(request, "top");
        const std::optional<long long> right = GetNumber(request, "right");
        const std::optional<long long> bottom = GetNumber(request, "bottom");
        if (left.has_value() || top.has_value() || right.has_value() || bottom.has_value()) {
            if (!left.has_value() || !top.has_value() || !right.has_value() || !bottom.has_value()) {
                return BuildErrorResponse(id, L"Page setup requires left, top, right, and bottom margins.");
            }

            margins.left = static_cast<LONG>(*left);
            margins.top = static_cast<LONG>(*top);
            margins.right = static_cast<LONG>(*right);
            margins.bottom = static_cast<LONG>(*bottom);
            if (!app.AutomationSetPageMarginsThousandths(margins, errorMessage)) {
                return BuildErrorResponse(id, errorMessage);
            }
        }

        ResponseWriter response(id, true);
        response.AddRaw("margins", BuildMarginsObject(app.AutomationGetPageMarginsThousandths()));
        return response.Finish();
    }

    if (name == L"printToTestSink") {
        std::wstring path;
        if (!RequireString(request, "path", path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        if (!app.AutomationPrintToTestSink(path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        ResponseWriter response(id, true);
        response.AddString("path", path);
        response.AddNumber("pages", 1);
        response.AddString("font", app.AutomationGetFont());
        response.AddRaw("margins", BuildMarginsObject(app.AutomationGetPageMarginsThousandths()));
        return response.Finish();
    }

    if (name == L"checkSpelling") {
        const std::wstring text = GetString(request, "text").value_or(app.AutomationGetText());
        const SpellCheckResult result = app.AutomationCheckSpelling(text);
        ResponseWriter response(id, true);
        response.AddBool("available", app.AutomationSpellCheckAvailable());
        AddSpellOperationFields(response, result.status, result.hresult);
        response.AddRaw("errors", result.status == SpellOperationStatus::Success
            ? BuildSpellingErrorsArray(result.value, text) : std::string("[]"));
        return response.Finish();
    }

    if (name == L"suggestSpelling") {
        std::wstring word;
        if (!RequireString(request, "word", word, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        const long long requestedLimit = GetNumber(request, "limit").value_or(5);
        const std::size_t limit = requestedLimit <= 0
            ? 0U
            : static_cast<std::size_t>(std::min<long long>(requestedLimit, 20));
        const SpellSuggestResult result = app.AutomationSuggestSpelling(word, limit);
        ResponseWriter response(id, true);
        response.AddBool("available", app.AutomationSpellCheckAvailable());
        AddSpellOperationFields(response, result.status, result.hresult);
        response.AddRaw("suggestions", result.status == SpellOperationStatus::Success
            ? BuildStringArray(result.value) : std::string("[]"));
        return response.Finish();
    }

    if (name == L"ignoreSpelling") {
        std::wstring word;
        if (!RequireString(request, "word", word, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        const SpellActionResult result = app.AutomationIgnoreSpelling(word);
        ResponseWriter response(id, true);
        response.AddBool("available", app.AutomationSpellCheckAvailable());
        AddSpellOperationFields(response, result.status, result.hresult);
        response.AddBool("changed", result.status == SpellOperationStatus::Success && result.value);
        return response.Finish();
    }

    if (name == L"addSpelling") {
        std::wstring word;
        if (!RequireString(request, "word", word, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        const bool dryRun = GetBool(request, "dryRun", true);
        const SpellActionResult result = app.AutomationAddSpelling(word, dryRun);
        ResponseWriter response(id, true);
        response.AddBool("available", app.AutomationSpellCheckAvailable());
        AddSpellOperationFields(response, result.status, result.hresult);
        response.AddBool("dryRun", dryRun);
        response.AddBool("persisted", result.status == SpellOperationStatus::Success && !dryRun && result.value);
        return response.Finish();
    }

    if (name == L"configureSpellingTestBackend") {
        SpellTestBackendConfiguration configuration;
        configuration.available = GetBool(request, "available", true);
        if (const auto misspelledWord = GetString(request, "misspelledWord")) {
            if (!misspelledWord->empty()) configuration.misspelledWords.push_back(*misspelledWord);
        }
        if (const auto suggestions = GetString(request, "suggestions")) {
            std::size_t start = 0U;
            while (start < suggestions->size()) {
                const std::size_t separator = suggestions->find(L'|', start);
                const std::size_t end = separator == std::wstring::npos ? suggestions->size() : separator;
                if (end > start) configuration.suggestions.push_back(suggestions->substr(start, end - start));
                if (separator == std::wstring::npos) break;
                start = separator + 1U;
            }
        }
        const auto parseStatusField = [&request, &errorMessage](const char* key, SpellOperationStatus& output) {
            const std::optional<std::wstring> value = GetString(request, key);
            if (!value.has_value()) return true;
            const std::optional<SpellOperationStatus> parsed = ParseSpellOperationStatus(*value);
            if (!parsed.has_value()) {
                errorMessage = L"Invalid spelling test status for field: ";
                errorMessage += Utf8ToWide(key);
                return false;
            }
            output = *parsed;
            return true;
        };
        const auto parseHRESULTField = [&request, &errorMessage](const char* key, HRESULT& output) {
            const std::optional<long long> value = GetNumber(request, key);
            if (!value.has_value()) return true;
            if (*value < 0 || static_cast<unsigned long long>(*value) > 0xffffffffULL) {
                errorMessage = L"Invalid HRESULT for spelling test field: ";
                errorMessage += Utf8ToWide(key);
                return false;
            }
            output = static_cast<HRESULT>(static_cast<std::uint32_t>(*value));
            return true;
        };
        const auto parseFailAfter = [&request, &errorMessage](const char* key, std::size_t& output) {
            const std::optional<long long> value = GetNumber(request, key);
            if (!value.has_value()) return true;
            if (*value < 0) {
                errorMessage = L"Enumeration failure position must be non-negative.";
                return false;
            }
            output = static_cast<std::size_t>(*value);
            return true;
        };
        if (!parseStatusField("checkStatus", configuration.checkStatus) ||
            !parseStatusField("suggestStatus", configuration.suggestStatus) ||
            !parseStatusField("ignoreStatus", configuration.ignoreStatus) ||
            !parseStatusField("addStatus", configuration.addStatus) ||
            !parseHRESULTField("checkHresult", configuration.checkHresult) ||
            !parseHRESULTField("suggestHresult", configuration.suggestHresult) ||
            !parseHRESULTField("ignoreHresult", configuration.ignoreHresult) ||
            !parseHRESULTField("addHresult", configuration.addHresult) ||
            !parseFailAfter("failCheckAfter", configuration.failCheckAfter) ||
            !parseFailAfter("failSuggestAfter", configuration.failSuggestAfter)) {
            return BuildErrorResponse(id, errorMessage);
        }
        if (!app.AutomationConfigureSpellingTestBackend(configuration)) {
            return BuildErrorResponse(id, L"The spelling test backend is available only in automation mode.");
        }
        ResponseWriter response(id, true);
        response.AddBool("available", app.AutomationSpellCheckAvailable());
        response.AddString("status", configuration.available ? L"success" : L"unavailable");
        return response.Finish();
    }

    if (name == L"clearSpellingTestBackend") {
        if (!app.AutomationClearSpellingTestBackend()) {
            return BuildErrorResponse(id, L"The spelling test backend is available only in automation mode.");
        }
        ResponseWriter response(id, true);
        response.AddBool("available", app.AutomationSpellCheckAvailable());
        return response.Finish();
    }

    if (name == L"getSpellingTestBackendCalls") {
        ResponseWriter response(id, true);
        response.AddNumber("check", static_cast<long long>(app.AutomationSpellingTestBackendCallCount(L"check")));
        response.AddNumber("suggest", static_cast<long long>(app.AutomationSpellingTestBackendCallCount(L"suggest")));
        response.AddNumber("ignore", static_cast<long long>(app.AutomationSpellingTestBackendCallCount(L"ignore")));
        response.AddNumber("add", static_cast<long long>(app.AutomationSpellingTestBackendCallCount(L"add")));
        return response.Finish();
    }

    if (name == L"refreshSpellCheckNow") {
        if (!app.AutomationRefreshSpellCheckNow()) {
            return BuildErrorResponse(id, L"Manual spelling refresh is available only in automation mode.");
        }
        const classic_notepad::SpellingAnnotationBatch batch = app.AutomationGetSpellingAnnotationBatch();
        ResponseWriter response(id, true);
        response.AddBool("valid", batch.valid);
        response.AddNumber("sessionId", static_cast<long long>(batch.revision.session));
        response.AddNumber("contentRevision", static_cast<long long>(batch.revision.content));
        response.AddNumber("layoutRevision", static_cast<long long>(batch.revision.layout));
        response.AddRaw("annotations", BuildSpellingAnnotationsArray(batch));
        return response.Finish();
    }

    if (name == L"getSpellingAnnotations") {
        const classic_notepad::SpellingAnnotationBatch batch = app.AutomationGetSpellingAnnotationBatch();
        ResponseWriter response(id, true);
        response.AddBool("valid", batch.valid);
        response.AddNumber("sessionId", static_cast<long long>(batch.revision.session));
        response.AddNumber("contentRevision", static_cast<long long>(batch.revision.content));
        response.AddNumber("layoutRevision", static_cast<long long>(batch.revision.layout));
        response.AddRaw("annotations", BuildSpellingAnnotationsArray(batch));
        return response.Finish();
    }

    if (name == L"applySpellingAction") {
        std::wstring action;
        std::wstring targetText;
        std::wstring suggestion;
        long long parsedSession = 0;
        long long parsedContent = 0;
        long long parsedLayout = 0;
        long long parsedStart = 0;
        long long parsedLength = 0;
        if (!RequireString(request, "action", action, errorMessage) ||
            !RequireNumber(request, "sessionId", parsedSession, errorMessage) ||
            !RequireNumber(request, "contentRevision", parsedContent, errorMessage) ||
            !RequireNumber(request, "layoutRevision", parsedLayout, errorMessage) ||
            !RequireNumber(request, "start", parsedStart, errorMessage) ||
            !RequireNumber(request, "length", parsedLength, errorMessage) ||
            !RequireString(request, "text", targetText, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }
        if (parsedSession <= 0 || parsedContent <= 0 || parsedLayout < 0 || parsedStart < 0 || parsedLength <= 0) {
            return BuildErrorResponse(id, L"Spelling action coordinates and revision tags must be positive and in range.");
        }
        suggestion = GetString(request, "suggestion").value_or(L"");
        const bool injectMappedInsertFailureForTesting =
            GetBool(request, "injectMappedInsertFailureForTesting", false);
        classic_notepad::SpellingContextTarget target;
        target.revision = {
            static_cast<std::uint64_t>(parsedSession),
            static_cast<std::uint64_t>(parsedContent),
            static_cast<std::uint64_t>(parsedLayout)};
        target.start = static_cast<std::uint64_t>(parsedStart);
        target.length = static_cast<std::uint64_t>(parsedLength);
        target.text = std::move(targetText);
        const auto result = app.AutomationApplySpellingAction(
            action, target, suggestion, injectMappedInsertFailureForTesting);
        ResponseWriter response(id, true);
        AddSpellOperationFields(response, result.status, result.hresult);
        response.AddBool("changed", result.changed);
        response.AddBool("partial", result.partial);
        response.AddNumber("nativeNotification", result.nativeNotification);
        response.AddString("verification", result.verification);
        response.AddString("message", result.message);
        return response.Finish();
    }

    if (name == L"close") {
        shouldClose = true;
        return ResponseWriter(id, true).Finish();
    }

    errorMessage = L"Unsupported command: ";
    errorMessage += name;
    return BuildErrorResponse(id, errorMessage);
}

} // namespace

WindowsAutomationController::WindowsAutomationController(ClassicNotepadApp& app)
    : app_(app)
{
}

int WindowsAutomationController::Run()
{
    bool shouldClose = false;
    std::wstring testClipboard;
    const auto processLine = [&](const std::string& line) {
        JsonObject request;
        std::string parseError;
        std::string response;

        JsonParser parser(line);
        if (!parser.ParseObject(request, parseError)) {
            response = BuildErrorResponse(0, Utf8ToWide(parseError));
        } else {
            response = HandleCommand(app_, testClipboard, request, shouldClose);
        }

        std::cout << response << '\n';
        std::cout.flush();
    };

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE || GetFileType(input) != FILE_TYPE_PIPE) {
        std::string line;
        while (!shouldClose && std::getline(std::cin, line)) processLine(line);
        return 0;
    }

    std::string pending;
    while (!shouldClose) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                shouldClose = true;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (shouldClose) break;

        DWORD available = 0;
        if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available == 0U) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            continue;
        }
        char buffer[4096];
        DWORD read = 0;
        if (!ReadFile(input, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || read == 0U) break;
        pending.append(buffer, buffer + read);
        for (;;) {
            const std::size_t newline = pending.find('\n');
            if (newline == std::string::npos) break;
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1U);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            processLine(line);
            if (shouldClose) break;
        }
    }

    return 0;
}
