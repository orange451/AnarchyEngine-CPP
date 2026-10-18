// The tree codec: bytes, JSON values, and operations, and that no input
// crashes it (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "ByteBuffer.hpp"
#include "JsonWire.hpp"
#include "TreeCodec.hpp"

#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>

using engine_core::JsonValue;
using engine_network::ByteReader;
using engine_network::ByteWriter;
using engine_network::Op;

namespace {

JsonValue round_trip(const JsonValue& value) {
    ByteWriter out;
    engine_network::write_json(out, value);
    ByteReader in(out.data().data(), out.data().size());
    JsonValue back;
    REQUIRE(engine_network::read_json(in, back));
    REQUIRE(in.done());
    return back;
}

}  // namespace

TEST_CASE("RP10 bytes, varints, doubles and strings round-trip", "[replication][codec]") {
    ByteWriter out;
    out.u8(7);
    out.u32(0xDEADBEEFu);
    for (const std::uint64_t v : {0ull, 1ull, 127ull, 128ull, 300ull, 0xFFFFFFFFull, ~0ull}) out.varuint(v);
    out.f64(0.1);
    out.f64(-std::numeric_limits<double>::infinity());
    out.string("");
    out.string("Läuft ✓");
    ByteReader in(out.data().data(), out.data().size());
    REQUIRE(in.u8() == 7);
    REQUIRE(in.u32() == 0xDEADBEEFu);
    for (const std::uint64_t v : {0ull, 1ull, 127ull, 128ull, 300ull, 0xFFFFFFFFull, ~0ull}) REQUIRE(in.varuint() == v);
    REQUIRE(in.f64() == 0.1);
    REQUIRE(in.f64() == -std::numeric_limits<double>::infinity());
    REQUIRE(in.string().empty());
    REQUIRE(in.string() == "Läuft ✓");
    REQUIRE(in.done());
    REQUIRE(in.u8() == 0);   // past the end
    REQUIRE(in.failed());
}

TEST_CASE("RP11 a string longer than what is left fails without allocating it", "[replication][codec]") {
    ByteWriter out;
    out.varuint(1ull << 40);
    out.u8('x');
    ByteReader in(out.data().data(), out.data().size());
    REQUIRE(in.string().empty());
    REQUIRE(in.failed());
}

TEST_CASE("RP12 JSON values round-trip exactly", "[replication][codec]") {
    JsonValue object = JsonValue::object();
    object.set("a", JsonValue::number(1.0 / 3.0));
    object.set("b", JsonValue::array({JsonValue::boolean(true), JsonValue(), JsonValue::string("x")}));
    const float f = 0.1f;
    const JsonValue values[] = {JsonValue(), JsonValue::boolean(false), JsonValue::boolean(true), JsonValue::number(0.0),
                                JsonValue::number(-1e300), JsonValue::number_from_float(f), JsonValue::string("guid-1"),
                                JsonValue::array(), object};
    for (const JsonValue& value : values) {
        REQUIRE(round_trip(value) == value);
    }
    REQUIRE(static_cast<float>(round_trip(JsonValue::number_from_float(f)).as_number()) == f);
}

TEST_CASE("RP13 operations round-trip and strings are defined once, before first use", "[replication][codec]") {
    engine_network::StringTable server_strings;
    ByteWriter out;
    engine_network::OpWriter writer(out, server_strings);
    engine_core::PropertyBag props;
    engine_core::bag_set(props, "Brightness", JsonValue::number(2.5));
    engine_core::bag_set(props, "Enabled", JsonValue::boolean(false));
    const std::string source = "print('hi')";
    writer.bind(1, "workspace");
    writer.create(10, 1, "PointLight", "abc123", "Lamp", props, nullptr);
    writer.create(11, 1, "Script", "def456", "Main", {}, &source);
    writer.set(10, "Brightness", JsonValue::number(3.0));
    writer.rename(10, "Lamp2");
    writer.parent(11, 10);
    writer.source(11, "print(2)");
    writer.destroy(10);
    REQUIRE(writer.ops() == 8);
    REQUIRE(server_strings.size() == 4);   // PointLight, Brightness, Enabled, Script

    engine_network::StringTable client_strings;
    ByteReader in(out.data().data(), out.data().size());
    std::vector<Op> ops;
    REQUIRE(engine_network::read_ops(in, client_strings, ops));
    REQUIRE(client_strings.size() == 4);
    REQUIRE(ops.size() == 8);
    REQUIRE(ops[0].kind == Op::Kind::Bind);
    REQUIRE(ops[0].netid == 1);
    REQUIRE(ops[0].text == "workspace");
    REQUIRE(ops[1].kind == Op::Kind::Create);
    REQUIRE(ops[1].class_name == "PointLight");
    REQUIRE(ops[1].text == "abc123");
    REQUIRE(ops[1].name == "Lamp");
    REQUIRE(ops[1].parent == 1);
    REQUIRE(ops[1].properties == props);
    REQUIRE_FALSE(ops[1].has_source);
    REQUIRE(ops[2].has_source);
    REQUIRE(ops[2].source == source);
    REQUIRE(ops[3].kind == Op::Kind::Set);
    REQUIRE(ops[3].properties.size() == 1);
    REQUIRE(ops[3].properties[0].first == "Brightness");
    REQUIRE(ops[4].kind == Op::Kind::Rename);
    REQUIRE(ops[4].text == "Lamp2");
    REQUIRE(ops[5].kind == Op::Kind::Parent);
    REQUIRE(ops[5].parent == 10);
    REQUIRE(ops[6].kind == Op::Kind::Source);
    REQUIRE(ops[6].text == "print(2)");
    REQUIRE(ops[7].kind == Op::Kind::Destroy);

    // A second message defines only what is new; define_all starts a snapshot with everything.
    ByteWriter again;
    engine_network::OpWriter second(again, server_strings);
    second.set(10, "Brightness", JsonValue::number(4.0));
    std::vector<Op> more;
    ByteReader in2(again.data().data(), again.data().size());
    REQUIRE(engine_network::read_ops(in2, client_strings, more));
    REQUIRE(more.size() == 1);
    ByteWriter snapshot;
    engine_network::OpWriter fresh(snapshot, server_strings);
    fresh.define_all();
    engine_network::StringTable joiner;
    std::vector<Op> none;
    ByteReader in3(snapshot.data().data(), snapshot.data().size());
    REQUIRE(engine_network::read_ops(in3, joiner, none));
    REQUIRE(none.empty());
    REQUIRE(joiner.size() == server_strings.size());
}

TEST_CASE("RP14 truncated, garbled and hostile input fails without crashing", "[replication][codec]") {
    engine_network::StringTable strings;
    ByteWriter out;
    engine_network::OpWriter writer(out, strings);
    engine_core::PropertyBag props;
    engine_core::bag_set(props, "Brightness", JsonValue::number(2.5));
    writer.create(10, 0, "PointLight", "abc123", "Lamp", props, nullptr);
    writer.set(10, "Brightness", JsonValue::array({JsonValue::number(1), JsonValue::string("s")}));
    const std::vector<std::uint8_t> good = out.data();
    // Every truncation either fails or reads fewer ops; none crashes.
    for (std::size_t length = 0; length < good.size(); ++length) {
        engine_network::StringTable fresh;
        std::vector<Op> ops;
        ByteReader in(good.data(), length);
        if (engine_network::read_ops(in, fresh, ops)) {
            REQUIRE(ops.size() < 2);
        }
    }
    // Every single-byte change: no crash (the result may be ok or not).
    for (std::size_t at = 0; at < good.size(); ++at) {
        std::vector<std::uint8_t> bad = good;
        bad[at] ^= 0xFF;
        engine_network::StringTable fresh;
        std::vector<Op> ops;
        ByteReader in(bad.data(), bad.size());
        (void)engine_network::read_ops(in, fresh, ops);
    }
    // An unknown op kind, a Define out of order, a reference to an undefined string.
    const std::vector<std::vector<std::uint8_t>> hostile = {
        {0x7F},
        {1, 5, 1, 'x'},
        {7, 1, 0, 0, 0, 9, 0},
    };
    for (const auto& bytes : hostile) {
        engine_network::StringTable fresh;
        std::vector<Op> ops;
        ByteReader in(bytes.data(), bytes.size());
        REQUIRE_FALSE(engine_network::read_ops(in, fresh, ops));
    }
    // JSON nested deeper than 32 fails.
    ByteWriter deep;
    for (int i = 0; i < 40; ++i) {
        deep.u8(5);
        deep.varuint(1);
    }
    deep.u8(0);
    ByteReader in(deep.data().data(), deep.data().size());
    JsonValue value;
    REQUIRE_FALSE(engine_network::read_json(in, value));
}
