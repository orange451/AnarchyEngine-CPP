#include "Gui.hpp"

#include "AssetInstances.hpp"
#include "Containment.hpp"
#include "Contract.hpp"
#include "Enum.hpp"
#include "PVInstance.hpp"
#include "PropertyBag.hpp"
#include "PropertyReflection.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>
#include <limits>
#include <string_view>

namespace engine_core {
namespace {

// One property: its registry name and type, the kind its slot holds, and, for
// a number or a Vector2, the range a write is clamped to.
struct GuiSpec {
    const char* name;
    const char* type;
    LuaSlot::Kind kind;
    double min;
    double max;
};

constexpr double kNoLimit = std::numeric_limits<double>::infinity();

// In GuiProperty order.
constexpr GuiSpec kSpecs[] = {
    {"ClassList", "string", LuaSlot::Kind::String, 0, 0},
    {"Style", "string", LuaSlot::Kind::String, 0, 0},
    {"Size", "Vector2", LuaSlot::Kind::Vec2, 0, kNoLimit},
    {"Alignment", "EnumItem", LuaSlot::Kind::Enum, 0, 0},
    {"Visible", "boolean", LuaSlot::Kind::Bool, 0, 0},
    {"MouseTransparent", "boolean", LuaSlot::Kind::Bool, 0, 0},
    {"BackgroundColor", "Color3", LuaSlot::Kind::Color, 0, 0},
    {"BackgroundTransparency", "number", LuaSlot::Kind::Number, 0, 1},
    {"Spacing", "number", LuaSlot::Kind::Number, 0, kNoLimit},
    {"Text", "string", LuaSlot::Kind::String, 0, 0},
    {"TextColor", "Color3", LuaSlot::Kind::Color, 0, 0},
    {"FontSize", "number", LuaSlot::Kind::Number, 1, 512},
    {"Prompt", "string", LuaSlot::Kind::String, 0, 0},
    {"Source", "string", LuaSlot::Kind::String, 0, 0},
    {"AlwaysOnTop", "boolean", LuaSlot::Kind::Bool, 0, 0},
    {"ImageTransparency", "number", LuaSlot::Kind::Number, 0, 1},
    {"TextScaled", "boolean", LuaSlot::Kind::Bool, 0, 0},
    {"Title", "string", LuaSlot::Kind::String, 0, 0},
    {"Enabled", "boolean", LuaSlot::Kind::Bool, 0, 0},
    {"Value", "number", LuaSlot::Kind::Number, -kNoLimit, kNoLimit},
    {"Min", "number", LuaSlot::Kind::Number, -kNoLimit, kNoLimit},
    {"Max", "number", LuaSlot::Kind::Number, -kNoLimit, kNoLimit},
    {"Step", "number", LuaSlot::Kind::Number, 0, kNoLimit},
    {"AssetType", "EnumItem", LuaSlot::Kind::Enum, 0, 0},
};
static_assert(std::size(kSpecs) == static_cast<std::size_t>(GuiProperty::Count), "a GuiProperty has no spec");

const GuiSpec& spec(GuiProperty property) { return kSpecs[static_cast<int>(property)]; }

// The Enum an EnumItem property holds.
const EnumType& enum_of(GuiProperty property) {
    return property == GuiProperty::AssetType ? asset_type_enum() : gui_alignment_enum();
}

// Enum.AssetType's classes, by value.
constexpr const char* kAssetClasses[] = {"Material", "Prefab", "Texture", "Mesh", "Sound", "Model"};

constexpr const char* kDefaultCss = "/* CSS Document */";

LuaSlot string_slot(std::string text) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::String;
    slot.text = std::move(text);
    return slot;
}

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

LuaSlot color_slot(float r, float g, float b) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Color;
    slot.color = ColorRgb{r, g, b, 1.f};
    return slot;
}

LuaSlot vec2_slot(float x, float y) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec2;
    slot.vec = Vec3{x, y, 0.f};
    return slot;
}

LuaSlot enum_slot(const EnumType& type, int value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &type;
    slot.number = value;
    return slot;
}

bool finite(double value) { return std::isfinite(value); }

double clamp_to(const GuiSpec& property, double value) {
    return std::min(std::max(value, property.min), property.max);
}

}  // namespace

GuiValues::GuiValues(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

LuaSlot GuiValues::default_value(GuiProperty property, const char* class_name) {
    const std::string_view klass = class_name != nullptr ? class_name : "";
    switch (property) {
    case GuiProperty::ClassList:
    case GuiProperty::Style:
        return string_slot("");
    case GuiProperty::Size:
        return klass == "Pane" || klass == "ImagePane" ? vec2_slot(100.f, 100.f) : vec2_slot(0.f, 0.f);
    case GuiProperty::Alignment:
        return enum_slot(gui_alignment_enum(), 0);
    case GuiProperty::Visible:
        return bool_slot(true);
    case GuiProperty::MouseTransparent:
        return bool_slot(false);
    case GuiProperty::BackgroundColor:
        return color_slot(1.f, 1.f, 1.f);
    case GuiProperty::BackgroundTransparency:
    case GuiProperty::Spacing:
    case GuiProperty::ImageTransparency:
        return number_slot(0);
    case GuiProperty::Text:
        return string_slot(klass == "Label" ? "Label" : klass == "Button" ? "Button" : "");
    case GuiProperty::TextColor:
        return color_slot(0.f, 0.f, 0.f);
    case GuiProperty::FontSize:
        return number_slot(16);
    case GuiProperty::Prompt:
        return string_slot("Prompt");
    case GuiProperty::Source:
        return string_slot(kDefaultCss);
    case GuiProperty::Title:
        return string_slot("");
    case GuiProperty::Value:
    case GuiProperty::Min:
    case GuiProperty::Step:
        return number_slot(0);
    case GuiProperty::Max:
        return number_slot(1);
    case GuiProperty::AssetType:
        return enum_slot(asset_type_enum(), 0);
    case GuiProperty::AlwaysOnTop:
    case GuiProperty::TextScaled:
    case GuiProperty::WidgetEnabled:
        return bool_slot(false);
    case GuiProperty::Count:
        break;
    }
    return LuaSlot{};
}

void GuiValues::reset_values() {
    for (int index = 0; index < static_cast<int>(GuiProperty::Count); ++index) {
        values_[index] = default_value(static_cast<GuiProperty>(index), class_name());
    }
    ++revision_;
}

void GuiValues::on_reuse() { reset_values(); }

std::optional<std::string> GuiValues::set_value(GuiProperty property, LuaSlot value) {
    if (!on_gameplay_thread()) {
        contract_fail("Gui setters run on SimulationThread");
    }
    const GuiSpec& about = spec(property);
    const std::string name = about.name;
    // Only what the kind uses, so two equal values compare equal.
    LuaSlot clean;
    clean.kind = about.kind;
    switch (about.kind) {
    case LuaSlot::Kind::String:
        if (value.kind != LuaSlot::Kind::String) {
            return name + " must be a string";
        }
        clean.text = std::move(value.text);
        break;
    case LuaSlot::Kind::Bool:
        if (value.kind != LuaSlot::Kind::Bool) {
            return name + " must be true or false";
        }
        clean.flag = value.flag;
        break;
    case LuaSlot::Kind::Number:
        if (value.kind != LuaSlot::Kind::Number || !finite(value.number)) {
            return name + " must be a finite number";
        }
        clean.number = fit_number(property, clamp_to(about, value.number));
        break;
    case LuaSlot::Kind::Color:
        if (value.kind != LuaSlot::Kind::Color || !finite(value.color.r) || !finite(value.color.g) ||
            !finite(value.color.b)) {
            return name + " must be a finite Color3";
        }
        // A Color3 has no alpha.
        clean.color = ColorRgb{value.color.r, value.color.g, value.color.b, 1.f};
        break;
    case LuaSlot::Kind::Vec2:
        if (value.kind != LuaSlot::Kind::Vec2 || !finite(value.vec.x) || !finite(value.vec.y)) {
            return name + " must be a finite Vector2";
        }
        clean.vec = Vec3{static_cast<float>(clamp_to(about, value.vec.x)),
                         static_cast<float>(clamp_to(about, value.vec.y)), 0.f};
        break;
    case LuaSlot::Kind::Enum: {
        const EnumType& type = enum_of(property);
        if (value.kind != LuaSlot::Kind::Enum || value.enum_type != &type ||
            enum_item_name(type, static_cast<int>(value.number)) == nullptr) {
            return name + " must be an Enum." + type.name + " item";
        }
        clean.enum_type = &type;
        clean.number = value.number;
        break;
    }
    default:
        return name + " cannot be set";
    }
    LuaSlot& slot = values_[static_cast<int>(property)];
    if (same_slot(slot, clean)) {
        return std::nullopt;
    }
    const LuaSlot previous = slot;
    slot = std::move(clean);
    ++revision_;
    note_property_change(about.name, previous, slot);
    after_change(property);
    return std::nullopt;
}

std::optional<std::string> GuiValues::set_text(GuiProperty property, std::string text) {
    return set_value(property, string_slot(std::move(text)));
}

ScreenGui::ScreenGui(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBase(tag, state, id) {
    reset_values();
}

const char* ScreenGui::class_name() const { return "ScreenGui"; }

BillboardGui::BillboardGui(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBase(tag, state, id) {
    reset_values();
}

const char* BillboardGui::class_name() const { return "BillboardGui"; }

LuaSlot BillboardGui::adornee() const { return instance_reference_slot(adornee_ref_, "PVInstance"); }

InstanceId BillboardGui::adornee_id() const {
    // What adornee() resolves to, without building a slot and copying the GUID
    // into it: the pump calls this for every billboard each frame.
    const InstanceId target = adornee_ref_.resolve(*this);
    if (target == 0) {
        return 0;
    }
    const DataModel* object = instance(target);
    return object != nullptr && lua_class_inherits(object->class_name(), "PVInstance") ? target : 0;
}

std::optional<std::string> BillboardGui::set_adornee(const LuaSlot& value) {
    if (!on_gameplay_thread()) {
        contract_fail("Gui setters run on SimulationThread");
    }
    return set_instance_reference("Adornee", "PVInstance", adornee_ref_, value);
}

InstanceId BillboardGui::anchor_instance() const {
    if (const InstanceId linked = adornee_id(); linked != 0) {
        return linked;
    }
    const InstanceId above = parent(id());
    if (above == kNoParent || above == 0) {
        return 0;
    }
    return dynamic_cast<const PVInstance*>(instance(above)) != nullptr ? above : 0;
}

Vec3 BillboardGui::anchor() const { return anchor_of(anchor_instance()); }

Vec3 BillboardGui::anchor_of(InstanceId target) const {
    const auto* object = target != 0 ? dynamic_cast<const PVInstance*>(instance(target)) : nullptr;
    if (object == nullptr) {
        return Vec3{};
    }
    const Matrix4 transform = object->transform();
    return Vec3{transform.m[12], transform.m[13], transform.m[14]};
}

bool BillboardGui::drawn() const {
    if (!in_workspace(id()) && !in_core(id())) {
        return false;
    }
    for (InstanceId above = parent(id()); above != kNoParent && above != 0; above = parent(above)) {
        if (dynamic_cast<const GuiBase*>(instance(above)) != nullptr) {
            return false;
        }
    }
    return true;
}

void BillboardGui::on_reuse() {
    GuiValues::on_reuse();
    adornee_ref_.set_guid(std::string());
}

Pane::Pane(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBasePane(tag, state, id) {
    reset_values();
}

const char* Pane::class_name() const { return "Pane"; }

ImagePane::ImagePane(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBasePane(tag, state, id) {
    reset_values();
}

const char* ImagePane::class_name() const { return "ImagePane"; }

LuaSlot ImagePane::image() const { return instance_reference_slot(image_ref_, "Texture"); }

const Texture* ImagePane::image_texture() const {
    const InstanceId target = image_ref_.resolve(*this);
    return target != 0 ? dynamic_cast<const Texture*>(instance(target)) : nullptr;
}

std::optional<std::string> ImagePane::set_image(const LuaSlot& value) {
    if (!on_gameplay_thread()) {
        contract_fail("Gui setters run on SimulationThread");
    }
    const std::string before = image_ref_.guid();
    std::optional<std::string> error = set_instance_reference("Image", "Texture", image_ref_, value);
    if (image_ref_.guid() != before) {
        touch();
    }
    return error;
}

void ImagePane::on_reuse() {
    GuiValues::on_reuse();
    image_ref_.set_guid(std::string());
}

HBox::HBox(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBasePane(tag, state, id) {
    reset_values();
}

const char* HBox::class_name() const { return "HBox"; }

VBox::VBox(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBasePane(tag, state, id) {
    reset_values();
}

const char* VBox::class_name() const { return "VBox"; }

Label::Label(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBase(tag, state, id) {
    reset_values();
}

const char* Label::class_name() const { return "Label"; }

Button::Button(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBase(tag, state, id) {
    reset_values();
}

const char* Button::class_name() const { return "Button"; }

TextField::TextField(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBase(tag, state, id) {
    reset_values();
}

const char* TextField::class_name() const { return "TextField"; }

Slider::Slider(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBase(tag, state, id) {
    reset_values();
}

const char* Slider::class_name() const { return "Slider"; }

double Slider::fit_number(GuiProperty property, double value) const {
    if (property != GuiProperty::Value) {
        return value;
    }
    const double low = number(GuiProperty::Min);
    const double high = number(GuiProperty::Max);
    const double step = number(GuiProperty::Step);
    if (step > 0) {
        value = low + std::round((value - low) / step) * step;
    }
    return std::min(std::max(value, low), high);
}

void Slider::after_change(GuiProperty property) {
    LuaSlot bound = value(property);
    if (property == GuiProperty::Min && number(GuiProperty::Max) < bound.number) {
        set_value(GuiProperty::Max, bound);
    } else if (property == GuiProperty::Max && number(GuiProperty::Min) > bound.number) {
        set_value(GuiProperty::Min, bound);
    }
    if (property != GuiProperty::Value) {
        // Refit Value to the new range or Step.
        set_value(GuiProperty::Value, value(GuiProperty::Value));
    }
}

AssetPicker::AssetPicker(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBase(tag, state, id) {
    reset_values();
}

const char* AssetPicker::class_name() const { return "AssetPicker"; }

const char* AssetPicker::asset_class() const {
    const int item = static_cast<int>(number(GuiProperty::AssetType));
    return item >= 0 && item < static_cast<int>(std::size(kAssetClasses)) ? kAssetClasses[item] : kAssetClasses[0];
}

LuaSlot AssetPicker::asset() const { return instance_reference_slot(asset_ref_, asset_class()); }

InstanceId AssetPicker::asset_id() const {
    const InstanceId target = asset_ref_.resolve(*this);
    const DataModel* object = target != 0 ? instance(target) : nullptr;
    return object != nullptr && lua_class_inherits(object->class_name(), asset_class()) ? target : 0;
}

std::optional<std::string> AssetPicker::set_asset(const LuaSlot& value) {
    if (!on_gameplay_thread()) {
        contract_fail("Gui setters run on SimulationThread");
    }
    const std::string before = asset_ref_.guid();
    std::optional<std::string> error = set_instance_reference("Value", asset_class(), asset_ref_, value);
    if (asset_ref_.guid() != before) {
        touch();
    }
    return error;
}

void AssetPicker::after_change(GuiProperty property) {
    if (property != GuiProperty::AssetType) {
        return;
    }
    // A Value of the old class goes. One not loaded yet stays, to be read once it is.
    const InstanceId target = asset_ref_.resolve(*this);
    const DataModel* object = target != 0 ? instance(target) : nullptr;
    if (object != nullptr && !lua_class_inherits(object->class_name(), asset_class())) {
        set_asset(LuaSlot{});
    }
}

void AssetPicker::on_reuse() {
    GuiValues::on_reuse();
    asset_ref_.set_guid(std::string());
}

Css::Css(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiValues(tag, state, id) {
    reset_values();
}

const char* Css::class_name() const { return "CSS"; }

DockWidget::DockWidget(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBase(tag, state, id) {
    reset_values();
}

const char* DockWidget::class_name() const { return "DockWidget"; }

void DockWidget::set_origin(std::string plugin, std::string key) {
    plugin_ = std::move(plugin);
    key_ = std::move(key);
}

std::string DockWidget::pane_name() const { return "plugin:" + plugin_ + "/" + key_; }

void DockWidget::set_title(std::string title) { set_value(GuiProperty::Title, string_slot(std::move(title))); }

void DockWidget::set_enabled(bool enabled) { set_value(GuiProperty::WidgetEnabled, bool_slot(enabled)); }

void DockWidget::on_reuse() {
    GuiValues::on_reuse();
    plugin_.clear();
    key_.clear();
    initial_dock = DockSide::TopRight;
    width = 300;
    height = 400;
    min_width = 0;
    min_height = 0;
}

void Css::context_actions(std::vector<ContextAction>& out) const {
    out.push_back(ContextAction{InstanceAction::Edit, true});
    DataModel::context_actions(out);
}

namespace {

template <GuiProperty P>
bool read_gui(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* gui = dynamic_cast<const GuiValues*>(&object);
    if (gui == nullptr) {
        return false;
    }
    out = gui->value(P);
    return true;
}

template <GuiProperty P>
bool write_gui(DataModel&, DataModel& object, LuaSlot& in) {
    auto* gui = dynamic_cast<GuiValues*>(&object);
    if (gui == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = gui->set_value(P, in)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

// A registered default must outlive the registry, which keeps its pointer.
const char* kept(std::string text) {
    static std::deque<std::string> texts;
    texts.push_back(std::move(text));
    return texts.back().c_str();
}

// The saved field for P, with class_name's default as a file would hold it.
template <GuiProperty P>
LuaField gui_field(const char* class_name) {
    const GuiSpec& about = spec(P);
    JsonValue json;
    slot_to_json(GuiValues::default_value(P, class_name), about.type, json);
    const char* default_json = kept(write_json(json));
    if (about.kind == LuaSlot::Kind::Enum) {
        return lua_saved_enum(about.name, enum_of(P), read_gui<P>, write_gui<P>, default_json);
    }
    LuaField field = lua_saved_property(about.name, about.type, read_gui<P>, write_gui<P>, default_json);
    if constexpr (P == GuiProperty::BackgroundTransparency || P == GuiProperty::ImageTransparency) {
        field = lua_slider(field, 0.0, 1.0);
    }
    return field;
}

template <std::size_t N>
void add_class(const char* name, const char* base, const LuaField (&fields)[N]) {
    register_lua_class(name, base, fields, static_cast<int>(N));
}

bool read_adornee(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* board = dynamic_cast<const BillboardGui*>(&object);
    if (board == nullptr) {
        return false;
    }
    out = board->adornee();
    return true;
}

bool write_adornee(DataModel&, DataModel& object, LuaSlot& in) {
    auto* board = dynamic_cast<BillboardGui*>(&object);
    if (board == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = board->set_adornee(in)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

bool read_asset(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* picker = dynamic_cast<const AssetPicker*>(&object);
    if (picker == nullptr) {
        return false;
    }
    out = picker->asset();
    return true;
}

bool write_asset(DataModel&, DataModel& object, LuaSlot& in) {
    auto* picker = dynamic_cast<AssetPicker*>(&object);
    if (picker == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = picker->set_asset(in)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

bool read_image(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* pane = dynamic_cast<const ImagePane*>(&object);
    if (pane == nullptr) {
        return false;
    }
    out = pane->image();
    return true;
}

bool write_image(DataModel&, DataModel& object, LuaSlot& in) {
    auto* pane = dynamic_cast<ImagePane*>(&object);
    if (pane == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = pane->set_image(in)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

ANARCHY_LUA_REGISTER(register_gui_lua) {
    const LuaField base[] = {
        lua_group("Layout"),
        gui_field<GuiProperty::Size>("GuiBase"),
        gui_field<GuiProperty::Alignment>("GuiBase"),
        lua_group("Style"),
        gui_field<GuiProperty::ClassList>("GuiBase"),
        gui_field<GuiProperty::Style>("GuiBase"),
        lua_group("Behavior"),
        gui_field<GuiProperty::Visible>("GuiBase"),
        gui_field<GuiProperty::MouseTransparent>("GuiBase"),
        lua_event(kGuiMouseClicked),
        lua_event(kGuiMousePressed),
        lua_event(kGuiMouseReleased),
        lua_event(kGuiMouseEntered),
        lua_event(kGuiMouseExited),
    };
    add_class("GuiBase", "Instance", base);
    register_lua_class("ScreenGui", "GuiBase", nullptr, 0);

    const LuaField billboard[] = {
        lua_group("Behavior"),
        lua_saved_property("Adornee", "PVInstance?", read_adornee, write_adornee, "null"),
        gui_field<GuiProperty::AlwaysOnTop>("BillboardGui"),
    };
    add_class("BillboardGui", "GuiBase", billboard);

    const LuaField pane_base[] = {
        lua_group("Appearance"),
        gui_field<GuiProperty::BackgroundColor>("GuiBasePane"),
        gui_field<GuiProperty::BackgroundTransparency>("GuiBasePane"),
    };
    add_class("GuiBasePane", "GuiBase", pane_base);
    // A Pane starts 100 by 100, as the legacy one did.
    const LuaField pane[] = {lua_group("Layout"), gui_field<GuiProperty::Size>("Pane")};
    add_class("Pane", "GuiBasePane", pane);
    const LuaField image_pane[] = {
        lua_group("Layout"),
        gui_field<GuiProperty::Size>("ImagePane"),
        lua_group("Appearance"),
        lua_saved_property("Image", "Texture?", read_image, write_image, "null"),
        gui_field<GuiProperty::ImageTransparency>("ImagePane"),
    };
    add_class("ImagePane", "GuiBasePane", image_pane);
    const LuaField box[] = {lua_group("Layout"), gui_field<GuiProperty::Spacing>("HBox")};
    add_class("HBox", "GuiBasePane", box);
    add_class("VBox", "GuiBasePane", box);

    const LuaField label[] = {
        lua_group("Text"),
        gui_field<GuiProperty::Text>("Label"),
        gui_field<GuiProperty::TextColor>("Label"),
        gui_field<GuiProperty::FontSize>("Label"),
        gui_field<GuiProperty::TextScaled>("Label"),
    };
    add_class("Label", "GuiBase", label);
    const LuaField button[] = {
        lua_group("Text"),
        gui_field<GuiProperty::Text>("Button"),
        gui_field<GuiProperty::TextScaled>("Button"),
        lua_event(kGuiAction),
    };
    add_class("Button", "GuiBase", button);
    const LuaField text_field[] = {
        lua_group("Text"),
        gui_field<GuiProperty::Text>("TextField"),
        gui_field<GuiProperty::Prompt>("TextField"),
        lua_event(kGuiAction),
    };
    add_class("TextField", "GuiBase", text_field);
    const LuaField slider[] = {
        gui_field<GuiProperty::Value>("Slider"),
        gui_field<GuiProperty::Min>("Slider"),
        gui_field<GuiProperty::Max>("Slider"),
        gui_field<GuiProperty::Step>("Slider"),
    };
    add_class("Slider", "GuiBase", slider);
    const LuaField picker[] = {
        gui_field<GuiProperty::AssetType>("AssetPicker"),
        lua_saved_property("Value", "Instance?", read_asset, write_asset, "null"),
    };
    add_class("AssetPicker", "GuiBase", picker);

    const LuaField css[] = {gui_field<GuiProperty::Source>("CSS")};
    add_class("CSS", "Instance", css);

    // Made only by plugin:CreateDockWidget, never by Instance.new.
    const LuaField dock[] = {gui_field<GuiProperty::Title>("DockWidget"),
                             gui_field<GuiProperty::WidgetEnabled>("DockWidget")};
    add_class("DockWidget", "GuiBase", dock);

    // A ScreenGui goes only in Gui, a BillboardGui in Workspace or on a
    // PVInstance. Panes and controls go in either, or a pane; a control holds
    // no GUI of its own. CSS styles Gui or any GUI.
    register_suited_parents("ScreenGui", {"Gui"});
    register_suited_parents("BillboardGui", {"Workspace", "PVInstance"});
    register_suited_parents("GuiBasePane", {"ScreenGui", "BillboardGui", "GuiBasePane", "DockWidget"});
    for (const char* control : {"Label", "Button", "TextField", "Slider", "AssetPicker"}) {
        register_suited_parents(control, {"ScreenGui", "BillboardGui", "GuiBasePane", "DockWidget"});
    }
    register_suited_parents("CSS", {"Gui", "GuiBase"});
}

}  // namespace

}  // namespace engine_core
