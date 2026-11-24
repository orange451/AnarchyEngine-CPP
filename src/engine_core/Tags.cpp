#include "Tags.hpp"

#include "PropertyBag.hpp"

#include <vector>

namespace engine_core {

std::optional<std::string> tag_error(std::string_view tag) {
    bool valid = !tag.empty() && tag.size() <= kMaxTag;
    for (const char c : tag) {
        const auto byte = static_cast<unsigned char>(c);
        valid = valid && byte >= 0x20 && byte != 0x7f;
    }
    if (valid) {
        return std::nullopt;
    }
    return std::string("a tag is 1 to 100 characters, with no control characters");
}

std::string tags_to_json(const TagSet& tags) {
    if (tags.empty()) {
        return {};
    }
    std::vector<JsonValue> items;
    for (const std::string& tag : tags) {
        items.push_back(JsonValue::string(tag));
    }
    return compact_json(JsonValue::array(std::move(items)));
}

bool tags_from_json(std::string_view text, TagSet& out, std::string& error) {
    out.clear();
    if (text.empty()) {
        return true;
    }
    JsonValue root;
    std::string why;
    if (!parse_json(text, root, why)) {
        error = "Tags is not JSON: " + why;
        return false;
    }
    if (root.is_null()) {
        return true;
    }
    if (!root.is_array()) {
        error = "Tags must be a JSON array";
        return false;
    }
    for (const JsonValue& item : root.items()) {
        if (item.is_string() && !tag_error(item.as_string())) {
            out.insert(item.as_string());
            continue;
        }
        if (!error.empty()) {
            error += '\n';
        }
        error += item.is_string() ? "tag \"" + item.as_string() + "\": " + *tag_error(item.as_string())
                                  : std::string("a tag must be a string: ") + compact_json(item);
    }
    return true;
}

}  // namespace engine_core
