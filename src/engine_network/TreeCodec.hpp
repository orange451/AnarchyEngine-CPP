#pragma once

// The tree codec: how instance operations travel between a server and its
// clients (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).
// After a message's header (Replicator and ReplicaApplier write those), the
// wire is a sequence of ops, each a u8 kind and then:
//   Define   varuint index (the table's size, or an earlier index with the same text), string text
//   Bind     u32 netid, string guid
//   Create   u32 netid, u32 parent, varuint class, string guid, string name,
//            varuint count, count x (varuint property, json value),
//            u8 has_source, [string source]
//   Destroy  u32 netid
//   Parent   u32 netid, u32 parent
//   Rename   u32 netid, string name
//   Set      u32 netid, varuint property, json value
//   Source   u32 netid, string source
// A property value is a JSON value as JsonWire writes it.

#include "ByteBuffer.hpp"

#include "PropertyBag.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine_network {

constexpr std::uint8_t kCodecVersion = 1;

enum class MessageType : std::uint8_t { SnapshotPart = 1, Changes = 2 };

// One decoded operation. Which fields mean something depends on the kind.
struct Op {
    enum class Kind : std::uint8_t { Define = 1, Bind, Create, Destroy, Parent, Rename, Set, Source };
    Kind kind = Kind::Define;
    std::uint32_t netid = 0;
    std::uint32_t parent = 0;             // Create, Parent
    std::string text;                     // Define: the string; Bind/Create: guid; Rename: name; Source: source
    std::string class_name;               // Create
    std::string name;                     // Create
    engine_core::PropertyBag properties;  // Create: name -> value; Set: exactly one
    bool has_source = false;              // Create
    std::string source;                   // Create
};

// Class and property names: the server keeps one table for every client, and
// each message defines the strings it is first to use.
class StringTable {
public:
    // The string's index; *added is true when this call added it.
    std::uint32_t intern(std::string_view text, bool* added = nullptr);
    const std::string* at(std::uint32_t index) const;
    std::size_t size() const;
    void clear();

private:
    std::vector<std::string> strings_;
    std::unordered_map<std::string, std::uint32_t> index_;
};

// Builds an op stream. Strings it interns for the first time are defined in
// the stream before their first use.
class OpWriter {
public:
    OpWriter(ByteWriter& out, StringTable& strings);
    // The strings at indices [from, to): what a message adds to a table clients already hold.
    void define_range(std::uint32_t from, std::uint32_t to);
    // Every string in the table, in index order: a snapshot's start.
    void define_all();
    void bind(std::uint32_t netid, std::string_view guid);
    void create(std::uint32_t netid, std::uint32_t parent, std::string_view class_name, std::string_view guid,
                std::string_view name, const engine_core::PropertyBag& properties, const std::string* source);
    void destroy(std::uint32_t netid);
    void parent(std::uint32_t netid, std::uint32_t parent);
    void rename(std::uint32_t netid, std::string_view name);
    void set(std::uint32_t netid, std::string_view property, const engine_core::JsonValue& value);
    void source(std::uint32_t netid, std::string_view source);
    // How many ops (not counting Defines) this writer has written.
    std::size_t ops() const { return ops_; }

private:
    void define(std::uint32_t index);
    std::uint32_t ref(std::string_view text);

    ByteWriter& out_;
    StringTable& strings_;
    std::size_t ops_ = 0;
};

// Reads ops until the bytes run out. Define ops update the client's table and
// are not returned. False, with nothing guaranteed in out, on bad data.
bool read_ops(ByteReader& in, StringTable& strings, std::vector<Op>& out);

}  // namespace engine_network
