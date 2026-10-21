#include "SourceText.hpp"

#include "Contract.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace engine_core {

void SourceText::context_actions(std::vector<ContextAction>& out) const {
    out.push_back(ContextAction{InstanceAction::Edit, true});
    DataModel::context_actions(out);
}

bool SourceText::load_property(const std::string& key, const JsonValue& value, std::string& error) {
    if (key == "Source") {
        error = std::string("Source belongs in the .") + source_extension() + " file, not in json";
        return true;
    }
    return DataModel::load_property(key, value, error);
}

std::string& SourceText::stage_text(int stage) const {
    if (stage <= 0) {
        return const_cast<std::string&>(source_);
    }
    const auto at = static_cast<std::size_t>(stage - 1);
    if (extras_.size() <= at) {
        extras_.resize(static_cast<std::size_t>(std::max(stage_count() - 1, stage)));
    }
    return extras_[at];
}

const std::string& SourceText::source(int stage) const { return stage_text(stage); }

void SourceText::set_source(int stage, std::string source) {
    if (!on_gameplay_thread()) {
        contract_fail("set_source runs on SimulationThread");
    }
    if (stage < 0 || stage >= stage_count()) {
        return;
    }
    source = prepare_source(stage, std::move(source));
    std::string& text = stage_text(stage);
    if (text == source) {
        return;
    }
    const std::string previous = text;
    text = std::move(source);
    ++source_version_;
    const Field field = stage == 0 ? Field::Source : Field::VertexSource;
    record_string(id(), field, previous, text);
    emit_own(field);
    on_source_changed(stage);
}

void SourceText::write_place(std::vector<std::byte>& out) const {
    for (int stage = 0; stage < stage_count(); ++stage) {
        const std::string& text = source(stage);
        const std::uint32_t length = static_cast<std::uint32_t>(text.size());
        const auto* bytes = reinterpret_cast<const std::byte*>(&length);
        out.insert(out.end(), bytes, bytes + sizeof(length));
        const auto* chars = reinterpret_cast<const std::byte*>(text.data());
        out.insert(out.end(), chars, chars + text.size());
    }
}

void SourceText::read_place(const std::byte* data, std::size_t size) {
    std::size_t at = 0;
    for (int stage = 0; stage < stage_count(); ++stage) {
        std::string& text = stage_text(stage);
        const std::string previous = text;
        text.clear();
        if (data != nullptr && at + sizeof(std::uint32_t) <= size) {
            std::uint32_t length = 0;
            std::memcpy(&length, data + at, sizeof(length));
            at += sizeof(std::uint32_t);
            if (at + static_cast<std::size_t>(length) <= size) {
                text.assign(reinterpret_cast<const char*>(data + at), length);
                at += length;
            }
        }
        // Stop restores authored source through here, not through set_source.
        if (text != previous) {
            ++source_version_;
            on_source_changed(stage);
        }
    }
}

}  // namespace engine_core
