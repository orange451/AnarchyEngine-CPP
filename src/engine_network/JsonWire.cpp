#include "JsonWire.hpp"

#include <cmath>
#include <limits>

namespace engine_network {

// A float holds d exactly: true for -0 (its sign survives the cast) and the
// infinities, false for NaN, which keeps its payload in the double form. A
// finite d past a float's range is checked first, since casting it is undefined.
bool float_exact(double d) {
    if (std::isnan(d)) {
        return false;
    }
    if (std::isfinite(d) && std::fabs(d) > static_cast<double>(std::numeric_limits<float>::max())) {
        return false;
    }
    return static_cast<double>(static_cast<float>(d)) == d;
}

namespace {

// Number carries a double; Float32 carries a number the float holds exactly, in half the bytes.
enum Tag : std::uint8_t { Null = 0, False = 1, True = 2, Number = 3, String = 4, Array = 5, Object = 6, Float32 = 7 };
constexpr int kMaxDepth = 32;

bool read_value(ByteReader& in, engine_core::JsonValue& out, int depth) {
    using engine_core::JsonValue;
    switch (in.u8()) {
        case Null: out = JsonValue(); break;
        case False: out = JsonValue::boolean(false); break;
        case True: out = JsonValue::boolean(true); break;
        case Number: out = JsonValue::number(in.f64()); break;
        case Float32: out = JsonValue::number(static_cast<double>(in.f32())); break;
        case String: out = JsonValue::string(in.string()); break;
        case Array: {
            if (depth >= kMaxDepth) {
                return false;
            }
            const std::uint64_t count = in.varuint();
            // Each item takes at least one byte, so a count beyond what is left is a lie.
            if (in.failed() || count > in.left()) {
                return false;
            }
            std::vector<JsonValue> items(static_cast<std::size_t>(count));
            for (JsonValue& item : items) {
                if (!read_value(in, item, depth + 1)) {
                    return false;
                }
            }
            out = JsonValue::array(std::move(items));
            break;
        }
        case Object: {
            if (depth >= kMaxDepth) {
                return false;
            }
            const std::uint64_t count = in.varuint();
            if (in.failed() || count > in.left()) {
                return false;
            }
            out = JsonValue::object();
            for (std::uint64_t i = 0; i < count; ++i) {
                std::string key = in.string();
                JsonValue value;
                if (in.failed() || !read_value(in, value, depth + 1)) {
                    return false;
                }
                out.set(std::move(key), std::move(value));
            }
            break;
        }
        default:
            return false;
    }
    return !in.failed();
}

}  // namespace

void write_json(ByteWriter& out, const engine_core::JsonValue& value) {
    using Kind = engine_core::JsonValue::Kind;
    switch (value.kind()) {
        case Kind::Null: out.u8(Null); break;
        case Kind::Bool: out.u8(value.as_bool() ? True : False); break;
        case Kind::Number: {
            const double number = value.as_number();
            if (float_exact(number)) {
                out.u8(Float32);
                out.f32(static_cast<float>(number));
            } else {
                out.u8(Number);
                out.f64(number);
            }
            break;
        }
        case Kind::String:
            out.u8(String);
            out.string(value.as_string());
            break;
        case Kind::Array:
            out.u8(Array);
            out.varuint(value.items().size());
            for (const engine_core::JsonValue& item : value.items()) {
                write_json(out, item);
            }
            break;
        case Kind::Object:
            out.u8(Object);
            out.varuint(value.members().size());
            for (const auto& [key, member] : value.members()) {
                out.string(key);
                write_json(out, member);
            }
            break;
    }
}

bool read_json(ByteReader& in, engine_core::JsonValue& out) { return read_value(in, out, 0); }

}  // namespace engine_network
