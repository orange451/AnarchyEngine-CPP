// How long the H1Z1 car takes to respawn, piece by piece: the ButtonController
// destroys the old car, clones a template, and parents the clone to
// Workspace, all inside one Touched handler. Hidden ([.spawn]): it needs the
// project on this machine, and it prints timings rather than checking them.

#include "physics_rig.hpp"
#include "support.hpp"

#include "InstanceFile.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PhysicsWorld.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct SpawnRig : ScriptRig {
    engine_core::PhysicsWorld physics;
    SpawnRig() { game.set_physics(&physics); }
    ~SpawnRig() { game.set_physics(nullptr); }

    // Runs source as a command, and returns how long it took in ms.
    double timed(const char* source) {
        const Clock::time_point start = Clock::now();
        runtime.run_chunk(source);
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    double timed_step() {
        const Clock::time_point start = Clock::now();
        game.assemblies().sync_play(game);
        physics.step(game, 1.0 / 240.0);
        frames(1);
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
};

}  // namespace

TEST_CASE("SB1 the H1Z1 car's respawn, timed piece by piece", "[.spawn]") {
    const std::filesystem::path project = "C:/Users/Andrew/Documents/Anarchy Engine Projects/H1Z1 Z1";
    if (!std::filesystem::exists(project)) {
        SKIP("the H1Z1 project is not on this machine");
    }
    TempDir dir;
    std::filesystem::copy(project, dir.path, std::filesystem::copy_options::recursive);
    SpawnRig rig;
    engine_core::Project loaded = engine_core::Project::load(dir.path, rig.game);
    rig.game.capture_place();
    rig.game.start_simulation();
    for (int i = 0; i < 30; ++i) {
        rig.timed_step();
    }
    rig.timed("_G.template = workspace.Car:Clone()");
    for (int round = 0; round < 5; ++round) {
        const double destroy = rig.timed("workspace.Car:Destroy()");
        const double clone = rig.timed("_G.fresh = _G.template:Clone()");
        const double parent = rig.timed("_G.fresh.Parent = workspace");
        double steps[3];
        for (double& step : steps) {
            step = rig.timed_step();
        }
        std::printf("round %d: destroy %.3f ms, clone %.3f ms, parent %.3f ms; steps after %.3f %.3f %.3f ms\n", round,
                    destroy, clone, parent, steps[0], steps[1], steps[2]);
        for (int i = 0; i < 30; ++i) {
            rig.timed_step();
        }
    }
    rig.game.stop_simulation();
}

namespace {

// Builds node under parent as build_copy does, timing each property's load by "Class.Property".
engine_core::InstanceId timed_build(engine_core::DataModel& world, const engine_core::CopiedNode& node,
                                    engine_core::InstanceId parent, std::map<std::string, double>& costs,
                                    std::map<std::string, int>& counts) {
    auto ms = [](Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); };
    Clock::time_point t = Clock::now();
    engine_core::DataModel* made = engine_core::lua_create_instance(world, node.class_name.c_str());
    costs[node.class_name + ".<create>"] += ms(t);
    const engine_core::InstanceId id = made->id();
    for (const engine_core::JsonValue::Member& member : node.properties) {
        t = Clock::now();
        std::string error;
        made->load_property(member.first, member.second, error);
        const std::string key = node.class_name + "." + member.first;
        costs[key] += ms(t);
        ++counts[key];
    }
    t = Clock::now();
    world.set_name(id, node.name);
    costs[node.class_name + ".<name>"] += ms(t);
    if (parent != engine_core::DataModel::kNoParent) {
        t = Clock::now();
        world.set_parent(id, parent);
        costs[node.class_name + ".<parent>"] += ms(t);
    }
    for (const engine_core::CopiedNode& child : node.children) {
        timed_build(world, child, id, costs, counts);
    }
    return id;
}

int count_nodes(const engine_core::CopiedNode& node) {
    int n = 1;
    for (const engine_core::CopiedNode& child : node.children) {
        n += count_nodes(child);
    }
    return n;
}

}  // namespace

TEST_CASE("SB2 where the H1Z1 car's Clone spends its time", "[.spawn]") {
    const std::filesystem::path project = "C:/Users/Andrew/Documents/Anarchy Engine Projects/H1Z1 Z1";
    if (!std::filesystem::exists(project)) {
        SKIP("the H1Z1 project is not on this machine");
    }
    TempDir dir;
    std::filesystem::copy(project, dir.path, std::filesystem::copy_options::recursive);
    SpawnRig rig;
    engine_core::Project loaded = engine_core::Project::load(dir.path, rig.game);
    rig.game.capture_place();
    rig.game.start_simulation();
    rig.timed_step();
    const engine_core::InstanceId car = rig.game.find_first_child(rig.game.scene_service("Workspace"), "Car");
    REQUIRE(car != 0);
    std::map<std::string, double> costs;
    std::map<std::string, int> counts;
    double copy = 0;
    double remap = 0;
    double whole = 0;
    const int rounds = 10;
    int nodes = 0;
    for (int round = 0; round < rounds; ++round) {
        Clock::time_point t = Clock::now();
        const engine_core::CopiedNode root = engine_core::copy_tree(rig.game, car);
        copy += std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        nodes = count_nodes(root);
        timed_build(rig.game, root, engine_core::DataModel::kNoParent, costs, counts);
        t = Clock::now();
        const engine_core::InstanceId cloned = engine_core::clone_tree(rig.game, car);
        whole += std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        (void)cloned;
        (void)remap;
    }
    std::printf("%d nodes. per clone: copy_tree %.3f ms, clone_tree %.3f ms\n", nodes, copy / rounds, whole / rounds);
    std::vector<std::pair<double, std::string>> sorted;
    for (const auto& [key, cost] : costs) {
        sorted.emplace_back(cost / rounds, key);
    }
    std::sort(sorted.rbegin(), sorted.rend());
    for (std::size_t i = 0; i < sorted.size() && i < 25; ++i) {
        std::printf("  %8.3f ms  %s (x%d)\n", sorted[i].first, sorted[i].second.c_str(),
                    counts.count(sorted[i].second) ? counts[sorted[i].second] / rounds : 0);
    }
    rig.game.stop_simulation();
}

