#pragma once

// SaveService's stores, with no Lua in them
// (docs/superpowers/specs/2026-11-23-tweens-tags-gamepad-saves-design.md).
// A store is one JSON object, key to value, kept as <directory>/<name>.json.
// A Vector2, Vector3, Color3, or Matrix4 value is {"$type": "Vector3",
// "value": [x, y, z]}; SaveServiceBindings.cpp makes and reads those.

#include "PropertyBag.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine_core {

// Why name cannot name a store, or nothing: a name is 1 to 50 letters, digits,
// _ and -, and not one Windows keeps for a device (con, prn, aux, nul, com1-9, lpt1-9).
std::optional<std::string> store_name_error(std::string_view name);
// name lowercased: the store it opens and its file, so names differing only in
// case are one store, as they are one file on macOS and Windows.
std::string store_key(std::string_view name);

class SaveStore {
public:
    using Clock = std::chrono::steady_clock;

    // Null when key is missing.
    const JsonValue* get(std::string_view key) const;
    void set(std::string key, JsonValue value);
    // False when key was missing.
    bool remove(std::string_view key);
    // Sorted.
    std::vector<std::string> keys() const;
    void clear();

private:
    friend class SaveStores;

    JsonValue values_ = JsonValue::object();
    // Changed since the file was last written.
    bool dirty_ = false;
    // When flush_due first saw it dirty, or last failed to write it: it is
    // written a second after.
    std::optional<Clock::time_point> since_;
    // The last write's failure, said once until it changes or a write works.
    std::string failure_;
};

// Every store a world has opened, by name, and the folder they live in.
// The world's own (DataModel::saves); used on SimulationThread.
class SaveStores {
public:
    using Clock = SaveStore::Clock;

    // Writes what changed to the old folder, then forgets every store, so the
    // next open reads from the new one. Empty keeps stores in memory only.
    // The same folder again changes nothing. From no folder, the stores are
    // kept and written to the new one.
    void set_directory(std::filesystem::path directory);
    const std::filesystem::path& directory() const { return directory_; }
    // Counts the times set_directory forgot the stores, so a handle from
    // before one knows its store is gone.
    std::uint64_t generation() const { return generation_; }

    // The store of that name in any case, the same one each time, read from its file the
    // first time. A file that cannot be read loads as empty and queues a
    // message; it is not written over until the store changes. Null, with
    // error set, for a bad name.
    SaveStore* open(std::string_view name, std::string& error);

    // Writes each store that has been changed for a second or more by now.
    void flush_due(Clock::time_point now);
    // Writes every changed store now.
    void flush();
    // What went wrong with a file since the last take, for the console.
    std::vector<std::string> take_messages();

private:
    void write(const std::string& name, SaveStore& store);

    std::filesystem::path directory_;
    std::map<std::string, std::unique_ptr<SaveStore>, std::less<>> stores_;
    std::vector<std::string> messages_;
    std::uint64_t generation_ = 0;
};

}  // namespace engine_core
