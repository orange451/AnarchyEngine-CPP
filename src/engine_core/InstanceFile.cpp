#include "InstanceFile.hpp"

#include "Constraint.hpp"
#include "FileBytes.hpp"
#include "LuaApi.hpp"
#include "SourceText.hpp"
#include "PropertyReflection.hpp"

#include <algorithm>
#include <optional>
#include <system_error>
#include <utility>

namespace engine_core {
namespace {

InstanceId build_copy(DataModel& world, const CopiedNode& node, InstanceId parent, std::string* refused,
                      CopyMap* map) {
    const auto refuse = [refused](std::string reason) {
        if (refused != nullptr && refused->empty()) {
            *refused = std::move(reason);
        }
    };
    if (world.room_left() == 0) {
        refuse(InstanceCapacityError(world.instance_capacity()).what());
        return 0;
    }
    DataModel* made = lua_create_instance(world, node.class_name.c_str());
    if (made == nullptr) {
        refuse(node.class_name + " can't be copied.");
        return 0;
    }
    const InstanceId id = made->id();
    if (map != nullptr && !node.guid.empty()) {
        (*map)[node.guid] = id;
    }
    for (const JsonValue::Member& member : node.properties) {
        std::string error;
        if (!made->load_property(member.first, member.second, error) && error.empty()) {
            world.set_extra_property(id, member.first, member.second);
        }
    }
    if (node.has_source) {
        if (auto* lua = dynamic_cast<SourceText*>(made)) {
            lua->set_source(node.source);
            for (std::size_t i = 0; i < node.extra_sources.size(); ++i) {
                lua->set_source(static_cast<int>(i) + 1, node.extra_sources[i]);
            }
        }
    }
    if (!world.rename_error(id, node.name)) {
        world.set_name(id, node.name);
    }
    // kNoParent: a clone's root, left with no parent.
    if (parent != DataModel::kNoParent) {
        if (std::optional<std::string> error = world.parent_error(id, parent)) {
            refuse(std::move(*error));
            world.destroy_tree(id);
            return 0;
        }
        world.set_parent(id, parent);
    }
    for (const CopiedNode& child : node.children) {
        build_copy(world, child, id, refused, map);
    }
    return id;
}

JsonValue node_json(const CopiedNode& node) {
    JsonValue out = JsonValue::object();
    out.set("class", JsonValue::string(node.class_name));
    out.set("name", JsonValue::string(node.name));
    if (!node.guid.empty()) {
        out.set("guid", JsonValue::string(node.guid));
    }
    if (!node.properties.empty()) {
        JsonValue properties = JsonValue::object();
        for (const JsonValue::Member& member : node.properties) {
            properties.set(member.first, member.second);
        }
        out.set("properties", std::move(properties));
    }
    if (node.has_source) {
        out.set("source", JsonValue::string(node.source));
        if (std::any_of(node.extra_sources.begin(), node.extra_sources.end(),
                        [](const std::string& text) { return !text.empty(); })) {
            std::vector<JsonValue> extras;
            for (const std::string& text : node.extra_sources) {
                extras.push_back(JsonValue::string(text));
            }
            out.set("sources", JsonValue::array(std::move(extras)));
        }
    }
    if (!node.children.empty()) {
        std::vector<JsonValue> children;
        children.reserve(node.children.size());
        for (const CopiedNode& child : node.children) {
            children.push_back(node_json(child));
        }
        out.set("children", JsonValue::array(std::move(children)));
    }
    return out;
}

bool read_node(const JsonValue& json, const std::string& where, CopiedNode& out, std::string& error) {
    if (!json.is_object()) {
        error = where + " must be an object";
        return false;
    }
    const JsonValue* klass = json.find("class");
    if (klass == nullptr || !klass->is_string()) {
        error = where + ".class must be a string";
        return false;
    }
    if (!lua_creatable_known(klass->as_string().c_str())) {
        error = where + ".class: there is no class " + klass->as_string();
        return false;
    }
    const JsonValue* name = json.find("name");
    if (name == nullptr || !name->is_string()) {
        error = where + ".name must be a string";
        return false;
    }
    out.class_name = klass->as_string();
    out.name = name->as_string();
    if (const JsonValue* guid = json.find("guid")) {
        if (!guid->is_string()) {
            error = where + ".guid must be a string";
            return false;
        }
        out.guid = guid->as_string();
    }
    if (const JsonValue* properties = json.find("properties")) {
        if (!properties->is_object()) {
            error = where + ".properties must be an object";
            return false;
        }
        for (const JsonValue::Member& member : properties->members()) {
            bag_set(out.properties, member.first, member.second);
        }
    }
    if (const JsonValue* source = json.find("source")) {
        if (!source->is_string()) {
            error = where + ".source must be a string";
            return false;
        }
        out.has_source = true;
        out.source = source->as_string();
    }
    if (const JsonValue* sources = json.find("sources")) {
        if (!sources->is_array()) {
            error = where + ".sources must be an array";
            return false;
        }
        for (const JsonValue& text : sources->items()) {
            if (!text.is_string()) {
                error = where + ".sources must hold strings";
                return false;
            }
            out.extra_sources.push_back(text.as_string());
        }
    }
    if (const JsonValue* children = json.find("children")) {
        if (!children->is_array()) {
            error = where + ".children must be an array";
            return false;
        }
        for (std::size_t i = 0; i < children->items().size(); ++i) {
            CopiedNode child;
            if (!read_node(children->items()[i], where + ".children[" + std::to_string(i) + "]", child, error)) {
                return false;
            }
            out.children.push_back(std::move(child));
        }
    }
    return true;
}

}  // namespace

CopiedNode copy_tree(const DataModel& game, InstanceId id) {
    CopiedNode node;
    const DataModel* object = game.instance(id);
    if (object == nullptr) {
        return node;
    }
    node.guid = game.guid(id);
    node.class_name = object->class_name();
    node.name = game.name(id);
    object->save_properties(node.properties);
    for (const JsonValue::Member& member : game.extra_properties(id)) {
        if (bag_find(node.properties, member.first) == nullptr) {  // a key the class writes wins over a stale extra
            bag_set(node.properties, member.first, member.second);
        }
    }
    if (const auto* lua = dynamic_cast<const SourceText*>(object)) {
        node.has_source = true;
        node.source = lua->source();
        for (int stage = 1; stage < lua->stage_count(); ++stage) {
            node.extra_sources.push_back(lua->source(stage));
        }
    }
    for (InstanceId child = game.first_child(id); child != 0; child = game.next_sibling(child)) {
        node.children.push_back(copy_tree(game, child));
    }
    return node;
}

bool paste_copies(DataModel& world, const std::vector<CopiedNode>& roots, InstanceId parent,
                  std::vector<InstanceId>* made, std::string* refused, CopyMap* map) {
    if (parent == DataModel::kNoParent || (parent != 0 && !world.alive(parent))) {
        return false;
    }
    bool any = false;
    for (const CopiedNode& root : roots) {
        const InstanceId id = build_copy(world, root, parent, refused, map);
        if (id != 0) {
            any = true;
            if (made != nullptr) {
                made->push_back(id);
            }
        }
    }
    return any;
}

InstanceId clone_tree(DataModel& world, InstanceId id, std::string* refused) {
    if (!world.alive(id)) {
        return 0;
    }
    const CopiedNode root = copy_tree(world, id);
    CopyMap map;
    const InstanceId made = build_copy(world, root, DataModel::kNoParent, refused, &map);
    if (made != 0) {
        remap_references(world, map);
    }
    return made;
}

void remap_references(DataModel& world, const CopyMap& map) {
    for (const auto& [source, copy] : map) {
        DataModel* object = world.instance(copy);
        if (object == nullptr) {
            continue;
        }
        // A RigidConstraint's ends as copied, before any is remapped: whether
        // each is inside the copied set, and named at all.
        bool inside1 = false;
        bool inside2 = false;
        bool named1 = false;
        bool named2 = false;
        const auto* rigid = dynamic_cast<const RigidConstraint*>(object);
        if (rigid != nullptr) {
            inside1 = map.count(rigid->pv_instance1_ref().guid()) != 0;
            inside2 = map.count(rigid->pv_instance2_ref().guid()) != 0;
            named1 = !rigid->pv_instance1_ref().guid().empty();
            named2 = !rigid->pv_instance2_ref().guid().empty();
        }
        const std::shared_ptr<const std::vector<LuaField>> fields = lua_saved_field_list(object->class_name());
        for (const LuaField& field : *fields) {
            if (field.read == nullptr || field.write == nullptr || reference_class(field.type_name).empty()) {
                continue;
            }
            LuaSlot slot;
            if (!field.read(world, *object, slot) || slot.text.empty()) {
                continue;
            }
            const auto found = map.find(slot.text);
            if (found == map.end()) {
                continue;
            }
            LuaSlot next;
            next.kind = LuaSlot::Kind::Instance;
            next.id = found->second;
            field.write(world, *object, next);
        }
        // An Instance attribute naming a source in the copied set names its
        // copy, as a reference property does; one naming anything else stays.
        std::vector<std::pair<std::string, InstanceId>> moved;
        for (const auto& [name, slot] : object->attributes()) {
            if (slot.kind != LuaSlot::Kind::Instance) {
                continue;
            }
            const auto found = map.find(slot.text);
            if (found != map.end()) {
                moved.emplace_back(name, found->second);
            }
        }
        for (const auto& [name, target] : moved) {
            LuaSlot next;
            next.kind = LuaSlot::Kind::Instance;
            next.id = target;
            object->set_attribute(name, next);
        }
        // Copied with one end and not the other: the end left outside is
        // cleared, or the copy would snap the original (or be snapped onto it).
        if (inside1 && named2 && !inside2) {
            dynamic_cast<RigidConstraint*>(object)->set_pv_instance2(LuaSlot{});
        } else if (inside2 && named1 && !inside1) {
            dynamic_cast<RigidConstraint*>(object)->set_pv_instance1(LuaSlot{});
        }
    }
}

JsonValue write_instance_file(const std::vector<CopiedNode>& roots) {
    JsonValue out = JsonValue::object();
    out.set("format", JsonValue::string("aeinst"));
    out.set("version", JsonValue::number(1));
    std::vector<JsonValue> items;
    items.reserve(roots.size());
    for (const CopiedNode& root : roots) {
        items.push_back(node_json(root));
    }
    out.set("roots", JsonValue::array(std::move(items)));
    return out;
}

bool read_instance_file(const JsonValue& json, std::vector<CopiedNode>& roots, std::string& error) {
    roots.clear();
    const JsonValue* format = json.find("format");
    if (format == nullptr || !format->is_string() || format->as_string() != "aeinst") {
        error = "format must be \"aeinst\"";
        return false;
    }
    const JsonValue* version = json.find("version");
    if (version == nullptr || !version->is_number() || version->as_number() != 1) {
        error = "version must be 1";
        return false;
    }
    const JsonValue* items = json.find("roots");
    if (items == nullptr || !items->is_array()) {
        error = "roots must be an array";
        return false;
    }
    std::vector<CopiedNode> out;
    for (std::size_t i = 0; i < items->items().size(); ++i) {
        CopiedNode node;
        if (!read_node(items->items()[i], "roots[" + std::to_string(i) + "]", node, error)) {
            return false;
        }
        out.push_back(std::move(node));
    }
    roots = std::move(out);
    return true;
}

bool save_instance_file(const std::filesystem::path& path, const std::vector<CopiedNode>& roots, std::string& error) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    if (!write_file(temp, write_json(write_instance_file(roots)), error)) {
        return false;
    }
    std::error_code code;
    std::filesystem::rename(temp, path, code);
    if (code) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        error = "could not replace " + path.u8string() + ": " + code.message();
        return false;
    }
    return true;
}

bool load_instance_file(const std::filesystem::path& path, std::vector<CopiedNode>& roots, std::string& error) {
    roots.clear();
    std::string text;
    if (!read_file(path, text, error)) {
        return false;
    }
    JsonValue json;
    if (!parse_json(text, json, error)) {
        return false;
    }
    return read_instance_file(json, roots, error);
}

}  // namespace engine_core
