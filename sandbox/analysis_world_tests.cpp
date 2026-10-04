#include "AnalysisWorld.hpp"
#include "LuaApi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using engine_core::InstanceId;
using engine_core::LuaNode;
using engine_core::analysis::TreeDiff;
using engine_core::analysis::diff_worlds;
using engine_core::analysis::keep_place_only;
using engine_core::analysis::world_from_nodes;

constexpr InstanceId kNone = 0xffffffffu;

LuaNode node(InstanceId id, InstanceId parent, const char* name, const char* class_name, const char* source = "") {
    LuaNode out;
    out.id = id;
    out.parent = parent;
    out.name = name;
    out.class_name = class_name;
    out.source = source;
    return out;
}

// game(1) > Workspace(2) > { Props(3) > Crate(5), Main(4) }. Children follow node order.
std::vector<LuaNode> base() {
    return {node(1, kNone, "Game", "Game"), node(2, 1, "Workspace", "Workspace"), node(3, 2, "Props", "Folder"),
            node(4, 2, "Main", "Script", "print(1)\n"), node(5, 3, "Crate", "GameObject")};
}

std::unordered_set<InstanceId> set_of(std::initializer_list<InstanceId> ids) { return {ids}; }

LuaNode& at(std::vector<LuaNode>& nodes, InstanceId id) {
    return *std::find_if(nodes.begin(), nodes.end(), [id](const LuaNode& item) { return item.id == id; });
}

}  // namespace

TEST_CASE("AW1 a snapshot finds every node and child by the index", "[AW1]") {
    const auto world = world_from_nodes(base());
    for (InstanceId id : {1u, 2u, 3u, 4u, 5u}) {
        REQUIRE(world->find(id) != nullptr);
        REQUIRE(world->find(id)->id == id);
    }
    REQUIRE(world->find(99) == nullptr);
    REQUIRE(world->child_named(2, "Props") == std::optional<InstanceId>(3));
    REQUIRE(world->workspace() == std::optional<InstanceId>(2));
    REQUIRE(world->find(4)->lua);
}

TEST_CASE("AW2 the same tree twice has an empty diff", "[AW2]") {
    const TreeDiff diff = diff_worlds(*world_from_nodes(base()), *world_from_nodes(base()));
    REQUIRE(diff.parents.empty());
    REQUIRE(diff.moved.empty());
    REQUIRE(diff.added_scripts.empty());
    REQUIRE(diff.removed_scripts.empty());
    REQUIRE(diff.edited_scripts.empty());
}

TEST_CASE("AW3 adding a child marks its parent, and an added script is listed", "[AW3]") {
    std::vector<LuaNode> after = base();
    after.push_back(node(6, 3, "Box", "GameObject"));
    after.push_back(node(7, 2, "Other", "Script", "print(2)\n"));
    const TreeDiff diff = diff_worlds(*world_from_nodes(base()), *world_from_nodes(after));
    REQUIRE(diff.parents == set_of({2, 3}));
    REQUIRE(diff.moved.empty());
    REQUIRE(diff.added_scripts == std::vector<InstanceId>{7});
}

TEST_CASE("AW4 a rename marks the instance and its parent", "[AW4]") {
    std::vector<LuaNode> after = base();
    at(after, 3).name = "Stuff";
    const TreeDiff diff = diff_worlds(*world_from_nodes(base()), *world_from_nodes(after));
    REQUIRE(diff.moved == set_of({3}));
    REQUIRE(diff.parents == set_of({2}));
}

TEST_CASE("AW5 a reparent marks the instance and both parents", "[AW5]") {
    std::vector<LuaNode> after = base();
    at(after, 5).parent = 2;
    const TreeDiff diff = diff_worlds(*world_from_nodes(base()), *world_from_nodes(after));
    REQUIRE(diff.moved == set_of({5}));
    REQUIRE(diff.parents == set_of({2, 3}));
}

TEST_CASE("AW6 a reorder among siblings marks the parent only", "[AW6]") {
    std::vector<LuaNode> after = base();
    std::swap(after[2], after[3]);  // Main before Props under Workspace
    const TreeDiff diff = diff_worlds(*world_from_nodes(base()), *world_from_nodes(after));
    REQUIRE(diff.parents == set_of({2}));
    REQUIRE(diff.moved.empty());
}

TEST_CASE("AW7 a destroyed script is removed, moved, and marks its parent", "[AW7]") {
    std::vector<LuaNode> after = base();
    after.erase(after.begin() + 3);  // Main
    const TreeDiff diff = diff_worlds(*world_from_nodes(base()), *world_from_nodes(after));
    REQUIRE(diff.removed_scripts == std::vector<InstanceId>{4});
    REQUIRE(diff.moved == set_of({4}));
    REQUIRE(diff.parents == set_of({2}));
}

TEST_CASE("AW8 a source edit lists the script and nothing else", "[AW8]") {
    std::vector<LuaNode> after = base();
    at(after, 4).source = "print(2)\n";
    const TreeDiff diff = diff_worlds(*world_from_nodes(base()), *world_from_nodes(after));
    REQUIRE(diff.edited_scripts == std::vector<InstanceId>{4});
    REQUIRE(diff.parents.empty());
    REQUIRE(diff.moved.empty());
}

TEST_CASE("AW9 a detached subtree is out of the place, and back in when reattached", "[AW9]") {
    // Props(3) is destroyed: Crate(5) and a script under it, Inner(6), are left
    // parentless or under a parentless node, outside the place.
    std::vector<LuaNode> grouped = base();
    grouped.push_back(node(6, 3, "Inner", "ModuleScript", "return 1\n"));
    std::vector<LuaNode> detached = grouped;
    detached.erase(detached.begin() + 2);  // Props
    at(detached, 5).parent = kNone;
    at(detached, 6).parent = 7;
    detached.push_back(node(7, kNone, "Loose", "Folder"));
    auto placed = world_from_nodes(grouped);
    keep_place_only(*placed);
    auto apart = world_from_nodes(detached);
    keep_place_only(*apart);
    REQUIRE(placed->nodes.size() == 6);
    REQUIRE(apart->nodes.size() == 3);
    REQUIRE(apart->find(5) == nullptr);
    REQUIRE(apart->find(6) == nullptr);
    REQUIRE(apart->find(7) == nullptr);
    REQUIRE(apart->find(4) != nullptr);

    const TreeDiff gone = diff_worlds(*placed, *apart);
    REQUIRE(gone.removed_scripts == std::vector<InstanceId>{6});
    REQUIRE(gone.moved == set_of({3, 5, 6}));
    REQUIRE(gone.parents == set_of({2, 3}));

    const TreeDiff back = diff_worlds(*apart, *placed);
    REQUIRE(back.added_scripts == std::vector<InstanceId>{6});
    REQUIRE(back.removed_scripts.empty());
    REQUIRE(back.parents == set_of({2, 3}));
}
