#include "ParticleBursts.hpp"

#include "CurveJson.hpp"
#include "CurveLua.hpp"

#include "lualib.h"

#include <cmath>
#include <memory>

namespace engine_core {

JsonValue bursts_to_json(const std::vector<ParticleBurst>& bursts) {
    std::vector<JsonValue> items;
    items.reserve(bursts.size());
    for (const ParticleBurst& burst : bursts) {
        JsonValue row = JsonValue::object();
        row.set("time", JsonValue::number_from_float(burst.time));
        row.set("count", number_curve_to_json(burst.count));
        items.push_back(std::move(row));
    }
    return JsonValue::array(std::move(items));
}

std::optional<std::vector<ParticleBurst>> bursts_from_json(const JsonValue& value, std::string& error) {
    if (!value.is_array()) {
        error = "must be a list of bursts";
        return std::nullopt;
    }
    std::vector<ParticleBurst> out;
    for (const JsonValue& row : value.items()) {
        const JsonValue* time = row.find("time");
        const JsonValue* count = row.find("count");
        if (time == nullptr || !time->is_number() || count == nullptr) {
            error = "has a burst without a time and a count";
            return std::nullopt;
        }
        std::string why;
        auto curve = number_curve_from_json(*count, why);
        if (!curve) {
            error = "has a burst whose count " + why;
            return std::nullopt;
        }
        out.push_back(ParticleBurst{static_cast<float>(time->as_number()), std::move(*curve)});
    }
    if (std::optional<std::string> refused = check_bursts(out)) {
        error = *refused;
        return std::nullopt;
    }
    return out;
}

std::optional<std::string> check_bursts(const std::vector<ParticleBurst>& bursts) {
    for (const ParticleBurst& burst : bursts) {
        if (!std::isfinite(burst.time) || burst.time < 0.f) {
            return std::string("a burst's Time must be 0 or more");
        }
        const NumberCurve& c = burst.count;
        const bool shaped = c.mode() == NumberCurveMode::Constant || c.mode() == NumberCurveMode::Range;
        if (!shaped || !std::isfinite(c.min()) || !std::isfinite(c.max()) || c.min() < 0.f || c.max() < 0.f) {
            return std::string("a burst's Count must be a Constant or a Range, 0 or more");
        }
    }
    return std::nullopt;
}

LuaSlot bursts_slot(const std::vector<ParticleBurst>& bursts) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::String;
    slot.text = compact_json(bursts_to_json(bursts));
    return slot;
}

std::optional<std::vector<ParticleBurst>> bursts_from_slot(const LuaSlot& slot, std::string& error) {
    if (slot.kind != LuaSlot::Kind::String) {
        error = "must be a list of bursts";
        return std::nullopt;
    }
    JsonValue value;
    std::string why;
    if (!parse_json(slot.text.empty() ? "[]" : slot.text, value, why)) {
        error = "must be a list of bursts";
        return std::nullopt;
    }
    return bursts_from_json(value, error);
}

void push_bursts(lua_State* state, const std::string& json_text) {
    std::string error;
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::String;
    slot.text = json_text;
    const auto bursts = bursts_from_slot(slot, error);
    const int count = bursts ? static_cast<int>(bursts->size()) : 0;
    lua_createtable(state, count, 0);
    for (int i = 0; i < count; ++i) {
        const ParticleBurst& burst = (*bursts)[static_cast<std::size_t>(i)];
        lua_createtable(state, 0, 2);
        lua_pushnumber(state, burst.time);
        lua_setfield(state, -2, "Time");
        push_number_curve(state, std::make_shared<const NumberCurve>(burst.count));
        lua_setfield(state, -2, "Count");
        lua_rawseti(state, -2, i + 1);
    }
}

bool read_bursts(lua_State* state, int index, std::string& json_text, std::string& error) {
    if (!lua_istable(state, index)) {
        error = "Bursts expects a list of {Time = number, Count = number or NumberCurve}";
        return false;
    }
    std::vector<ParticleBurst> bursts;
    const int count = lua_objlen(state, index);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, index, i);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            error = "Bursts expects a list of {Time = number, Count = number or NumberCurve}";
            return false;
        }
        lua_getfield(state, -1, "Time");
        lua_getfield(state, -2, "Count");
        ParticleBurst burst;
        bool ok = lua_isnumber(state, -2) != 0;
        burst.time = static_cast<float>(lua_tonumber(state, -2));
        if (lua_isnumber(state, -1)) {
            burst.count = NumberCurve::constant(static_cast<float>(lua_tonumber(state, -1)));
        } else if (auto curve = to_number_curve(state, -1)) {
            burst.count = *curve;
        } else {
            ok = false;
        }
        lua_pop(state, 3);
        if (!ok) {
            error = "Bursts expects a list of {Time = number, Count = number or NumberCurve}";
            return false;
        }
        bursts.push_back(std::move(burst));
    }
    if (std::optional<std::string> refused = check_bursts(bursts)) {
        error = "Bursts: " + *refused;
        return false;
    }
    json_text = compact_json(bursts_to_json(bursts));
    return true;
}

}  // namespace engine_core
