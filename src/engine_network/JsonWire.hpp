#pragma once

#include "ByteBuffer.hpp"

#include "PropertyBag.hpp"

namespace engine_network {

// A JSON value as a tag byte and its contents: what a property's value is on
// the wire, exactly as a project save would write it.
void write_json(ByteWriter& out, const engine_core::JsonValue& value);
// False on bad data, or nesting deeper than 32.
bool read_json(ByteReader& in, engine_core::JsonValue& out);
// A float holds d exactly, so it travels in four bytes: true for -0 and the
// infinities, false for NaN.
bool float_exact(double d);

}  // namespace engine_network
