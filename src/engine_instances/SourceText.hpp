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
// A class may keep more than one text, its stages: stage 0 is Source; a
// Shader's stage 1 is its vertex text, in <Name>.<guid>.vert.glsl, written
// only while it has text.
class SourceText : public DataModel {
public:
    SourceText(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    // How many texts the class keeps; 1 unless it says otherwise.
    virtual int stage_count() const { return 1; }
    // SimulationThread. Records undo and fires Changed for Source (stage 0)
    // or VertexSource (stage 1).
    void set_source(std::string source) { set_source(0, std::move(source)); }
    void set_source(int stage, std::string source);
    const std::string& source() const { return source_; }
    const std::string& source(int stage) const;
    // Moves each time any stage changes, by set_source or a Stop restoring
    // it. A reader that keeps a copy checks this to know the copy is current.
    std::uint64_t source_version() const { return source_version_; }
    // The stage's file extension without its dot: "luau", "glsl", "vert.glsl".
    virtual const char* source_extension(int stage = 0) const = 0;

    // Edit, then the actions every instance has. Edit is the double-click.
    void context_actions(std::vector<ContextAction>& out) const override;

    // Source is never a property: it lives in its own file.
    bool load_property(const std::string& key, const JsonValue& value, std::string& error) override;

protected:
    // What set_source stores for stage: text as it is, unless the class takes
    // something out of it (a Shader's phase 1 header lines).
    virtual std::string prepare_source(int stage, std::string text) {
        (void)stage;
        return text;
    }
    // After a stage's text changed, by set_source or a Stop's read_place. The
    // stage one calls the plain one for stage 0.
    virtual void on_source_changed() {}
    virtual void on_source_changed(int stage) {
        if (stage == 0) {
            on_source_changed();
        }
    }
    // Each stage's length, then its bytes, stage 0 first.
    void write_place(std::vector<std::byte>& out) const override;
    void read_place(const std::byte* data, std::size_t size) override;

    std::string source_;
    // Stages past the first, sized on first use.
    mutable std::vector<std::string> extras_;
    std::uint64_t source_version_ = 1;

private:
    std::string& stage_text(int stage) const;
};

}  // namespace engine_core
