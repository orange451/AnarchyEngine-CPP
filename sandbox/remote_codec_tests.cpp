// RemoteCodec: a fired payload on the wire, instances by NetId, and that no
// input crashes it (docs/superpowers/specs/2026-10-20-remote-events-design.md).

#include "ByteBuffer.hpp"
#include "RemoteCodec.hpp"
#include "TreeCodec.hpp"

#include "CurveJson.hpp"
#include "Enum.hpp"
#include "PropertyReflection.hpp"
#include "RemoteValues.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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
                buffer[1] = engine_network::kCodecVersion;
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
        out.u8(engine_network::kCodecVersion);
        out.varuint(1);
        out.varuint(1);
        out.u8(5);  // String
        out.varuint(1000000);
        out.u8('a');
        REQUIRE_FALSE(decode(out.data(), out.data().size(), remote, back));

        ByteWriter count;
        count.u8(static_cast<std::uint8_t>(engine_network::MessageType::Remote));
        count.u8(engine_network::kCodecVersion);
        count.varuint(1);
        count.varuint(~0ull);  // a billion billion arguments
        REQUIRE_FALSE(decode(count.data(), count.data().size(), remote, back));

        ByteWriter array;
        array.u8(static_cast<std::uint8_t>(engine_network::MessageType::Remote));
        array.u8(engine_network::kCodecVersion);
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
        out.u8(engine_network::kCodecVersion);
        out.varuint(1);
        out.varuint(1);
        out.u8(200);
        REQUIRE_FALSE(decode(out.data(), out.data().size(), remote, back));

        // An unknown engine kind, and an Enum type no one has.
        ByteWriter kind;
        kind.u8(static_cast<std::uint8_t>(engine_network::MessageType::Remote));
        kind.u8(engine_network::kCodecVersion);
        kind.varuint(1);
        kind.varuint(1);
        kind.u8(7);
        kind.u8(99);
        kind.u8(0);
        REQUIRE_FALSE(decode(kind.data(), kind.data().size(), remote, back));

        ByteWriter item;
        item.u8(static_cast<std::uint8_t>(engine_network::MessageType::Remote));
        item.u8(engine_network::kCodecVersion);
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
        version[1] = static_cast<std::uint8_t>(engine_network::kCodecVersion + 1);
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
