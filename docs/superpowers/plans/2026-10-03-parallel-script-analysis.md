# Parallel Whole-Place Script Analysis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Check every Script and ModuleScript in the place at all times, on a thread pool, rechecking only the scripts a change can affect.

**Architecture:** `ScriptAnalysis` gets two Luau checkers. The editor checker is today's worker, now serving only completion/hover requests on its own thread. The place checker is a coordinator thread that takes due scripts in batches and type-checks them with Luau's `checkQueuedModules` on an `AnalysisPool`. A `customModuleCheck` hook publishes each module's diagnostics as it finishes and records its *reached set*: the instances its expressions are typed as. Tree changes are diffed against the last checked snapshot (`diff_worlds`), per-instance Luau types are updated in place instead of rebuilt, and only scripts whose reached set touches the diff are rechecked.

**Tech Stack:** C++17 (Xcode 13 / libc++ 13 on Mac), Luau 0.739 (`Luau.Analysis`, vendored under `build/_deps/luau-src`), Catch2 v3 sandbox tests, CMake.

**Spec:** `docs/superpowers/specs/2026-10-03-parallel-script-analysis-design.md`

## Global Constraints

- Pool size: `max(1, hardware_concurrency - 1)`; `ScriptAnalysis(game, threads)` with `threads == 0` means that default.
- Debounce stays `kDebounce{75}` ms.
- Every thread that parses or type-checks Luau is a `StackThread` with `kWorkerStackBytes` (16 MB). Never `std::thread` for Luau work.
- Only `pump()` publishes, on the gameplay thread or the UI thread. `diagnostics_changed` fires per script, as now.
- Tree changes during a playtest stay ignored (`DataModel::note_tree_changed` already returns while the simulation runs).
- No C++20 library features: no `std::jthread`, `std::latch`, `std::barrier`, `std::stop_token`, `std::span`. libc++ 13 lacks or gates them.
- Code style matches `src/engine_core`: snake_case functions and locals, `state_->` members, comments that say what and why in plain sentences, no section banners.
- A Luau type hook (`MagicFunction::infer`, `prepareModuleScope`) must never throw: on a pool thread, an exception escaping Luau's task leaves `checkQueuedModules` waiting forever. Each one catches everything and falls back to the declared type.
- Luau is not built with ThreadSanitizer (only `ENGINE_PACKAGES` are), so TSAN sees races in our code only.

## Review Focus

- **A require cycle** (A requires B requires A): Luau checks it as one SCC item. The batch must still finish and publish both modules. Pinned by A38 in Task 5.
- **A script edited while a batch is checking it:** the final published result must be the newest source, never the batch's older one. Pinned by A39 in Task 5.
- **A required module with a syntax error:** it is not type-checked by us, but Luau checks it as a dependency. Its requirer must still get its own results, and the module only its Syntax diagnostic. Pinned by A38 in Task 5.
- **Analysis turned off mid-batch, then on again:** no results from the cancelled batch may appear, and turning it back on must recheck everything. Pinned by A37 in Task 5.
- **Destroying a folder that holds scripts:** each script inside leaves the cache, and whatever required them is rechecked with the new error. Pinned by A41 "destroying a folder of scripts" in Task 6.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `src/engine_core/AnalysisWorld.hpp/.cpp` (new) | Snapshots of the tree the checkers read (`NodeSnap`, `WorldSnap` with an id index), capturing them, `same_tree`, and `diff_worlds`. Pure; no Luau. |
| `src/engine_core/AnalysisPool.hpp/.cpp` (new) | A fixed set of `StackThread`s running posted tasks. Pure; no Luau. |
| `src/engine_core/ScriptAnalysis.hpp/.cpp` | Scheduling, the editor checker, the place checker's batches, place types, publishing. |
| `sandbox/analysis_world_tests.cpp` (new) | AW1–AW8. |
| `sandbox/analysis_pool_tests.cpp` (new) | APL1–APL4. |
| `sandbox/analysis_tests.cpp` | A14/A15 removed; A34–A41 and the `[.perf]` timing test added. |
| `src/ide/IdeLayout.cpp`, `src/ide/IdeScriptEditor.cpp`, `src/ide/McpTools.cpp`, `src/player/main.cpp` | Callers of the removed open scope; the player turns analysis off. |
| `CMakeLists.txt` | New sources in `engine_core` and `sandbox`. |

Commands used throughout:

- Build tests: `cmake --build build --target sandbox -j8`
- Run all analysis tests: `./build/sandbox -# "[#analysis_tests]"`. The baseline before Task 1 is 34 test cases and 219 assertions, all passing.
- Run one: `./build/sandbox "[A35]"`

---

### Task 1: Tree snapshots in their own file, indexed, with a diff

**Files:**
- Create: `src/engine_core/AnalysisWorld.hpp`, `src/engine_core/AnalysisWorld.cpp`
- Create: `sandbox/analysis_world_tests.cpp`
- Modify: `src/engine_core/ScriptAnalysis.cpp`: remove `NodeSnap`, `WorldSnap` (lines ~62–130), `same_tree` (~975–1000), `world_from_nodes` (~1237–1270), `capture_world` (~2368–2396); add `using` declarations.
- Modify: `CMakeLists.txt`: add `src/engine_core/AnalysisWorld.cpp` after `src/engine_core/ScriptAnalysis.cpp` (line 514), and `sandbox/analysis_world_tests.cpp` after `sandbox/analysis_tests.cpp` (line 762).

**Interfaces:**
- Produces (namespace `engine_core::analysis`):
  - `struct NodeSnap { InstanceId id; InstanceId parent; std::string name, class_name, source; bool lua, module; std::vector<InstanceId> children; }`
  - `struct WorldSnap { InstanceId root; std::vector<NodeSnap> nodes; std::unordered_map<InstanceId, std::size_t> index; void reindex(); const NodeSnap* find(InstanceId) const; std::optional<InstanceId> child_named(InstanceId, std::string_view) const; std::optional<InstanceId> module_named(std::string_view, std::optional<InstanceId>) const; std::optional<InstanceId> workspace() const; }`
  - `std::shared_ptr<WorldSnap> capture_world(DataModel& game);`
  - `std::shared_ptr<WorldSnap> world_from_nodes(const std::vector<LuaNode>& nodes);`
  - `bool same_tree(const WorldSnap& a, const WorldSnap& b);`
  - `struct TreeDiff { std::unordered_set<InstanceId> parents, moved; std::vector<InstanceId> added_scripts, removed_scripts, edited_scripts; }`
  - `TreeDiff diff_worlds(const WorldSnap& before, const WorldSnap& after);`

- [ ] **Step 1: Write the failing tests**

Create `sandbox/analysis_world_tests.cpp`:

```cpp
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
```

Add `sandbox/analysis_world_tests.cpp` to the `sandbox` executable in `CMakeLists.txt`, after `sandbox/analysis_tests.cpp`.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build --target sandbox -j8`
Expected: compile error `'AnalysisWorld.hpp' file not found`.

- [ ] **Step 3: Create `src/engine_core/AnalysisWorld.hpp`**

```cpp
#pragma once

#include "LuaApi.hpp"
#include "types.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine_core {

class DataModel;

namespace analysis {

// One instance as script analysis copies it from the tree.
struct NodeSnap {
    InstanceId id = 0;
    InstanceId parent = 0xffffffffu;
    std::string name;
    std::string class_name;
    std::string source;
    bool lua = false;
    bool module = false;
    // Direct children in the same order FindFirstChild walks them.
    std::vector<InstanceId> children;
};

// The tree a check reads, copied on the gameplay thread so the checkers never
// touch the DataModel. The type hooks look instances up constantly, so lookups
// go through `index`, which reindex() rebuilds after nodes change.
struct WorldSnap {
    InstanceId root = 0;
    std::vector<NodeSnap> nodes;
    std::unordered_map<InstanceId, std::size_t> index;

    void reindex();
    const NodeSnap* find(InstanceId id) const;
    std::optional<InstanceId> child_named(InstanceId parent, std::string_view name) const;
    // The ModuleScript with this name, preferring one under prefer_parent.
    std::optional<InstanceId> module_named(std::string_view name, std::optional<InstanceId> prefer_parent) const;
    // workspace: the root's Workspace child.
    std::optional<InstanceId> workspace() const;
};

// The whole tree now. Gameplay thread, or a thread holding the DataModel lock.
std::shared_ptr<WorldSnap> capture_world(DataModel& game);

// The place as completion sees it. The root is the parentless Game or DataModel,
// and children follow the order of `nodes`.
std::shared_ptr<WorldSnap> world_from_nodes(const std::vector<LuaNode>& nodes);

// Two snapshots with the same instances, parents, names, classes, and child
// order. Sources may differ.
bool same_tree(const WorldSnap& a, const WorldSnap& b);

// What changed between two snapshots of one place, as far as a script's types can tell.
struct TreeDiff {
    // Instances whose child list changed: a child added, removed, renamed, or
    // moved among its siblings. A lookup on one may now find something else.
    std::unordered_set<InstanceId> parents;
    // Instances renamed, reparented, or destroyed.
    std::unordered_set<InstanceId> moved;
    std::vector<InstanceId> added_scripts;
    std::vector<InstanceId> removed_scripts;
    // Scripts in both whose source differs.
    std::vector<InstanceId> edited_scripts;
};

TreeDiff diff_worlds(const WorldSnap& before, const WorldSnap& after);

}  // namespace analysis
}  // namespace engine_core
```

- [ ] **Step 4: Create `src/engine_core/AnalysisWorld.cpp`**

Move the bodies of `capture_world`, `world_from_nodes` and `same_tree` from `ScriptAnalysis.cpp` unchanged, except as noted in the comments below. Add the index methods and `diff_worlds`:

```cpp
#include "AnalysisWorld.hpp"

#include "DataModel.hpp"
#include "LuaSource.hpp"
#include "ModuleScript.hpp"

namespace engine_core {
namespace analysis {

void WorldSnap::reindex() {
    index.clear();
    index.reserve(nodes.size());
    for (std::size_t at = 0; at < nodes.size(); ++at) {
        index.emplace(nodes[at].id, at);
    }
}

const NodeSnap* WorldSnap::find(InstanceId id) const {
    const auto found = index.find(id);
    return found == index.end() ? nullptr : &nodes[found->second];
}

std::optional<InstanceId> WorldSnap::child_named(InstanceId parent, std::string_view name) const {
    const NodeSnap* parent_node = find(parent);
    if (parent_node == nullptr) {
        return std::nullopt;
    }
    for (InstanceId child : parent_node->children) {
        const NodeSnap* node = find(child);
        if (node != nullptr && node->name == name) {
            return child;
        }
    }
    return std::nullopt;
}

std::optional<InstanceId> WorldSnap::module_named(std::string_view name, std::optional<InstanceId> prefer_parent) const {
    std::optional<InstanceId> fallback;
    for (const NodeSnap& node : nodes) {
        if (!node.module || node.name != name) {
            continue;
        }
        if (prefer_parent && node.parent == *prefer_parent) {
            return node.id;
        }
        if (!fallback) {
            fallback = node.id;
        }
    }
    return fallback;
}

std::optional<InstanceId> WorldSnap::workspace() const {
    const NodeSnap* root_node = find(root);
    if (root_node != nullptr) {
        for (InstanceId child : root_node->children) {
            const NodeSnap* node = find(child);
            if (node != nullptr && node->class_name == "Workspace") {
                return child;
            }
        }
    }
    return std::nullopt;
}

std::shared_ptr<WorldSnap> capture_world(DataModel& game) {
    // Body moved from ScriptAnalysis.cpp unchanged, except that it calls
    // world->reindex() after the for_each_instance loop and before the loop
    // that fills each node's children. That loop indexes world->nodes by
    // position, as before.
}

std::shared_ptr<WorldSnap> world_from_nodes(const std::vector<LuaNode>& nodes) {
    // Body moved from ScriptAnalysis.cpp, with its local `index` map replaced
    // by world->reindex() and world->find():
    //   ...push every node, find the root, as before...
    //   world->reindex();
    //   for (const NodeSnap& child : world->nodes) — copy the ids first, since
    //   children are appended to other nodes in place:
    //   for (std::size_t at = 0; at < world->nodes.size(); ++at) {
    //       const InstanceId id = world->nodes[at].id;
    //       const InstanceId parent = world->nodes[at].parent;
    //       const auto found = world->index.find(parent);
    //       if (found != world->index.end() && parent != id) {
    //           world->nodes[found->second].children.push_back(id);
    //       }
    //   }
    //   return world;
}

bool same_tree(const WorldSnap& a, const WorldSnap& b) {
    // Body moved from ScriptAnalysis.cpp, with its local `before` map replaced by a.find(node.id).
}

TreeDiff diff_worlds(const WorldSnap& before, const WorldSnap& after) {
    TreeDiff diff;
    const auto mark_parent = [&diff](InstanceId parent) {
        if (parent != DataModel::kNoParent) {
            diff.parents.insert(parent);
        }
    };
    for (const NodeSnap& node : after.nodes) {
        const NodeSnap* was = before.find(node.id);
        if (was == nullptr) {
            mark_parent(node.parent);
            if (node.lua) {
                diff.added_scripts.push_back(node.id);
            }
            continue;
        }
        if (was->parent != node.parent) {
            diff.moved.insert(node.id);
            mark_parent(was->parent);
            mark_parent(node.parent);
        }
        if (was->name != node.name || was->class_name != node.class_name) {
            diff.moved.insert(node.id);
            mark_parent(node.parent);
        }
        if (was->children != node.children) {
            diff.parents.insert(node.id);
        }
        if (node.lua && was->lua && was->source != node.source) {
            diff.edited_scripts.push_back(node.id);
        }
    }
    for (const NodeSnap& node : before.nodes) {
        if (after.find(node.id) != nullptr) {
            continue;
        }
        diff.moved.insert(node.id);
        mark_parent(node.parent);
        if (node.lua) {
            diff.removed_scripts.push_back(node.id);
        }
    }
    return diff;
}

}  // namespace analysis
}  // namespace engine_core
```

The placeholder comments inside `capture_world`, `world_from_nodes` and `same_tree` describe a move, not new code. Paste each function's current body from `ScriptAnalysis.cpp` and make only the listed changes.

- [ ] **Step 5: Point `ScriptAnalysis.cpp` at the new file**

Delete `NodeSnap`, `WorldSnap`, `same_tree`, `world_from_nodes` and `capture_world` from `ScriptAnalysis.cpp`. Add `#include "AnalysisWorld.hpp"` after `#include "ScriptAnalysis.hpp"`. At the top of the first `namespace {` block, after `constexpr std::chrono::milliseconds kDebounce{75};`, add:

```cpp
using analysis::NodeSnap;
using analysis::TreeDiff;
using analysis::WorldSnap;
using analysis::capture_world;
using analysis::diff_worlds;
using analysis::same_tree;
using analysis::world_from_nodes;
```

`capture_world` was declared in the later anonymous-namespace block near line 2368, and `schedule` uses it. The `using` above covers that block too, because both blocks are the same unnamed namespace in one translation unit. Add `src/engine_core/AnalysisWorld.cpp` to the `engine_core` sources in `CMakeLists.txt`.

- [ ] **Step 6: Run the tests**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[AW1],[AW2],[AW3],[AW4],[AW5],[AW6],[AW7],[AW8]" && ./build/sandbox -# "[#analysis_tests]"`
Expected: AW1–AW8 pass; analysis tests still 34 test cases passing.

- [ ] **Step 7: Commit**

```bash
git add src/engine_core/AnalysisWorld.hpp src/engine_core/AnalysisWorld.cpp src/engine_core/ScriptAnalysis.cpp sandbox/analysis_world_tests.cpp CMakeLists.txt
git commit -m "Move analysis snapshots to their own file, index them by id, and diff two of them"
```

---

### Task 2: A thread pool for analysis

**Files:**
- Create: `src/engine_core/AnalysisPool.hpp`, `src/engine_core/AnalysisPool.cpp`
- Create: `sandbox/analysis_pool_tests.cpp`
- Modify: `CMakeLists.txt`: `src/engine_core/AnalysisPool.cpp` in `engine_core`, `sandbox/analysis_pool_tests.cpp` in `sandbox`.

**Interfaces:**
- Produces: `class engine_core::AnalysisPool { AnalysisPool(unsigned threads, std::size_t stack_bytes); unsigned size() const; void post(std::vector<std::function<void()>> tasks); void run_all(std::vector<std::function<void()>> tasks); static unsigned default_size(); }`

- [ ] **Step 1: Write the failing tests**

Create `sandbox/analysis_pool_tests.cpp`:

```cpp
#include "AnalysisPool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kStack = std::size_t{1} << 20;

bool wait_for(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

}  // namespace

TEST_CASE("APL1 run_all runs every task once and returns after the last", "[APL1]") {
    engine_core::AnalysisPool pool(3, kStack);
    std::atomic<int> ran{0};
    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < 100; ++i) {
        tasks.push_back([&ran] { ran.fetch_add(1); });
    }
    pool.run_all(std::move(tasks));
    REQUIRE(ran.load() == 100);
}

TEST_CASE("APL2 tasks run on several threads at once", "[APL2]") {
    engine_core::AnalysisPool pool(4, kStack);
    REQUIRE(pool.size() == 4);
    std::atomic<int> arrived{0};
    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < 4; ++i) {
        // Each waits for all four, which only happens if all four run together.
        tasks.push_back([&arrived] {
            arrived.fetch_add(1);
            wait_for([&arrived] { return arrived.load() == 4; });
        });
    }
    pool.run_all(std::move(tasks));
    REQUIRE(arrived.load() == 4);
}

TEST_CASE("APL3 post returns before its tasks finish", "[APL3]") {
    engine_core::AnalysisPool pool(1, kStack);
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    pool.post({[&] {
        wait_for([&release] { return release.load(); });
        finished.store(true);
    }});
    REQUIRE_FALSE(finished.load());
    release.store(true);
    REQUIRE(wait_for([&finished] { return finished.load(); }));
}

TEST_CASE("APL4 the default size leaves a core free and is at least one", "[APL4]") {
    const unsigned hardware = std::thread::hardware_concurrency();
    const unsigned expected = hardware > 1 ? hardware - 1 : 1;
    REQUIRE(engine_core::AnalysisPool::default_size() == expected);
    engine_core::AnalysisPool pool(0, kStack);
    REQUIRE(pool.size() == 1);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build --target sandbox -j8`
Expected: compile error `'AnalysisPool.hpp' file not found`.

- [ ] **Step 3: Create `src/engine_core/AnalysisPool.hpp`**

```cpp
#pragma once

#include "StackThread.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace engine_core {

// Threads that run script analysis tasks: parsing and linting a batch, and the
// type checks Luau's checkQueuedModules hands out. post() queues and returns at
// once, as checkQueuedModules wants of its executeTasks; run_all() queues and
// waits. Each thread has a stack as deep as Luau needs.
//
// A task must not throw. Luau's own task reports itself done only by returning.
// run_all must not be called from one of the pool's threads.
class AnalysisPool {
public:
    // At least one thread, whatever `threads` says.
    AnalysisPool(unsigned threads, std::size_t stack_bytes);
    // Drops tasks that have not started and joins the threads.
    ~AnalysisPool();
    AnalysisPool(const AnalysisPool&) = delete;
    AnalysisPool& operator=(const AnalysisPool&) = delete;

    unsigned size() const { return static_cast<unsigned>(threads_.size()); }
    void post(std::vector<std::function<void()>> tasks);
    void run_all(std::vector<std::function<void()>> tasks);

    // One fewer than the hardware threads, so the UI and simulation keep a core,
    // and at least one.
    static unsigned default_size();

private:
    void work();

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stop_ = false;
    std::vector<StackThread> threads_;
};

}  // namespace engine_core
```

- [ ] **Step 4: Create `src/engine_core/AnalysisPool.cpp`**

```cpp
#include "AnalysisPool.hpp"

#include <algorithm>
#include <memory>
#include <thread>

namespace engine_core {

AnalysisPool::AnalysisPool(unsigned threads, std::size_t stack_bytes) {
    threads = std::max(1u, threads);
    threads_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        threads_.emplace_back(stack_bytes, [this] { work(); });
    }
}

AnalysisPool::~AnalysisPool() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (StackThread& thread : threads_) {
        thread.join();
    }
}

void AnalysisPool::post(std::vector<std::function<void()>> tasks) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (std::function<void()>& task : tasks) {
            queue_.push_back(std::move(task));
        }
    }
    cv_.notify_all();
}

void AnalysisPool::run_all(std::vector<std::function<void()>> tasks) {
    if (tasks.empty()) {
        return;
    }
    struct Waiter {
        std::mutex mu;
        std::condition_variable cv;
        std::size_t left = 0;
    };
    auto waiter = std::make_shared<Waiter>();
    waiter->left = tasks.size();
    std::vector<std::function<void()>> counted;
    counted.reserve(tasks.size());
    for (std::function<void()>& task : tasks) {
        counted.push_back([task = std::move(task), waiter] {
            task();
            {
                std::lock_guard<std::mutex> lock(waiter->mu);
                --waiter->left;
            }
            waiter->cv.notify_all();
        });
    }
    post(std::move(counted));
    std::unique_lock<std::mutex> lock(waiter->mu);
    waiter->cv.wait(lock, [&waiter] { return waiter->left == 0; });
}

unsigned AnalysisPool::default_size() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware > 1 ? hardware - 1 : 1;
}

void AnalysisPool::work() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

}  // namespace engine_core
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[APL1],[APL2],[APL3],[APL4]"`
Expected: 4 test cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/AnalysisPool.hpp src/engine_core/AnalysisPool.cpp sandbox/analysis_pool_tests.cpp CMakeLists.txt
git commit -m "Add a thread pool for script analysis"
```

---

### Task 3: Check every script, always

This task removes behaviour. No test can fail first: the check is that the build and every existing test still pass with the open scope gone.

**Files:**
- Modify: `src/engine_core/ScriptAnalysis.hpp`: remove `AnalysisScope`, `set_scope`, `scope`, `watch`, `unwatch`, `active_locked`, `drop_inactive_locked`.
- Modify: `src/engine_core/ScriptAnalysis.cpp`: remove their definitions, `State::scope`, `State::watched`, `State::to_schedule`, and every use.
- Modify: `sandbox/analysis_tests.cpp`: delete A14, A15 and the `analyzed` helper. Remove `set_scope`/`watch` from A21.
- Modify: `src/ide/IdeLayout.cpp:37-39`, `src/ide/IdeScriptEditor.cpp:160-161,626`, `src/ide/McpTools.cpp:557-580,591-603`, `src/player/main.cpp:113`.

**Interfaces:**
- Produces: `ScriptAnalysis` with no scope and no watch API. Later tasks assume `State` has no `scope`, `watched` or `to_schedule`.

- [ ] **Step 1: Remove the open scope from the header**

In `src/engine_core/ScriptAnalysis.hpp`, delete:
- the comment and `enum class AnalysisScope { All, Open };`
- the comments and declarations of `set_scope`, `scope`, `watch`, `unwatch`
- the private `active_locked` and `drop_inactive_locked` declarations and their comments

Change the class comment's first line to: `// Incremental analysis of every Lua source in one DataModel, whether or not anything shows it.`

- [ ] **Step 2: Remove it from `ScriptAnalysis.cpp`**

- In `struct ScriptAnalysis::State`, delete `AnalysisScope scope`, `watched` and `to_schedule`, with their comments.
- Delete the definitions of `active_locked`, `drop_inactive_locked`, `set_scope`, `scope`, `watch` and `unwatch`.
- In `invalidate`, delete the `if (state_->scope == AnalysisScope::Open) { ... }` block that filters `chain`.
- Replace `invalidate_all` with:

```cpp
void ScriptAnalysis::invalidate_all() {
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            return;
        }
    }
    std::vector<InstanceId> ids;
    game_.for_each_instance([&ids](DataModel& object) {
        if (dynamic_cast<LuaSource*>(&object) != nullptr) {
            ids.push_back(object.id());
        }
    });
    schedule(ids);
}
```

- In `schedule`, delete `state_->to_schedule.erase(id);` and its comment.
- In `pump`, delete the `// Newly watched scripts...` block (the `wanted` vector and its `schedule(wanted)`). Also delete the `if (state_->scope == AnalysisScope::Open && !fired.empty()) { ... }` block, which ends with `fired.insert(fired.end(), dropped.begin(), dropped.end());`.
- In `busy`, `idle` and `settled`, delete every `to_schedule` term.

- [ ] **Step 3: Update the callers**

`src/ide/IdeLayout.cpp`: delete lines 37–39 (the comment and `runner_.simulation().analysis().set_scope(engine_core::AnalysisScope::Open);`).

`src/ide/IdeScriptEditor.cpp`: delete `// Checked against the tree as it is now, not as it was at its last check.` and `engine_.analysis().watch(id_);` in the constructor, and `engine_.analysis().unwatch(id_);` in the destructor.

`src/ide/McpTools.cpp`: delete `class Watching` with its comment. In `CheckScripts`, delete `const Watching watching(analysis, ids);`, and change the function comment to `// Waits up to kAnalysisWait for analysis to check these scripts.`

`src/player/main.cpp`, after `engine_core::Engine& engine = runner_.simulation();`:

```cpp
        // Nothing in the player reads diagnostics, so its scripts are not checked.
        engine.analysis().set_enabled(false);
```

If `ScriptAnalysis.hpp` is not already included there, add `#include "ScriptAnalysis.hpp"`.

- [ ] **Step 4: Update the tests**

In `sandbox/analysis_tests.cpp`:
- Delete the `TEST_CASE("A14 ...")` and `TEST_CASE("A15 ...")` blocks.
- Delete the `analyzed` helper in the `namespace { ... }` above A14, and keep `add_module`.
- In A21, delete `analysis.set_scope(engine_core::AnalysisScope::Open);` and `analysis.watch(script.id());`.

- [ ] **Step 5: Build everything and run the tests**

Run:
```bash
cmake --build build --target sandbox AnarchyStudio AnarchyPlayer studio-tests mcp-tests engine-tests -j8
./build/sandbox -# "[#analysis_tests]"
./build/studio-tests && ./build/mcp-tests && ./build/engine-tests
grep -rn "AnalysisScope\|analysis().watch\|analysis().unwatch\|set_scope" src sandbox tests
```
Expected: builds clean; analysis tests are 32 test cases, all passing. The other suites pass, and `grep` prints nothing.

- [ ] **Step 6: Commit**

```bash
git add -A src/engine_core/ScriptAnalysis.hpp src/engine_core/ScriptAnalysis.cpp src/ide src/player sandbox/analysis_tests.cpp
git commit -m "Check every script whether or not an editor shows it, and none in the player"
```

---

### Task 4: The editor checker on its own thread

**Files:**
- Modify: `src/engine_core/ScriptAnalysis.hpp`: private `void run();` becomes `void run_editor();` and `void run_place();`
- Modify: `src/engine_core/ScriptAnalysis.cpp`: `State` (thread and condition variable), `ensure_worker`, `shutdown`, `queue_luau`, `run`
- Test: `sandbox/analysis_tests.cpp`: helper `long_source`, test A34

**Interfaces:**
- Produces: `State::place` and `State::editor` (`StackThread`), `State::cv` (the place checker's queue) and `State::editor_cv` (completions). `run_place()` keeps today's per-script job loop; Task 5 replaces its body.
- Produces (test helper): `std::string long_source(int lines, bool strict, int salt)`

- [ ] **Step 1: Write the failing test**

In `sandbox/analysis_tests.cpp`, add to the first `namespace { ... }`, after `has_code`:

```cpp
// About `lines` lines of ordinary code: loops, tables, string formatting, a
// FindFirstChild, and in each function one type error and one unknown global.
// `salt` keeps function names apart when several such scripts share a place.
std::string long_source(int lines, bool strict, int salt) {
    std::ostringstream out;
    if (strict) {
        out << "--!strict\n";
    }
    for (int line = 0, f = 0; line < lines; line += 14, ++f) {
        out << "local function fn" << f << "_" << salt << "(a: number, b: string)\n"
            << "    local t = { x = a, y = b, list = {} }\n"
            << "    for i = 1, a do\n"
            << "        table.insert(t.list, i * 2)\n"
            << "        if i % 3 == 0 then t.x += i else t.x -= 1 end\n"
            << "    end\n"
            << "    local name = string.format(\"%s-%d\", b, #t.list)\n"
            << "    local part = workspace:FindFirstChild(name)\n"
            << "    if part then print(part.Name) end\n"
            << "    local bad: number = \"oops\"\n"
            << "    undefinedThing" << f << "()\n"
            << "    return t.x + #name\n"
            << "end\n"
            << "print(fn" << f << "_" << salt << "(" << f << ", \"k\"))\n";
    }
    return out.str();
}
```

At the end of the file:

```cpp
TEST_CASE("A34 an editor request is answered while a long check runs", "[A34]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& small = add_script(rig.game, "Small", "local x = 1\n");
    settle(analysis);
    add_script(rig.game, "Huge", long_source(60000, false, 0).c_str());
    // Past the debounce, so the place checker is inside Huge's check.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(analysis.busy());
    const engine_core::LuauFacts asked =
        analysis.luau_facts(completion_nodes(rig.game, small.id(), small.source()), small.id(), small.source(),
                            small.source().size(), {}, std::chrono::seconds(20));
    REQUIRE(asked.ran);
    // Answered before Huge's check finished, not after it.
    REQUIRE(analysis.busy());
    settle(analysis);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[A34]"`
Expected: FAIL at the second `REQUIRE(analysis.busy())`. Today the one worker finishes Huge before it answers the request.

- [ ] **Step 3: Split the worker**

In `struct ScriptAnalysis::State`:
- replace `StackThread worker;` with:

```cpp
    // The place checker's thread, and the editor checker's, which answers Luau
    // requests so typing never waits behind a check of the place.
    StackThread place;
    StackThread editor;
    // Wakes the editor thread for completions. `cv` wakes the place thread.
    std::condition_variable editor_cv;
```

Replace `ensure_worker`'s thread start with:

```cpp
    state_->place = StackThread(kWorkerStackBytes, [this] { run_place(); });
    state_->editor = StackThread(kWorkerStackBytes, [this] { run_editor(); });
```

In `shutdown`, add `state_->editor_cv.notify_all();` after `state_->cv.notify_all();`. Replace the join with:

```cpp
    if (state_->place.joinable()) {
        state_->place.join();
    }
    if (state_->editor.joinable()) {
        state_->editor.join();
    }
```

In `queue_luau`, change `state_->cv.notify_all();` to `state_->editor_cv.notify_all();`.

Replace `ScriptAnalysis::run()` with two functions:

```cpp
void ScriptAnalysis::run_editor() {
    // Its own frontend: an unsaved buffer never reaches the place checker's cache.
    auto owned = std::make_unique<WorkerEnv>();
    owned->init();
    while (true) {
        std::shared_ptr<CompleteRequest> request;
        {
            std::unique_lock<std::mutex> lock(state_->mu);
            state_->editor_cv.wait(lock, [&] { return state_->stop || !state_->completions.empty(); });
            if (state_->stop) {
                return;
            }
            request = std::move(state_->completions.front());
            state_->completions.pop_front();
            state_->serving = request;
        }
        if (owned->revision != lua_registry_revision()) {
            owned = std::make_unique<WorkerEnv>();
            owned->init();
        }
        request->answer->facts = facts_job(*owned, *request);
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            state_->serving.reset();
        }
        finish_request(*request);
    }
}

void ScriptAnalysis::run_place() {
    // This thread never calls the play VM and never takes the DataModel lock.
    // Jobs carry a copy of Source and Name taken on the gameplay thread.
    auto owned = std::make_unique<WorkerEnv>();
    owned->init();
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(state_->mu);
            state_->cv.wait(lock, [&] { return state_->stop || !state_->pending.empty(); });
            if (state_->stop) {
                return;
            }
            // From here to the end of the block: today's `run()` code from
            // `const auto now = std::chrono::steady_clock::now();` through
            // `state_->running = job.id;`, unchanged.
        }
        if (owned->revision != lua_registry_revision()) {
            owned = std::make_unique<WorkerEnv>();
            owned->init();
        }
        // From here to the end of the loop: today's `run()` code from
        // `Finished finished = analyze_job(*owned, job);` to the end of its
        // loop body, unchanged.
    }
}
```

The two comments marking unchanged code refer to the current `run()` lines quoted in them. Copy those lines in verbatim.

In `ScriptAnalysis.hpp`, replace `void run();` with:

```cpp
    void run_editor();
    void run_place();
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[A34]" && ./build/sandbox -# "[#analysis_tests]"`
Expected: A34 passes; 33 analysis test cases pass (A26's `cached_modules() == 1` counts only the place checker).

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/ScriptAnalysis.hpp src/engine_core/ScriptAnalysis.cpp sandbox/analysis_tests.cpp
git commit -m "Answer editor requests on their own thread, beside the place checker"
```

---

### Task 5: Check the place in batches on the pool

The first thing this task proves is the spec's open risk: A35 shows Luau hands `customModuleCheck` each module's expression types under parallel checking. **If A35 cannot be made to pass, stop and report back before going further.** Section 4 of the spec depends on it.

**Files:**
- Modify: `src/engine_core/ScriptAnalysis.hpp`: constructor takes `threads`; add `threads()`, `checks()`, `reached()`; private `threads_`
- Modify: `src/engine_core/ScriptAnalysis.cpp`
- Test: `sandbox/analysis_tests.cpp`: helper `build_place`, A35–A39

**Interfaces:**
- Consumes: `AnalysisPool` (Task 2); `WorldSnap::find` (Task 1); `run_place` and `State::cv` (Task 4).
- Produces (public):
  - `explicit ScriptAnalysis(DataModel& game, unsigned threads = 0);`
  - `unsigned threads() const;`
  - `std::uint64_t checks(InstanceId script) const;`: how many results `pump()` has published for the script.
  - `std::vector<InstanceId> reached(InstanceId script) const;`: its last published reached set, sorted.
- Produces (internal, `ScriptAnalysis.cpp` anonymous namespace, used by Task 6):
  - `struct CheckInput`, `void prepare_script(const WorkerEnv&, const WorldSnap&, CheckInput&)`
  - `void add_type_errors(const Luau::Module&, const CheckInput&, Luau::FileResolver&, std::vector<Diagnostic>&)`
  - `std::vector<InstanceId> reached_instances(const Luau::Module&)`
  - `void add_dependents(const Luau::Frontend&, const std::string& name, std::unordered_set<std::string>& out)`
  - `struct PlaceChecker { std::unique_ptr<WorkerEnv> env; std::mutex reached_mu; std::unordered_map<InstanceId, std::vector<InstanceId>> reached; }`
  - `using Claimed = std::unordered_map<InstanceId, std::uint64_t>;`
  - `struct BatchHost { std::function<std::optional<std::uint64_t>(InstanceId)> claim; std::function<void(Finished)> publish; }`
  - `void run_batch(PlaceChecker&, AnalysisPool&, const std::shared_ptr<const WorldSnap>&, Claimed&, const std::shared_ptr<Luau::FrontendCancellationToken>&, const BatchHost&)`
  - `State::latest_world`, `State::in_batch`, `State::batch_cancel`, `State::checks`, `State::adopt(world, seq)`

- [ ] **Step 1: Write the failing tests**

In `sandbox/analysis_tests.cpp`, add at the end of the file:

```cpp
namespace {

// Ten modules, every other one with a strict-mode error, and twenty scripts that
// require them and each have a type error and an unknown global. Returns the
// modules' ids, then the scripts'.
std::vector<engine_core::InstanceId> build_place(engine_core::DataModel& game) {
    std::vector<engine_core::InstanceId> ids;
    for (int i = 0; i < 10; ++i) {
        const std::string name = "Mod" + std::to_string(i);
        const char* source = i % 2 == 0 ? "local M = {}\n"
                                          "function M.add(a: number, b: number): number\n"
                                          "    return a + b\n"
                                          "end\n"
                                          "return M\n"
                                        : "--!strict\n"
                                          "local M = {}\n"
                                          "local wrong: number = \"x\"\n"
                                          "function M.add(a: number, b: number): number\n"
                                          "    return a + b + wrong\n"
                                          "end\n"
                                          "return M\n";
        ids.push_back(add_module(game, name.c_str(), source).id());
    }
    for (int j = 0; j < 20; ++j) {
        const std::string name = "User" + std::to_string(j);
        const std::string source = "local M = require(workspace.Mod" + std::to_string(j % 10) +
                                   ")\nprint(M.add(1, \"two\"))\nprint(undefined" + std::to_string(j) + ")\n";
        ids.push_back(add_script(game, name.c_str(), source.c_str()).id());
    }
    return ids;
}

std::vector<std::string> place_report(unsigned threads) {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game, threads);
    REQUIRE(analysis.threads() == threads);
    const std::vector<engine_core::InstanceId> ids = build_place(rig.game);
    settle(analysis);
    std::vector<std::string> out;
    for (engine_core::InstanceId id : ids) {
        out.push_back(std::string(rig.game.name(id)) + "\n" + dump(analysis.diagnostics(id)));
    }
    return out;
}

bool contains(const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

TEST_CASE("A35 a script's reached set is the instances its expressions are typed as", "[A35]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Folder& lights = rig.game.create<engine_core::Folder>();
    rig.game.set_name(lights.id(), "Lights");
    rig.game.set_parent(lights.id(), workspace_of(rig.game));
    engine_core::GameObject& lamp = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(lamp.id(), "Lamp");
    rig.game.set_parent(lamp.id(), lights.id());
    engine_core::GameObject& crate = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(crate.id(), "Crate");
    rig.game.set_parent(crate.id(), workspace_of(rig.game));
    // Lamp is reached only through a parameter and FindFirstChild.
    engine_core::Script& script = add_script(rig.game, "Show",
                                             "local folder = workspace.Lights\n"
                                             "local function show(f: typeof(folder))\n"
                                             "    print(f:FindFirstChild(\"Lamp\"))\n"
                                             "end\n"
                                             "show(folder)\n");
    settle(analysis);
    const std::vector<engine_core::InstanceId> reached = analysis.reached(script.id());
    REQUIRE(contains(reached, workspace_of(rig.game)));
    REQUIRE(contains(reached, lights.id()));
    REQUIRE(contains(reached, lamp.id()));
    REQUIRE_FALSE(contains(reached, crate.id()));
    REQUIRE(analysis.checks(script.id()) >= 1);
}

TEST_CASE("A36 checking on several threads finds what checking on one does", "[A36]") {
    const std::vector<std::string> serial = place_report(1);
    const std::vector<std::string> parallel = place_report(4);
    REQUIRE(serial == parallel);
    // The first script found its type error, so the comparison is about something.
    INFO(serial[10]);
    REQUIRE(serial[10].find("Type") != std::string::npos);
}

TEST_CASE("A37 turning analysis off or destroying it during a batch stops cleanly", "[A37]") {
    ScriptRig rig;
    SECTION("off during a batch, then on again") {
        engine_core::ScriptAnalysis analysis(rig.game);
        const std::vector<engine_core::InstanceId> ids = build_place(rig.game);
        add_script(rig.game, "Huge", long_source(20000, false, 0).c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        analysis.set_enabled(false);
        settle(analysis);
        REQUIRE(analysis.diagnostics().empty());
        analysis.set_enabled(true);
        settle(analysis);
        REQUIRE_FALSE(analysis.diagnostics(ids[10]).empty());
    }
    SECTION("destroyed during a batch") {
        auto analysis = std::make_unique<engine_core::ScriptAnalysis>(rig.game);
        build_place(rig.game);
        add_script(rig.game, "Huge", long_source(20000, false, 0).c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto started = std::chrono::steady_clock::now();
        analysis.reset();
        REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    }
}

TEST_CASE("A38 a require cycle and a module with a syntax error still finish", "[A38]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game, 4);
    engine_core::ModuleScript& a = add_module(rig.game, "A", "local B = require(script.Parent.B)\nreturn {}\n");
    engine_core::ModuleScript& b = add_module(rig.game, "B", "local A = require(script.Parent.A)\nreturn {}\n");
    engine_core::ModuleScript& broken = add_module(rig.game, "Broken", "return {\n");
    engine_core::Script& user =
        add_script(rig.game, "User", "local Broken = require(workspace.Broken)\nprint(Broken, undefinedName)\n");
    settle(analysis);
    REQUIRE(analysis.analyzed_source(a.id()).has_value());
    REQUIRE(analysis.analyzed_source(b.id()).has_value());
    INFO(dump(analysis.diagnostics(broken.id())));
    REQUIRE(has_code(analysis.diagnostics(broken.id()), "Syntax"));
    INFO(dump(analysis.diagnostics(user.id())));
    REQUIRE(has_code(analysis.diagnostics(user.id()), "Lint/UnknownGlobal"));
}

TEST_CASE("A39 a script edited while a batch checks it ends with its newest source", "[A39]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game, 4);
    add_script(rig.game, "Huge", long_source(20000, false, 0).c_str());
    engine_core::Script& script = add_script(rig.game, "Edited", "print(first)\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    script.set_source("print(second)\n");
    settle(analysis);
    REQUIRE(analysis.analyzed_source(script.id()) == std::optional<std::string>("print(second)\n"));
    const std::string report = dump(analysis.diagnostics(script.id()));
    INFO(report);
    REQUIRE(report.find("second") != std::string::npos);
    REQUIRE(report.find("first") == std::string::npos);
}
```

If `<algorithm>` and `<memory>` are not already included at the top of `analysis_tests.cpp`, add them.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build --target sandbox -j8`
Expected: compile errors: no `ScriptAnalysis(DataModel&, unsigned)`, no `threads`, `reached` or `checks`.

- [ ] **Step 3: Public API in `ScriptAnalysis.hpp`**

Replace the constructor declaration:

```cpp
    // `threads` type-check the place at once; 0 means one fewer than the hardware has, and at least one.
    explicit ScriptAnalysis(DataModel& game, unsigned threads = 0);
```

After `print_report`:

```cpp
    unsigned threads() const;
    // How many results pump() has published for this script. For tests.
    std::uint64_t checks(InstanceId script) const;
    // The instances the script's last published check typed an expression as,
    // sorted. A tree change at one of them, or among its children, rechecks it.
    std::vector<InstanceId> reached(InstanceId script) const;
```

In `private:`, after `DiagnosticsSignal signal_;`, add `unsigned threads_;`. It is declared after `signal_`, so the constructor's initializer list (Step 8) names it last.

- [ ] **Step 4: Make the type hooks thread-safe and unable to throw**

In `ScriptAnalysis.cpp`:

1. Delete `InstanceId self = 0;` from `WorkerEnv`, and every `env.self = ...;` line (in `analyze_job`, which Step 7 deletes anyway, and in `with_checked_buffer`).
2. Rename `FindChildMagic::infer` to `FindChildMagic::infer_child`, declared `bool infer_child(const Luau::MagicFunctionCallContext& context);` in the struct. Replace its first lines, from `InstanceId self = env->self;` through the closing `}` of the `moduleName` block, with:

```cpp
    // Modules are checked on several threads, a required one in the same pass,
    // so `script` is whichever module this call is in.
    if (context.constraint->moduleName == nullptr) {
        return false;
    }
    const std::optional<InstanceId> owner = instance_of_module(*context.constraint->moduleName);
    if (!owner) {
        return false;
    }
    const InstanceId self = *owner;
```

and add:

```cpp
// On a pool thread an exception escaping a Luau task leaves checkQueuedModules
// waiting for it forever, so a hook that fails keeps the declared type instead.
bool FindChildMagic::infer(const Luau::MagicFunctionCallContext& context) {
    try {
        return infer_child(context);
    } catch (...) {
        return false;
    }
}
```

3. Do the same for `NarrowMagic`: rename its body to `infer_narrow` and wrap it in an `infer` with the same `try`/`catch (...)`.
4. In `WorkerEnv::init`, wrap the whole body of the `prepareModuleScope` lambda in `try { ... } catch (...) { }`.

- [ ] **Step 5: Teach the resolver a batch's sources**

In `SourceFileResolver`, add after `const WorldSnap* world = nullptr;`:

```cpp
    // While the place checker runs a batch: what Luau checks for each script in
    // it, by module name, with a header --!nonstrict made --!strict.
    const std::unordered_map<std::string, std::string>* batch_sources = nullptr;
```

At the top of `readSource`, before the `module_name` check:

```cpp
        if (batch_sources != nullptr) {
            const auto found = batch_sources->find(name);
            if (found != batch_sources->end()) {
                const std::optional<InstanceId> id = instance_of_module(name);
                const NodeSnap* node = id && world != nullptr ? world->find(*id) : nullptr;
                const bool module = node != nullptr && node->module;
                return Luau::SourceCode{found->second, module ? Luau::SourceCode::Module : Luau::SourceCode::Script};
            }
        }
```

- [ ] **Step 6: Add the batch pieces**

In `ScriptAnalysis.cpp`, change `Job` and `Finished` near the top:

```cpp
struct Job {
    InstanceId id = 0;
    std::uint64_t generation = 0;
};

struct Finished {
    InstanceId id = 0;
    std::uint64_t generation = 0;
    std::string name;
    std::string source;
    std::vector<Diagnostic> diagnostics;
    std::vector<InstanceId> requires;
    std::vector<InstanceId> reached;
};
```

Before `ScriptAnalysis::State` (after `facts_job`), add:

```cpp
// One script in a batch: stages 1 and 2 of its check, and what the type check
// needs. prepare_script reads only the snapshot and the frozen globals, so a
// batch prepares its scripts in parallel.
struct CheckInput {
    InstanceId id = 0;
    std::uint64_t generation = 0;
    std::string name;
    std::string source;
    std::string module_name;
    std::string check_source;
    Luau::Mode mode = Luau::Mode::Nonstrict;
    // False after a syntax error, under --!nocheck, or when analysis cannot run.
    bool type_check = false;
    std::vector<Diagnostic> diagnostics;
    std::vector<InstanceId> requires;
};

void prepare_script(const WorkerEnv& env, const WorldSnap& world, CheckInput& input) {
    const NodeSnap* self = world.find(input.id);
    if (self == nullptr || !self->lua) {
        return;
    }
    input.name = self->name.empty() ? self->class_name : self->name;
    input.source = self->source;
    input.module_name = module_name_of(input.id);

    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseOptions parse_options;
    parse_options.captureComments = true;
    const Luau::ParseResult parsed =
        Luau::Parser::parse(self->source.c_str(), self->source.size(), names, allocator, parse_options);

    // Stage 1. A parse failure stops the pipeline. The compiler is unchanged.
    if (!parsed.errors.empty()) {
        for (const Luau::ParseError& error : parsed.errors) {
            input.diagnostics.push_back(make_diagnostic(input.id, range_from(error.getLocation()), Severity::Error,
                                                        "Syntax", error.getMessage()));
        }
        if (parsed.root != nullptr) {
            input.requires = find_requires(world, input.id, parsed.root);
        }
        return;
    }

    // A script without a `--!` mode comment on its first lines is checked nonstrict.
    input.mode = Luau::parseMode(parsed.hotcomments).value_or(Luau::Mode::Nonstrict);
    if (parsed.root != nullptr) {
        input.requires = find_requires(world, input.id, parsed.root);
    }
    if (input.mode == Luau::Mode::NoCheck) {
        return;
    }
    if (!env.init_error.empty() || env.frontend == nullptr || env.frontend->globals.globalScope == nullptr) {
        const std::string message = env.init_error.empty() ? "script analysis is unavailable" : env.init_error;
        input.diagnostics.push_back(make_diagnostic(input.id, TextRange{}, Severity::Error, "Analysis", message));
        return;
    }

    // Stage 2. Builtin lints. Unknown globals are left to the type checker.
    Luau::LintOptions lint_options;
    lint_options.setDefaults();
    lint_options.warningMask &= ~Luau::LintWarning::parseMask(parsed.hotcomments);
    lint_options.disableWarning(Luau::LintWarning::Code_UnknownGlobal);
    if (input.mode == Luau::Mode::Strict) {
        lint_options.disableWarning(Luau::LintWarning::Code_ImplicitReturn);
    }
    const std::vector<Luau::LintWarning> lints = Luau::lint(parsed.root, names, env.frontend->globals.globalScope,
                                                            env.untyped.get(), parsed.hotcomments, lint_options);
    for (const Luau::LintWarning& warning : lints) {
        Severity severity = Severity::Warning;
        if (warning.code == Luau::LintWarning::Code_LocalUnused ||
            warning.code == Luau::LintWarning::Code_FunctionUnused ||
            warning.code == Luau::LintWarning::Code_ImportUnused) {
            severity = Severity::Hint;
        }
        const char* name = Luau::LintWarning::getName(warning.code);
        input.diagnostics.push_back(make_diagnostic(input.id, range_from(warning.location), severity,
                                                    std::string("Lint/") + (name != nullptr ? name : "Unknown"),
                                                    warning.text));
    }
    input.check_source = checked_source(self->source, parsed, input.mode);
    input.type_check = true;
}

// Stage 3's diagnostics: the module's own type errors. The full checker runs for
// strict and nonstrict alike; nonstrict then reads type errors as warnings.
void add_type_errors(const Luau::Module& module, const CheckInput& input, Luau::FileResolver& files,
                     std::vector<Diagnostic>& out) {
    Luau::TypeErrorToStringOptions stringify;
    stringify.fileResolver = &files;
    for (const Luau::TypeError& error : module.errors) {
        // A required module reports its own problems when it is analyzed.
        if (error.moduleName != input.module_name) {
            continue;
        }
        if (Luau::get<Luau::SyntaxError>(error) != nullptr) {
            out.push_back(make_diagnostic(input.id, range_from(error.location), Severity::Error, "Syntax",
                                          Luau::toString(error, stringify)));
            continue;
        }
        if (const Luau::UnknownSymbol* symbol = Luau::get<Luau::UnknownSymbol>(error)) {
            if (symbol->context == Luau::UnknownSymbol::Binding) {
                out.push_back(make_diagnostic(input.id, range_from(error.location), Severity::Warning,
                                              "Lint/UnknownGlobal", Luau::toString(error, stringify)));
                continue;
            }
        }
        Severity severity = input.mode == Luau::Mode::Strict ? Severity::Error : Severity::Warning;
        if (missing_render_member(error)) {
            severity = Severity::Warning;
        }
        out.push_back(
            make_diagnostic(input.id, range_from(error.location), severity, "Type", Luau::toString(error, stringify)));
    }
}

// Every instance one of the module's expressions is typed as, directly or as an
// option of a union or intersection, as Door? is. Locals, parameters, script,
// and FindFirstChild all end up here, because this reads what Luau inferred.
std::vector<InstanceId> reached_instances(const Luau::Module& module) {
    std::vector<InstanceId> out;
    std::unordered_set<InstanceId> seen;
    const auto take = [&](Luau::TypeId type) {
        if (const std::optional<InstanceId> id = tagged_instance(type)) {
            if (seen.insert(*id).second) {
                out.push_back(*id);
            }
        }
    };
    for (const auto& entry : module.astTypes) {
        const Luau::TypeId type = Luau::follow(entry.second);
        take(type);
        if (const Luau::UnionType* options = Luau::get<Luau::UnionType>(type)) {
            for (Luau::TypeId option : options->options) {
                take(option);
            }
        } else if (const Luau::IntersectionType* parts = Luau::get<Luau::IntersectionType>(type)) {
            for (Luau::TypeId part : parts->parts) {
                take(part);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// `name` and every module that requires it, however deep, as far as the
// frontend knows. markDirty's own list stops at modules already dirty, so it
// cannot say this.
void add_dependents(const Luau::Frontend& frontend, const std::string& name, std::unordered_set<std::string>& out) {
    std::vector<std::string> stack{name};
    while (!stack.empty()) {
        std::string next = std::move(stack.back());
        stack.pop_back();
        if (!out.insert(next).second) {
            continue;
        }
        const auto node = frontend.sourceNodes.find(next);
        if (node == frontend.sourceNodes.end() || node->second == nullptr) {
            continue;
        }
        for (const std::string& dependent : node->second->dependents) {
            stack.push_back(dependent);
        }
    }
}

// The place checker's own state, kept between batches on its thread.
struct PlaceChecker {
    std::unique_ptr<WorkerEnv> env;
    // Each script's reached set from its last check. Written from pool threads.
    std::mutex reached_mu;
    std::unordered_map<InstanceId, std::vector<InstanceId>> reached;
};

// The scripts a batch checks, each with its generation when the batch took it.
using Claimed = std::unordered_map<InstanceId, std::uint64_t>;

struct BatchHost {
    // Takes a script into the batch and returns its generation now, or nothing
    // when it is waiting for its own batch with a newer source.
    std::function<std::optional<std::uint64_t>(InstanceId)> claim;
    // Hands one script's result to pump(). Any thread.
    std::function<void(Finished)> publish;
};

Finished finished_from(const CheckInput& input) {
    Finished finished;
    finished.id = input.id;
    finished.generation = input.generation;
    finished.name = input.name;
    finished.source = input.source;
    finished.diagnostics = input.diagnostics;
    finished.requires = input.requires;
    return finished;
}

void run_batch(PlaceChecker& checker, AnalysisPool& pool, const std::shared_ptr<const WorldSnap>& world,
               Claimed& claimed, const std::shared_ptr<Luau::FrontendCancellationToken>& cancel,
               const BatchHost& host) {
    WorkerEnv& env = *checker.env;
    sync_world(env, world, true);

    // Every module the batch changes: the scripts taken, and what requires them.
    std::unordered_set<std::string> names;
    for (const auto& entry : claimed) {
        add_dependents(*env.frontend, module_name_of(entry.first), names);
    }
    std::vector<CheckInput> inputs;
    inputs.reserve(names.size());
    for (const std::string& name : names) {
        const std::optional<InstanceId> id = instance_of_module(name);
        const NodeSnap* node = id ? world->find(*id) : nullptr;
        if (node == nullptr || !node->lua) {
            continue;
        }
        if (claimed.count(*id) == 0) {
            const std::optional<std::uint64_t> generation = host.claim(*id);
            if (!generation) {
                continue;
            }
            claimed.emplace(*id, *generation);
        }
        CheckInput input;
        input.id = *id;
        input.generation = claimed.at(*id);
        inputs.push_back(std::move(input));
        env.frontend->markDirty(name);
    }

    std::vector<std::function<void()>> prepare;
    prepare.reserve(inputs.size());
    for (CheckInput& input : inputs) {
        prepare.push_back([&env, &world, &input] { prepare_script(env, *world, input); });
    }
    pool.run_all(std::move(prepare));

    std::unordered_map<std::string, std::string> sources;
    std::unordered_map<InstanceId, const CheckInput*> by_id;
    std::vector<Luau::ModuleName> queue;
    for (const CheckInput& input : inputs) {
        by_id.emplace(input.id, &input);
        if (!input.type_check) {
            host.publish(finished_from(input));
            continue;
        }
        sources.emplace(input.module_name, input.check_source);
        queue.push_back(input.module_name);
    }
    if (queue.empty() || cancel->requested()) {
        return;
    }

    std::mutex done_mu;
    std::unordered_set<InstanceId> done;
    env.files.batch_sources = &sources;
    env.files.world = world.get();
    env.world = world.get();
    Luau::FrontendOptions options;
    options.runLintChecks = false;
    options.retainFullTypeGraphs = false;
    options.cancellationToken = cancel;
    // Runs on a pool thread as each module finishes, before Luau drops its
    // expression types, so its reached set can still be read.
    options.customModuleCheck = [&](const Luau::SourceModule& source, const Luau::Module& module) {
        const std::optional<InstanceId> id = instance_of_module(source.name);
        const auto found = id ? by_id.find(*id) : by_id.end();
        if (found == by_id.end() || !found->second->type_check || module.cancelled) {
            return;
        }
        const CheckInput& input = *found->second;
        Finished finished = finished_from(input);
        add_type_errors(module, input, env.files, finished.diagnostics);
        finished.reached = reached_instances(module);
        {
            std::lock_guard<std::mutex> lock(checker.reached_mu);
            checker.reached[input.id] = finished.reached;
        }
        {
            std::lock_guard<std::mutex> lock(done_mu);
            done.insert(input.id);
        }
        host.publish(std::move(finished));
    };
    try {
        env.frontend->queueModuleCheck(queue);
        env.frontend->checkQueuedModules(
            options, [&pool](std::vector<std::function<void()>> tasks) { pool.post(std::move(tasks)); });
    } catch (...) {
        // One module's internal error stops Luau's whole batch. What did not
        // finish is checked one at a time, so only the broken one says so.
        for (const std::string& name : queue) {
            if (cancel->requested()) {
                break;
            }
            const InstanceId id = *instance_of_module(name);
            {
                std::lock_guard<std::mutex> lock(done_mu);
                if (done.count(id) != 0) {
                    continue;
                }
            }
            std::string failure;
            try {
                env.frontend->check(name, options);
                continue;
            } catch (const std::exception& error) {
                failure = error.what();
            } catch (...) {
                failure = "analysis failed";
            }
            if (cancel->requested()) {
                break;
            }
            Finished finished = finished_from(*by_id.at(id));
            finished.diagnostics.push_back(
                make_diagnostic(id, TextRange{}, Severity::Error, "Analysis", "could not be checked: " + failure));
            host.publish(std::move(finished));
        }
    }
    env.files.batch_sources = nullptr;
    env.files.world = nullptr;
    env.world = nullptr;
}
```

Add `#include "AnalysisPool.hpp"` with the other project includes, and `#include <algorithm>` if it is missing.

- [ ] **Step 7: Delete the per-script job path**

Delete `analyze_job`. In `State`, delete `tokens`, `inflight` and `running`, and add:

```cpp
    // The newest tree captured, and its capture number: two captures can finish
    // out of order, and the newer one wins.
    std::shared_ptr<const WorldSnap> latest_world;
    std::uint64_t latest_seq = 0;
    std::atomic<std::uint64_t> world_seq{0};
    // Scripts in the batch the place checker is running, including what
    // requires them, and the batch's cancel.
    std::unordered_set<InstanceId> in_batch;
    std::shared_ptr<Luau::FrontendCancellationToken> batch_cancel;
    // Results pump() published, per script.
    std::unordered_map<InstanceId, std::uint64_t> checks;

    void adopt(std::shared_ptr<const WorldSnap> world, std::uint64_t seq) {
        if (seq > latest_seq) {
            latest_world = std::move(world);
            latest_seq = seq;
        }
    }
```

Add `std::vector<InstanceId> reached;` to `State::Record`.

Replace `schedule`'s body:

```cpp
void ScriptAnalysis::schedule(const std::vector<InstanceId>& ids) {
    if (ids.empty()) {
        return;
    }
    {
        // The capture copies every script's source. Skip it when nothing will run.
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            return;
        }
    }
    const std::uint64_t seq = state_->world_seq.fetch_add(1) + 1;
    const std::shared_ptr<WorldSnap> world = capture_world(game_);
    ensure_worker();
    std::lock_guard<std::mutex> lock(state_->mu);
    if (!state_->enabled || state_->stop) {
        return;
    }
    state_->adopt(world, seq);
    const auto ready_at = std::chrono::steady_clock::now() + kDebounce;
    for (InstanceId id : ids) {
        // Handled either way: a dead or non-script id has nothing to check.
        const NodeSnap* node = world->find(id);
        if (node == nullptr || !node->lua) {
            continue;
        }
        std::uint64_t& generation = state_->generations[id];
        ++generation;
        Job job;
        job.id = id;
        job.generation = generation;
        state_->pending[id] = Pending{job, ready_at};
    }
    state_->cv.notify_all();
}
```

Replace `run_place`:

```cpp
void ScriptAnalysis::run_place() {
    // This thread and the pool never call the play VM and never take the
    // DataModel lock. They read snapshots captured on the gameplay thread.
    PlaceChecker checker;
    checker.env = std::make_unique<WorkerEnv>();
    checker.env->init();
    AnalysisPool pool(threads_, kWorkerStackBytes);
    BatchHost host;
    host.claim = [this](InstanceId id) -> std::optional<std::uint64_t> {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (state_->pending.count(id) != 0) {
            return std::nullopt;
        }
        state_->in_batch.insert(id);
        return state_->generations[id];
    };
    host.publish = [this](Finished finished) {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (state_->stop || !state_->enabled) {
            return;
        }
        const auto generation = state_->generations.find(finished.id);
        if (generation == state_->generations.end() || generation->second != finished.generation) {
            return;
        }
        state_->results.push_back(std::move(finished));
    };
    while (true) {
        Claimed claimed;
        std::shared_ptr<const WorldSnap> world;
        std::shared_ptr<Luau::FrontendCancellationToken> cancel;
        {
            std::unique_lock<std::mutex> lock(state_->mu);
            state_->cv.wait(lock, [&] { return state_->stop || !state_->pending.empty(); });
            if (state_->stop) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            auto next = std::chrono::steady_clock::time_point::max();
            for (auto it = state_->pending.begin(); it != state_->pending.end();) {
                if (it->second.ready_at <= now) {
                    claimed.emplace(it->first, it->second.job.generation);
                    state_->in_batch.insert(it->first);
                    it = state_->pending.erase(it);
                } else {
                    next = std::min(next, it->second.ready_at);
                    ++it;
                }
            }
            if (claimed.empty()) {
                state_->cv.wait_until(lock, next);
                continue;
            }
            world = state_->latest_world;
            cancel = std::make_shared<Luau::FrontendCancellationToken>();
            state_->batch_cancel = cancel;
        }
        if (checker.env->revision != lua_registry_revision()) {
            checker.env = std::make_unique<WorkerEnv>();
            checker.env->init();
            std::lock_guard<std::mutex> lock(checker.reached_mu);
            checker.reached.clear();
        }
        if (world != nullptr) {
            run_batch(checker, pool, world, claimed, cancel, host);
        }
        if (checker.env->frontend != nullptr) {
            state_->cached_modules.store(checker.env->frontend->sourceNodes.size(), std::memory_order_relaxed);
        }
        std::lock_guard<std::mutex> lock(state_->mu);
        for (const auto& entry : claimed) {
            state_->in_batch.erase(entry.first);
        }
        if (state_->batch_cancel == cancel) {
            state_->batch_cancel.reset();
        }
    }
}
```

`kWorkerStackBytes` is defined just above `ensure_worker`, before `run_editor` and `run_place`, so both see it as they are.

- [ ] **Step 8: Update the state functions**

- Constructor: `ScriptAnalysis::ScriptAnalysis(DataModel& game, unsigned threads) : game_(game), state_(std::make_unique<State>()), threads_(threads == 0 ? AnalysisPool::default_size() : threads) {`. Keep the body.
- `shutdown`: replace the `for (auto& entry : state_->tokens) ...` loop and `state_->tokens.clear();` with `if (state_->batch_cancel) { state_->batch_cancel->cancel(); }`.
- `set_enabled(false)`: replace the `tokens` loop and `state_->tokens.clear();` the same way.
- `remove`: delete the `token` lookup, its cancel, and `state_->tokens.erase(script);`.
- `pump`: in the loop that publishes `ready`, after `record.diagnostics = std::move(finished.diagnostics);` add:

```cpp
            record.reached = std::move(finished.reached);
            ++state_->checks[finished.id];
```

- `busy`: `return !state_->pending.empty() || !state_->in_batch.empty();` after the `world_stale` check.
- `idle`: `return state_->pending.empty() && state_->in_batch.empty() && state_->results.empty();` after the `world_stale` check.
- `settled`: replace `state_->running == script` with `state_->in_batch.count(script) != 0`.
- Add:

```cpp
unsigned ScriptAnalysis::threads() const { return threads_; }

std::uint64_t ScriptAnalysis::checks(InstanceId script) const {
    std::lock_guard<std::mutex> lock(state_->mu);
    const auto found = state_->checks.find(script);
    return found == state_->checks.end() ? 0 : found->second;
}

std::vector<InstanceId> ScriptAnalysis::reached(InstanceId script) const {
    std::lock_guard<std::mutex> lock(state_->mu);
    const auto found = state_->published.find(script);
    return found == state_->published.end() ? std::vector<InstanceId>{} : found->second.reached;
}
```

- Update the class comment in `ScriptAnalysis.hpp`: replace `A background worker parses, lints, and typechecks that copy.` with `A coordinator thread takes due scripts in batches and parses, lints, and type-checks them on a pool of threads; a second thread answers Luau requests from editors.`

- [ ] **Step 9: Run the tests**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[A35]"`
Expected: PASS. **If it fails because `reached` is empty, stop here and report:** `customModuleCheck` did not see expression types.

Then run `./build/sandbox "[A36],[A37],[A38],[A39]" && ./build/sandbox -# "[#analysis_tests]"`.
Expected: all pass, 38 analysis test cases.

- [ ] **Step 10: Run the rest and ThreadSanitizer**

Run:
```bash
cmake --build build --target studio-tests mcp-tests engine-tests -j8 && ./build/studio-tests && ./build/mcp-tests && ./build/engine-tests
cmake --build build-tsan --target sandbox -j8 && ./build-tsan/sandbox -# "[#analysis_tests]"
```
Expected: all pass; no `WARNING: ThreadSanitizer` in the TSAN run's output.

- [ ] **Step 11: Commit**

```bash
git add src/engine_core/ScriptAnalysis.hpp src/engine_core/ScriptAnalysis.cpp sandbox/analysis_tests.cpp
git commit -m "Type-check the place in batches on a thread pool, publishing each script as it finishes"
```

---

### Task 6: A tree change rechecks only what it reaches

**Files:**
- Modify: `src/engine_core/ScriptAnalysis.hpp`: private `void note_tree();`
- Modify: `src/engine_core/ScriptAnalysis.cpp`: place types updated in place, `sync_place`, `run_batch`, `run_place`, `pump`, `busy`, `idle`, `settled`
- Test: `sandbox/analysis_tests.cpp`: A40, A41

**Interfaces:**
- Consumes: `diff_worlds`, `TreeDiff` (Task 1); `PlaceChecker`, `run_batch`, `add_dependents`, `State::adopt`, `State::world_seq` (Task 5); `ScriptAnalysis::checks` (Task 5).
- Produces: `PlaceChecker::last_world`; `void add_instance_type(PlaceTypes&, const Luau::Scope&, const NodeSnap&)`; `void fill_instance_props(PlaceTypes&, InstanceId)`; `void update_place_types(PlaceTypes&, const Luau::Scope&, std::shared_ptr<const WorldSnap>, const TreeDiff&)`; `void sync_place(PlaceChecker&, const std::shared_ptr<const WorldSnap>&, std::unordered_set<std::string>&)`; `State::tree_pending`, `State::tree_due`.

- [ ] **Step 1: Write the failing tests**

At the end of `sandbox/analysis_tests.cpp`:

```cpp
TEST_CASE("A40 a module's instance types still match after a rename it does not reach", "[A40]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::GameObject& door = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(door.id(), "Door");
    rig.game.set_parent(door.id(), workspace_of(rig.game));
    engine_core::Folder& props = rig.game.create<engine_core::Folder>();
    rig.game.set_name(props.id(), "Props");
    rig.game.set_parent(props.id(), workspace_of(rig.game));
    engine_core::GameObject& crate = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(crate.id(), "Crate");
    rig.game.set_parent(crate.id(), props.id());
    engine_core::ModuleScript& doors =
        add_module(rig.game, "Doors", "--!strict\nlocal Doors = {}\nDoors.main = workspace.Door\nreturn Doors\n");
    engine_core::Script& user = add_script(rig.game, "User",
                                           "--!strict\n"
                                           "local Doors = require(workspace.Doors)\n"
                                           "local door: typeof(workspace.Door) = Doors.main\n"
                                           "print(door)\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(user.id())));
    REQUIRE(analysis.diagnostics(user.id()).empty());
    const std::uint64_t doors_checks = analysis.checks(doors.id());

    // Inside Props, which neither script reached: Doors keeps its cached types,
    // and User is checked again against them.
    rig.game.set_name(crate.id(), "Box");
    user.set_source(user.source() + "\n");
    settle(analysis);
    REQUIRE(analysis.checks(doors.id()) == doors_checks);
    INFO(dump(analysis.diagnostics(user.id())));
    REQUIRE(analysis.diagnostics(user.id()).empty());
}

TEST_CASE("A41 a change rechecks only the scripts it can affect", "[A41]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    const engine_core::InstanceId workspace = workspace_of(rig.game);
    const auto folder = [&rig](const char* name, engine_core::InstanceId parent) -> engine_core::Folder& {
        engine_core::Folder& made = rig.game.create<engine_core::Folder>();
        rig.game.set_name(made.id(), name);
        rig.game.set_parent(made.id(), parent);
        return made;
    };
    const auto part = [&rig](const char* name, engine_core::InstanceId parent) -> engine_core::GameObject& {
        engine_core::GameObject& made = rig.game.create<engine_core::GameObject>();
        rig.game.set_name(made.id(), name);
        rig.game.set_parent(made.id(), parent);
        return made;
    };
    engine_core::Folder& props = folder("Props", workspace);
    engine_core::GameObject& crate = part("Crate", props.id());
    engine_core::Folder& lights = folder("Lights", workspace);
    engine_core::GameObject& lamp = part("Lamp", lights.id());
    engine_core::Folder& extra = folder("Extra", workspace);
    engine_core::ModuleScript& util = add_module(rig.game, "Util",
                                                 "local Util = {}\n"
                                                 "function Util.add(a: number, b: number): number\n"
                                                 "    return a + b\n"
                                                 "end\n"
                                                 "return Util\n");
    engine_core::Script& uses_util =
        add_script(rig.game, "UsesUtil", "--!strict\nlocal Util = require(workspace.Util)\nprint(Util.add(1, 2))\n");
    engine_core::Script& uses_props =
        add_script(rig.game, "UsesProps", "--!strict\nlocal crate = workspace.Props.Crate\nprint(crate)\n");
    engine_core::Script& uses_lamp = add_script(rig.game, "UsesLamp",
                                                "--!strict\n"
                                                "local lights = workspace.Lights\n"
                                                "local function show(folder: typeof(lights))\n"
                                                "    print(folder.Lamp)\n"
                                                "end\n"
                                                "show(lights)\n");
    engine_core::Script& plain = add_script(rig.game, "Plain", "print(\"hi\")\n");
    settle(analysis);
    const std::vector<engine_core::InstanceId> watched{util.id(), uses_util.id(), uses_props.id(), uses_lamp.id(),
                                                       plain.id()};
    for (engine_core::InstanceId id : watched) {
        INFO(rig.game.name(id) << "\n" << dump(analysis.diagnostics(id)));
        REQUIRE(analysis.diagnostics(id).empty());
    }
    const auto counts = [&] {
        std::vector<std::uint64_t> out;
        for (engine_core::InstanceId id : watched) {
            out.push_back(analysis.checks(id));
        }
        return out;
    };
    // Which of util, uses_util, uses_props, uses_lamp, plain were checked again since `before`.
    const auto rechecked = [&](const std::vector<std::uint64_t>& before) {
        const std::vector<std::uint64_t> now = counts();
        std::vector<bool> out;
        for (std::size_t i = 0; i < now.size(); ++i) {
            out.push_back(now[i] != before[i]);
        }
        return out;
    };
    const std::vector<std::uint64_t> before = counts();

    SECTION("editing a module rechecks what requires it, and nothing else") {
        util.set_source("local Util = {}\nfunction Util.add(a: number): number\n    return a\nend\nreturn Util\n");
        settle(analysis);
        REQUIRE(rechecked(before) == std::vector<bool>{true, true, false, false, false});
        REQUIRE_FALSE(analysis.diagnostics(uses_util.id()).empty());
    }
    SECTION("a rename inside a folder rechecks only the scripts that reached the folder") {
        rig.game.set_name(crate.id(), "Box");
        settle(analysis);
        REQUIRE(rechecked(before) == std::vector<bool>{false, false, true, false, false});
        REQUIRE_FALSE(analysis.diagnostics(uses_props.id()).empty());
    }
    SECTION("a child reached through a parameter is followed both ways") {
        rig.game.set_name(lamp.id(), "Bulb");
        settle(analysis);
        REQUIRE(rechecked(before) == std::vector<bool>{false, false, false, true, false});
        REQUIRE_FALSE(analysis.diagnostics(uses_lamp.id()).empty());
        rig.game.set_name(lamp.id(), "Lamp");
        settle(analysis);
        REQUIRE(analysis.diagnostics(uses_lamp.id()).empty());
    }
    SECTION("a property change rechecks nothing") {
        crate.set_position(engine_core::Vec3{1.f, 2.f, 3.f});
        settle(analysis);
        REQUIRE(rechecked(before) == std::vector<bool>{false, false, false, false, false});
    }
    SECTION("a script added where nothing looked checks only itself") {
        engine_core::Script& added = add_script(rig.game, extra.id(), "Added", "print(1)\n");
        settle(analysis);
        REQUIRE(rechecked(before) == std::vector<bool>{false, false, false, false, false});
        REQUIRE(analysis.checks(added.id()) >= 1);
    }
    SECTION("destroying a module rechecks what required it") {
        rig.game.destroy(util.id());
        settle(analysis);
        REQUIRE(rechecked(before)[1]);
        REQUIRE_FALSE(rechecked(before)[4]);
        REQUIRE_FALSE(analysis.diagnostics(uses_util.id()).empty());
        REQUIRE_FALSE(analysis.analyzed_source(util.id()).has_value());
    }
    SECTION("destroying a folder of scripts drops them and rechecks what required them") {
        engine_core::Folder& group = folder("Group", extra.id());
        engine_core::ModuleScript& inner = add_module(rig.game, "Inner", "return { value = 1 }\n");
        rig.game.set_parent(inner.id(), group.id());
        engine_core::Script& uses_inner = add_script(rig.game, extra.id(), "UsesInner",
                                                     "--!strict\nlocal Inner = require(script.Parent.Group.Inner)\n"
                                                     "print(Inner.value)\n");
        settle(analysis);
        REQUIRE(analysis.diagnostics(uses_inner.id()).empty());
        const std::vector<std::uint64_t> grouped = counts();
        rig.game.destroy(group.id());
        settle(analysis);
        REQUIRE_FALSE(analysis.analyzed_source(inner.id()).has_value());
        REQUIRE_FALSE(analysis.diagnostics(uses_inner.id()).empty());
        REQUIRE(rechecked(grouped) == std::vector<bool>{false, false, false, false, false});
    }
    SECTION("undoing a rename rechecks what the rename did") {
        begin_step(rig.game, "Rename");
        rig.game.set_name(crate.id(), "Box");
        end_step(rig.game);
        settle(analysis);
        REQUIRE_FALSE(analysis.diagnostics(uses_props.id()).empty());
        const std::vector<std::uint64_t> renamed = counts();
        rig.game.history().undo();
        settle(analysis);
        REQUIRE(rechecked(renamed) == std::vector<bool>{false, false, true, false, false});
        REQUIRE(analysis.diagnostics(uses_props.id()).empty());
    }
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[A40],[A41]"`
Expected: FAIL. A40 fails `checks(doors) == doors_checks`, and A41 fails the rename sections: today every tree change rechecks every script.

- [ ] **Step 3: Place types updated in place**

In `ScriptAnalysis.cpp`, replace `build_place_types` with these four functions (the bodies come from its two loops):

```cpp
// A new extern type for the instance, extending its class, tagged with its id.
void add_instance_type(PlaceTypes& place, const Luau::Scope& globals, const NodeSnap& node) {
    std::optional<Luau::TypeId> base = class_type(globals, node.class_name);
    if (!base) {
        base = class_type(globals, "DataModel");
    }
    if (!base) {
        return;
    }
    const Luau::ExternType* base_class = Luau::get<Luau::ExternType>(*base);
    place.types[node.id] = place.arena.addType(Luau::ExternType{base_class->name, {}, *base, std::nullopt, {},
                                                                std::make_shared<InstanceTag>(node.id), "@anarchy",
                                                                std::nullopt});
}

// The instance's children and Parent as its type's fields, from place.world.
// The fields are rewritten, never the type: cached modules hold it.
void fill_instance_props(PlaceTypes& place, InstanceId id) {
    const NodeSnap* node = place.world->find(id);
    const std::optional<Luau::TypeId> own = place.find(id);
    if (node == nullptr || !own) {
        return;
    }
    Luau::ExternType* type = Luau::getMutable<Luau::ExternType>(*own);
    const Luau::ExternType* base_class = Luau::get<Luau::ExternType>(*type->parent);
    type->props.clear();
    for (InstanceId child : node->children) {
        const NodeSnap* child_node = place.world->find(child);
        const std::optional<Luau::TypeId> child_type = place.find(child);
        if (child_node == nullptr || !child_type) {
            continue;
        }
        const std::string& name = child_node->name;
        if (name.empty() || type->props.count(name) != 0 || Luau::lookupExternTypeProp(base_class, name) != nullptr) {
            continue;
        }
        type->props[name] = Luau::Property::readonly(*child_type);
    }
    const std::optional<Luau::TypeId> parent =
        node->parent != DataModel::kNoParent ? place.find(node->parent) : std::nullopt;
    if (!parent) {
        return;
    }
    const Luau::Property* declared = Luau::lookupExternTypeProp(base_class, "Parent");
    if (declared != nullptr && declared->writeTy) {
        type->props["Parent"] = Luau::Property::rw(*parent, *declared->writeTy);
    } else {
        type->props["Parent"] = Luau::Property::readonly(*parent);
    }
}

std::unique_ptr<PlaceTypes> build_place_types(const Luau::Scope& globals, std::shared_ptr<const WorldSnap> world) {
    auto place = std::make_unique<PlaceTypes>();
    place->world = std::move(world);
    for (const NodeSnap& node : place->world->nodes) {
        add_instance_type(*place, globals, node);
    }
    for (const NodeSnap& node : place->world->nodes) {
        fill_instance_props(*place, node.id);
    }
    return place;
}

// Brings the place's types to `world` between batches. New instances get
// types; instances the diff names get their fields rewritten; a destroyed one
// keeps its type with no fields, since cached modules may still hold it.
void update_place_types(PlaceTypes& place, const Luau::Scope& globals, std::shared_ptr<const WorldSnap> world,
                        const TreeDiff& diff) {
    place.world = std::move(world);
    std::vector<InstanceId> refill(diff.parents.begin(), diff.parents.end());
    refill.insert(refill.end(), diff.moved.begin(), diff.moved.end());
    for (const NodeSnap& node : place.world->nodes) {
        if (place.types.count(node.id) == 0) {
            add_instance_type(place, globals, node);
            refill.push_back(node.id);
        }
    }
    for (InstanceId id : refill) {
        if (place.world->find(id) != nullptr) {
            fill_instance_props(place, id);
        } else if (const std::optional<Luau::TypeId> type = place.find(id)) {
            Luau::getMutable<Luau::ExternType>(*type)->props.clear();
        }
    }
}
```

`sync_world` keeps calling `build_place_types`. The editor checker still rebuilds its own place types, which is right for its small cache.

- [ ] **Step 4: Sync the place checker from the diff**

Add `std::shared_ptr<const WorldSnap> last_world;` to `PlaceChecker`, with the comment `// The tree its cached modules and place types were checked against.` Then add after `PlaceChecker`:

```cpp
// Brings the place checker to `world` and adds to `names` every module the
// change can affect: scripts added or edited, and scripts whose last check
// reached an instance the diff names. A removed script leaves the cache, and
// what required it is added.
void sync_place(PlaceChecker& checker, const std::shared_ptr<const WorldSnap>& world,
                std::unordered_set<std::string>& names) {
    WorkerEnv& env = *checker.env;
    if (checker.last_world == nullptr || env.place == nullptr) {
        env.place = build_place_types(*env.frontend->globals.globalScope, world);
        for (const NodeSnap& node : world->nodes) {
            if (node.lua) {
                names.insert(module_name_of(node.id));
            }
        }
        checker.last_world = world;
        return;
    }
    if (checker.last_world == world) {
        return;
    }
    const TreeDiff diff = diff_worlds(*checker.last_world, *world);
    update_place_types(*env.place, *env.frontend->globals.globalScope, world, diff);
    for (InstanceId id : diff.added_scripts) {
        names.insert(module_name_of(id));
    }
    for (InstanceId id : diff.edited_scripts) {
        names.insert(module_name_of(id));
    }
    {
        std::lock_guard<std::mutex> lock(checker.reached_mu);
        for (const auto& entry : checker.reached) {
            for (InstanceId id : entry.second) {
                if (diff.parents.count(id) != 0 || diff.moved.count(id) != 0) {
                    names.insert(module_name_of(entry.first));
                    break;
                }
            }
        }
        for (InstanceId id : diff.removed_scripts) {
            checker.reached.erase(id);
        }
    }
    std::vector<Luau::ModuleName> gone;
    for (InstanceId id : diff.removed_scripts) {
        const std::string name = module_name_of(id);
        std::unordered_set<std::string> dependents;
        add_dependents(*env.frontend, name, dependents);
        dependents.erase(name);
        names.insert(dependents.begin(), dependents.end());
        names.erase(name);
        gone.push_back(name);
    }
    env.frontend->clearModules(gone);
    checker.last_world = world;
}
```

In `run_batch`, replace `sync_world(env, world, true);` and the `names` declaration and loop with:

```cpp
    std::unordered_set<std::string> names;
    sync_place(checker, world, names);
    for (const auto& entry : claimed) {
        names.insert(module_name_of(entry.first));
    }
    // And what requires each of them.
    std::unordered_set<std::string> affected;
    for (const std::string& name : names) {
        add_dependents(*env.frontend, name, affected);
    }
```

In the loop that builds `inputs`, iterate over `affected` instead of `names`.

In `run_place`, where the env is rebuilt for a registry revision change, also reset `checker.last_world.reset();` so the new frontend checks the whole place once.

- [ ] **Step 5: A tree change becomes a batch, not a recheck of everything**

In `State`, add after `batch_cancel`:

```cpp
    // A tree change the place checker has not taken yet, and when it is due.
    bool tree_pending = false;
    std::chrono::steady_clock::time_point tree_due{};
```

and change the `world_stale` comment to `// Set by note_world_changed. pump() turns it into one note_tree().`

Add to `ScriptAnalysis.hpp` private: `void note_tree();`, with the comment `// The tree changed: capture it, and let the place checker diff it against the last.`

In `ScriptAnalysis.cpp`:

```cpp
void ScriptAnalysis::note_tree() {
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->enabled || state_->stop) {
            return;
        }
    }
    const std::uint64_t seq = state_->world_seq.fetch_add(1) + 1;
    const std::shared_ptr<WorldSnap> world = capture_world(game_);
    ensure_worker();
    std::lock_guard<std::mutex> lock(state_->mu);
    if (!state_->enabled || state_->stop) {
        return;
    }
    state_->adopt(world, seq);
    state_->tree_pending = true;
    state_->tree_due = std::chrono::steady_clock::now() + kDebounce;
    state_->cv.notify_all();
}
```

In `pump`, replace `invalidate_all();` inside the `world_stale` block with `note_tree();`, and the comment above it with `// A tree change is diffed once, however many changes came in. Only while stopped: play changes are not the authored tree.`

In `run_place`, change the wait predicate to `state_->stop || !state_->pending.empty() || state_->tree_pending`. After the `for` loop over `pending`, before `if (claimed.empty())`, add:

```cpp
            bool tree = false;
            if (state_->tree_pending) {
                if (state_->tree_due <= now) {
                    tree = true;
                    state_->tree_pending = false;
                } else {
                    next = std::min(next, state_->tree_due);
                }
            }
```

and change `if (claimed.empty())` to `if (claimed.empty() && !tree)`.

In `busy`: `|| state_->tree_pending`. In `idle`: `&& !state_->tree_pending`. In `settled`: return false when `state_->tree_pending` (inside the lock, before the other checks).

`set_enabled(false)` and `shutdown`: add `state_->tree_pending = false;` next to `state_->pending.clear();`.

- [ ] **Step 6: Run the tests**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[A40],[A41]" && ./build/sandbox -# "[#analysis_tests]"`
Expected: A40 and A41 pass. All 40 analysis test cases pass, including A12, A13, A21 and A29, which cover tree changes.

- [ ] **Step 7: Run the rest and ThreadSanitizer**

Run:
```bash
cmake --build build --target studio-tests mcp-tests engine-tests -j8 && ./build/studio-tests && ./build/mcp-tests && ./build/engine-tests
cmake --build build-tsan --target sandbox -j8 && ./build-tsan/sandbox -# "[#analysis_tests]"
```
Expected: all pass; no ThreadSanitizer warnings.

- [ ] **Step 8: Commit**

```bash
git add src/engine_core/ScriptAnalysis.hpp src/engine_core/ScriptAnalysis.cpp sandbox/analysis_tests.cpp
git commit -m "Recheck after a tree change only the scripts whose types reached what changed"
```

---

### Task 7: Timing, and a look in the studio

**Files:**
- Test: `sandbox/analysis_tests.cpp`: hidden `[.perf]` test

**Interfaces:**
- Consumes: `long_source` (Task 4), `ScriptAnalysis(game, threads)` and `checks` (Task 5).

- [ ] **Step 1: Add the timing test**

At the end of `sandbox/analysis_tests.cpp`:

```cpp
namespace {

double settle_ms(engine_core::ScriptAnalysis& analysis) {
    const auto started = std::chrono::steady_clock::now();
    settle(analysis);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

}  // namespace

// Hidden: run with ./build/sandbox "[.perf]". Each time includes the 75 ms debounce.
TEST_CASE("A-perf timing of a large script and a large place", "[.perf]") {
    for (bool strict : {false, true}) {
        ScriptRig rig;
        engine_core::ScriptAnalysis analysis(rig.game);
        const std::string source = long_source(1000, strict, 0);
        engine_core::Script& script = add_script(rig.game, "Big", source.c_str());
        const double first = settle_ms(analysis);
        script.set_source(source + "\n-- edit\n");
        const double again = settle_ms(analysis);
        std::printf("1000 lines %s: first %.0f ms, after an edit %.0f ms\n", strict ? "strict" : "nonstrict", first,
                    again);
    }
    for (unsigned threads : {1u, 0u}) {
        ScriptRig rig;
        engine_core::ScriptAnalysis analysis(rig.game, threads);
        std::vector<engine_core::Script*> scripts;
        for (int i = 0; i < 20; ++i) {
            const std::string name = "Big" + std::to_string(i);
            scripts.push_back(&add_script(rig.game, name.c_str(), long_source(1000, false, i).c_str()));
        }
        const double place = settle_ms(analysis);
        scripts[0]->set_source(scripts[0]->source() + "\n-- edit\n");
        const double edit = settle_ms(analysis);
        engine_core::Folder& away = rig.game.create<engine_core::Folder>();
        rig.game.set_parent(away.id(), scripts[0]->id());
        const std::uint64_t before = analysis.checks(scripts[1]->id());
        rig.game.set_name(away.id(), "Renamed");
        const double rename = settle_ms(analysis);
        std::printf("20 x 1000 lines on %u threads: place %.0f ms, one edit %.0f ms, unrelated rename %.0f ms "
                    "(script 2 rechecked: %s)\n",
                    analysis.threads(), place, edit, rename,
                    analysis.checks(scripts[1]->id()) != before ? "yes" : "no");
    }
}
```

Add `#include <cstdio>` at the top if it is missing.

- [ ] **Step 2: Run it and record the numbers**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[.perf]"`
Expected: it prints three lines per section. For comparison, before this work the measurements were: 1000 lines took 106–113 ms (first check and after an edit), and 20 × 1000 lines took 679–690 ms on the one worker. Expect the place time on the default thread count to be well under the single-thread number, and `script 2 rechecked: no`.

- [ ] **Step 3: Run the whole suite and the TSAN suite once more**

Run:
```bash
./build/sandbox && ./build/studio-tests && ./build/mcp-tests && ./build/engine-tests
cmake --build build-tsan --target sandbox -j8 && ./build-tsan/sandbox -# "[#analysis_tests]"
```
Expected: all pass; no ThreadSanitizer warnings.

- [ ] **Step 4: Check it in the studio**

Rebuild the app with its resources (`cmake --build build --target AnarchyStudio bundle-resources -j8`; without `bundle-resources`, icons and themes go missing). Open a project with several scripts. Ask the studio over MCP, `get_diagnostics` with no script open, for a script with a known error: it reports the error without the script ever being opened. Rename an unrelated part and edit a script while watching the studio stay responsive.

- [ ] **Step 5: Commit**

```bash
git add sandbox/analysis_tests.cpp
git commit -m "Time script analysis of a large script and a large place, before and after a change" -m "<paste the [.perf] output here>"
```
