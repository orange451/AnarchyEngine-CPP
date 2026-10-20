// RemoteCodec: a fired payload on the wire, instances by NetId, and that no
// input crashes it (docs/superpowers/specs/2026-10-20-remote-events-design.md).

#include "ByteBuffer.hpp"
#include "JsonWire.hpp"
#include "RemoteCodec.hpp"
#include "TreeCodec.hpp"

#include "Curve.hpp"
#include "CurveJson.hpp"
#include "Enum.hpp"
#include "PropertyReflection.hpp"
#include "RemoteValues.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

using engine_core::LuaSlot;
using engine_core::RemoteTable;
using engine_core::RemoteValue;
using engine_core::RemoteValues;
using engine_network::ByteReader;
using engine_network::ByteWriter;

namespace {

RemoteValue nil() { return {}; }

RemoteValue boolean(bool flag) {
    RemoteValue value;
    value.slot.kind = LuaSlot::Kind::Bool;
    value.slot.flag = flag;
    return value;
}

RemoteValue number(double n) {
    RemoteValue value;
    value.slot.kind = LuaSlot::Kind::Number;
    value.slot.number = n;
    return value;
}

RemoteValue text(std::string s) {
    RemoteValue value;
    value.slot.kind = LuaSlot::Kind::String;
    value.slot.text = std::move(s);
    return value;
}

RemoteValue instance(engine_core::InstanceId id) {
    RemoteValue value;
    value.slot.kind = LuaSlot::Kind::Instance;
    value.slot.id = id;
    return value;
}

RemoteValue table(std::vector<RemoteValue> array, std::vector<std::pair<std::string, RemoteValue>> fields = {}) {
    auto t = std::make_shared<RemoteTable>();
    t->array = std::move(array);
    t->fields = std::move(fields);
    RemoteValue value;
    value.table = std::move(t);
    return value;
}

// depth tables, each holding the next; the innermost holds 1.
RemoteValue nested(int depth) {
    RemoteValue value = number(1);
    for (int level = 0; level < depth; ++level) {
        value = table({value});
    }
    return value;
}

bool same(const RemoteValue& a, const RemoteValue& b) {
    if ((a.table == nullptr) != (b.table == nullptr)) {
        return false;
    }
    if (a.table != nullptr) {
        if (a.table->array.size() != b.table->array.size() || a.table->fields.size() != b.table->fields.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.table->array.size(); ++i) {
            if (!same(a.table->array[i], b.table->array[i])) {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.table->fields.size(); ++i) {
            if (a.table->fields[i].first != b.table->fields[i].first ||
                !same(a.table->fields[i].second, b.table->fields[i].second)) {
                return false;
            }
        }
        return true;
    }
    return engine_core::same_slot(a.slot, b.slot);
}

const engine_network::IdToNet identity_to_net = [](engine_core::InstanceId id) { return id; };
const engine_network::NetToId identity_to_id = [](std::uint32_t netid) { return netid; };

std::vector<std::uint8_t> encode(std::uint32_t remote, const RemoteValues& values,
                                 const engine_network::IdToNet& to_net = identity_to_net) {
    ByteWriter out;
    engine_network::encode_remote(remote, values, to_net, out);
    return out.data();
}

bool decode(const std::vector<std::uint8_t>& bytes, std::size_t size, std::uint32_t& remote, RemoteValues& out,
            std::size_t max_bytes = engine_core::kRemoteMaxReliableBytes) {
    ByteReader in(bytes.data(), size);
    return engine_network::decode_remote(in, remote, out, identity_to_id, max_bytes);
}

RemoteValues sample() {
    RemoteValues values;
    auto& v = values.values;
    v.push_back(nil());
    v.push_back(boolean(false));
    v.push_back(boolean(true));
    v.push_back(number(0.1));
    v.push_back(number(1e300));
    v.push_back(number(-0.5));
    v.push_back(text(""));
    v.push_back(text(std::string(1000, 'x')));
    RemoteValue vec3;
    vec3.slot.kind = LuaSlot::Kind::Vec3;
    vec3.slot.vec = engine_core::Vec3{1.5f, -2.f, 0.1f};
    v.push_back(vec3);
    RemoteValue vec2;
    vec2.slot.kind = LuaSlot::Kind::Vec2;
    vec2.slot.vec = engine_core::Vec3{3.f, 4.25f, 0.f};
    v.push_back(vec2);
    RemoteValue color;
    color.slot.kind = LuaSlot::Kind::Color;
    color.slot.color = engine_core::ColorRgb{0.2f, 0.4f, 0.6f, 1.f};
    v.push_back(color);
    RemoteValue matrix;
    matrix.slot.kind = LuaSlot::Kind::Matrix4;
    for (int i = 0; i < 16; ++i) {
        matrix.slot.transform.m[i] = static_cast<float>(i) * 0.3f;
    }
    v.push_back(matrix);
    RemoteValue item;
    item.slot.kind = LuaSlot::Kind::Enum;
    item.slot.enum_type = &engine_core::particle_shape_enum();
    item.slot.number = static_cast<double>(engine_core::ParticleShape::Cone);
    v.push_back(item);
    RemoteValue curve;
    curve.slot = engine_core::number_curve_slot(engine_core::NumberCurve::range(2.f, 5.f));
    v.push_back(curve);
    v.push_back(instance(77));
    v.push_back(table({number(1), nil(), table({text("deep")}, {{"k", boolean(true)}})},
                      {{"name", text("Ball")}, {"where", vec3}}));
    // game itself, which both sides hold.
    v.push_back(instance(0));
    // Trailing nils count too.
    v.push_back(nil());
    v.push_back(nil());
    return values;
}

}  // namespace

TEST_CASE("RE10 every value kind round-trips", "[remote]") {
    const RemoteValues values = sample();
    const std::vector<std::uint8_t> bytes = encode(4242, values);
    REQUIRE(bytes[0] == static_cast<std::uint8_t>(engine_network::MessageType::Remote));

    std::uint32_t remote = 0;
    RemoteValues back;
    REQUIRE(decode(bytes, bytes.size(), remote, back));
    REQUIRE(remote == 4242);
    REQUIRE(back.values.size() == values.values.size());
    for (std::size_t i = 0; i < values.values.size(); ++i) {
        INFO("value " << i);
        REQUIRE(same(values.values[i], back.values[i]));
    }
    // Spot checks that same() is not vacuous.
    REQUIRE(back.values[3].slot.number == 0.1);
    REQUIRE(back.values[4].slot.number == 1e300);
    REQUIRE(back.values[7].slot.text.size() == 1000);
    REQUIRE(back.values[12].slot.enum_type == &engine_core::particle_shape_enum());
    REQUIRE(back.values[14].slot.kind == LuaSlot::Kind::Instance);
    REQUIRE(back.values[14].slot.id == 77);
    REQUIRE(back.values[15].table->fields[0].second.slot.text == "Ball");
    REQUIRE(back.values[16].slot.kind == LuaSlot::Kind::Instance);
    REQUIRE(back.values[16].slot.id == 0);

    // No arguments at all.
    const std::vector<std::uint8_t> empty = encode(1, RemoteValues{});
    REQUIRE(decode(empty, empty.size(), remote, back));
    REQUIRE(back.values.empty());

    // 32 levels are allowed.
    RemoteValues deep;
    deep.values.push_back(nested(32));
    const std::vector<std::uint8_t> deep_bytes = encode(2, deep);
    REQUIRE(decode(deep_bytes, deep_bytes.size(), remote, back));
    REQUIRE(same(deep.values[0], back.values[0]));
}

TEST_CASE("RE11 an instance the other side lacks decodes as nil", "[remote]") {
    RemoteValues values;
    values.values.push_back(instance(5));
    values.values.push_back(table({instance(6)}));
    const std::vector<std::uint8_t> bytes = encode(9, values, [](engine_core::InstanceId) { return 0u; });
    std::uint32_t remote = 0;
    RemoteValues back;
    REQUIRE(decode(bytes, bytes.size(), remote, back));
    REQUIRE(back.values.size() == 2);
    REQUIRE(back.values[0].slot.kind == LuaSlot::Kind::Nil);
    REQUIRE(back.values[1].table->array[0].slot.kind == LuaSlot::Kind::Nil);

    // And a netid the receiver does not know reads nil too.
    const std::vector<std::uint8_t> sent = encode(9, values);
    ByteReader in(sent.data(), sent.size());
    REQUIRE(engine_network::decode_remote(in, remote, back, [](std::uint32_t) { return 0u; },
                                          engine_core::kRemoteMaxReliableBytes));
    REQUIRE(back.values[0].slot.kind == LuaSlot::Kind::Nil);
    REQUIRE(back.values[1].table->array[0].slot.kind == LuaSlot::Kind::Nil);
}

TEST_CASE("RE12 hostile bytes fail without crashing", "[remote]") {
    const std::vector<std::uint8_t> valid = encode(4242, sample());
    std::uint32_t remote = 0;
    RemoteValues back;

    SECTION("truncation at every byte") {
        for (std::size_t size = 0; size < valid.size(); ++size) {
            INFO("size " << size);
            REQUIRE_FALSE(decode(valid, size, remote, back));
        }
    }

    SECTION("random buffers") {
        std::mt19937 random(1234);
        std::vector<std::uint8_t> buffer;
        for (int round = 0; round < 10000; ++round) {
            buffer.resize(random() % 64);
            for (std::uint8_t& byte : buffer) {
                byte = static_cast<std::uint8_t>(random());
            }
            // Half of them start with a real header, so the value reader sees them.
            if (round % 2 == 0 && buffer.size() >= 2) {
                buffer[0] = static_cast<std::uint8_t>(engine_network::MessageType::Remote);
                buffer[1] = engine_network::kRemoteCodecVersion;
            }
            (void)decode(buffer, buffer.size(), remote, back);
        }
        // Random flips of a valid message too.
        for (int round = 0; round < 2000; ++round) {
            buffer = valid;
            buffer[random() % buffer.size()] = static_cast<std::uint8_t>(random());
            (void)decode(buffer, buffer.size(), remote, back);
        }
        SUCCEED();
    }

    SECTION("a declared length past the end") {
        ByteWriter out;
        out.u8(static_cast<std::uint8_t>(engine_network::MessageType::Remote));
        out.u8(engine_network::kRemoteCodecVersion);
        out.varuint(1);
        out.varuint(1);
        out.u8(5);  // String
        out.varuint(1000000);
        out.u8('a');
        REQUIRE_FALSE(decode(out.data(), out.data().size(), remote, back));

        ByteWriter count;
        count.u8(static_cast<std::uint8_t>(engine_network::MessageType::Remote));
        count.u8(engine_network::kRemoteCodecVersion);
        count.varuint(1);
        count.varuint(~0ull);  // a billion billion arguments
        REQUIRE_FALSE(decode(count.data(), count.data().size(), remote, back));

        ByteWriter array;
        array.u8(static_cast<std::uint8_t>(engine_network::MessageType::Remote));
        array.u8(engine_network::kRemoteCodecVersion);
        array.varuint(1);
        array.varuint(1);
        array.u8(8);  // Table
        array.varuint(1u << 30);
        REQUIRE_FALSE(decode(array.data(), array.data().size(), remote, back));
    }

    SECTION("depth 33") {
        RemoteValues deep;
        deep.values.push_back(nested(33));
        const std::vector<std::uint8_t> bytes = encode(2, deep);
        REQUIRE_FALSE(decode(bytes, bytes.size(), remote, back));
    }

    SECTION("an unknown tag") {
        ByteWriter out;
        out.u8(static_cast<std::uint8_t>(engine_network::MessageType::Remote));
        out.u8(engine_network::kRemoteCodecVersion);
        out.varuint(1);
        out.varuint(1);
        out.u8(200);
        REQUIRE_FALSE(decode(out.data(), out.data().size(), remote, back));

        // An unknown engine kind, and an Enum type no one has.
        ByteWriter kind;
        kind.u8(static_cast<std::uint8_t>(engine_network::MessageType::Remote));
        kind.u8(engine_network::kRemoteCodecVersion);
        kind.varuint(1);
        kind.varuint(1);
        kind.u8(7);
        kind.u8(99);
        kind.u8(0);
        REQUIRE_FALSE(decode(kind.data(), kind.data().size(), remote, back));

        ByteWriter item;
        item.u8(static_cast<std::uint8_t>(engine_network::MessageType::Remote));
        item.u8(engine_network::kRemoteCodecVersion);
        item.varuint(1);
        item.varuint(1);
        item.u8(7);
        item.u8(5);
        item.string("NoSuchEnum");
        item.u8(4);
        item.string("Cone");
        REQUIRE_FALSE(decode(item.data(), item.data().size(), remote, back));

        // A wrong message type or codec version.
        std::vector<std::uint8_t> type = valid;
        type[0] = static_cast<std::uint8_t>(engine_network::MessageType::Changes);
        REQUIRE_FALSE(decode(type, type.size(), remote, back));
        std::vector<std::uint8_t> version = valid;
        version[1] = static_cast<std::uint8_t>(engine_network::kRemoteCodecVersion + 1);
        REQUIRE_FALSE(decode(version, version.size(), remote, back));

        // Bytes left over after the values.
        std::vector<std::uint8_t> extra = valid;
        extra.push_back(0);
        REQUIRE_FALSE(decode(extra, extra.size(), remote, back));
    }

    SECTION("a message larger than max_bytes") {
        REQUIRE(decode(valid, valid.size(), remote, back, valid.size()));
        REQUIRE_FALSE(decode(valid, valid.size(), remote, back, valid.size() - 1));
    }
}

namespace {

RemoteValue slot_value(LuaSlot slot) {
    RemoteValue value;
    value.slot = std::move(slot);
    return value;
}

std::vector<engine_core::NumberKey> number_keys(std::size_t count) {
    std::vector<engine_core::NumberKey> keys;
    for (std::size_t i = 0; i < count; ++i) {
        const float time = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.5f;
        keys.push_back({time, static_cast<float>(i) * 0.37f - 3.f, static_cast<engine_core::CurveInterp>(i % 3)});
    }
    return keys;
}

std::vector<engine_core::ColorKey> color_keys(std::size_t count) {
    std::vector<engine_core::ColorKey> keys;
    for (std::size_t i = 0; i < count; ++i) {
        const float time = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.25f;
        const float f = static_cast<float>(i) * 0.1f;
        keys.push_back({time, engine_core::ColorRgb{f, 1.f - f, 0.3f, 1.f}, f * 0.5f});
    }
    return keys;
}

// A message whose one argument is an Engine value of kind; the caller writes the rest.
ByteWriter engine_message(std::uint8_t kind) {
    ByteWriter out;
    out.u8(static_cast<std::uint8_t>(engine_network::MessageType::Remote));
    out.u8(engine_network::kRemoteCodecVersion);
    out.varuint(1);
    out.varuint(1);
    out.u8(7);  // Engine
    out.u8(kind);
    return out;
}

bool decodes(const ByteWriter& out) {
    std::uint32_t remote = 0;
    RemoteValues back;
    return decode(out.data(), out.data().size(), remote, back);
}

}  // namespace

TEST_CASE("RE52 every engine kind round-trips in its fixed layout", "[remote]") {
    std::string error;
    std::vector<RemoteValue> values;
    LuaSlot vec3;
    vec3.kind = LuaSlot::Kind::Vec3;
    vec3.vec = engine_core::Vec3{1.5f, -0.f, std::numeric_limits<float>::max()};
    values.push_back(slot_value(vec3));
    LuaSlot vec2;
    vec2.kind = LuaSlot::Kind::Vec2;
    vec2.vec = engine_core::Vec3{0.1f, -7.25f, 0.f};
    values.push_back(slot_value(vec2));
    LuaSlot color;
    color.kind = LuaSlot::Kind::Color;
    color.color = engine_core::ColorRgb{0.2f, 0.4f, 1.6f, 1.f};
    values.push_back(slot_value(color));
    LuaSlot matrix;
    matrix.kind = LuaSlot::Kind::Matrix4;
    for (int i = 0; i < 16; ++i) {
        matrix.transform.m[i] = static_cast<float>(i) * 0.3f - 2.f;
    }
    values.push_back(slot_value(matrix));
    LuaSlot item;
    item.kind = LuaSlot::Kind::Enum;
    item.enum_type = &engine_core::particle_shape_enum();
    item.number = static_cast<double>(engine_core::ParticleShape::Cone);
    values.push_back(slot_value(item));
    values.push_back(slot_value(engine_core::number_curve_slot(engine_core::NumberCurve::constant(0.1f))));
    values.push_back(slot_value(engine_core::number_curve_slot(engine_core::NumberCurve::range(-2.f, 5.5f))));
    values.push_back(slot_value(engine_core::number_curve_slot(*engine_core::NumberCurve::keys(number_keys(7), error))));
    values.push_back(slot_value(engine_core::number_curve_slot(
        *engine_core::NumberCurve::keys_range(number_keys(1), number_keys(1024), error))));
    values.push_back(slot_value(engine_core::color_curve_slot(
        engine_core::ColorCurve::constant(engine_core::ColorRgb{0.1f, 0.2f, 0.3f, 1.f}, 0.4f))));
    values.push_back(slot_value(engine_core::color_curve_slot(engine_core::ColorCurve::between(
        engine_core::ColorRgb{1.f, 0.f, 0.f, 1.f}, 0.f, engine_core::ColorRgb{0.f, 0.f, 1.f, 1.f}, 0.75f))));
    values.push_back(slot_value(engine_core::color_curve_slot(*engine_core::ColorCurve::gradient(color_keys(5), error))));
    values.push_back(slot_value(engine_core::color_curve_slot(
        *engine_core::ColorCurve::gradient_range(color_keys(3), color_keys(1024), error))));

    RemoteValues sent;
    sent.values = values;
    const std::vector<std::uint8_t> bytes = encode(9, sent);
    std::uint32_t remote = 0;
    RemoteValues back;
    REQUIRE(decode(bytes, bytes.size(), remote, back));
    REQUIRE(back.values.size() == values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        INFO("value " << i);
        REQUIRE(back.values[i].slot.kind == values[i].slot.kind);
        REQUIRE(same(values[i], back.values[i]));
    }
    REQUIRE(back.values[4].slot.enum_type == &engine_core::particle_shape_enum());
    REQUIRE(back.values[8].slot.number_curve->keys_b().size() == 1024);

    // A Vector3 is its three floats: the header, the tags, and 12 bytes.
    RemoteValues one;
    one.values.push_back(slot_value(vec3));
    REQUIRE(encode(9, one).size() == 4 + 2 + 12);
    RemoteValues frame;
    frame.values.push_back(slot_value(matrix));
    REQUIRE(encode(9, frame).size() == 4 + 2 + 64);
}

TEST_CASE("RE53 a hostile engine value is refused quickly", "[remote]") {
    const auto started = std::chrono::steady_clock::now();

    SECTION("a curve count of a billion") {
        ByteWriter number = engine_message(6);
        number.u8(2);  // Curve
        number.varuint(1000000000);
        number.f32(0.f);
        number.f32(1.f);
        number.u8(0);
        REQUIRE_FALSE(decodes(number));
        ByteWriter color = engine_message(7);
        color.u8(2);  // Gradient
        color.varuint(1000000000);
        REQUIRE_FALSE(decodes(color));
    }

    SECTION("one key past the cap, with every byte there") {
        ByteWriter number = engine_message(6);
        number.u8(2);
        number.varuint(1025);
        for (std::size_t i = 0; i < 1025; ++i) {
            number.f32(0.5f);
            number.f32(1.f);
            number.u8(0);
        }
        REQUIRE_FALSE(decodes(number));
    }

    SECTION("no keys, a bad mode, a bad interp, a key outside 0..1, an item no type has") {
        ByteWriter none = engine_message(6);
        none.u8(2);
        none.varuint(0);
        REQUIRE_FALSE(decodes(none));
        ByteWriter mode = engine_message(6);
        mode.u8(9);
        mode.f32(1.f);
        REQUIRE_FALSE(decodes(mode));
        ByteWriter interp = engine_message(6);
        interp.u8(2);
        interp.varuint(1);
        interp.f32(0.5f);
        interp.f32(1.f);
        interp.u8(3);
        REQUIRE_FALSE(decodes(interp));
        ByteWriter late = engine_message(6);
        late.u8(2);
        late.varuint(1);
        late.f32(2.f);
        late.f32(1.f);
        late.u8(0);
        REQUIRE_FALSE(decodes(late));
        ByteWriter item = engine_message(5);
        item.string("ParticleShape");
        item.varuint(100000);
        REQUIRE_FALSE(decodes(item));
    }

    SECTION("truncated floats, for every kind") {
        // Each kind's whole value, then the same one byte short.
        const std::pair<std::uint8_t, int> fixed[] = {{1, 3}, {2, 2}, {3, 3}, {4, 16}};
        for (const auto& [kind, floats] : fixed) {
            INFO("kind " << static_cast<int>(kind));
            ByteWriter whole = engine_message(kind);
            for (int i = 0; i < floats; ++i) whole.f32(1.f);
            REQUIRE(decodes(whole));
            ByteWriter cut = engine_message(kind);
            for (int i = 0; i < floats - 1; ++i) cut.f32(1.f);
            cut.u8(0);
            cut.u8(0);
            cut.u8(0);
            REQUIRE_FALSE(decodes(cut));
        }
        ByteWriter range = engine_message(6);
        range.u8(1);
        range.f32(1.f);
        range.u8(0);
        REQUIRE_FALSE(decodes(range));
        ByteWriter between = engine_message(7);
        between.u8(1);
        for (int i = 0; i < 7; ++i) between.f32(0.5f);
        REQUIRE_FALSE(decodes(between));
    }

    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    REQUIRE(ms < 50.0);
}

TEST_CASE("RE54 a message of the old JSON shape fails to decode", "[remote]") {
    // Version 2 sent engine values as JSON.
    std::vector<std::uint8_t> old = encode(9, sample());
    old[1] = 2;
    std::uint32_t remote = 0;
    RemoteValues back;
    REQUIRE_FALSE(decode(old, old.size(), remote, back));

    // And the JSON layout under this version's byte is refused too.
    std::string error;
    RemoteValues values = sample();
    values.values.push_back(slot_value(engine_core::color_curve_slot(*engine_core::ColorCurve::gradient(color_keys(2), error))));
    const char* types[] = {"Vector3", "Vector2", "Color3", "Matrix4", "", "NumberCurve", "ColorCurve"};
    int checked = 0;
    for (const RemoteValue& value : values.values) {
        const LuaSlot& slot = value.slot;
        int kind = 0;
        switch (slot.kind) {
        case LuaSlot::Kind::Vec3: kind = 1; break;
        case LuaSlot::Kind::Vec2: kind = 2; break;
        case LuaSlot::Kind::Color: kind = 3; break;
        case LuaSlot::Kind::Matrix4: kind = 4; break;
        case LuaSlot::Kind::Enum: kind = 5; break;
        case LuaSlot::Kind::NumberCurve: kind = 6; break;
        case LuaSlot::Kind::ColorCurve: kind = 7; break;
        default: continue;
        }
        INFO("kind " << kind);
        engine_core::JsonValue json;
        REQUIRE(engine_core::slot_to_json(slot, types[kind - 1], json));
        ByteWriter out = engine_message(static_cast<std::uint8_t>(kind));
        if (kind == 5) {
            out.string(slot.enum_type->name);
        }
        engine_network::write_json(out, json);
        REQUIRE_FALSE(decodes(out));
        ++checked;
    }
    REQUIRE(checked == 7);
}

TEST_CASE("RE55 the worst valid 256 KB message decodes in under 50 ms", "[remote]") {
    std::string error;
    // Every message here is valid, near the reliable cap, and costly in its own way.
    std::vector<std::pair<const char*, RemoteValues>> worst;
    {
        // Many values, each a field with its own key.
        std::vector<std::pair<std::string, RemoteValue>> fields;
        for (int i = 0; i < 38000; ++i) fields.emplace_back(std::to_string(i), nil());
        RemoteValues values;
        values.values.push_back(table({}, std::move(fields)));
        worst.emplace_back("fields", std::move(values));
    }
    {
        // The most values a message holds.
        RemoteValues values;
        std::vector<RemoteValue> falses(65535, boolean(false));
        values.values.push_back(table(std::move(falses)));
        worst.emplace_back("array", std::move(values));
    }
    {
        // Curves of the most keys, each sorted on the way in.
        RemoteValues values;
        for (int i = 0; i < 27; ++i) {
            values.values.push_back(
                slot_value(engine_core::number_curve_slot(*engine_core::NumberCurve::keys(number_keys(1024), error))));
        }
        worst.emplace_back("number curves", std::move(values));
    }
    {
        RemoteValues values;
        for (int i = 0; i < 12; ++i) {
            values.values.push_back(
                slot_value(engine_core::color_curve_slot(*engine_core::ColorCurve::gradient(color_keys(1024), error))));
        }
        worst.emplace_back("color curves", std::move(values));
    }
    {
        // Enum items, each found by its type's name.
        LuaSlot item;
        item.kind = LuaSlot::Kind::Enum;
        item.enum_type = &engine_core::particle_shape_enum();
        item.number = static_cast<double>(engine_core::ParticleShape::Cone);
        std::vector<RemoteValue> items(15000, slot_value(item));
        RemoteValues values;
        values.values.push_back(table(std::move(items)));
        worst.emplace_back("enums", std::move(values));
    }
    for (const auto& [name, values] : worst) {
        INFO(name);
        const std::vector<std::uint8_t> bytes = encode(9, values);
        REQUIRE(bytes.size() <= engine_core::kRemoteMaxReliableBytes);
        REQUIRE(bytes.size() > engine_core::kRemoteMaxReliableBytes / 4);
        std::uint32_t remote = 0;
        RemoteValues back;
        const auto started = std::chrono::steady_clock::now();
        REQUIRE(decode(bytes, bytes.size(), remote, back));
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        INFO(ms << " ms for " << bytes.size() << " bytes");
#ifdef NDEBUG
        REQUIRE(ms < 50.0);
#else
        CHECK(ms >= 0.0);
#endif
    }
}
