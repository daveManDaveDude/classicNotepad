#include "gtk_automation.h"

#include "gtk_app.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

namespace classic_notepad::linux_ui {
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

std::string NarrowAscii(const std::wstring& text)
{
    std::string narrowed;
    narrowed.reserve(text.size());
    for (wchar_t character : text) {
        narrowed.push_back(character >= 0 && character <= 0x7f ? static_cast<char>(character) : '?');
    }
    return narrowed;
}

void AppendUtf8Chunk(std::wstring& target, std::string& chunk)
{
    if (!chunk.empty()) {
        target += WideFromUtf8(chunk);
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

std::optional<std::uint32_t> ParseHexQuad(const std::string& text, std::size_t position)
{
    if (position + 4U > text.size()) {
        return std::nullopt;
    }

    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        const char character = text[position + index];
        if (!IsHexDigit(character)) {
            return std::nullopt;
        }

        value = (value << 4U) | static_cast<std::uint32_t>(HexValue(character));
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
                const std::optional<std::uint32_t> codeUnit = ParseHexQuad(text_, position_);
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

void AppendEscapedCodePoint(std::ostringstream& output, std::uint32_t codePoint)
{
    constexpr char digits[] = "0123456789abcdef";
    auto appendQuad = [&](std::uint32_t value) {
        output << "\\u"
               << digits[(value >> 12U) & 0xfU]
               << digits[(value >> 8U) & 0xfU]
               << digits[(value >> 4U) & 0xfU]
               << digits[value & 0xfU];
    };

    if (codePoint <= 0xffffU) {
        appendQuad(codePoint);
        return;
    }

    const std::uint32_t adjusted = codePoint - 0x10000U;
    appendQuad(0xd800U + (adjusted >> 10U));
    appendQuad(0xdc00U + (adjusted & 0x3ffU));
}

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
        default: {
            const auto codePoint = static_cast<std::uint32_t>(character);
            if (codePoint >= 0x20U && codePoint <= 0x7eU) {
                output << static_cast<char>(codePoint);
            } else {
                AppendEscapedCodePoint(output, codePoint);
            }
            break;
        }
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
        error += WideFromUtf8(key);
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
        error += WideFromUtf8(key);
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

std::string BuildMetadataObject(const GtkNotepadApp::AutomationDocumentMetadata& metadata)
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

std::string BuildSelectionObject(const GtkNotepadApp::AutomationSelection& selection)
{
    std::ostringstream output;
    output << "{\"start\":" << selection.start << ",\"end\":" << selection.end << "}";
    return output.str();
}

std::string BuildMarginsObject(const GtkNotepadApp::AutomationPageMargins& margins)
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

std::string BuildCapabilitiesObject()
{
    std::ostringstream output;
    output << "{"
           << "\"platform\":\"linux\""
           << ",\"nativeUi\":\"gtk4\""
           << ",\"printing\":true"
           << ",\"pageSetup\":true"
           << ",\"fontChooser\":true"
           << ",\"spellCheck\":false"
           << ",\"darkMode\":false"
           << "}";
    return output.str();
}

std::size_t ClampSelectionValue(long long value)
{
    if (value <= 0) {
        return 0;
    }

    return static_cast<std::size_t>(std::min<long long>(value, static_cast<long long>(G_MAXINT)));
}

std::wstring SelectedText(GtkNotepadApp& app)
{
    const GtkNotepadApp::AutomationSelection selection = app.GetSelection();
    if (selection.end <= selection.start) {
        return {};
    }

    const std::wstring text = app.GetText();
    const std::size_t start = std::min(selection.start, text.size());
    const std::size_t end = std::min(selection.end, text.size());
    return start < end ? text.substr(start, end - start) : std::wstring();
}

std::string HandleCommand(GtkNotepadApp& app, std::wstring& testClipboard, const JsonObject& request, bool& shouldClose)
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
        response.AddRaw("capabilities", BuildCapabilitiesObject());
        return response.Finish();
    }

    if (name == L"newDocument") {
        app.NewDocument();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"openFile") {
        std::wstring path;
        if (!RequireString(request, "path", path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        if (!app.OpenFile(path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        return ResponseWriter(id, true).Finish();
    }

    if (name == L"save") {
        if (!app.Save(errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        return ResponseWriter(id, true).Finish();
    }

    if (name == L"saveAs") {
        std::wstring path;
        if (!RequireString(request, "path", path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        if (!app.SaveAs(path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        return ResponseWriter(id, true).Finish();
    }

    if (name == L"setText") {
        std::wstring text;
        if (!RequireString(request, "text", text, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        app.SetText(text);
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getText") {
        ResponseWriter response(id, true);
        response.AddString("text", app.GetText());
        return response.Finish();
    }

    if (name == L"insertText") {
        std::wstring text;
        if (!RequireString(request, "text", text, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        app.InsertText(text);
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getTitle") {
        ResponseWriter response(id, true);
        response.AddString("title", app.GetTitle());
        return response.Finish();
    }

    if (name == L"isModified") {
        ResponseWriter response(id, true);
        response.AddBool("modified", app.IsModified());
        return response.Finish();
    }

    if (name == L"getDocumentMetadata") {
        ResponseWriter response(id, true);
        response.AddRaw("metadata", BuildMetadataObject(app.GetDocumentMetadata()));
        return response.Finish();
    }

    if (name == L"getStatusText") {
        ResponseWriter response(id, true);
        response.AddString("statusText", app.GetStatusText());
        return response.Finish();
    }

    if (name == L"getSelection") {
        ResponseWriter response(id, true);
        response.AddRaw("selection", BuildSelectionObject(app.GetSelection()));
        return response.Finish();
    }

    if (name == L"setSelection") {
        long long start = 0;
        long long end = 0;
        if (!RequireNumber(request, "start", start, errorMessage) ||
            !RequireNumber(request, "end", end, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        app.SetSelection(ClampSelectionValue(start), ClampSelectionValue(end));
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"selectAll") {
        app.SelectAll();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"undo") {
        app.Undo();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"copy") {
        testClipboard = SelectedText(app);
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"cut") {
        testClipboard = SelectedText(app);
        app.DeleteSelection();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"paste") {
        app.InsertText(testClipboard);
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"deleteSelection") {
        app.DeleteSelection();
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

            found = app.Find(text, matchCase, wholeWord, searchDown);
        } else {
            found = app.FindNext(matchCase, wholeWord, searchDown);
        }

        ResponseWriter response(id, true);
        response.AddBool("found", found);
        response.AddRaw("selection", BuildSelectionObject(app.GetSelection()));
        return response.Finish();
    }

    if (name == L"replace") {
        std::wstring text;
        std::wstring replacement;
        if (!RequireString(request, "text", text, errorMessage) ||
            !RequireString(request, "replacement", replacement, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        const bool replaced = app.Replace(
            text,
            replacement,
            GetBool(request, "matchCase", false),
            GetBool(request, "wholeWord", false),
            GetBool(request, "searchDown", true));
        ResponseWriter response(id, true);
        response.AddBool("replaced", replaced);
        response.AddRaw("selection", BuildSelectionObject(app.GetSelection()));
        return response.Finish();
    }

    if (name == L"replaceAll") {
        std::wstring text;
        std::wstring replacement;
        if (!RequireString(request, "text", text, errorMessage) ||
            !RequireString(request, "replacement", replacement, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        const std::size_t count = app.ReplaceAll(
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

        if (!app.GoToLine(static_cast<int>(lineNumber), errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        ResponseWriter response(id, true);
        response.AddRaw("selection", BuildSelectionObject(app.GetSelection()));
        return response.Finish();
    }

    if (name == L"insertTimeDate") {
        app.InsertTimeDate();
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"setWordWrap") {
        app.SetWordWrap(GetBool(request, "enabled", false));
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getWordWrap") {
        ResponseWriter response(id, true);
        response.AddBool("enabled", app.GetWordWrap());
        return response.Finish();
    }

    if (name == L"setStatusBarVisible") {
        app.SetStatusBarVisible(GetBool(request, "visible", true));
        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getStatusBarVisible") {
        ResponseWriter response(id, true);
        response.AddBool("visible", app.GetStatusBarVisible());
        return response.Finish();
    }

    if (name == L"setFont") {
        std::wstring font;
        if (!RequireString(request, "font", font, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        if (!app.SetFont(font, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        return ResponseWriter(id, true).Finish();
    }

    if (name == L"getFont") {
        ResponseWriter response(id, true);
        response.AddString("font", app.GetFont());
        return response.Finish();
    }

    if (name == L"pageSetup") {
        GtkNotepadApp::AutomationPageMargins margins = app.GetPageMargins();
        const std::optional<long long> left = GetNumber(request, "left");
        const std::optional<long long> top = GetNumber(request, "top");
        const std::optional<long long> right = GetNumber(request, "right");
        const std::optional<long long> bottom = GetNumber(request, "bottom");
        if (left.has_value() || top.has_value() || right.has_value() || bottom.has_value()) {
            if (!left.has_value() || !top.has_value() || !right.has_value() || !bottom.has_value()) {
                return BuildErrorResponse(id, L"Page setup requires left, top, right, and bottom margins.");
            }

            margins.left = static_cast<int>(*left);
            margins.top = static_cast<int>(*top);
            margins.right = static_cast<int>(*right);
            margins.bottom = static_cast<int>(*bottom);
            if (!app.SetPageMargins(margins, errorMessage)) {
                return BuildErrorResponse(id, errorMessage);
            }
        }

        ResponseWriter response(id, true);
        response.AddRaw("margins", BuildMarginsObject(app.GetPageMargins()));
        return response.Finish();
    }

    if (name == L"printToTestSink") {
        std::wstring path;
        if (!RequireString(request, "path", path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        if (!app.PrintToTestSink(path, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        ResponseWriter response(id, true);
        response.AddString("path", path);
        response.AddNumber("pages", 1);
        response.AddString("font", app.GetFont());
        response.AddRaw("margins", BuildMarginsObject(app.GetPageMargins()));
        return response.Finish();
    }

    if (name == L"checkSpelling") {
        ResponseWriter response(id, true);
        response.AddBool("available", app.SpellCheckAvailable());
        response.AddRaw("errors", "[]");
        return response.Finish();
    }

    if (name == L"suggestSpelling") {
        std::wstring word;
        if (!RequireString(request, "word", word, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        (void)word;
        ResponseWriter response(id, true);
        response.AddBool("available", app.SpellCheckAvailable());
        response.AddRaw("suggestions", BuildStringArray(std::vector<std::wstring>{}));
        return response.Finish();
    }

    if (name == L"ignoreSpelling" || name == L"addSpelling") {
        std::wstring word;
        if (!RequireString(request, "word", word, errorMessage)) {
            return BuildErrorResponse(id, errorMessage);
        }

        (void)word;
        ResponseWriter response(id, true);
        response.AddBool("available", app.SpellCheckAvailable());
        if (name == L"addSpelling") {
            response.AddBool("persisted", false);
        }
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

GtkAutomationController::GtkAutomationController(GtkNotepadApp& app)
    : app_(app)
{
}

int GtkAutomationController::Run()
{
    bool shouldClose = false;
    std::string line;

    while (!shouldClose && std::getline(std::cin, line)) {
        JsonObject request;
        std::string parseError;
        std::string response;

        JsonParser parser(line);
        if (!parser.ParseObject(request, parseError)) {
            response = BuildErrorResponse(0, WideFromUtf8(parseError));
        } else {
            response = HandleCommand(app_, testClipboard_, request, shouldClose);
        }

        app_.PumpEvents();
        std::cout << response << '\n';
        std::cout.flush();
    }

    return 0;
}

} // namespace classic_notepad::linux_ui
