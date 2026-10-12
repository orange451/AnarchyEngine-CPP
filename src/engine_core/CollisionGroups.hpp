#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine_core {

// The collision groups PhysicsService keeps while the place runs
// (docs/superpowers/specs/2026-10-11-collision-groups-design.md): each
// registered group's type and filter bit, and the explicit pair settings.
// Pure data: PhysicsWorld turns it into Box3D filter bits. The place saves it
// as Workspace's hidden CollisionGroups property (save_text), so it is undone
// and restored at Stop like any saved property. Simulation thread, under the
// DataModel lock.
class CollisionGroups {
public:
    // Exclude: collides with every group but those set not to. Include:
    // with none but those set to. Enum.CollisionFilterType's values.
    enum class FilterType { Exclude = 0, Include = 1 };
    struct Info {
        std::string name;
        FilterType filter_type = FilterType::Exclude;
        // The exceptions to its filter type: for an Exclude group, the groups
        // it is set not to collide with; for an Include group, those it is set to.
        std::vector<std::string> list;
    };

    static constexpr int kMaxGroups = 62;
    static constexpr std::uint64_t kGroundBit = std::uint64_t{1} << 62;
    static constexpr std::uint64_t kRayBit = std::uint64_t{1} << 63;
    static constexpr const char* kDefault = "Default";

    CollisionGroups();

    // Each returns why it refused, changing nothing. Registering a name that
    // is registered already changes nothing, not even its type.
    std::optional<std::string> register_group(const std::string& name, FilterType filter_type);
    std::optional<std::string> unregister_group(const std::string& name);
    std::optional<std::string> set_filter_type(const std::string& name, FilterType filter_type);
    std::optional<std::string> set_collidable(const std::string& a, const std::string& b, bool collidable);
    // Keeps the group's bit, type, order, and pair settings. Default cannot be
    // renamed; to must be non-empty and not registered. To its own name: nothing.
    std::optional<std::string> rename_group(const std::string& from, const std::string& to);

    // The registry as a place saves it (Workspace.CollisionGroups): JSON,
    // {"groups":[[name, "Exclude"|"Include"], ...], "pairs":[[a, b, bool], ...]},
    // groups in registration order, each explicit setting once with its
    // earlier group first. "" while it is Default alone, Exclude, with no settings.
    std::string save_text() const;
    // Rebuilds the registry from save_text's form ("" is Default alone). The
    // revision only grows. Refuses, changing nothing, anything else: see
    // docs/superpowers/specs/2026-10-12-collision-groups-editor-design.md §1.
    std::optional<std::string> load_text(const std::string& text);

    bool registered(std::string_view name) const;
    // An explicit setting decides; else true only when both are Exclude.
    // An unregistered name counts as Default.
    bool collidable(std::string_view a, std::string_view b) const;
    // In registration order, Default first.
    std::vector<Info> groups() const;
    // Default alone, Exclude, with no explicit settings.
    void reset();

    // Filter bits. An unregistered name counts as Default.
    std::uint64_t category(std::string_view name) const;
    // The category bits of every group name collides with.
    std::uint64_t collides_with(std::string_view name) const;

    // Moves on every change, reset included.
    std::uint64_t revision() const { return revision_; }

private:
    struct Slot {
        std::string name;  // empty: free
        FilterType filter_type = FilterType::Exclude;
        std::uint64_t order = 0;
    };
    int bit_of(std::string_view name) const;
    // The registered slots in registration order.
    std::vector<int> ordered() const;
    int resolved(std::string_view name) const;
    bool pair(int a, int b) const;
    void changed();

    std::array<Slot, kMaxGroups> slots_{};
    // Per bit: the bits with an explicit setting, and those settings' values.
    std::array<std::uint64_t, kMaxGroups> explicit_{};
    std::array<std::uint64_t, kMaxGroups> value_{};
    // Per bit: collides_with, kept current by changed().
    std::array<std::uint64_t, kMaxGroups> mask_{};
    std::uint64_t next_order_ = 0;
    std::uint64_t revision_ = 0;
};

}  // namespace engine_core
