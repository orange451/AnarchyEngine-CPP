#pragma once

#include "DataModel.hpp"
#include "InstanceRef.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace engine_core {

// Holds one value for scripts to share, under any instance. Abstract: the Lua
// class ValueBase cannot be made; the five below can. Each has one saved
// property, Value, whose type is the class's own:
//
// IntValue     number      0. A whole number: a fraction is rounded to the
//                          nearest, and one past kMaxInt either way is
//                          clamped. Not finite is refused.
// NumberValue  number      0. Not finite is refused.
// StringValue  string      "".
// BoolValue    boolean     false.
// ObjectValue  Instance?   nil. Saved, by GUID. Names a dead instance as nil
//                          and keeps its GUID, so an undo that brings it back
//                          heals it, as every reference does.
//
// Value is a saved registry property (lua_saved_property), so DataModel saves,
// loads, undoes, and restores it at Stop, and a change fires Changed.
// Each class_name is defined in ValueBase.cpp so that file, and its Lua
// registration, stays linked: nothing else there need be called.
class ValueBase : public DataModel {
public:
    ValueBase(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
};

class IntValue final : public ValueBase {
public:
    // The largest whole number a Luau number holds exactly: 2^53.
    static constexpr std::int64_t kMaxInt = 9007199254740992;

    IntValue(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : ValueBase(tag, state, id) {}
    const char* class_name() const override;

    std::int64_t value() const { return value_; }
    // SimulationThread. Returns why it refused the value, changing nothing.
    std::optional<std::string> set_value(double value);

protected:
    void on_reuse() override { value_ = 0; }

private:
    std::int64_t value_ = 0;
};

class NumberValue final : public ValueBase {
public:
    NumberValue(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : ValueBase(tag, state, id) {}
    const char* class_name() const override;

    double value() const { return value_; }
    // SimulationThread. Returns why it refused the value, changing nothing.
    std::optional<std::string> set_value(double value);

protected:
    void on_reuse() override { value_ = 0; }

private:
    double value_ = 0;
};

class StringValue final : public ValueBase {
public:
    StringValue(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : ValueBase(tag, state, id) {}
    const char* class_name() const override;

    const std::string& value() const { return value_; }
    // SimulationThread.
    void set_value(std::string value);

protected:
    void on_reuse() override { value_.clear(); }

private:
    std::string value_;
};

class BoolValue final : public ValueBase {
public:
    BoolValue(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : ValueBase(tag, state, id) {}
    const char* class_name() const override;

    bool value() const { return value_; }
    // SimulationThread.
    void set_value(bool value);

protected:
    void on_reuse() override { value_ = false; }

private:
    bool value_ = false;
};

class ObjectValue final : public ValueBase {
public:
    ObjectValue(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : ValueBase(tag, state, id) {}
    const char* class_name() const override;

    // The instance it names, or nil.
    LuaSlot value() const;
    // The live instance it names, or 0.
    InstanceId value_id() const { return ref_.resolve(*this); }
    const InstanceRef& value_ref() const { return ref_; }
    // SimulationThread. Returns why it refused the value, changing nothing.
    std::optional<std::string> set_value(const LuaSlot& value);

protected:
    void on_reuse() override { ref_.set_guid(std::string()); }

private:
    InstanceRef ref_;
};

}  // namespace engine_core
