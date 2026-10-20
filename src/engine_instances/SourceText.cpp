#include "SourceText.hpp"

#include "Contract.hpp"

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

void SourceText::set_source(std::string source) {
    if (!on_gameplay_thread()) {
        contract_fail("set_source runs on SimulationThread");
    }
    if (source_ == source) {
        return;
    }
    const std::string previous = source_;
    source_ = std::move(source);
    ++source_version_;
    record_string(id(), Field::Source, previous, source_);
    emit_own(Field::Source);
    on_source_changed();
}

void SourceText::write_place(std::vector<std::byte>& out) const {
    const std::uint32_t length = static_cast<std::uint32_t>(source_.size());
    const auto* bytes = reinterpret_cast<const std::byte*>(&length);
    out.insert(out.end(), bytes, bytes + sizeof(length));
    const auto* text = reinterpret_cast<const std::byte*>(source_.data());
    out.insert(out.end(), text, text + source_.size());
}

void SourceText::read_place(const std::byte* data, std::size_t size) {
    const std::string previous = source_;
    source_.clear();
    if (data != nullptr && size >= sizeof(std::uint32_t)) {
        std::uint32_t length = 0;
        std::memcpy(&length, data, sizeof(length));
        if (sizeof(std::uint32_t) + static_cast<std::size_t>(length) <= size) {
            source_.assign(reinterpret_cast<const char*>(data + sizeof(std::uint32_t)), length);
        }
    }
    // Stop restores authored source through here, not through set_source.
    if (source_ != previous) {
        ++source_version_;
        on_source_changed();
    }
}

}  // namespace engine_core
