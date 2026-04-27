#include "automation.h"

#include "app.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <iostream>
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

std::string BuildCapabilitiesObject(const ClassicNotepadApp& app)
{
    std::ostringstream output;
    output << "{"
           << "\"platform\":\"windows\""
           << ",\"nativeUi\":\"win32\""
           << ",\"printing\":true"
           << ",\"pageSetup\":true"
           << ",\"fontChooser\":true"
           << ",\"spellCheck\":" << (app.AutomationSpellCheckAvailable() ? "true" : "false")
           << ",\"darkMode\":" << (app.AutomationDarkModeEnabled() ? "true" : "false")
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

    if (name == L"newDocument") {
        app.AutomationNewDocument();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"openFile") {
        std::wstring path;
        if (!RequireString(request, "path", path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        if (!app.AutomationOpenFile(path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        return ResponseWriter(id, true).Finish();
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

    if (name == L"getText") {
        ResponseWriter response(id, true);
        response.AddString("text", app.AutomationGetText());
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

    if (name == L"deleteSelection") {
        app.AutomationDeleteSelection();
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

        if (!app.AutomationGoToLine(static_cast<int>(lineNumber), errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        ResponseWriter response(id, true);
        response.AddRaw("selection", BuildSelectionObject(app.AutomationGetSelection()));
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
        ResponseWriter response(id, true);
        response.AddBool("available", app.AutomationSpellCheckAvailable());
        response.AddRaw(
            "errors",
            app.AutomationSpellCheckAvailable()
                ? BuildSpellingErrorsArray(app.AutomationCheckSpelling(text), text)
                : std::string("[]"));
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
        ResponseWriter response(id, true);
        response.AddBool("available", app.AutomationSpellCheckAvailable());
        response.AddRaw(
            "suggestions",
            app.AutomationSpellCheckAvailable()
                ? BuildStringArray(app.AutomationSuggestSpelling(word, limit))
                : std::string("[]"));
        return response.Finish();
    }

    if (name == L"ignoreSpelling") {
        std::wstring word;
        if (!RequireString(request, "word", word, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        const bool available = app.AutomationSpellCheckAvailable();
        if (available && !app.AutomationIgnoreSpelling(word, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        ResponseWriter response(id, true);
        response.AddBool("available", available);
        return response.Finish();
    }

    if (name == L"addSpelling") {
        std::wstring word;
        if (!RequireString(request, "word", word, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        const bool dryRun = GetBool(request, "dryRun", true);
        const bool available = app.AutomationSpellCheckAvailable();
        if (available && !app.AutomationAddSpelling(word, dryRun, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        ResponseWriter response(id, true);
        response.AddBool("available", available);
        response.AddBool("persisted", available && !dryRun);
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
    std::string line;

    while (!shouldClose && std::getline(std::cin, line)) {
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
    }

    return 0;
}
