#include "SaveStore.hpp"

#include "FileBytes.hpp"

#include <exception>
#include <system_error>
#include <utility>

namespace engine_core {

namespace fs = std::filesystem;

std::optional<std::string> store_name_error(std::string_view name) {
    bool plain = !name.empty() && name.size() <= 50;
    for (const char c : name) {
        plain = plain && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                          c == '-');
    }
    if (!plain) {
        return "a store name is 1 to 50 letters, digits, _ and -";
    }
    // Windows keeps these names for devices, whatever the case or extension.
    const std::string lower = store_key(name);
    const bool numbered = lower.size() == 4 && (lower.compare(0, 3, "com") == 0 || lower.compare(0, 3, "lpt") == 0) &&
                          lower[3] >= '1' && lower[3] <= '9';
    if (numbered || lower == "con" || lower == "prn" || lower == "aux" || lower == "nul") {
        return "a store cannot be named " + std::string(name) + ", which Windows keeps for a device";
    }
    return std::nullopt;
}

std::string store_key(std::string_view name) {
    std::string key(name);
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return key;
}

const JsonValue* SaveStore::get(std::string_view key) const { return values_.find(key); }

void SaveStore::set(std::string key, JsonValue value) {
    values_.set(std::move(key), std::move(value));
    dirty_ = true;
}

bool SaveStore::remove(std::string_view key) {
    if (!values_.erase(key)) {
        return false;
    }
    dirty_ = true;
    return true;
}

std::vector<std::string> SaveStore::keys() const {
    std::vector<std::string> out;
    out.reserve(values_.members().size());
    for (const JsonValue::Member& member : values_.members()) {
        out.push_back(member.first);
    }
    return out;
}

void SaveStore::clear() {
    values_ = JsonValue::object();
    dirty_ = true;
}

void SaveStores::set_directory(fs::path directory) {
    if (directory == directory_) {
        return;
    }
    // Stores an untitled place kept in memory go with it to its first folder.
    if (directory_.empty()) {
        for (const auto& [name, store] : stores_) {
            store->dirty_ = true;
        }
        directory_ = std::move(directory);
        return;
    }
    flush();
    stores_.clear();
    ++generation_;
    directory_ = std::move(directory);
}

SaveStore* SaveStores::open(std::string_view name, std::string& error) {
    if (const auto bad = store_name_error(name)) {
        error = *bad;
        return nullptr;
    }
    const std::string key = store_key(name);
    if (const auto found = stores_.find(key); found != stores_.end()) {
        return found->second.get();
    }
    auto store = std::make_unique<SaveStore>();
    if (!directory_.empty()) {
        const fs::path file = directory_ / (key + ".json");
        std::error_code missing;
        if (fs::exists(file, missing)) {
            std::string bytes;
            std::string why;
            JsonValue doc;
            if (read_file(file, bytes, why)) {
                if (!parse_json(bytes, doc, why)) {
                    why = "it is not JSON: " + why;
                } else if (!doc.is_object()) {
                    why = "it is not a JSON object";
                } else {
                    store->values_ = std::move(doc);
                }
            }
            if (!why.empty()) {
                messages_.push_back("SaveService: " + utf8_path(file) + " could not be read (" + why +
                                    "). The store opens empty, and the file is kept until the store changes.");
            }
        }
    }
    return stores_.emplace(key, std::move(store)).first->second.get();
}

void SaveStores::write(const std::string& name, SaveStore& store) {
    std::string error;
    try {
        if (write_file(directory_ / (name + ".json"), write_json(store.values_), error)) {
            store.dirty_ = false;
            store.since_.reset();
            store.failure_.clear();
            return;
        }
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    if (error != store.failure_) {
        store.failure_ = error;
        messages_.push_back("SaveService: the store " + name + " could not be saved: " + error);
    }
}

void SaveStores::flush_due(Clock::time_point now) {
    if (directory_.empty()) {
        return;
    }
    for (const auto& [name, store] : stores_) {
        if (!store->dirty_) {
            continue;
        }
        if (!store->since_) {
            store->since_ = now;
        } else if (now - *store->since_ >= std::chrono::seconds(1)) {
            write(name, *store);
            if (store->dirty_) {
                store->since_ = now;
            }
        }
    }
}

void SaveStores::flush() {
    if (directory_.empty()) {
        return;
    }
    for (const auto& [name, store] : stores_) {
        if (store->dirty_) {
            write(name, *store);
        }
    }
}

std::vector<std::string> SaveStores::take_messages() { return std::exchange(messages_, {}); }

}  // namespace engine_core
