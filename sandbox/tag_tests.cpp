// Tags and CollectionService: names any instance carries, saved as one hidden
// JSON property, undone, restored at Stop, and watched from Lua.

#include "support.hpp"

#include "Folder.hpp"
#include "GameObject.hpp"
#include "InstanceFile.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"
#include "Tags.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using engine_core::LuaSlot;

std::vector<std::string> printed(const engine_core::ScriptRuntime::OutputBatch& batch) {
    std::vector<std::string> out;
    for (const auto& line : batch.lines) {
        out.push_back(line.text);
    }
    return out;
}

std::vector<std::string> output(ScriptRig& rig) {
    std::vector<std::string> out = printed(rig.runtime.drain_output());
    for (const std::string& line : out) {
        UNSCOPED_INFO(line);
    }
    return out;
}

bool has_prefix(const std::vector<std::string>& lines, const std::string& prefix) {
    for (const std::string& line : lines) {
        if (line.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::size_t count_line(const std::vector<std::string>& lines, const std::string& text) {
    return static_cast<std::size_t>(std::count(lines.begin(), lines.end(), text));
}

// Where text first appears in lines, or lines.size().
std::size_t index_of(const std::vector<std::string>& lines, const std::string& text) {
    return static_cast<std::size_t>(std::find(lines.begin(), lines.end(), text) - lines.begin());
}

engine_core::InstanceId named_part(engine_core::DataModel& game, engine_core::InstanceId parent, const char* name) {
    engine_core::GameObject& part = game.create_game_object();
    game.set_parent(part.id(), parent);
    game.set_name(part.id(), name);
    return part.id();
}

std::vector<std::string> tags_of(const engine_core::DataModel& object) {
    return {object.tags().begin(), object.tags().end()};
}

}  // namespace

TEST_CASE("TAG1 a tag is 1 to 100 characters with no control characters", "[tags]") {
    REQUIRE(!engine_core::tag_error("Enemy"));
    REQUIRE(!engine_core::tag_error("has space and.dots!"));
    REQUIRE(!engine_core::tag_error(std::string(100, 'a')));
    REQUIRE(engine_core::tag_error(""));
    REQUIRE(engine_core::tag_error(std::string(101, 'a')));
    REQUIRE(engine_core::tag_error("tab\there"));
    REQUIRE(engine_core::tag_error(std::string("nul\0x", 5)));
    REQUIRE(engine_core::tag_error("del\x7f"));
}

TEST_CASE("TAG2 tags round-trip through JSON, sorted", "[tags]") {
    const engine_core::TagSet tags{"Zed", "Alpha", "Mid"};
    const std::string json = engine_core::tags_to_json(tags);
    REQUIRE(json == R"(["Alpha","Mid","Zed"])");
    engine_core::TagSet back;
    std::string error;
    REQUIRE(engine_core::tags_from_json(json, back, error));
    REQUIRE(error.empty());
    REQUIRE(back == tags);

    REQUIRE(engine_core::tags_to_json({}).empty());
    REQUIRE(engine_core::tags_from_json("", back, error));
    REQUIRE(back.empty());
    REQUIRE(engine_core::tags_from_json("null", back, error));
    REQUIRE(back.empty());
    REQUIRE(!engine_core::tags_from_json(R"({"a":1})", back, error));
    REQUIRE(!engine_core::tags_from_json("not json", back, error));

    // A bad entry is skipped and named; the rest load.
    error.clear();
    REQUIRE(engine_core::tags_from_json(R"(["B", 3, "", "A"])", back, error));
    REQUIRE(back == engine_core::TagSet{"A", "B"});
    REQUIRE(!error.empty());
}

TEST_CASE("TAG3 adding and removing a tag each undo as one step, and Changed says Tags", "[tags]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    const engine_core::LuaField* field = engine_core::lua_class_find("DataModel", "Tags");
    REQUIRE(field != nullptr);
    REQUIRE(field->hidden);
    REQUIRE(field->saved);
    REQUIRE(field->json_text);

    std::vector<std::string> changed;
    game.changed(part.id()).connect([&](engine_core::InstanceId, engine_core::Field) {
        const char* name = engine_core::lua_property_name(static_cast<std::uint32_t>(game.events().payload()));
        changed.push_back(name != nullptr ? name : "");
    });
    REQUIRE(game.add_tag("X") == "game cannot hold tags");
    REQUIRE(part.add_tag("") == engine_core::tag_error(""));
    REQUIRE(part.tags().empty());

    begin_step(game);
    REQUIRE(!part.add_tag("Enemy"));
    end_step(game);
    begin_step(game);
    REQUIRE(!part.add_tag("Boss"));
    end_step(game);
    // Adding a held tag and removing an absent one change nothing.
    REQUIRE(!part.add_tag("Boss"));
    REQUIRE(!part.remove_tag("Absent"));
    begin_step(game);
    REQUIRE(part.remove_tag("Enemy"));
    end_step(game);
    game.events().drain();
    REQUIRE(changed == std::vector<std::string>{"Tags", "Tags", "Tags"});
    REQUIRE(tags_of(part) == std::vector<std::string>{"Boss"});
    REQUIRE(part.holds_tag("Boss"));
    REQUIRE(!part.holds_tag("Enemy"));
    LuaSlot out;
    REQUIRE(field->read(game, part, out));
    REQUIRE(out.text == R"(["Boss"])");

    game.history().undo();
    REQUIRE(tags_of(part) == std::vector<std::string>{"Boss", "Enemy"});
    game.history().undo();
    REQUIRE(tags_of(part) == std::vector<std::string>{"Enemy"});
    game.history().undo();
    REQUIRE(part.tags().empty());
    REQUIRE(!game.history().can_undo().first);
    game.history().redo();
    REQUIRE(tags_of(part) == std::vector<std::string>{"Enemy"});
}

TEST_CASE("TAG4 tags save with the place and load back", "[tags][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const engine_core::InstanceId holder = named_part(game, game.scene_service("Workspace"), "Holder");
    REQUIRE(!game.instance(holder)->add_tag("Enemy"));
    REQUIRE(!game.instance(holder)->add_tag("Boss"));
    project.save();

    engine_core::Game other;
    engine_core::Project read = engine_core::Project::load(dir.path, other);
    const engine_core::InstanceId loaded = other.find_first_child(other.scene_service("Workspace"), "Holder");
    REQUIRE(loaded != 0);
    REQUIRE(tags_of(*other.instance(loaded)) == std::vector<std::string>{"Boss", "Enemy"});
    REQUIRE(other.tagged("Enemy") == std::vector<engine_core::InstanceId>{loaded});
}

TEST_CASE("TAG5 Clone copies tags", "[tags][copy]") {
    ScriptRig rig;
    engine_core::DataModel& game = rig.game;
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), workspace_of(game));
    const engine_core::InstanceId a = named_part(game, folder.id(), "A");
    REQUIRE(!game.instance(a)->add_tag("Enemy"));
    const engine_core::InstanceId copy = engine_core::clone_tree(game, folder.id());
    REQUIRE(copy != 0);
    const engine_core::InstanceId a2 = game.find_first_child(copy, "A");
    REQUIRE(a2 != 0);
    REQUIRE(tags_of(*game.instance(a2)) == std::vector<std::string>{"Enemy"});
    // The copy counts once it is in the place.
    REQUIRE(game.tagged("Enemy") == std::vector<engine_core::InstanceId>{a});
    game.set_parent(copy, workspace_of(game));
    REQUIRE(game.tagged("Enemy") == std::vector<engine_core::InstanceId>{a, a2});
}

TEST_CASE("TAG6 Stop puts tags back, and a destroyed instance leaves GetTagged", "[tags]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId holder = named_part(game, game.scene_service("Workspace"), "Holder");
    const engine_core::InstanceId other = named_part(game, game.scene_service("Workspace"), "Other");
    REQUIRE(!game.instance(holder)->add_tag("Enemy"));
    game.capture_place();
    game.start_simulation();
    REQUIRE(game.instance(holder)->remove_tag("Enemy"));
    REQUIRE(!game.instance(holder)->add_tag("Extra"));
    REQUIRE(!game.instance(other)->add_tag("Enemy"));
    const engine_core::InstanceId made = named_part(game, game.scene_service("Workspace"), "Made");
    REQUIRE(!game.instance(made)->add_tag("Enemy"));
    REQUIRE(game.tagged("Enemy") == std::vector<engine_core::InstanceId>{other, made});
    game.stop_simulation();
    REQUIRE(tags_of(*game.instance(holder)) == std::vector<std::string>{"Enemy"});
    REQUIRE(game.instance(other)->tags().empty());
    REQUIRE(game.tagged("Enemy") == std::vector<engine_core::InstanceId>{holder});
    REQUIRE(game.tagged("Extra").empty());

    // Destroying a tagged instance fires its removed signal once per tag, then
    // it is no longer tagged.
    REQUIRE(!game.instance(holder)->add_tag("Boss"));
    int removed = 0;
    engine_core::InstanceId removed_id = 0;
    game.tag_signal("Enemy", false).connect([&](engine_core::InstanceId, engine_core::Field) {
        const engine_core::EventArgs* args = game.events().current_args();
        REQUIRE(args != nullptr);
        REQUIRE(args->size() == 1);
        REQUIRE(args->at(0).kind == LuaSlot::Kind::Instance);
        removed_id = args->at(0).id;
        ++removed;
    });
    game.destroy(holder);
    game.events().drain();
    REQUIRE(removed == 1);
    REQUIRE(removed_id == holder);
    REQUIRE(game.tagged("Enemy").empty());
    REQUIRE(game.tagged("Boss").empty());
}

TEST_CASE("TAG7 scripts add, remove, and query tags, and the tag signals fire with the instance", "[tags][lua]") {
    ScriptRig rig;
    named_part(rig.game, workspace_of(rig.game), "A");
    named_part(rig.game, workspace_of(rig.game), "B");
    add_script(rig.game, "Tags", R"(
        local CollectionService = game:GetService("CollectionService")
        local a, b = workspace.A, workspace.B
        CollectionService:GetInstanceAddedSignal("Enemy"):Connect(function(i) print("added", i.Name) end)
        CollectionService:GetInstanceRemovedSignal("Enemy"):Connect(function(i)
            print("removed", if i == b then "B" else i.Name)
        end)
        a:AddTag("Enemy")
        b:AddTag("Enemy")
        a:AddTag("Enemy")
        a:AddTag("Boss")
        print("has", a:HasTag("Enemy"), a:HasTag("Other"))
        print("tags", table.concat(a:GetTags(), ","))
        local tagged = CollectionService:GetTagged("Enemy")
        print("tagged", #tagged, tagged[1].Name, tagged[2].Name)
        print("empty", pcall(function() a:AddTag("") end))
        print("long", pcall(function() a:AddTag(string.rep("x", 101)) end))
        print("control", pcall(function() a:AddTag("a\nb") end))
        print("root", pcall(function() game:AddTag("X") end))
        print("signal", pcall(function() CollectionService:GetInstanceAddedSignal("") end))
        task.wait()
        a:RemoveTag("Enemy")
        a:RemoveTag("Enemy")
        print("after", #CollectionService:GetTagged("Enemy"), a:HasTag("Enemy"))
        task.wait()
        b:Destroy()
        print("gone", #CollectionService:GetTagged("Enemy"))
    )");
    rig.game.start_simulation();
    rig.frames(10);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    const std::vector<std::string> out = output(rig);
    REQUIRE(count_line(out, "added\tA\n") == 1);
    REQUIRE(count_line(out, "added\tB\n") == 1);
    REQUIRE(count_line(out, "has\ttrue\tfalse\n") == 1);
    REQUIRE(count_line(out, "tags\tBoss,Enemy\n") == 1);
    REQUIRE(count_line(out, "tagged\t2\tA\tB\n") == 1);
    REQUIRE(has_prefix(out, "empty\tfalse\t"));
    REQUIRE(has_prefix(out, "long\tfalse\t"));
    REQUIRE(has_prefix(out, "control\tfalse\t"));
    REQUIRE(has_prefix(out, "root\tfalse\t"));
    REQUIRE(has_prefix(out, "signal\tfalse\t"));
    REQUIRE(count_line(out, "after\t1\tfalse\n") == 1);
    REQUIRE(count_line(out, "removed\tA\n") == 1);
    REQUIRE(count_line(out, "gone\t0\n") == 1);
    REQUIRE(count_line(out, "removed\tB\n") == 1);
    REQUIRE(index_of(out, "removed\tA\n") < index_of(out, "removed\tB\n"));
    rig.game.stop_simulation();
}

TEST_CASE("TAG8 Observe runs once for every instance each time it gains the tag, until Disconnect", "[tags][lua]") {
    ScriptRig rig;
    for (const char* name : {"A", "B", "C", "D", "E"}) {
        named_part(rig.game, workspace_of(rig.game), name);
    }
    add_script(rig.game, "Observe", R"(
        local CollectionService = game:GetService("CollectionService")
        local a, b, c, d, e = workspace.A, workspace.B, workspace.C, workspace.D, workspace.E
        a:AddTag("T")
        e:AddTag("T")
        local connection = CollectionService:Observe("T", function(i)
            local name = i.Name
            print("observe", name)
            if name == "E" then
                error("boom")
            end
            return function() print("cleanup", name) end
        end)
        CollectionService:GetInstanceRemovedSignal("T"):Connect(function(i) print("removed", i.Name) end)
        task.wait()
        print("mark", 1)
        b:AddTag("T")
        c:AddTag("T")
        d:AddTag("T")
        task.wait()
        print("mark", 2)
        a:RemoveTag("T")
        task.wait()
        a:AddTag("T")
        task.wait()
        print("mark", 3)
        b:Destroy()
        task.wait()
        print("mark", 4)
        connection:Disconnect()
        connection:Disconnect()
        print("mark", 5)
        a:RemoveTag("T")
        a:AddTag("T")
        c:RemoveTag("T")
        task.wait()
        task.wait()
        print("mark", 6)
    )");
    rig.game.start_simulation();
    rig.frames(20);
    const std::vector<std::string> out = output(rig);
    // E's callback raised; the others ran on.
    REQUIRE(rig.runtime.last_error().find("boom") != std::string::npos);
    REQUIRE(count_line(out, "mark\t6\n") == 1);
    // A lost the tag and gained it again while observed, and once more after Disconnect.
    REQUIRE(count_line(out, "observe\tA\n") == 2);
    for (const char* name : {"B", "C", "D", "E"}) {
        INFO(name);
        REQUIRE(count_line(out, std::string("observe\t") + name + "\n") == 1);
    }
    // What callback returns is not called, at a removal or at Disconnect.
    for (const std::string& line : out) {
        REQUIRE(line.find("cleanup") == std::string::npos);
    }
    REQUIRE(count_line(out, "removed\tA\n") == 2);
    REQUIRE(count_line(out, "removed\tC\n") == 1);
    REQUIRE(index_of(out, "observe\tA\n") < index_of(out, "mark\t1\n"));
    REQUIRE(index_of(out, "observe\tB\n") > index_of(out, "mark\t1\n"));
    REQUIRE(index_of(out, "observe\tD\n") < index_of(out, "mark\t2\n"));
    rig.game.stop_simulation();
}

TEST_CASE("TAG9 strict script analysis accepts the tag and CollectionService members", "[tags][lua]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Uses", R"(--!strict
local CollectionService = game:GetService("CollectionService")
local p = workspace
p:AddTag("Enemy")
p:RemoveTag("Enemy")
local has: boolean = p:HasTag("Enemy")
local tags = p:GetTags()
local first: string = tags[1]
local tagged = CollectionService:GetTagged("Enemy")
CollectionService:GetInstanceAddedSignal("Enemy"):Connect(function(instance) print(instance) end)
CollectionService:GetInstanceRemovedSignal("Enemy"):Connect(function(instance) print(instance) end)
local connection = CollectionService:Observe("Enemy", function(instance) print(instance) end)
connection:Disconnect()
print(has, first, #tags, #tagged, tagged[1].Name)
)");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!analysis.idle()) {
        analysis.pump();
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    std::string problems;
    for (const engine_core::Diagnostic& diagnostic : analysis.diagnostics(script.id())) {
        if (diagnostic.severity == engine_core::Severity::Error || diagnostic.severity == engine_core::Severity::Warning) {
            problems += diagnostic.code + " @" + std::to_string(diagnostic.range.start.line) + " " + diagnostic.message + "\n";
        }
    }
    INFO(problems);
    REQUIRE(problems.empty());
}

TEST_CASE("TAG10 a tagged instance counts only while it is in the place", "[tags][lua]") {
    ScriptRig rig;
    add_script(rig.game, "Loose", R"(
        local CollectionService = game:GetService("CollectionService")
        local loose = Instance.new("Folder")
        loose.Name = "Loose"
        loose:AddTag("T")
        CollectionService:GetInstanceAddedSignal("T"):Connect(function(i) print("added", i.Name) end)
        CollectionService:GetInstanceRemovedSignal("T"):Connect(function(i) print("removed", i.Name) end)
        print("tagged", #CollectionService:GetTagged("T"))
        CollectionService:Observe("T", function(i) print("observe", i.Name) end)
        task.wait()
        print("mark", 1)
        loose.Parent = workspace
        task.wait()
        print("mark", 2, #CollectionService:GetTagged("T"))
        loose.Parent = nil
        task.wait()
        print("mark", 3, #CollectionService:GetTagged("T"))
    )");
    rig.game.start_simulation();
    rig.frames(10);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    const std::vector<std::string> out = output(rig);
    REQUIRE(count_line(out, "tagged\t0\n") == 1);
    REQUIRE(count_line(out, "added\tLoose\n") == 1);
    REQUIRE(count_line(out, "observe\tLoose\n") == 1);
    REQUIRE(count_line(out, "removed\tLoose\n") == 1);
    REQUIRE(count_line(out, "mark\t2\t1\n") == 1);
    REQUIRE(count_line(out, "mark\t3\t0\n") == 1);
    REQUIRE(index_of(out, "observe\tLoose\n") > index_of(out, "mark\t1\n"));
    REQUIRE(index_of(out, "observe\tLoose\n") < index_of(out, "mark\t2\t1\n"));
    rig.game.stop_simulation();
}

TEST_CASE("TAG11 destroying a parent takes its tagged children out of the place", "[tags][lua]") {
    ScriptRig rig;
    add_script(rig.game, "Level", R"(
        local CollectionService = game:GetService("CollectionService")
        local level = Instance.new("Folder")
        level.Name = "Level"
        local first
        for i = 1, 3 do
            local enemy = Instance.new("Folder")
            enemy.Name = "E" .. i
            enemy:AddTag("Enemy")
            enemy.Parent = level
            first = first or enemy
        end
        level.Parent = workspace
        CollectionService:GetInstanceRemovedSignal("Enemy"):Connect(function() print("removed") end)
        task.wait()
        print("before", #CollectionService:GetTagged("Enemy"))
        level:Destroy()
        print("child", pcall(function() return first.Name end), first.Parent == nil)
        task.wait()
        print("after", #CollectionService:GetTagged("Enemy"))
    )");
    rig.game.start_simulation();
    rig.frames(10);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    const std::vector<std::string> out = output(rig);
    REQUIRE(count_line(out, "before\t3\n") == 1);
    REQUIRE(count_line(out, "removed\n") == 3);
    REQUIRE(count_line(out, "after\t0\n") == 1);
    rig.game.stop_simulation();
}

TEST_CASE("TAG12 a play script's tag signals and Observe never see an instance Core holds", "[tags][lua]") {
    ScriptRig rig;
    const engine_core::InstanceId secret = named_part(rig.game, rig.game.core(), "Secret");
    const engine_core::InstanceId seen = named_part(rig.game, workspace_of(rig.game), "Seen");
    add_script(rig.game, "Watch", R"(
        local CollectionService = game:GetService("CollectionService")
        CollectionService:GetInstanceAddedSignal("T"):Connect(function(i) print("added", i.Name) end)
        CollectionService:GetInstanceRemovedSignal("T"):Connect(function(i) print("removed", i.Name) end)
        CollectionService:Observe("T", function(i) print("observe", i.Name) end)
    )");
    rig.game.start_simulation();
    rig.frames(2);
    REQUIRE(!rig.game.instance(secret)->add_tag("T"));
    rig.frames(2);
    REQUIRE(!rig.game.instance(seen)->add_tag("T"));
    rig.frames(2);
    REQUIRE(rig.game.instance(secret)->remove_tag("T"));
    REQUIRE(rig.game.instance(seen)->remove_tag("T"));
    rig.frames(2);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    const std::vector<std::string> out = output(rig);
    for (const std::string& line : out) {
        REQUIRE(line.find("Secret") == std::string::npos);
    }
    REQUIRE(count_line(out, "added\tSeen\n") == 1);
    REQUIRE(count_line(out, "observe\tSeen\n") == 1);
    REQUIRE(count_line(out, "removed\tSeen\n") == 1);
    rig.game.stop_simulation();
}

TEST_CASE("TAG13 Observe's callback can yield", "[tags][lua]") {
    ScriptRig rig;
    named_part(rig.game, workspace_of(rig.game), "A");
    named_part(rig.game, workspace_of(rig.game), "B");
    add_script(rig.game, "Yields", R"(
        local CollectionService = game:GetService("CollectionService")
        workspace.A:AddTag("T")
        CollectionService:Observe("T", function(i)
            print("start", i.Name)
            task.wait()
            print("end", i.Name)
        end)
        print("mark", 1)
        task.wait()
        workspace.B:AddTag("T")
    )");
    rig.game.start_simulation();
    rig.frames(10);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    const std::vector<std::string> out = output(rig);
    for (const char* name : {"A", "B"}) {
        INFO(name);
        REQUIRE(count_line(out, std::string("start\t") + name + "\n") == 1);
        REQUIRE(count_line(out, std::string("end\t") + name + "\n") == 1);
    }
    REQUIRE(index_of(out, "start\tA\n") < index_of(out, "end\tA\n"));
    REQUIRE(index_of(out, "start\tB\n") < index_of(out, "end\tB\n"));
    rig.game.stop_simulation();
}
