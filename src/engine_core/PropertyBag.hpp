#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine_core {

// One JSON value. Objects keep their keys sorted and unique, so two equal
// values always write the same bytes.
class JsonValue {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, JsonValue>;

    JsonValue() = default;

    static JsonValue boolean(bool value);
    static JsonValue number(double value);
    // The shortest decimal that reads back as this float. 0.1f writes as 0.1.
    static JsonValue number_from_float(float value);
    static JsonValue string(std::string value);
    static JsonValue array(std::vector<JsonValue> items = {});
    static JsonValue object();

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::Null; }
    bool is_bool() const { return kind_ == Kind::Bool; }
    bool is_number() const { return kind_ == Kind::Number; }
    bool is_string() const { return kind_ == Kind::String; }
    bool is_array() const { return kind_ == Kind::Array; }
    bool is_object() const { return kind_ == Kind::Object; }

    // A wrong kind reads as false, 0, or empty.
    bool as_bool() const { return kind_ == Kind::Bool && flag_; }
    double as_number() const { return kind_ == Kind::Number ? number_ : 0.0; }
    const std::string& as_string() const { return text_; }
    const std::vector<JsonValue>& items() const { return items_; }
    std::vector<JsonValue>& items() { return items_; }
    const std::vector<Member>& members() const { return members_; }

    // Object access. find returns null when the key is missing or this is not an object.
    const JsonValue* find(std::string_view key) const;
    // Replaces an existing key. Keeps the members sorted.
    void set(std::string key, JsonValue value);
    bool erase(std::string_view key);

    bool operator==(const JsonValue& other) const;
    bool operator!=(const JsonValue& other) const { return !(*this == other); }

private:
    Kind kind_ = Kind::Null;
    bool flag_ = false;
    double number_ = 0.0;
    std::string text_;
    std::vector<JsonValue> items_;
    std::vector<Member> members_;
};

// Keys an instance class does not know, and the properties a class writes.
// Sorted by key.
using PropertyBag = std::vector<JsonValue::Member>;

const JsonValue* bag_find(const PropertyBag& bag, std::string_view key);
void bag_set(PropertyBag& bag, std::string key, JsonValue value);
bool bag_erase(PropertyBag& bag, std::string_view key);

// An array of floats, each written with number_from_float.
JsonValue json_floats(const float* values, std::size_t count);
// Reads an array of min..max finite numbers that fit a float. False otherwise.
bool read_json_floats(const JsonValue& value, std::size_t min, std::size_t max, std::vector<float>& out);

// Strict JSON. Duplicate keys and trailing text are errors. error names the byte offset.
bool parse_json(std::string_view text, JsonValue& out, std::string& error);

// Canonical bytes: 2-space indent, LF, trailing newline. Object keys are
// byte-sorted, except that "class" and then "id" come first. An array of
// numbers stays on one line. Every other array puts one item on each line.
// A non-finite number throws std::invalid_argument.
std::string write_json(const JsonValue& value);

// One formatter for every number written to disk. Integers print without a
// fraction. Others use the shortest decimal that reads back to the same double.
std::string format_json_number(double value);

}  // namespace engine_core
