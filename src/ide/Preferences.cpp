#include "Preferences.hpp"

#include "IdeResources.hpp"

#include <system_error>
#include <utility>

namespace ide {

Preferences::Preferences(std::filesystem::path file) : file_(std::move(file)) {
    std::error_code missing;
    if (file_.empty() || !std::filesystem::exists(file_, missing)) {
        return;
    }
    std::string text;
    std::string error;
    engine_core::JsonValue root;
    if (!read_file(file_, text, error)) {
        load_error_ = error;
    } else if (!engine_core::parse_json(text, root, error)) {
        load_error_ = utf8_path(file_) + ": " + error;
    } else if (!root.is_object()) {
        load_error_ = utf8_path(file_) + " is not a JSON object";
    } else {
        root_ = std::move(root);
    }
}

std::string Preferences::theme() const {
    const engine_core::JsonValue* theme = root_.find("theme");
    return theme != nullptr && theme->is_string() && !theme->as_string().empty() ? theme->as_string() : "light";
}

void Preferences::set_theme(const std::string& id) { root_.set("theme", engine_core::JsonValue::string(id)); }

bool Preferences::save(std::string& error) const {
    if (file_.empty()) {
        return true;
    }
    return write_file(file_, engine_core::write_json(root_), error);
}

}  // namespace ide
