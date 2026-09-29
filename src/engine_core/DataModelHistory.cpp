// DataModel: recording edits for ChangeHistoryService, and applying its undo and
// redo back to the world.

#include "DataModel.hpp"

#include "DataModelState.hpp"

namespace engine_core {

namespace {

PropertyValue value_transform(const Transform& value) {
    PropertyValue out;
    out.prop = HistoryProp::Transform;
    out.transform = value;
    return out;
}

PropertyValue value_color(ColorRgb value) {
    PropertyValue out;
    out.prop = HistoryProp::Color;
    out.color = value;
    return out;
}

PropertyValue value_size(float x, float y, float z) {
    PropertyValue out;
    out.prop = HistoryProp::Size;
    out.size[0] = x;
    out.size[1] = y;
    out.size[2] = z;
    return out;
}

PropertyValue value_flag(HistoryProp prop, bool value) {
    PropertyValue out;
    out.prop = prop;
    out.flag = value;
    return out;
}

PropertyValue value_text(HistoryProp prop, std::string value) {
    PropertyValue out;
    out.prop = prop;
    out.text = std::move(value);
    return out;
}

PropertyValue value_position(const Vec3& value) {
    PropertyValue out;
    out.prop = HistoryProp::Position;
    out.vector = value;
    return out;
}

HistoryProp history_prop(Field field) {
    switch (field) {
    case Field::Transform:
        return HistoryProp::Transform;
    case Field::Color:
        return HistoryProp::Color;
    case Field::Size:
        return HistoryProp::Size;
    case Field::Simulated:
        return HistoryProp::Simulated;
    case Field::VisualOnly:
        return HistoryProp::VisualOnly;
    case Field::Name:
        return HistoryProp::Name;
    case Field::Source:
        return HistoryProp::Source;
    case Field::Enabled:
        return HistoryProp::Enabled;
    case Field::Position:
        return HistoryProp::Position;
    case Field::LinearVelocity:
    case Field::Parent:
    case Field::Count:
        break;
    }
    return HistoryProp::Name;
}

void note_property(ChangeHistoryService* history, InstanceId id, PropertyValue before, PropertyValue after) {
    if (history == nullptr) {
        return;
    }
    Mutation mutation;
    mutation.kind = MutationKind::SetProperty;
    mutation.id = id;
    mutation.before = std::move(before);
    mutation.after = std::move(after);
    history->note(std::move(mutation));
}

}  // namespace

void DataModel::record_transform(InstanceId id, const Transform& before, const Transform& after) {
    mark_authored_dirty(id);
    note_property(state_->history.get(), id, value_transform(before), value_transform(after));
}

void DataModel::record_color(InstanceId id, ColorRgb before, ColorRgb after) {
    mark_authored_dirty(id);
    note_property(state_->history.get(), id, value_color(before), value_color(after));
}

void DataModel::record_size(InstanceId id, float bx, float by, float bz, float ax, float ay, float az) {
    mark_authored_dirty(id);
    note_property(state_->history.get(), id, value_size(bx, by, bz), value_size(ax, ay, az));
}

void DataModel::record_bool(InstanceId id, Field field, bool before, bool after) {
    if (field != Field::Simulated && field != Field::VisualOnly && field != Field::Enabled) {
        return;
    }
    mark_authored_dirty(id);
    const HistoryProp prop = history_prop(field);
    note_property(state_->history.get(), id, value_flag(prop, before), value_flag(prop, after));
}

void DataModel::record_string(InstanceId id, Field field, const std::string& before, const std::string& after) {
    if (field != Field::Name && field != Field::Source) {
        return;
    }
    mark_authored_dirty(id);
    const HistoryProp prop = history_prop(field);
    note_property(state_->history.get(), id, value_text(prop, before), value_text(prop, after));
}

void DataModel::record_position(InstanceId id, const Vec3& before, const Vec3& after) {
    mark_authored_dirty(id);
    note_property(state_->history.get(), id, value_position(before), value_position(after));
}

void DataModel::record_parent(InstanceId id, InstanceId old_parent, InstanceId new_parent, int old_index) {
    // The child's path moves. Each parent's child order, and whether it is a
    // folder or a leaf, may change.
    mark_authored_dirty(id);
    mark_authored_dirty(old_parent);
    mark_authored_dirty(new_parent);
    if (state_->history == nullptr) {
        return;
    }
    Mutation mutation;
    mutation.kind = MutationKind::SetParent;
    mutation.id = id;
    mutation.old_parent = old_parent;
    mutation.new_parent = new_parent;
    mutation.old_sibling_index = old_index;
    state_->history->note(std::move(mutation));
}

void DataModel::record_created(InstanceId id) {
    if (state_->history == nullptr || !state_->history->wants_mutation()) {
        return;
    }
    Mutation mutation;
    mutation.kind = MutationKind::CreateInstance;
    mutation.id = id;
    mutation.record = capture_record(id, false);
    state_->history->note(std::move(mutation));
}

void DataModel::record_destroyed(AuthoredRecord record) {
    if (state_->history == nullptr) {
        return;
    }
    Mutation mutation;
    mutation.kind = MutationKind::DestroyInstance;
    mutation.id = record.id;
    mutation.record = std::move(record);
    state_->history->note(std::move(mutation));
}

const void* DataModel::type_key_of(InstanceId id) const {
    const Slot* part = slot(id);
    if (part == nullptr || part->pool >= state_->pools.size() || state_->pools[part->pool] == nullptr) {
        return nullptr;
    }
    return state_->pools[part->pool]->key;
}

int DataModel::sibling_index_of(InstanceId id) const {
    const Slot* part = slot(id);
    if (part == nullptr || part->parent == kNoParent) {
        return -1;
    }
    int index = 0;
    for (InstanceId child = first_child(part->parent); child != 0; child = next_sibling(child)) {
        if (child == id) {
            return index;
        }
        ++index;
    }
    return -1;
}

void DataModel::take_free_index(std::uint32_t index) {
    std::vector<std::uint32_t>& free = state_->free_list;
    free.erase(std::remove(free.begin(), free.end(), index), free.end());
}

AuthoredRecord DataModel::capture_record(InstanceId id, bool subtree) const {
    AuthoredRecord record;
    const Slot* part = slot(id);
    if (part == nullptr || part->instance == nullptr) {
        return record;
    }
    const DataModel* object = part->instance;
    record.id = id;
    record.type_key = type_key_of(id);
    const char* class_name = object->class_name();
    record.class_name = class_name != nullptr ? class_name : "";
    record.name = object->name_;
    record.guid = object->guid_;
    record.extras = object->extras_;
    record.parent = part->parent;
    record.sibling_index = sibling_index_of(id);
    record.simulated = part->simulated;
    record.visual_only = part->visual_only;
    if (const GameObject* body = dynamic_cast<const GameObject*>(object)) {
        record.spatial = true;
        record.transform = body->transform();
        record.color = body->color();
        body->copy_size(record.size);
    }
    if (const LuaSource* source = dynamic_cast<const LuaSource*>(object)) {
        record.has_source = true;
        record.source = source->source();
    }
    if (const Script* script = dynamic_cast<const Script*>(object)) {
        record.enabled = script->enabled();
    }
    if (!record.spatial && !record.has_source) {
        object->write_place(record.extra);
    }
    if (subtree) {
        for (InstanceId child = part->first_child; child != 0; child = next_sibling(child)) {
            record.children.push_back(capture_record(child, true));
        }
    }
    return record;
}

void DataModel::apply_record_fields(const AuthoredRecord& record) {
    if (DataModel* object = instance(record.id)) {
        object->guid_ = record.guid.empty() ? make_guid() : record.guid;
        object->extras_ = record.extras;
    }
    mark_authored_dirty(record.id);
    set_name(record.id, record.name);
    set_simulated(record.id, record.simulated);
    set_visual_only(record.id, record.visual_only);
    if (record.spatial) {
        if (GameObject* body = game_object(record.id)) {
            body->set_transform(record.transform);
            body->set_color(record.color);
            body->set_size(record.size[0], record.size[1], record.size[2]);
        }
    }
    if (record.has_source) {
        if (auto* source = dynamic_cast<LuaSource*>(instance(record.id))) {
            source->set_source(record.source);
        }
        if (auto* script = dynamic_cast<Script*>(instance(record.id))) {
            script->set_enabled(record.enabled);
        }
    } else if (!record.spatial) {
        if (DataModel* object = instance(record.id)) {
            const std::byte* bytes = record.extra.empty() ? nullptr : record.extra.data();
            object->read_place(bytes, record.extra.size());
        }
    }
}

void DataModel::revive_record(const AuthoredRecord& record) {
    if (record.id == 0 || record.type_key == nullptr || alive(record.id)) {
        return;
    }
    const std::uint32_t index = id_slot(record.id);
    if (index >= state_->slots.size()) {
        contract_fail("history revive lost an instance");
    }
    if (state_->slots[index].alive) {
        contract_fail("history revive collided with a live instance");
    }
    const std::uint16_t pool = pool_index_for(record.type_key);
    if (pool == kNoPool) {
        contract_fail("history revive lost an instance type");
    }
    take_free_index(index);
    adopt_slot(pool, record.id);
    apply_record_fields(record);
    if (record.spatial) {
        note(record.id, VisualField::Transform | VisualField::Color | VisualField::Size, WriteOrigin::Simulation);
    }
    if (dynamic_cast<LuaSource*>(instance(record.id)) != nullptr) {
        if (ScriptAnalysis* analysis = script_analysis()) {
            analysis->invalidate(record.id);
        }
    }
}

void DataModel::revive_tree(const AuthoredRecord& record) {
    if (!alive(record.id)) {
        revive_record(record);
    }
    for (const AuthoredRecord& child : record.children) {
        revive_tree(child);
    }
}

void DataModel::place_at_sibling(InstanceId id, int index) {
    Slot* part = slot(id);
    if (part == nullptr || index < 0 || part->parent == kNoParent) {
        return;
    }
    const InstanceId parent = part->parent;
    std::vector<InstanceId> kids = child_ids(parent);
    const auto found = std::find(kids.begin(), kids.end(), id);
    if (found == kids.end()) {
        return;
    }
    const int current = static_cast<int>(found - kids.begin());
    if (current == index) {
        return;
    }
    kids.erase(found);
    if (index > static_cast<int>(kids.size())) {
        index = static_cast<int>(kids.size());
    }
    kids.insert(kids.begin() + index, id);
    mark_authored_dirty(parent);
    note_tree_changed();
    for (InstanceId child : kids) {
        if (Slot* child_slot = slot(child)) {
            unlink_parent(child, *child_slot);
        }
    }
    link_children(parent, kids);
}

void DataModel::reparent_record(const AuthoredRecord& record) {
    if (!alive(record.id)) {
        return;
    }
    if (parent(record.id) != record.parent) {
        set_parent(record.id, record.parent);
    }
    if (record.parent != kNoParent && record.sibling_index >= 0) {
        place_at_sibling(record.id, record.sibling_index);
    }
    std::vector<const AuthoredRecord*> ordered;
    ordered.reserve(record.children.size());
    for (const AuthoredRecord& child : record.children) {
        ordered.push_back(&child);
    }
    std::sort(ordered.begin(), ordered.end(), [](const AuthoredRecord* a, const AuthoredRecord* b) {
        return a->sibling_index < b->sibling_index;
    });
    for (const AuthoredRecord* child : ordered) {
        reparent_record(*child);
    }
}

void DataModel::apply_property(InstanceId id, const PropertyValue& value) {
    if (id != 0 && !alive(id)) {
        return;
    }
    switch (value.prop) {
    case HistoryProp::Transform:
        if (GameObject* body = game_object(id)) {
            body->set_transform(value.transform);
        }
        break;
    case HistoryProp::Color:
        if (GameObject* body = game_object(id)) {
            body->set_color(value.color);
        }
        break;
    case HistoryProp::Size:
        if (GameObject* body = game_object(id)) {
            body->set_size(value.size[0], value.size[1], value.size[2]);
        }
        break;
    case HistoryProp::Simulated:
        set_simulated(id, value.flag);
        break;
    case HistoryProp::VisualOnly:
        set_visual_only(id, value.flag);
        break;
    case HistoryProp::Name:
        set_name(id, value.text);
        break;
    case HistoryProp::Source:
        if (auto* source = dynamic_cast<LuaSource*>(instance(id))) {
            source->set_source(value.text);
        }
        break;
    case HistoryProp::Enabled:
        if (auto* script = dynamic_cast<Script*>(instance(id))) {
            script->set_enabled(value.flag);
        }
        break;
    case HistoryProp::Position:
        if (auto* triangle = dynamic_cast<TestTriangle*>(instance(id))) {
            triangle->set_position(value.vector.x, value.vector.y, value.vector.z);
        }
        break;
    }
}

void DataModel::apply_parent(InstanceId id, InstanceId parent_id, int sibling_index) {
    if (!alive(id)) {
        return;
    }
    if (parent(id) != parent_id) {
        set_parent(id, parent_id);
    }
    if (sibling_index >= 0 && parent_id != kNoParent) {
        place_at_sibling(id, sibling_index);
    }
}

void DataModel::apply_history(const Mutation& mutation, bool inverse) {
    switch (mutation.kind) {
    case MutationKind::SetProperty:
        apply_property(mutation.id, inverse ? mutation.before : mutation.after);
        break;
    case MutationKind::SetParent:
        if (inverse) {
            apply_parent(mutation.id, mutation.old_parent, mutation.old_sibling_index);
        } else {
            apply_parent(mutation.id, mutation.new_parent, -1);
        }
        break;
    case MutationKind::CreateInstance:
        if (inverse) {
            if (mutation.record.id != 0 && alive(mutation.record.id)) {
                destroy(mutation.record.id);
            }
        } else {
            revive_record(mutation.record);
        }
        break;
    case MutationKind::DestroyInstance:
        if (inverse) {
            revive_tree(mutation.record);
            reparent_record(mutation.record);
        } else if (mutation.record.id != 0 && alive(mutation.record.id)) {
            destroy(mutation.record.id);
        }
        break;
    }
}

}  // namespace engine_core
