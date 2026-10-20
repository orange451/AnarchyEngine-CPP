#include "RemoteCodec.hpp"

#include "JsonWire.hpp"
#include "TreeCodec.hpp"

#include "Curve.hpp"
#include "CurveJson.hpp"
#include "Enum.hpp"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine_network {

namespace {

using engine_core::LuaSlot;
using engine_core::RemoteTable;
using engine_core::RemoteValue;

enum Tag : std::uint8_t { Nil = 0, False = 1, True = 2, Float32 = 3, Float64 = 4, String = 5, Instance = 6, Engine = 7,
                          Table = 8, Root = 9 };

// An engine value's kind on the wire. Fixed here, so LuaSlot::Kind may grow.
enum EngineKind : std::uint8_t { Vec3 = 1, Vec2 = 2, Color = 3, Matrix4 = 4, Enum = 5, NumberCurve = 6, ColorCurve = 7 };

// Every Enum type by name, made once.
const engine_core::EnumType* enum_type_named(std::string_view name) {
    static const std::unordered_map<std::string_view, const engine_core::EnumType*> types = [] {
        std::unordered_map<std::string_view, const engine_core::EnumType*> out;
        for (int index = 0; index < engine_core::enum_type_count(); ++index) {
            const engine_core::EnumType& type = engine_core::enum_type_at(index);
            out.emplace(type.name, &type);
        }
        return out;
    }();
    const auto found = types.find(name);
    return found != types.end() ? found->second : nullptr;
}

void write_floats(ByteWriter& out, const float* values, int count) {
    for (int index = 0; index < count; ++index) {
        out.f32(values[index]);
    }
}

void write_number_keys(ByteWriter& out, const std::vector<engine_core::NumberKey>& keys) {
    out.varuint(keys.size());
    for (const engine_core::NumberKey& key : keys) {
        out.f32(key.time);
        out.f32(key.value);
        out.u8(static_cast<std::uint8_t>(key.interp));
    }
}

void write_color_key(ByteWriter& out, const engine_core::ColorKey& key) {
    out.f32(key.color.r);
    out.f32(key.color.g);
    out.f32(key.color.b);
    out.f32(key.transparency);
}

void write_color_keys(ByteWriter& out, const std::vector<engine_core::ColorKey>& keys) {
    out.varuint(keys.size());
    for (const engine_core::ColorKey& key : keys) {
        out.f32(key.time);
        write_color_key(out, key);
    }
}

// The layouts the header names. False, having written nothing, for a slot
// that is not an engine value or one that cannot travel: an Enum with no
// type or item, a curve with too many keys.
bool write_engine(ByteWriter& out, const LuaSlot& slot) {
    switch (slot.kind) {
    case LuaSlot::Kind::Vec3: {
        out.u8(Engine);
        out.u8(Vec3);
        const float axes[3] = {slot.vec.x, slot.vec.y, slot.vec.z};
        write_floats(out, axes, 3);
        return true;
    }
    case LuaSlot::Kind::Vec2: {
        out.u8(Engine);
        out.u8(Vec2);
        const float axes[2] = {slot.vec.x, slot.vec.y};
        write_floats(out, axes, 2);
        return true;
    }
    case LuaSlot::Kind::Color: {
        out.u8(Engine);
        out.u8(Color);
        const float channels[3] = {slot.color.r, slot.color.g, slot.color.b};
        write_floats(out, channels, 3);
        return true;
    }
    case LuaSlot::Kind::Matrix4:
        out.u8(Engine);
        out.u8(Matrix4);
        write_floats(out, slot.transform.m, 16);
        return true;
    case LuaSlot::Kind::Enum: {
        const int item = static_cast<int>(slot.number);
        if (slot.enum_type == nullptr || item < 0 || engine_core::enum_item_name(*slot.enum_type, item) == nullptr) {
            return false;
        }
        out.u8(Engine);
        out.u8(Enum);
        out.string(slot.enum_type->name);
        out.varuint(static_cast<std::uint64_t>(item));
        return true;
    }
    case LuaSlot::Kind::NumberCurve: {
        const engine_core::NumberCurve* curve = slot.number_curve.get();
        if (curve == nullptr || curve->keys_a().size() > engine_core::kRemoteMaxCurveKeys ||
            curve->keys_b().size() > engine_core::kRemoteMaxCurveKeys) {
            return false;
        }
        out.u8(Engine);
        out.u8(NumberCurve);
        out.u8(static_cast<std::uint8_t>(curve->mode()));
        switch (curve->mode()) {
        case engine_core::NumberCurveMode::Constant:
            out.f32(curve->min());
            break;
        case engine_core::NumberCurveMode::Range:
            out.f32(curve->min());
            out.f32(curve->max());
            break;
        case engine_core::NumberCurveMode::Curve:
            write_number_keys(out, curve->keys_a());
            break;
        case engine_core::NumberCurveMode::CurveRange:
            write_number_keys(out, curve->keys_a());
            write_number_keys(out, curve->keys_b());
            break;
        }
        return true;
    }
    case LuaSlot::Kind::ColorCurve: {
        const engine_core::ColorCurve* curve = slot.color_curve.get();
        if (curve == nullptr || curve->keys_a().size() > engine_core::kRemoteMaxCurveKeys ||
            curve->keys_b().size() > engine_core::kRemoteMaxCurveKeys) {
            return false;
        }
        const bool one_key = curve->mode() == engine_core::ColorCurveMode::Constant ||
                             curve->mode() == engine_core::ColorCurveMode::RandomBetween;
        const bool two = curve->mode() == engine_core::ColorCurveMode::RandomBetween ||
                         curve->mode() == engine_core::ColorCurveMode::GradientRange;
        if (one_key && (curve->keys_a().size() != 1 || (two && curve->keys_b().size() != 1))) {
            return false;
        }
        out.u8(Engine);
        out.u8(ColorCurve);
        out.u8(static_cast<std::uint8_t>(curve->mode()));
        if (one_key) {
            write_color_key(out, curve->keys_a()[0]);
            if (two) {
                write_color_key(out, curve->keys_b()[0]);
            }
        } else {
            write_color_keys(out, curve->keys_a());
            if (two) {
                write_color_keys(out, curve->keys_b());
            }
        }
        return true;
    }
    default:
        return false;
    }
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
        // The root is game on both sides, so it needs no netid.
        if (slot.id == 0) {
            out.u8(Root);
            return;
        }
        const std::uint32_t netid = to_net ? to_net(slot.id) : 0;
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
    if (write_engine(out, slot)) {
        return;
    }
    // Nil, and what never travels (a Signal, an input record): nothing sends one.
    out.u8(Nil);
}

struct Decoder {
    ByteReader& in;
    const NetToId& to_id;
    std::size_t max_values;
    std::size_t values = 0;   // counted so far, at every depth

    // Counts more values against max_values, before anything is made for them.
    bool take(std::uint64_t count) {
        if (count > max_values - values) {
            return false;
        }
        values += static_cast<std::size_t>(count);
        return true;
    }

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
        case Root:
            slot.kind = LuaSlot::Kind::Instance;
            slot.id = 0;
            break;
        case Engine:
            return read_engine(slot);
        case Table:
            return read_table(out, depth);
        default:
            return false;
        }
        return !in.failed();
    }

    bool read_floats(float* out, int count) {
        for (int index = 0; index < count; ++index) {
            out[index] = in.f32();
        }
        return !in.failed();
    }

    // A curve's keys: a count of 1..kRemoteMaxCurveKeys, then each key.
    // key_bytes is what one takes, so a count past what is left is a lie.
    bool read_key_count(std::size_t key_bytes, std::size_t& count) {
        const std::uint64_t declared = in.varuint();
        if (in.failed() || declared == 0 || declared > engine_core::kRemoteMaxCurveKeys ||
            declared > in.left() / key_bytes) {
            return false;
        }
        count = static_cast<std::size_t>(declared);
        return true;
    }

    bool read_number_keys(std::vector<engine_core::NumberKey>& keys) {
        std::size_t count = 0;
        if (!read_key_count(9, count)) {
            return false;
        }
        keys.resize(count);
        for (engine_core::NumberKey& key : keys) {
            key.time = in.f32();
            key.value = in.f32();
            const std::uint8_t interp = in.u8();
            if (interp > static_cast<std::uint8_t>(engine_core::CurveInterp::Step)) {
                return false;
            }
            key.interp = static_cast<engine_core::CurveInterp>(interp);
        }
        return !in.failed();
    }

    bool read_color_key(engine_core::ColorKey& key) {
        float parts[4] = {};
        if (!read_floats(parts, 4)) {
            return false;
        }
        key.color = engine_core::ColorRgb{parts[0], parts[1], parts[2], 1.f};
        key.transparency = parts[3];
        return true;
    }

    bool read_color_keys(std::vector<engine_core::ColorKey>& keys) {
        std::size_t count = 0;
        if (!read_key_count(20, count)) {
            return false;
        }
        keys.resize(count);
        for (engine_core::ColorKey& key : keys) {
            key.time = in.f32();
            if (!read_color_key(key)) {
                return false;
            }
        }
        return true;
    }

    bool read_number_curve(LuaSlot& slot) {
        using engine_core::NumberCurveMode;
        const std::uint8_t mode = in.u8();
        std::optional<engine_core::NumberCurve> curve;
        std::vector<engine_core::NumberKey> a;
        std::vector<engine_core::NumberKey> b;
        std::string error;
        switch (static_cast<NumberCurveMode>(mode)) {
        case NumberCurveMode::Constant: {
            const float value = in.f32();
            curve = engine_core::NumberCurve::constant(value);
            break;
        }
        case NumberCurveMode::Range: {
            float bounds[2] = {};
            if (!read_floats(bounds, 2)) {
                return false;
            }
            curve = engine_core::NumberCurve::range(bounds[0], bounds[1]);
            break;
        }
        case NumberCurveMode::Curve:
            if (!read_number_keys(a)) {
                return false;
            }
            curve = engine_core::NumberCurve::keys(std::move(a), error);
            break;
        case NumberCurveMode::CurveRange:
            if (!read_number_keys(a) || !read_number_keys(b)) {
                return false;
            }
            curve = engine_core::NumberCurve::keys_range(std::move(a), std::move(b), error);
            break;
        default:
            return false;
        }
        if (in.failed() || !curve) {
            return false;
        }
        slot = engine_core::number_curve_slot(std::move(*curve));
        return true;
    }

    bool read_color_curve(LuaSlot& slot) {
        using engine_core::ColorCurveMode;
        const std::uint8_t mode = in.u8();
        std::optional<engine_core::ColorCurve> curve;
        std::vector<engine_core::ColorKey> a;
        std::vector<engine_core::ColorKey> b;
        engine_core::ColorKey one;
        engine_core::ColorKey other;
        std::string error;
        switch (static_cast<ColorCurveMode>(mode)) {
        case ColorCurveMode::Constant:
            if (!read_color_key(one)) {
                return false;
            }
            curve = engine_core::ColorCurve::constant(one.color, one.transparency);
            break;
        case ColorCurveMode::RandomBetween:
            if (!read_color_key(one) || !read_color_key(other)) {
                return false;
            }
            curve = engine_core::ColorCurve::between(one.color, one.transparency, other.color, other.transparency);
            break;
        case ColorCurveMode::Gradient:
            if (!read_color_keys(a)) {
                return false;
            }
            curve = engine_core::ColorCurve::gradient(std::move(a), error);
            break;
        case ColorCurveMode::GradientRange:
            if (!read_color_keys(a) || !read_color_keys(b)) {
                return false;
            }
            curve = engine_core::ColorCurve::gradient_range(std::move(a), std::move(b), error);
            break;
        default:
            return false;
        }
        if (in.failed() || !curve) {
            return false;
        }
        slot = engine_core::color_curve_slot(std::move(*curve));
        return true;
    }

    // An engine value's fixed layout (the header), read straight into slot:
    // every size is fixed or checked against what is left before anything is
    // made for it.
    bool read_engine(LuaSlot& slot) {
        const std::uint8_t kind = in.u8();
        if (in.failed()) {
            return false;
        }
        switch (kind) {
        case Vec3: {
            float axes[3] = {};
            if (!read_floats(axes, 3)) {
                return false;
            }
            slot.kind = LuaSlot::Kind::Vec3;
            slot.vec = engine_core::Vec3{axes[0], axes[1], axes[2]};
            return true;
        }
        case Vec2: {
            float axes[2] = {};
            if (!read_floats(axes, 2)) {
                return false;
            }
            slot.kind = LuaSlot::Kind::Vec2;
            slot.vec = engine_core::Vec3{axes[0], axes[1], 0.f};
            return true;
        }
        case Color: {
            float channels[3] = {};
            if (!read_floats(channels, 3)) {
                return false;
            }
            slot.kind = LuaSlot::Kind::Color;
            slot.color = engine_core::ColorRgb{channels[0], channels[1], channels[2], 1.f};
            return true;
        }
        case Matrix4:
            if (!read_floats(slot.transform.m, 16)) {
                return false;
            }
            slot.kind = LuaSlot::Kind::Matrix4;
            return true;
        case Enum: {
            const std::string name = in.string();
            const engine_core::EnumType* type = in.failed() ? nullptr : enum_type_named(name);
            const std::uint64_t item = in.varuint();
            if (type == nullptr || in.failed() || item > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
                engine_core::enum_item_name(*type, static_cast<int>(item)) == nullptr) {
                return false;
            }
            slot.kind = LuaSlot::Kind::Enum;
            slot.enum_type = type;
            slot.number = static_cast<double>(item);
            return true;
        }
        case NumberCurve:
            return read_number_curve(slot);
        case ColorCurve:
            return read_color_curve(slot);
        default:
            return false;
        }
    }

    bool read_table(RemoteValue& out, std::size_t depth) {
        if (depth >= engine_core::kRemoteMaxDepth) {
            return false;
        }
        auto table = std::make_shared<RemoteTable>();
        // Each value takes at least a byte, so a count past what is left is a lie.
        const std::uint64_t elements = in.varuint();
        if (in.failed() || elements > in.left() || !take(elements)) {
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
        if (in.failed() || fields > in.left() / 2 || !take(fields)) {
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
    out.u8(kRemoteCodecVersion);
    out.varuint(remote_netid);
    out.varuint(values.values.size());
    for (const RemoteValue& value : values.values) {
        write_value(out, value, to_net);
    }
}

bool decode_remote(ByteReader& in, std::uint32_t& remote_netid, engine_core::RemoteValues& out, const NetToId& to_id,
                   std::size_t max_bytes, std::size_t max_values, std::size_t* values_read) {
    if (values_read != nullptr) {
        *values_read = 0;
    }
    out.values.clear();
    if (in.left() > max_bytes) {
        return false;
    }
    if (in.u8() != static_cast<std::uint8_t>(MessageType::Remote) || in.u8() != kRemoteCodecVersion) {
        return false;
    }
    const std::uint64_t netid = in.varuint();
    if (in.failed() || netid > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    remote_netid = static_cast<std::uint32_t>(netid);
    const std::uint64_t count = in.varuint();
    Decoder decoder{in, to_id, max_values};
    if (in.failed() || count > engine_core::kRemoteMaxArgs || count > in.left() || !decoder.take(count)) {
        return false;
    }
    out.values.resize(static_cast<std::size_t>(count));
    for (RemoteValue& value : out.values) {
        if (!decoder.read(value, 0)) {
            out.values.clear();
            return false;
        }
    }
    if (!in.done()) {
        return false;
    }
    if (values_read != nullptr) {
        *values_read = decoder.values;
    }
    return true;
}

std::size_t pack_remote_payload(const std::vector<RemoteMessage>& messages, std::size_t budget,
                                std::vector<std::uint8_t>& out) {
    out.clear();
    std::size_t count = 0;
    std::size_t body = 0;
    for (const RemoteMessage& message : messages) {
        const std::size_t room = varuint_size(message->size()) + message->size();
        if (varuint_size(count + 1) + body + room > budget) {
            break;
        }
        body += room;
        ++count;
    }
    if (count == 0) {
        return 0;
    }
    ByteWriter writer;
    writer.data().swap(out);
    writer.varuint(count);
    for (std::size_t index = 0; index < count; ++index) {
        writer.varuint(messages[index]->size());
        writer.bytes(messages[index]->data(), messages[index]->size());
    }
    out.swap(writer.data());
    return count;
}

bool read_remote_payload(const std::uint8_t* data, std::size_t size,
                         const std::function<void(const std::uint8_t*, std::size_t)>& visit) {
    // A varuint at *at, which moves past it; false past the end.
    const auto varuint_at = [data, size](std::size_t& at, std::uint64_t& value) {
        ByteReader in(data + at, size - at);
        value = in.varuint();
        at = size - in.left();
        return !in.failed();
    };
    // The framing is checked whole first, so a bad payload delivers nothing.
    std::size_t at = 0;
    std::uint64_t count = 0;
    if (!varuint_at(at, count) || count > size - at) {
        return false;
    }
    const std::size_t first = at;
    for (std::uint64_t index = 0; index < count; ++index) {
        std::uint64_t length = 0;
        if (!varuint_at(at, length) || length > size - at) {
            return false;
        }
        at += static_cast<std::size_t>(length);
    }
    if (at != size) {
        return false;
    }
    at = first;
    for (std::uint64_t index = 0; index < count; ++index) {
        std::uint64_t length = 0;
        varuint_at(at, length);
        visit(data + at, static_cast<std::size_t>(length));
        at += static_cast<std::size_t>(length);
    }
    return true;
}

}  // namespace engine_network
