#include "TreeCodec.hpp"

#include "JsonWire.hpp"

namespace engine_network {

// StringTable keeps the strings in index order and a map back.
std::uint32_t StringTable::intern(std::string_view text, bool* added) {
    const auto found = index_.find(std::string(text));
    if (found != index_.end()) {
        if (added != nullptr) *added = false;
        return found->second;
    }
    const std::uint32_t index = static_cast<std::uint32_t>(strings_.size());
    strings_.emplace_back(text);
    index_.emplace(strings_.back(), index);
    if (added != nullptr) *added = true;
    return index;
}

const std::string* StringTable::at(std::uint32_t index) const {
    return index < strings_.size() ? &strings_[index] : nullptr;
}

std::size_t StringTable::size() const { return strings_.size(); }

void StringTable::clear() {
    strings_.clear();
    index_.clear();
}

OpWriter::OpWriter(ByteWriter& out, StringTable& strings) : out_(out), strings_(strings) {}

void OpWriter::define(std::uint32_t index) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Define));
    out_.varuint(index);
    out_.string(*strings_.at(index));
}

// The string's index, defining it first when this is its first use.
std::uint32_t OpWriter::ref(std::string_view text) {
    bool added = false;
    const std::uint32_t index = strings_.intern(text, &added);
    if (added) {
        define(index);
    }
    return index;
}

void OpWriter::define_range(std::uint32_t from, std::uint32_t to) {
    for (std::uint32_t index = from; index < to; ++index) {
        define(index);
    }
}

void OpWriter::define_all() { define_range(0, static_cast<std::uint32_t>(strings_.size())); }

void OpWriter::bind(std::uint32_t netid, std::string_view guid) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Bind));
    out_.u32(netid);
    out_.string(guid);
    ++ops_;
}

void OpWriter::create(std::uint32_t netid, std::uint32_t parent, std::string_view class_name, std::string_view guid,
                      std::string_view name, const engine_core::PropertyBag& properties, const std::string* source,
                      const std::vector<std::string>* extra_sources) {
    // Strings first: their Define ops must come before the op that uses them.
    const std::uint32_t klass = ref(class_name);
    std::vector<std::uint32_t> keys;
    keys.reserve(properties.size());
    for (const auto& member : properties) {
        keys.push_back(ref(member.first));
    }
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Create));
    out_.u32(netid);
    out_.u32(parent);
    out_.varuint(klass);
    out_.string(guid);
    out_.string(name);
    out_.varuint(properties.size());
    for (std::size_t i = 0; i < properties.size(); ++i) {
        out_.varuint(keys[i]);
        write_json(out_, properties[i].second);
    }
    out_.u8(source != nullptr ? 1 : 0);
    if (source != nullptr) {
        out_.string(*source);
        const std::size_t extras = extra_sources != nullptr ? extra_sources->size() : 0;
        out_.varuint(extras);
        for (std::size_t i = 0; i < extras; ++i) {
            out_.string((*extra_sources)[i]);
        }
    }
    ++ops_;
}

void OpWriter::destroy(std::uint32_t netid) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Destroy));
    out_.u32(netid);
    ++ops_;
}

void OpWriter::parent(std::uint32_t netid, std::uint32_t parent) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Parent));
    out_.u32(netid);
    out_.u32(parent);
    ++ops_;
}

void OpWriter::rename(std::uint32_t netid, std::string_view name) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Rename));
    out_.u32(netid);
    out_.string(name);
    ++ops_;
}

void OpWriter::set(std::uint32_t netid, std::string_view property, const engine_core::JsonValue& value) {
    const std::uint32_t key = ref(property);
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Set));
    out_.u32(netid);
    out_.varuint(key);
    write_json(out_, value);
    ++ops_;
}

void OpWriter::source(std::uint32_t netid, std::string_view source, std::uint8_t stage) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Source));
    out_.u32(netid);
    out_.u8(stage);
    out_.string(source);
    ++ops_;
}

namespace {

// The string a wire index names, or false when the table has no such string.
bool lookup(const StringTable& strings, std::uint64_t index, std::string& out) {
    const std::string* text = index <= 0xFFFFFFFFull ? strings.at(static_cast<std::uint32_t>(index)) : nullptr;
    if (text == nullptr) {
        return false;
    }
    out = *text;
    return true;
}

}  // namespace

bool read_ops(ByteReader& in, StringTable& strings, std::vector<Op>& out) {
    while (in.left() > 0) {
        Op op;
        op.kind = static_cast<Op::Kind>(in.u8());
        switch (op.kind) {
            case Op::Kind::Define: {
                const std::uint64_t index = in.varuint();
                std::string text = in.string();
                if (in.failed()) {
                    return false;
                }
                // One table serves every client, so a client can be told a string
                // twice: again is fine when it says the same thing.
                if (index < strings.size()) {
                    if (*strings.at(static_cast<std::uint32_t>(index)) != text) {
                        return false;
                    }
                    continue;
                }
                if (index != strings.size()) {
                    return false;
                }
                bool added = false;
                strings.intern(text, &added);
                if (!added) {
                    return false;   // the text is already at another index: the tables would drift apart
                }
                continue;   // a Define is for the table, not the caller
            }
            case Op::Kind::Bind:
                op.netid = in.u32();
                op.text = in.string();
                break;
            case Op::Kind::Create: {
                op.netid = in.u32();
                op.parent = in.u32();
                if (!lookup(strings, in.varuint(), op.class_name)) {
                    return false;
                }
                op.text = in.string();
                op.name = in.string();
                const std::uint64_t count = in.varuint();
                if (in.failed() || count > in.left()) {
                    return false;
                }
                for (std::uint64_t i = 0; i < count; ++i) {
                    std::string key;
                    engine_core::JsonValue value;
                    if (!lookup(strings, in.varuint(), key) || !read_json(in, value)) {
                        return false;
                    }
                    op.properties.emplace_back(std::move(key), std::move(value));
                }
                const std::uint8_t has_source = in.u8();
                if (has_source > 1) {
                    return false;
                }
                op.has_source = has_source == 1;
                if (op.has_source) {
                    op.source = in.string();
                    const std::uint64_t extras = in.varuint();
                    if (in.failed() || extras > 8) {
                        return false;
                    }
                    for (std::uint64_t i = 0; i < extras; ++i) {
                        op.extra_sources.push_back(in.string());
                    }
                }
                break;
            }
            case Op::Kind::Destroy:
                op.netid = in.u32();
                break;
            case Op::Kind::Parent:
                op.netid = in.u32();
                op.parent = in.u32();
                break;
            case Op::Kind::Rename:
                op.netid = in.u32();
                op.text = in.string();
                break;
            case Op::Kind::Set: {
                op.netid = in.u32();
                std::string key;
                engine_core::JsonValue value;
                if (!lookup(strings, in.varuint(), key) || !read_json(in, value)) {
                    return false;
                }
                op.properties.emplace_back(std::move(key), std::move(value));
                break;
            }
            case Op::Kind::Source:
                op.netid = in.u32();
                op.stage = in.u8();
                op.text = in.string();
                break;
            default:
                return false;
        }
        if (in.failed()) {
            return false;
        }
        out.push_back(std::move(op));
    }
    return !in.failed();
}

}  // namespace engine_network
