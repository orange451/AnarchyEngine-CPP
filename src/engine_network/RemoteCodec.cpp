#include "RemoteCodec.hpp"

#include "JsonWire.hpp"
#include "TreeCodec.hpp"

#include "Enum.hpp"
#include "PropertyReflection.hpp"

#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace engine_network {

namespace {

using engine_core::LuaSlot;
using engine_core::RemoteTable;
using engine_core::RemoteValue;

enum Tag : std::uint8_t { Nil = 0, False = 1, True = 2, Float32 = 3, Float64 = 4, String = 5, Instance = 6, Engine = 7,
                          Table = 8 };

// An engine value's kind on the wire. Fixed here, so LuaSlot::Kind may grow.
enum EngineKind : std::uint8_t { Vec3 = 1, Vec2 = 2, Color = 3, Matrix4 = 4, Enum = 5, NumberCurve = 6, ColorCurve = 7 };

// The type name slot_to_json and slot_from_json know an engine kind by. Null
// for a kind that is not an engine value.
const char* engine_type(EngineKind kind) {
    switch (kind) {
    case Vec3: return "Vector3";
    case Vec2: return "Vector2";
    case Color: return "Color3";
    case Matrix4: return "Matrix4";
    case NumberCurve: return "NumberCurve";
    case ColorCurve: return "ColorCurve";
    case Enum: return "";
    }
    return nullptr;
}

bool engine_kind_of(LuaSlot::Kind kind, EngineKind& out) {
    switch (kind) {
    case LuaSlot::Kind::Vec3: out = Vec3; return true;
    case LuaSlot::Kind::Vec2: out = Vec2; return true;
    case LuaSlot::Kind::Color: out = Color; return true;
    case LuaSlot::Kind::Matrix4: out = Matrix4; return true;
    case LuaSlot::Kind::Enum: out = Enum; return true;
    case LuaSlot::Kind::NumberCurve: out = NumberCurve; return true;
    case LuaSlot::Kind::ColorCurve: out = ColorCurve; return true;
    default: return false;
    }
}

const engine_core::EnumType* enum_type_named(const std::string& name) {
    for (int index = 0; index < engine_core::enum_type_count(); ++index) {
        const engine_core::EnumType& type = engine_core::enum_type_at(index);
        if (name == type.name) {
            return &type;
        }
    }
    return nullptr;
}

void write_value(ByteWriter& out, const RemoteValue& value, const IdToNet& to_net) {
    if (value.table != nullptr) {
        out.u8(Table);
        out.varuint(value.table->array.size());
        for (const RemoteValue& element : value.table->array) {
            write_value(out, element, to_net);
        }
        out.varuint(value.table->fields.size());
        for (const auto& [key, field] : value.table->fields) {
            out.string(key);
            write_value(out, field, to_net);
        }
        return;
    }
    const LuaSlot& slot = value.slot;
    switch (slot.kind) {
    case LuaSlot::Kind::Bool:
        out.u8(slot.flag ? True : False);
        return;
    case LuaSlot::Kind::Number:
        if (float_exact(slot.number)) {
            out.u8(Float32);
            out.f32(static_cast<float>(slot.number));
        } else {
            out.u8(Float64);
            out.f64(slot.number);
        }
        return;
    case LuaSlot::Kind::String:
        out.u8(String);
        out.string(slot.text);
        return;
    case LuaSlot::Kind::Instance: {
        const std::uint32_t netid = slot.id != 0 && to_net ? to_net(slot.id) : 0;
        if (netid == 0) {
            out.u8(Nil);
        } else {
            out.u8(Instance);
            out.varuint(netid);
        }
        return;
    }
    default:
        break;
    }
    EngineKind kind{};
    engine_core::JsonValue json;
    if (engine_kind_of(slot.kind, kind) &&
        (kind != Enum || slot.enum_type != nullptr) &&
        engine_core::slot_to_json(slot, engine_type(kind), json)) {
        out.u8(Engine);
        out.u8(kind);
        if (kind == Enum) {
            out.string(slot.enum_type->name);
        }
        write_json(out, json);
        return;
    }
    // Nil, and what never travels (a Signal, an input record): nothing sends one.
    out.u8(Nil);
}

struct Decoder {
    ByteReader& in;
    const NetToId& to_id;

    // depth: how many tables hold this value. A table argument is level 1.
    bool read(RemoteValue& out, std::size_t depth) {
        LuaSlot& slot = out.slot;
        switch (in.u8()) {
        case Nil:
            break;
        case False:
            slot.kind = LuaSlot::Kind::Bool;
            slot.flag = false;
            break;
        case True:
            slot.kind = LuaSlot::Kind::Bool;
            slot.flag = true;
            break;
        case Float32:
            slot.kind = LuaSlot::Kind::Number;
            slot.number = static_cast<double>(in.f32());
            break;
        case Float64:
            slot.kind = LuaSlot::Kind::Number;
            slot.number = in.f64();
            break;
        case String:
            slot.kind = LuaSlot::Kind::String;
            slot.text = in.string();
            break;
        case Instance: {
            const std::uint64_t netid = in.varuint();
            if (in.failed() || netid == 0 || netid > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            const engine_core::InstanceId id = to_id ? to_id(static_cast<std::uint32_t>(netid)) : 0;
            // One the receiver lacks reads nil.
            if (id != 0) {
                slot.kind = LuaSlot::Kind::Instance;
                slot.id = id;
            }
            break;
        }
        case Engine:
            return read_engine(slot);
        case Table:
            return read_table(out, depth);
        default:
            return false;
        }
        return !in.failed();
    }

    bool read_engine(LuaSlot& slot) {
        const std::uint8_t kind = in.u8();
        if (in.failed() || kind < Vec3 || kind > ColorCurve) {
            return false;
        }
        const engine_core::EnumType* enum_type = nullptr;
        if (kind == Enum) {
            const std::string name = in.string();
            enum_type = in.failed() ? nullptr : enum_type_named(name);
            if (enum_type == nullptr) {
                return false;
            }
        }
        engine_core::JsonValue json;
        if (!read_json(in, json)) {
            return false;
        }
        std::string error;
        return engine_core::slot_from_json(json, engine_type(static_cast<EngineKind>(kind)), "value", slot, error,
                                           enum_type);
    }

    bool read_table(RemoteValue& out, std::size_t depth) {
        if (depth >= engine_core::kRemoteMaxDepth) {
            return false;
        }
        auto table = std::make_shared<RemoteTable>();
        // Each value takes at least a byte, so a count past what is left is a lie.
        const std::uint64_t elements = in.varuint();
        if (in.failed() || elements > in.left()) {
            return false;
        }
        table->array.resize(static_cast<std::size_t>(elements));
        for (RemoteValue& element : table->array) {
            if (!read(element, depth + 1)) {
                return false;
            }
        }
        // A field takes at least two: its key's length and its value's tag.
        const std::uint64_t fields = in.varuint();
        if (in.failed() || fields > in.left() / 2) {
            return false;
        }
        table->fields.reserve(static_cast<std::size_t>(fields));
        for (std::uint64_t index = 0; index < fields; ++index) {
            std::string key = in.string();
            if (in.failed()) {
                return false;
            }
            table->fields.emplace_back(std::move(key), RemoteValue{});
            if (!read(table->fields.back().second, depth + 1)) {
                return false;
            }
        }
        out.table = std::move(table);
        return true;
    }
};

}  // namespace

void encode_remote(std::uint32_t remote_netid, const engine_core::RemoteValues& values, const IdToNet& to_net,
                   ByteWriter& out) {
    out.u8(static_cast<std::uint8_t>(MessageType::Remote));
    out.u8(kCodecVersion);
    out.varuint(remote_netid);
    out.varuint(values.values.size());
    for (const RemoteValue& value : values.values) {
        write_value(out, value, to_net);
    }
}

bool decode_remote(ByteReader& in, std::uint32_t& remote_netid, engine_core::RemoteValues& out, const NetToId& to_id,
                   std::size_t max_bytes) {
    out.values.clear();
    if (in.left() > max_bytes) {
        return false;
    }
    if (in.u8() != static_cast<std::uint8_t>(MessageType::Remote) || in.u8() != kCodecVersion) {
        return false;
    }
    const std::uint64_t netid = in.varuint();
    if (in.failed() || netid > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    remote_netid = static_cast<std::uint32_t>(netid);
    const std::uint64_t count = in.varuint();
    if (in.failed() || count > in.left()) {
        return false;
    }
    out.values.resize(static_cast<std::size_t>(count));
    Decoder decoder{in, to_id};
    for (RemoteValue& value : out.values) {
        if (!decoder.read(value, 0)) {
            out.values.clear();
            return false;
        }
    }
    return in.done();
}

}  // namespace engine_network
