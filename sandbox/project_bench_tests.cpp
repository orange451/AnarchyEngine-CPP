// Timing a real project's load and disk checks. Hidden ([.bench]): run with
//   ANARCHY_BENCH_PROJECT=<project folder> sandbox.exe "[.bench]"
// It only reads the project.

#include "support.hpp"

#include "Environment.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>

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
    auto start = std::chrono::steady_clock::now();
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
    }
}
