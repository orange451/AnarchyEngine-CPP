// The tree codec: bytes, JSON values, and operations, and that no input
// crashes it (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "ByteBuffer.hpp"
#include "JsonWire.hpp"
#include "TreeCodec.hpp"

#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

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
    out.f32(0.1f);
    out.string("");
    out.string("Läuft ✓");
    ByteReader in(out.data().data(), out.data().size());
    REQUIRE(in.u8() == 7);
    REQUIRE(in.u32() == 0xDEADBEEFu);
    for (const std::uint64_t v : {0ull, 1ull, 127ull, 128ull, 300ull, 0xFFFFFFFFull, ~0ull}) REQUIRE(in.varuint() == v);
    REQUIRE(in.f64() == 0.1);
    REQUIRE(in.f64() == -std::numeric_limits<double>::infinity());
    REQUIRE(in.f32() == 0.1f);
    REQUIRE(in.string().empty());
    REQUIRE(in.string() == "Läuft ✓");
    REQUIRE(in.done());
    REQUIRE(in.u8() == 0);   // past the end
    REQUIRE(in.failed());
    // The wire is little-endian: the format is the contract, whatever the host.
    const std::vector<std::uint8_t> le{out.data().begin() + 1, out.data().begin() + 5};
    REQUIRE(le == std::vector<std::uint8_t>{0xEF, 0xBE, 0xAD, 0xDE});
    ByteWriter one;
    one.f64(1.0);
    REQUIRE(one.data() == std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0xF0, 0x3F});
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

TEST_CASE("RP12b a number a float holds exactly travels in four bytes; every other keeps eight, exactly", "[replication][codec]") {
    // The tag and payload size one number takes on the wire.
    const auto wire_size = [](double d) {
        ByteWriter out;
        engine_network::write_json(out, JsonValue::number(d));
        return out.data().size();
    };
    const auto bits = [](double d) {
        std::uint64_t b = 0;
        std::memcpy(&b, &d, sizeof(b));
        return b;
    };
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // Float-exact: the float's own value, zero, both infinities, the largest float, the least denormal.
    for (const double d : {static_cast<double>(0.1f), 0.0, inf, -inf, 1.5, -1234.5,
                           static_cast<double>(std::numeric_limits<float>::max()),
                           static_cast<double>(std::numeric_limits<float>::denorm_min())}) {
        INFO(d);
        REQUIRE(wire_size(d) == 5);
        REQUIRE(bits(round_trip(JsonValue::number(d)).as_number()) == bits(d));
    }
    // JsonValue keeps one zero, so -0 reaches the wire as 0; the four-byte form
    // would keep its sign, as every float bit survives it.
    REQUIRE(wire_size(-0.0) == 5);
    ByteWriter negative_zero;
    negative_zero.f32(-0.0f);
    ByteReader nz(negative_zero.data().data(), negative_zero.data().size());
    REQUIRE(std::signbit(nz.f32()));
    // Not float-exact: 0.1 itself, a third, past a float's range, below its least denormal, NaN.
    for (const double d : {0.1, 1.0 / 3.0, 1e300, -1e39, 1e-50, nan}) {
        INFO(d);
        REQUIRE(wire_size(d) == 9);
        REQUIRE(bits(round_trip(JsonValue::number(d)).as_number()) == bits(d));
    }
    REQUIRE(std::isnan(round_trip(JsonValue::number(nan)).as_number()));
    // A Float32 tag cut short fails.
    const std::vector<std::uint8_t> short_float = {7, 0, 0, 0x80};
    ByteReader in(short_float.data(), short_float.size());
    JsonValue v;
    REQUIRE_FALSE(engine_network::read_json(in, v));
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

TEST_CASE("RP14b hostile varints, counts, depths and string tables", "[replication][codec]") {
    using engine_network::StringTable;
    const auto reads = [](const std::vector<std::uint8_t>& bytes) {
        StringTable fresh;
        std::vector<Op> ops;
        ByteReader in(bytes.data(), bytes.size());
        return engine_network::read_ops(in, fresh, ops);
    };
    // A varint of ten bytes ends in 0 or 1; above that, or an eleventh byte, fails.
    {
        std::vector<std::uint8_t> ten(9, 0xFF);
        ten.push_back(1);
        ByteReader ok(ten.data(), ten.size());
        REQUIRE(ok.varuint() == ~0ull);
        REQUIRE_FALSE(ok.failed());
        ten.back() = 2;
        ByteReader bad(ten.data(), ten.size());
        (void)bad.varuint();
        REQUIRE(bad.failed());
        std::vector<std::uint8_t> eleven(10, 0xFF);
        eleven.push_back(0);
        ByteReader longer(eleven.data(), eleven.size());
        (void)longer.varuint();
        REQUIRE(longer.failed());
    }
    // A JSON array or object claiming more items than bytes left.
    {
        const std::vector<std::uint8_t> array = {5, 100, 0};
        const std::vector<std::uint8_t> object = {6, 100, 0};
        JsonValue v;
        ByteReader a(array.data(), array.size());
        REQUIRE_FALSE(engine_network::read_json(a, v));
        ByteReader o(object.data(), object.size());
        REQUIRE_FALSE(engine_network::read_json(o, v));
    }
    // Depth: 32 nested arrays read, 33 do not.
    for (const int levels : {32, 33}) {
        ByteWriter nest;
        for (int i = 0; i < levels; ++i) {
            nest.u8(5);
            nest.varuint(1);
        }
        nest.u8(0);
        ByteReader in(nest.data().data(), nest.data().size());
        JsonValue v;
        REQUIRE(engine_network::read_json(in, v) == (levels == 32));
    }
    // A Create naming more properties than bytes remain; has_source of 2; a string index of 2^32.
    {
        ByteWriter w;
        w.u8(1); w.varuint(0); w.string("Part");
        w.u8(3); w.u32(5); w.u32(0); w.varuint(0); w.string("g"); w.string("n"); w.varuint(1000);
        REQUIRE_FALSE(reads(w.data()));
        ByteWriter s;
        s.u8(1); s.varuint(0); s.string("Part");
        s.u8(3); s.u32(5); s.u32(0); s.varuint(0); s.string("g"); s.string("n"); s.varuint(0); s.u8(2);
        REQUIRE_FALSE(reads(s.data()));
        ByteWriter big;
        big.u8(1); big.varuint(0); big.string("Part");
        big.u8(3); big.u32(5); big.u32(0); big.varuint(1ull << 32);
        REQUIRE_FALSE(reads(big.data()));
    }
    // A Define again with the same text reads; with other text, or text already held at another index, it fails.
    {
        ByteWriter same;
        same.u8(1); same.varuint(0); same.string("a");
        same.u8(1); same.varuint(0); same.string("a");
        same.u8(1); same.varuint(1); same.string("b");
        REQUIRE(reads(same.data()));
        ByteWriter other;
        other.u8(1); other.varuint(0); other.string("a");
        other.u8(1); other.varuint(0); other.string("b");
        REQUIRE_FALSE(reads(other.data()));
        ByteWriter drift;
        drift.u8(1); drift.varuint(0); drift.string("a");
        drift.u8(1); drift.varuint(1); drift.string("a");
        REQUIRE_FALSE(reads(drift.data()));
    }
}
