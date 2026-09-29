#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace engine_core {
namespace {

bool member_less(const JsonValue::Member& member, std::string_view key) { return member.first < key; }

// Reads with the classic locale. strtod follows LC_NUMERIC, which may use a comma.
template <typename T>
bool read_decimal(const std::string& text, T& out) {
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    T value{};
    stream >> value;
    if (stream.fail()) {
        return false;
    }
    stream.peek();
    if (!stream.eof()) {
        return false;
    }
    out = value;
    return true;
}

std::string print_g(int precision, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
    std::string text = buffer;
    // snprintf follows LC_NUMERIC too.
    std::replace(text.begin(), text.end(), ',', '.');
    return text;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool document(JsonValue& out, std::string& error) {
        skip_space();
        if (!value(out, 0)) {
            error = error_;
            return false;
        }
        skip_space();
        if (at_ < text_.size()) {
            fail("trailing text");
            error = error_;
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;

    bool fail(const char* message) {
        if (error_.empty()) {
            error_ = std::string(message) + " at byte " + std::to_string(at_);
        }
        return false;
    }

    void skip_space() {
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++at_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) {
            return fail("unexpected token");
        }
        at_ += word.size();
        return true;
    }

    bool value(JsonValue& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        if (at_ >= text_.size()) {
            return fail("unexpected end");
        }
        const char c = text_[at_];
        if (c == '{') {
            return object(out, depth);
        }
        if (c == '[') {
            return array(out, depth);
        }
        if (c == '"') {
            std::string text;
            if (!string(text)) {
                return false;
            }
            out = JsonValue::string(std::move(text));
            return true;
        }
        if (c == 't') {
            out = JsonValue::boolean(true);
            return literal("true");
        }
        if (c == 'f') {
            out = JsonValue::boolean(false);
            return literal("false");
        }
        if (c == 'n') {
            out = JsonValue();
            return literal("null");
        }
        return number(out);
    }

    bool object(JsonValue& out, int depth) {
        ++at_;
        out = JsonValue::object();
        skip_space();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return true;
        }
        for (;;) {
            skip_space();
            if (at_ >= text_.size() || text_[at_] != '"') {
                return fail("expected a key");
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            if (out.find(key) != nullptr) {
                return fail("duplicate key");
            }
            skip_space();
            if (at_ >= text_.size() || text_[at_] != ':') {
                return fail("expected ':'");
            }
            ++at_;
            skip_space();
            JsonValue item;
            if (!value(item, depth + 1)) {
                return false;
            }
            out.set(std::move(key), std::move(item));
            skip_space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(JsonValue& out, int depth) {
        ++at_;
        out = JsonValue::array();
        skip_space();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return true;
        }
        for (;;) {
            skip_space();
            JsonValue item;
            if (!value(item, depth + 1)) {
                return false;
            }
            out.items().push_back(std::move(item));
            skip_space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool hex4(unsigned& out) {
        if (at_ + 4 > text_.size()) {
            return fail("short \\u escape");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return fail("bad \\u escape");
            }
        }
        return true;
    }

    static void put_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    bool string(std::string& out) {
        ++at_;
        for (;;) {
            if (at_ >= text_.size()) {
                return fail("unterminated string");
            }
            const char c = text_[at_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control character in string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_ >= text_.size()) {
                return fail("unterminated escape");
            }
            const char e = text_[at_++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                out.push_back(e);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                unsigned code = 0;
                if (!hex4(code)) {
                    return false;
                }
                if (code >= 0xd800 && code <= 0xdbff) {
                    unsigned low = 0;
                    if (text_.substr(at_, 2) != "\\u") {
                        return fail("unpaired surrogate");
                    }
                    at_ += 2;
                    if (!hex4(low) || low < 0xdc00 || low > 0xdfff) {
                        return fail("unpaired surrogate");
                    }
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                } else if (code >= 0xdc00 && code <= 0xdfff) {
                    return fail("unpaired surrogate");
                }
                put_utf8(out, code);
                break;
            }
            default:
                return fail("bad escape");
            }
        }
    }

    bool digits() {
        const std::size_t start = at_;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
            ++at_;
        }
        return at_ > start;
    }

    bool number(JsonValue& out) {
        const std::size_t start = at_;
        if (at_ < text_.size() && text_[at_] == '-') {
            ++at_;
        }
        if (at_ < text_.size() && text_[at_] == '0') {
            ++at_;
        } else if (!digits()) {
            return fail("unexpected token");
        }
        if (at_ < text_.size() && text_[at_] == '.') {
            ++at_;
            if (!digits()) {
                return fail("bad number");
            }
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) {
                ++at_;
            }
            if (!digits()) {
                return fail("bad number");
            }
        }
        double value = 0.0;
        if (!read_decimal(std::string(text_.substr(start, at_ - start)), value) || !std::isfinite(value)) {
            at_ = start;
            return fail("bad number");
        }
        out = JsonValue::number(value);
        return true;
    }

    std::string_view text_;
    std::size_t at_ = 0;
    std::string error_;
};

void write_string(std::string& out, const std::string& text) {
    out.push_back('"');
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (byte < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
                out += buffer;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

int key_rank(const std::string& key) {
    if (key == "class") {
        return 0;
    }
    if (key == "id") {
        return 1;
    }
    return 2;
}

void write_value(std::string& out, const JsonValue& value, int indent);

void write_indent(std::string& out, int indent) { out.append(static_cast<std::size_t>(indent) * 2, ' '); }

void write_value(std::string& out, const JsonValue& value, int indent) {
    switch (value.kind()) {
    case JsonValue::Kind::Null:
        out += "null";
        return;
    case JsonValue::Kind::Bool:
        out += value.as_bool() ? "true" : "false";
        return;
    case JsonValue::Kind::Number:
        out += format_json_number(value.as_number());
        return;
    case JsonValue::Kind::String:
        write_string(out, value.as_string());
        return;
    case JsonValue::Kind::Array: {
        const std::vector<JsonValue>& items = value.items();
        if (items.empty()) {
            out += "[]";
            return;
        }
        const bool numbers = std::all_of(items.begin(), items.end(), [](const JsonValue& v) { return v.is_number(); });
        if (numbers) {
            out.push_back('[');
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i > 0) {
                    out += ", ";
                }
                out += format_json_number(items[i].as_number());
            }
            out.push_back(']');
            return;
        }
        out += "[\n";
        for (std::size_t i = 0; i < items.size(); ++i) {
            write_indent(out, indent + 1);
            write_value(out, items[i], indent + 1);
            out += i + 1 < items.size() ? ",\n" : "\n";
        }
        write_indent(out, indent);
        out.push_back(']');
        return;
    }
    case JsonValue::Kind::Object: {
        const std::vector<JsonValue::Member>& members = value.members();
        if (members.empty()) {
            out += "{}";
            return;
        }
        std::vector<const JsonValue::Member*> ordered;
        ordered.reserve(members.size());
        for (const JsonValue::Member& member : members) {
            ordered.push_back(&member);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const JsonValue::Member* a, const JsonValue::Member* b) {
            const int ra = key_rank(a->first);
            const int rb = key_rank(b->first);
            if (ra != rb) {
                return ra < rb;
            }
            return a->first < b->first;
        });
        out += "{\n";
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            write_indent(out, indent + 1);
            write_string(out, ordered[i]->first);
            out += ": ";
            write_value(out, ordered[i]->second, indent + 1);
            out += i + 1 < ordered.size() ? ",\n" : "\n";
        }
        write_indent(out, indent);
        out.push_back('}');
        return;
    }
    }
}

void write_compact(std::string& out, const JsonValue& value) {
    switch (value.kind()) {
    case JsonValue::Kind::Null:
        out += "null";
        return;
    case JsonValue::Kind::Bool:
        out += value.as_bool() ? "true" : "false";
        return;
    case JsonValue::Kind::Number:
        out += format_json_number(value.as_number());
        return;
    case JsonValue::Kind::String:
        write_string(out, value.as_string());
        return;
    case JsonValue::Kind::Array: {
        out.push_back('[');
        bool first = true;
        for (const JsonValue& item : value.items()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            write_compact(out, item);
        }
        out.push_back(']');
        return;
    }
    case JsonValue::Kind::Object: {
        out.push_back('{');
        bool first = true;
        for (const JsonValue::Member& member : value.members()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            write_string(out, member.first);
            out.push_back(':');
            write_compact(out, member.second);
        }
        out.push_back('}');
        return;
    }
    }
}

}  // namespace

JsonValue JsonValue::boolean(bool value) {
    JsonValue out;
    out.kind_ = Kind::Bool;
    out.flag_ = value;
    return out;
}

JsonValue JsonValue::number(double value) {
    JsonValue out;
    out.kind_ = Kind::Number;
    // One zero on disk.
    out.number_ = value == 0.0 ? 0.0 : value;
    return out;
}

JsonValue JsonValue::number_from_float(float value) {
    if (!std::isfinite(value)) {
        return number(static_cast<double>(value));
    }
    for (int precision = 1; precision <= 9; ++precision) {
        const std::string text = print_g(precision, static_cast<double>(value));
        float back = 0.f;
        if (read_decimal(text, back) && back == value) {
            double exact = 0.0;
            if (read_decimal(text, exact)) {
                return number(exact);
            }
        }
    }
    return number(static_cast<double>(value));
}

JsonValue JsonValue::string(std::string value) {
    JsonValue out;
    out.kind_ = Kind::String;
    out.text_ = std::move(value);
    return out;
}

JsonValue JsonValue::array(std::vector<JsonValue> items) {
    JsonValue out;
    out.kind_ = Kind::Array;
    out.items_ = std::move(items);
    return out;
}

JsonValue JsonValue::object() {
    JsonValue out;
    out.kind_ = Kind::Object;
    return out;
}

const JsonValue* JsonValue::find(std::string_view key) const {
    if (kind_ != Kind::Object) {
        return nullptr;
    }
    return bag_find(members_, key);
}

void JsonValue::set(std::string key, JsonValue value) {
    kind_ = Kind::Object;
    bag_set(members_, std::move(key), std::move(value));
}

bool JsonValue::erase(std::string_view key) { return kind_ == Kind::Object && bag_erase(members_, key); }

bool JsonValue::operator==(const JsonValue& other) const {
    if (kind_ != other.kind_) {
        return false;
    }
    switch (kind_) {
    case Kind::Null:
        return true;
    case Kind::Bool:
        return flag_ == other.flag_;
    case Kind::Number:
        return number_ == other.number_;
    case Kind::String:
        return text_ == other.text_;
    case Kind::Array:
        return items_ == other.items_;
    case Kind::Object:
        return members_ == other.members_;
    }
    return false;
}

const JsonValue* bag_find(const PropertyBag& bag, std::string_view key) {
    const auto it = std::lower_bound(bag.begin(), bag.end(), key, member_less);
    if (it == bag.end() || it->first != key) {
        return nullptr;
    }
    return &it->second;
}

void bag_set(PropertyBag& bag, std::string key, JsonValue value) {
    const auto it = std::lower_bound(bag.begin(), bag.end(), std::string_view(key), member_less);
    if (it != bag.end() && it->first == key) {
        it->second = std::move(value);
        return;
    }
    bag.insert(it, JsonValue::Member(std::move(key), std::move(value)));
}

bool bag_erase(PropertyBag& bag, std::string_view key) {
    const auto it = std::lower_bound(bag.begin(), bag.end(), key, member_less);
    if (it == bag.end() || it->first != key) {
        return false;
    }
    bag.erase(it);
    return true;
}

JsonValue json_floats(const float* values, std::size_t count) {
    std::vector<JsonValue> items;
    items.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        items.push_back(JsonValue::number_from_float(values[i]));
    }
    return JsonValue::array(std::move(items));
}

bool read_json_floats(const JsonValue& value, std::size_t min, std::size_t max, std::vector<float>& out) {
    if (!value.is_array() || value.items().size() < min || value.items().size() > max) {
        return false;
    }
    std::vector<float> floats;
    floats.reserve(value.items().size());
    for (const JsonValue& item : value.items()) {
        const double number = item.as_number();
        if (!item.is_number() || !std::isfinite(static_cast<float>(number))) {
            return false;
        }
        floats.push_back(static_cast<float>(number));
    }
    out = std::move(floats);
    return true;
}

bool parse_json(std::string_view text, JsonValue& out, std::string& error) {
    // A UTF-8 byte order mark is not JSON. Editors add one on Windows.
    if (text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);
    }
    Parser parser(text);
    JsonValue value;
    if (!parser.document(value, error)) {
        return false;
    }
    out = std::move(value);
    return true;
}

std::string write_json(const JsonValue& value) {
    std::string out;
    write_value(out, value, 0);
    out.push_back('\n');
    return out;
}

std::string compact_json(const JsonValue& value) {
    std::string out;
    write_compact(out, value);
    return out;
}

std::string format_json_number(double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("JSON cannot hold a non-finite number");
    }
    if (value == 0.0) {
        return "0";
    }
    if (std::floor(value) == value && std::fabs(value) < 1e15) {
        return print_g(17, value);
    }
    for (int precision = 1; precision <= 17; ++precision) {
        const std::string text = print_g(precision, value);
        double back = 0.0;
        if (read_decimal(text, back) && back == value) {
            return text;
        }
    }
    return print_g(17, value);
}

}  // namespace engine_core
