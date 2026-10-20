#pragma once

#include "DataModel.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace engine_core {

// Text an instance keeps in a file of its own beside its .meta.json: a
// Script's Luau, a Shader's GLSL. Source is never a saved property: a
// project writes it to <Name>.<guid>.<source_extension()>, and copy, paste,
// undo, Stop, and replication carry it beside the instance's properties.
class SourceText : public DataModel {
public:
    SourceText(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    // SimulationThread. Records undo and fires Changed for Source.
    void set_source(std::string source);
    const std::string& source() const { return source_; }
    // Moves each time the source changes, by set_source or a Stop restoring
    // it. A reader that keeps a copy checks this to know the copy is current.
    std::uint64_t source_version() const { return source_version_; }
    // The file extension without its dot: "luau", "glsl".
    virtual const char* source_extension() const = 0;

    // Edit, then the actions every instance has. Edit is the double-click.
    void context_actions(std::vector<ContextAction>& out) const override;

    // Source is never a property: it lives in its own file.
    bool load_property(const std::string& key, const JsonValue& value, std::string& error) override;

protected:
    // After the text changed, by set_source or a Stop's read_place.
    virtual void on_source_changed() {}
    // The source's length, then its bytes.
    void write_place(std::vector<std::byte>& out) const override;
    void read_place(const std::byte* data, std::size_t size) override;

    std::string source_;
    std::uint64_t source_version_ = 1;
};

}  // namespace engine_core
