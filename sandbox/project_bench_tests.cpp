// Timing a real project's load and disk checks. Hidden ([.bench]): run with
//   ANARCHY_BENCH_PROJECT=<project folder> sandbox.exe "[.bench]"
// It only reads the project.

#include "support.hpp"

#include "Environment.hpp"
#include "FileBytes.hpp"
#include "GameObject.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

TEST_CASE("BENCH a project's load, disk scan and focus check", "[.bench][project]") {
    const std::optional<std::string> path = engine_core::environment_variable("ANARCHY_BENCH_PROJECT");
    if (!path) {
        SKIP("set ANARCHY_BENCH_PROJECT to a project folder");
    }
    SimRole role;
    // Timed as the studio runs it: no whole read alongside each partial one.
    engine_core::Project::set_compare_check(false);
    auto start = std::chrono::steady_clock::now();
    {
        // As the studio opens a project: into a world it replaces.
        engine_core::Game world;
        engine_core::Project opened = engine_core::Project::load(*path, world);
        std::printf("load into world %8.2f s  (as the studio opens it)\n", seconds_since(start));
    }
    start = std::chrono::steady_clock::now();
    engine_core::Project project = engine_core::Project::load(*path);
    std::printf("load            %8.2f s\n", seconds_since(start));
    start = std::chrono::steady_clock::now();
    const engine_core::DiskScan scan = project.scan_disk();
    std::printf("scan_disk       %8.2f s  (changes %d, conflicts %zu)\n", seconds_since(start),
                scan.has_disk_changes ? 1 : 0, scan.conflicts.size());
    start = std::chrono::steady_clock::now();
    const engine_core::DiskScan applied = project.apply_disk();
    std::printf("apply_disk      %8.2f s  (the focus-regain check; loaded %zu)\n", seconds_since(start),
                applied.loaded.size());
    REQUIRE_FALSE(scan.has_disk_changes);
    // Saving writes to the project: only when asked, on a copy you can spare.
    if (engine_core::environment_variable("ANARCHY_BENCH_SAVE")) {
        for (int i = 0; i < 2; ++i) {
            start = std::chrono::steady_clock::now();
            project.save();
            std::printf("save            %8.2f s  (wrote %zu, moved %zu, removed %zu)\n", seconds_since(start),
                        project.last_save().written.size(), project.last_save().moved.size(),
                        project.last_save().removed.size());
        }
        // The two everyday saves: one object edited in the studio; and one
        // edited in the studio while another's file was edited outside, as an
        // agent editing the project does. Each as the studio saves: the disk
        // check, then the save.
        engine_core::DataModel& world = project.datamodel();
        engine_core::GameObject* moved = nullptr;
        std::vector<engine_core::InstanceId> pending{0};
        while (!pending.empty() && moved == nullptr) {
            const engine_core::InstanceId at = pending.back();
            pending.pop_back();
            moved = at != 0 ? world.game_object(at) : nullptr;
            for (const engine_core::InstanceId child : world.get_children(at)) {
                pending.push_back(child);
            }
        }
        REQUIRE(moved != nullptr);
        std::string outside;
        for (const auto& [guid, files] : project.base_paths_for_bench()) {
            if (world.guid(moved->id()) != guid && files.find("/Workspace.workspace/") != std::string::npos &&
                files.size() > 5 && files.compare(files.size() - 5, 5, ".json") == 0) {
                outside = files;
                break;
            }
        }
        REQUIRE_FALSE(outside.empty());
        for (const bool outside_edit : {false, true}) {
            for (int i = 0; i < 2; ++i) {
                engine_core::Matrix4 transform = moved->transform();
                transform.m[12] += 0.5f;
                moved->set_transform(transform);
                if (outside_edit) {
                    const std::filesystem::path file = std::filesystem::u8path(*path) / std::filesystem::u8path(outside);
                    std::string text;
                    std::string why;
                    REQUIRE(engine_core::read_file(file, text, why));
                    const std::size_t brace = text.rfind('}');
                    REQUIRE(brace != std::string::npos);
                    text.insert(brace, i % 2 == 0 ? " " : "  ");
                    REQUIRE(engine_core::write_file(file, text, why));
                }
                start = std::chrono::steady_clock::now();
                const engine_core::DiskScan checked = project.apply_disk();
                const double check = seconds_since(start);
                start = std::chrono::steady_clock::now();
                project.save({}, true);
                std::printf("%s check %6.3f s  save %6.3f s  (loaded %zu, conflicts %zu, wrote %zu)\n",
                            outside_edit ? "outside+studio edit:" : "studio edit:        ", check, seconds_since(start),
                            checked.loaded.size(), checked.conflicts.size(), project.last_save().written.size());
            }
        }
        std::printf("partial reads: %zu\n", engine_core::Project::partial_reads());
    }
    engine_core::Project::set_compare_check(true);
}
