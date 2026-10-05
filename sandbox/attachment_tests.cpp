// Attachment: a PVInstance that stores only an Offset from its parent, so its
// Transform is the parent's times Offset, and a Transform write solves for Offset.

#include "support.hpp"

#include "Attachment.hpp"
#include "ChangeHistoryService.hpp"
#include "Folder.hpp"
#include "LuaApi.hpp"
#include "Matrix4.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>
#include <string>

namespace {

using engine_core::Attachment;
using engine_core::Matrix4;
using engine_core::matrix4_translation;

bool near_matrix(const Matrix4& a, const Matrix4& b) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(a.m[i] - b.m[i]) > 1e-4f) {
            return false;
        }
    }
    return true;
}

Attachment& add_attachment(engine_core::DataModel& game, engine_core::InstanceId parent) {
    Attachment& attachment = game.create<Attachment>();
    game.set_parent(attachment.id(), parent);
    return attachment;
}

void require_globals(ScriptRig& rig, std::initializer_list<const char*> names) {
    INFO(rig.runtime.last_error());
    for (const char* name : names) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

}  // namespace

TEST_CASE("ATT1 setting Offset moves Transform with the parent", "[attachment]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("Attachment"));
    REQUIRE(engine_core::lua_class_inherits("Attachment", "PVInstance"));
    engine_core::GameObject& part = create_part(game);
    part.set_transform(matrix4_translation(0.f, 10.f, 0.f));
    Attachment& attachment = add_attachment(game, part.id());
    REQUIRE(near_matrix(attachment.offset(), engine_core::matrix4_identity()));
    REQUIRE(near_matrix(attachment.transform(), matrix4_translation(0.f, 10.f, 0.f)));

    REQUIRE_FALSE(attachment.set_offset(matrix4_translation(0.f, 0.f, 10.f)));
    REQUIRE(near_matrix(attachment.transform(), matrix4_translation(0.f, 10.f, 10.f)));

    // Offset is in the parent's space, so a turned parent turns it.
    const Matrix4 turned = engine_core::matrix4_multiply(matrix4_translation(0.f, 10.f, 0.f),
                                                         engine_core::matrix4_axis_angle({0.f, 1.f, 0.f}, 1.5707963267948966));
    part.set_transform(turned);
    REQUIRE(near_matrix(attachment.transform(),
                        engine_core::matrix4_multiply(turned, matrix4_translation(0.f, 0.f, 10.f))));
    REQUIRE(std::fabs(engine_core::matrix4_position(attachment.transform()).x - 10.f) < 1e-4f);
}

TEST_CASE("ATT2 setting Transform works out the Offset", "[attachment]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    const Matrix4 parent = engine_core::matrix4_multiply(matrix4_translation(5.f, 10.f, 0.f),
                                                         engine_core::matrix4_axis_angle({0.f, 0.f, 1.f}, 0.7));
    part.set_transform(parent);
    Attachment& attachment = add_attachment(game, part.id());

    const Matrix4 world = matrix4_translation(1.f, 2.f, 3.f);
    REQUIRE_FALSE(attachment.set_transform(world));
    REQUIRE(near_matrix(attachment.transform(), world));
    REQUIRE(near_matrix(engine_core::matrix4_multiply(parent, attachment.offset()), world));
    // Through PVInstance, as the Dragger writes one.
    engine_core::PVInstance& pv = attachment;
    REQUIRE_FALSE(pv.set_pv_transform(matrix4_translation(0.f, 0.f, 0.f)));
    REQUIRE(near_matrix(attachment.transform(), matrix4_translation(0.f, 0.f, 0.f)));
}

TEST_CASE("ATT3 a parent that is not a PVInstance counts as identity", "[attachment]") {
    SimRole role;
    engine_core::Game game;
    Attachment& in_workspace = add_attachment(game, workspace_of(game));
    REQUIRE_FALSE(in_workspace.set_offset(matrix4_translation(1.f, 2.f, 3.f)));
    REQUIRE(near_matrix(in_workspace.transform(), matrix4_translation(1.f, 2.f, 3.f)));
    REQUIRE_FALSE(in_workspace.set_transform(matrix4_translation(4.f, 5.f, 6.f)));
    REQUIRE(near_matrix(in_workspace.offset(), matrix4_translation(4.f, 5.f, 6.f)));

    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), workspace_of(game));
    game.set_parent(in_workspace.id(), folder.id());
    REQUIRE(near_matrix(in_workspace.transform(), matrix4_translation(4.f, 5.f, 6.f)));

    Attachment& loose = game.create<Attachment>();
    REQUIRE_FALSE(loose.set_offset(matrix4_translation(7.f, 0.f, 0.f)));
    REQUIRE(near_matrix(loose.transform(), matrix4_translation(7.f, 0.f, 0.f)));
}

TEST_CASE("ATT4 a new parent keeps Offset and moves Transform", "[attachment]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& first = create_part(game);
    first.set_transform(matrix4_translation(0.f, 10.f, 0.f));
    engine_core::GameObject& second = create_part(game);
    second.set_transform(matrix4_translation(100.f, 0.f, 0.f));
    Attachment& attachment = add_attachment(game, first.id());
    REQUIRE_FALSE(attachment.set_offset(matrix4_translation(0.f, 0.f, 10.f)));

    game.set_parent(attachment.id(), second.id());
    REQUIRE(near_matrix(attachment.offset(), matrix4_translation(0.f, 0.f, 10.f)));
    REQUIRE(near_matrix(attachment.transform(), matrix4_translation(100.f, 0.f, 10.f)));

    // An Attachment is a PVInstance, so one can hold another.
    Attachment& tip = add_attachment(game, attachment.id());
    REQUIRE_FALSE(tip.set_offset(matrix4_translation(1.f, 0.f, 0.f)));
    REQUIRE(near_matrix(tip.transform(), matrix4_translation(101.f, 0.f, 10.f)));

    game.set_parent(attachment.id(), workspace_of(game));
    REQUIRE(near_matrix(attachment.transform(), matrix4_translation(0.f, 0.f, 10.f)));
    REQUIRE(near_matrix(tip.transform(), matrix4_translation(1.f, 0.f, 10.f)));
}

TEST_CASE("ATT5 bad writes are refused", "[attachment]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    Matrix4 flat = engine_core::matrix4_identity();
    flat.m[5] = 0.f;
    part.set_transform(flat);
    Attachment& attachment = add_attachment(game, part.id());
    REQUIRE(attachment.set_transform(matrix4_translation(1.f, 1.f, 1.f)).has_value());
    REQUIRE(near_matrix(attachment.offset(), engine_core::matrix4_identity()));
    Matrix4 not_finite = engine_core::matrix4_identity();
    not_finite.m[12] = std::nanf("");
    REQUIRE(*attachment.set_offset(not_finite) == "Offset must be finite");
    REQUIRE(*attachment.set_transform(not_finite) == "Transform must be finite");
}

TEST_CASE("ATT6 Offset undoes, saves, loads, and comes back at Stop", "[attachment][project]") {
    SECTION("a Transform write undoes as the Offset it made") {
        SimRole role;
        engine_core::Game game;
        engine_core::GameObject& part = create_part(game);
        part.set_transform(matrix4_translation(0.f, 10.f, 0.f));
        Attachment& attachment = add_attachment(game, part.id());
        begin_step(game, "Move Attachment");
        REQUIRE_FALSE(attachment.set_transform(matrix4_translation(0.f, 10.f, 5.f)));
        end_step(game);
        REQUIRE(near_matrix(attachment.offset(), matrix4_translation(0.f, 0.f, 5.f)));
        game.history().undo();
        REQUIRE(near_matrix(attachment.offset(), engine_core::matrix4_identity()));
        REQUIRE(near_matrix(attachment.transform(), matrix4_translation(0.f, 10.f, 0.f)));
        game.history().redo();
        REQUIRE(near_matrix(attachment.transform(), matrix4_translation(0.f, 10.f, 5.f)));
    }
    SECTION("a project saves Offset, not Transform") {
        SimRole role;
        TempDir dir;
        {
            engine_core::Project project = engine_core::Project::create(dir.path);
            engine_core::DataModel& game = project.datamodel();
            engine_core::GameObject& part = game.create_game_object();
            game.set_name(part.id(), "Part");
            game.set_parent(part.id(), game.scene_service("Workspace"));
            part.set_transform(matrix4_translation(0.f, 10.f, 0.f));
            Attachment& attachment = add_attachment(game, part.id());
            game.set_name(attachment.id(), "Point");
            REQUIRE_FALSE(attachment.set_offset(matrix4_translation(0.f, 0.f, 10.f)));
            engine_core::PropertyBag saved;
            attachment.save_properties(saved);
            REQUIRE(engine_core::bag_find(saved, "Offset") != nullptr);
            REQUIRE(engine_core::bag_find(saved, "Transform") == nullptr);
            project.save();
        }
        engine_core::Game game;
        engine_core::Project loaded = engine_core::Project::load(dir.path, game);
        const engine_core::InstanceId part = game.find_first_child(game.scene_service("Workspace"), "Part");
        auto* attachment = dynamic_cast<Attachment*>(game.instance(game.find_first_child(part, "Point")));
        REQUIRE(attachment != nullptr);
        REQUIRE(near_matrix(attachment->transform(), matrix4_translation(0.f, 10.f, 10.f)));
    }
    SECTION("Stop puts back what it was before a script changed it") {
        ScriptRig rig;
        Attachment& attachment = add_attachment(rig.game, workspace_of(rig.game));
        rig.game.set_name(attachment.id(), "Point");
        REQUIRE_FALSE(attachment.set_offset(matrix4_translation(0.f, 3.f, 0.f)));
        add_script(rig.game, "Move", R"(
            workspace.Point.Transform = Matrix4.new() + Vector3.new(9, 9, 9)
            _G.moved = workspace.Point.Offset.Position == Vector3.new(9, 9, 9)
        )");
        rig.game.start_simulation();
        rig.frames(2);
        require_globals(rig, {"moved"});
        rig.game.stop_simulation();
        REQUIRE(near_matrix(attachment.offset(), matrix4_translation(0.f, 3.f, 0.f)));
    }
}

TEST_CASE("ATT7 scripts make Attachments, and Transform fires Changed", "[attachment]") {
    ScriptRig rig;
    add_script(rig.game, "Points", R"(
        local part = Instance.new("GameObject", workspace)
        part.Transform = Matrix4.new() + Vector3.new(0, 10, 0)
        local other = Instance.new("GameObject", workspace)
        local point = Instance.new("Attachment", part)
        _G.isa = point:IsA("PVInstance") and point.ClassName == "Attachment"
        local changed = {}
        point.Changed:Connect(function(name) changed[name] = (changed[name] or 0) + 1 end)
        point.Offset = Matrix4.new() + Vector3.new(0, 0, 10)
        _G.offset = point.Transform.Position == Vector3.new(0, 10, 10)
        point.Transform = Matrix4.new() + Vector3.new(0, 10, 3)
        _G.transform = point.Offset.Position == Vector3.new(0, 0, 3)
        task.wait()
        task.wait()
        _G.fired = changed.Offset == 2 and changed.Transform == 2
        point.Parent = other
        task.wait()
        task.wait()
        _G.reparent = point.Transform.Position == Vector3.new(0, 0, 3) and changed.Transform == 3
        _G.no_nan = not pcall(function() point.Offset = Matrix4.new() + Vector3.new(0 / 0, 0, 0) end)
    )");
    rig.game.start_simulation();
    rig.frames(4, 0.05);
    require_globals(rig, {"isa", "offset", "transform", "fired", "reparent", "no_nan"});
}

TEST_CASE("ATT8 a World OffsetSpace takes the parent's position, not its rotation", "[attachment]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    const Matrix4 turned = engine_core::matrix4_multiply(matrix4_translation(0.f, 10.f, 0.f),
                                                         engine_core::matrix4_axis_angle({0.f, 1.f, 0.f}, 1.5707963267948966));
    part.set_transform(turned);
    Attachment& attachment = add_attachment(game, part.id());
    REQUIRE(attachment.offset_space() == engine_core::TransformSpace::Local);
    REQUIRE_FALSE(attachment.set_offset(matrix4_translation(0.f, 0.f, 10.f)));
    // Local: the parent's +Z is the world's +X.
    REQUIRE(std::fabs(engine_core::matrix4_position(attachment.transform()).x - 10.f) < 1e-4f);

    // Switching keeps Offset, so the Transform moves to the new frame.
    REQUIRE_FALSE(attachment.set_offset_space(static_cast<int>(engine_core::TransformSpace::World)));
    REQUIRE(near_matrix(attachment.offset(), matrix4_translation(0.f, 0.f, 10.f)));
    REQUIRE(near_matrix(attachment.transform(), matrix4_translation(0.f, 10.f, 10.f)));

    // The parent's position still carries it; its rotation and scale do not.
    Matrix4 moved = engine_core::matrix4_multiply(matrix4_translation(5.f, 0.f, 0.f),
                                                  engine_core::matrix4_axis_angle({1.f, 0.f, 0.f}, 0.4));
    moved.m[0] *= 3.f;
    part.set_transform(moved);
    REQUIRE(near_matrix(attachment.transform(), matrix4_translation(5.f, 0.f, 10.f)));

    // A Transform write solves along the world's axes.
    REQUIRE_FALSE(attachment.set_transform(matrix4_translation(6.f, 1.f, 1.f)));
    REQUIRE(near_matrix(attachment.offset(), matrix4_translation(1.f, 1.f, 1.f)));

    // A parent that is not a PVInstance is identity in either space.
    game.set_parent(attachment.id(), workspace_of(game));
    REQUIRE(near_matrix(attachment.transform(), matrix4_translation(1.f, 1.f, 1.f)));

    REQUIRE(*attachment.set_offset_space(7) == "OffsetSpace must be an Enum.TransformSpace");
}

TEST_CASE("ATT9 a World OffsetSpace allows a Transform write under a flattened parent", "[attachment]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    Matrix4 flat = matrix4_translation(0.f, 2.f, 0.f);
    flat.m[5] = 0.f;
    part.set_transform(flat);
    Attachment& attachment = add_attachment(game, part.id());
    REQUIRE(attachment.set_transform(matrix4_translation(1.f, 1.f, 1.f)).has_value());
    REQUIRE_FALSE(attachment.set_offset_space(static_cast<int>(engine_core::TransformSpace::World)));
    REQUIRE_FALSE(attachment.set_transform(matrix4_translation(1.f, 1.f, 1.f)));
    REQUIRE(near_matrix(attachment.offset(), matrix4_translation(1.f, -1.f, 1.f)));
}

TEST_CASE("ATT10 OffsetSpace undoes, saves, loads, and scripts set it", "[attachment][project]") {
    const int world = static_cast<int>(engine_core::TransformSpace::World);
    SECTION("undo and redo set it back and forth") {
        SimRole role;
        engine_core::Game game;
        Attachment& attachment = add_attachment(game, workspace_of(game));
        begin_step(game, "Set OffsetSpace");
        REQUIRE_FALSE(attachment.set_offset_space(world));
        end_step(game);
        game.history().undo();
        REQUIRE(attachment.offset_space() == engine_core::TransformSpace::Local);
        game.history().redo();
        REQUIRE(attachment.offset_space() == engine_core::TransformSpace::World);
    }
    SECTION("a project saves it only when it is not Local") {
        SimRole role;
        TempDir dir;
        {
            engine_core::Project project = engine_core::Project::create(dir.path);
            engine_core::DataModel& game = project.datamodel();
            Attachment& plain = add_attachment(game, game.scene_service("Workspace"));
            engine_core::PropertyBag saved;
            plain.save_properties(saved);
            REQUIRE(engine_core::bag_find(saved, "OffsetSpace") == nullptr);
            Attachment& attachment = add_attachment(game, game.scene_service("Workspace"));
            game.set_name(attachment.id(), "Point");
            REQUIRE_FALSE(attachment.set_offset_space(world));
            project.save();
        }
        engine_core::Game game;
        engine_core::Project loaded = engine_core::Project::load(dir.path, game);
        auto* attachment = dynamic_cast<Attachment*>(
            game.instance(game.find_first_child(game.scene_service("Workspace"), "Point")));
        REQUIRE(attachment != nullptr);
        REQUIRE(attachment->offset_space() == engine_core::TransformSpace::World);
    }
    SECTION("scripts read and write it as an Enum.TransformSpace") {
        ScriptRig rig;
        add_script(rig.game, "Space", R"(
            local part = Instance.new("GameObject", workspace)
            part.Transform = Matrix4.new() + Vector3.new(0, 10, 0)
            local point = Instance.new("Attachment", part)
            _G.default = point.OffsetSpace == Enum.TransformSpace.Local
            point.OffsetSpace = Enum.TransformSpace.World
            point.Offset = Matrix4.new() + Vector3.new(0, 0, 10)
            _G.world = point.OffsetSpace == Enum.TransformSpace.World
                and point.Transform.Position == Vector3.new(0, 10, 10)
            point.OffsetSpace = 1
            _G.by_value = point.OffsetSpace == Enum.TransformSpace.Local
            point.OffsetSpace = "World"
            _G.by_name = point.OffsetSpace == Enum.TransformSpace.World
            _G.refused = not pcall(function() point.OffsetSpace = Enum.Axis.X end)
                and not pcall(function() point.OffsetSpace = "Sideways" end)
        )");
        rig.game.start_simulation();
        rig.frames(2);
        require_globals(rig, {"default", "world", "by_value", "by_name", "refused"});
    }
}
