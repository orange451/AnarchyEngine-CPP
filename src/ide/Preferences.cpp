#include "Preferences.hpp"

#include "IdeResources.hpp"

#include <algorithm>
#include <cmath>
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

int Preferences::frame_rate() const {
    const engine_core::JsonValue* rate = root_.find("frameRate");
    if (rate == nullptr || !rate->is_number() || !std::isfinite(rate->as_number())) {
        return kDefaultFrameRate;
    }
    const double value = std::round(rate->as_number());
    if (value == kUncappedFrameRate) {
        return kUncappedFrameRate;
    }
    if (value < 1) {
        return kDefaultFrameRate;
    }
    return static_cast<int>(std::clamp(value, static_cast<double>(kMinFrameRate), static_cast<double>(kMaxFrameRate)));
}

void Preferences::set_frame_rate(int fps) {
    const int kept = fps == kUncappedFrameRate ? fps : std::clamp(fps, kMinFrameRate, kMaxFrameRate);
    root_.set("frameRate", engine_core::JsonValue::number(kept));
}

double Preferences::stage_frame_rate(int fps) { return fps == kUncappedFrameRate ? 0.0 : static_cast<double>(fps); }

std::string Preferences::assets_view() const {
    const engine_core::JsonValue* view = root_.find("assetsView");
    if (view != nullptr && view->is_string()) {
        const std::string& text = view->as_string();
        if (text == "icons" || text == "list" || text == "columns") {
            return text;
        }
    }
    return "icons";
}

void Preferences::set_assets_view(const std::string& view) { root_.set("assetsView", engine_core::JsonValue::string(view)); }

double Preferences::zoom() const {
    const engine_core::JsonValue* zoom = root_.find("zoom");
    if (zoom == nullptr || !zoom->is_number() || !std::isfinite(zoom->as_number())) {
        return 1.0;
    }
    return std::clamp(zoom->as_number(), kMinZoom, kMaxZoom);
}

void Preferences::set_zoom(double zoom) {
    root_.set("zoom", engine_core::JsonValue::number(std::clamp(zoom, kMinZoom, kMaxZoom)));
}

bool Preferences::save(std::string& error) const {
    if (file_.empty()) {
        return true;
    }
    return write_file(file_, engine_core::write_json(root_), error);
}

}  // namespace ide
