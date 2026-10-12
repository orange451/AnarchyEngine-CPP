#include "ValueBase.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace engine_core {
namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot string_slot(std::string value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::String;
    slot.text = std::move(value);
    return slot;
}

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("ValueBase setters run on SimulationThread");
    }
}

}  // namespace

std::optional<std::string> IntValue::set_value(double value) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string("Value must be a finite number");
    }
    const double max = static_cast<double>(kMaxInt);
    const auto whole = static_cast<std::int64_t>(std::clamp(std::round(value), -max, max));
    if (whole == value_) {
        return std::nullopt;
    }
    const std::int64_t previous = value_;
    value_ = whole;
    note_property_change("Value", number_slot(static_cast<double>(previous)), number_slot(static_cast<double>(whole)));
    return std::nullopt;
}

std::optional<std::string> NumberValue::set_value(double value) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string("Value must be a finite number");
    }
    if (value == value_) {
        return std::nullopt;
    }
    const double previous = value_;
    value_ = value;
    note_property_change("Value", number_slot(previous), number_slot(value));
    return std::nullopt;
}

void StringValue::set_value(std::string value) {
    require_thread(*this);
    if (value == value_) {
        return;
    }
    LuaSlot previous = string_slot(std::move(value_));
    value_ = std::move(value);
    note_property_change("Value", previous, string_slot(value_));
}

void BoolValue::set_value(bool value) {
    require_thread(*this);
    if (value == value_) {
        return;
    }
    value_ = value;
    note_property_change("Value", bool_slot(!value), bool_slot(value));
}

LuaSlot ObjectValue::value() const { return instance_reference_slot(ref_, "Instance"); }

std::optional<std::string> ObjectValue::set_value(const LuaSlot& value) {
    require_thread(*this);
    return set_instance_reference("Value", "Instance", ref_, value);
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

template <class T>
bool read_value(DataModel&, DataModel& object, LuaSlot& out);

template <>
bool read_value<IntValue>(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* holder = dynamic_cast<const IntValue*>(&object);
    if (holder == nullptr) {
        return false;
    }
    out = number_slot(static_cast<double>(holder->value()));
    return true;
}

template <>
bool read_value<NumberValue>(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* holder = dynamic_cast<const NumberValue*>(&object);
    if (holder == nullptr) {
        return false;
    }
    out = number_slot(holder->value());
    return true;
}

template <>
bool read_value<StringValue>(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* holder = dynamic_cast<const StringValue*>(&object);
    if (holder == nullptr) {
        return false;
    }
    out = string_slot(holder->value());
    return true;
}

template <>
bool read_value<BoolValue>(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* holder = dynamic_cast<const BoolValue*>(&object);
    if (holder == nullptr) {
        return false;
    }
    out = bool_slot(holder->value());
    return true;
}

template <>
bool read_value<ObjectValue>(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* holder = dynamic_cast<const ObjectValue*>(&object);
    if (holder == nullptr) {
        return false;
    }
    out = holder->value();
    return true;
}

bool write_int(DataModel&, DataModel& object, LuaSlot& in) {
    auto* holder = dynamic_cast<IntValue*>(&object);
    return holder != nullptr && refuse(in, holder->set_value(in.number));
}

bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    auto* holder = dynamic_cast<NumberValue*>(&object);
    return holder != nullptr && refuse(in, holder->set_value(in.number));
}

bool write_string(DataModel&, DataModel& object, LuaSlot& in) {
    auto* holder = dynamic_cast<StringValue*>(&object);
    if (holder == nullptr) {
        return false;
    }
    holder->set_value(in.text);
    return true;
}

bool write_bool(DataModel&, DataModel& object, LuaSlot& in) {
    auto* holder = dynamic_cast<BoolValue*>(&object);
    if (holder == nullptr) {
        return false;
    }
    holder->set_value(in.flag);
    return true;
}

bool write_object(DataModel&, DataModel& object, LuaSlot& in) {
    auto* holder = dynamic_cast<ObjectValue*>(&object);
    return holder != nullptr && refuse(in, holder->set_value(in));
}

ANARCHY_LUA_REGISTER(register_value_base_lua) {
    // Abstract: no factory registers it.
    register_lua_class("ValueBase", "Instance", nullptr, 0);
    const LuaField int_fields[] = {
        lua_saved_property("Value", "number", read_value<IntValue>, write_int, "0"),
    };
    register_lua_class("IntValue", "ValueBase", int_fields, static_cast<int>(std::size(int_fields)));
    const LuaField number_fields[] = {
        lua_saved_property("Value", "number", read_value<NumberValue>, write_number, "0"),
    };
    register_lua_class("NumberValue", "ValueBase", number_fields, static_cast<int>(std::size(number_fields)));
    const LuaField string_fields[] = {
        lua_saved_property("Value", "string", read_value<StringValue>, write_string, "\"\""),
    };
    register_lua_class("StringValue", "ValueBase", string_fields, static_cast<int>(std::size(string_fields)));
    const LuaField bool_fields[] = {
        lua_saved_property("Value", "boolean", read_value<BoolValue>, write_bool, "false"),
    };
    register_lua_class("BoolValue", "ValueBase", bool_fields, static_cast<int>(std::size(bool_fields)));
    const LuaField object_fields[] = {
        lua_saved_property("Value", "Instance?", read_value<ObjectValue>, write_object, "null"),
    };
    register_lua_class("ObjectValue", "ValueBase", object_fields, static_cast<int>(std::size(object_fields)));
}

}  // namespace

}  // namespace engine_core
