// TweenService: tweens of instance properties and plain values, stepped once a
// play frame before Heartbeat's handlers resume, dropped at Stop.

#include "support.hpp"

#include "Animation.hpp"
#include "AssetInstances.hpp"
#include "LuaApi.hpp"
#include "Matrix4.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"
#include "TweenWorld.hpp"
#include "WireframeAdornment.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

namespace {

using engine_core::LuaSlot;
namespace aanim = anarchy::aanim;

std::vector<std::string> printed(const engine_core::ScriptRuntime::OutputBatch& batch) {
    std::vector<std::string> out;
    for (const auto& line : batch.lines) {
        out.push_back(line.text);
    }
    return out;
}

bool has_line(const std::vector<std::string>& lines, const std::string& text) {
    return std::find(lines.begin(), lines.end(), text) != lines.end();
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

bool near(double a, double b, double tolerance = 1e-4) { return std::abs(a - b) <= tolerance; }

engine_core::InstanceId named_part(engine_core::DataModel& game, const char* name) {
    engine_core::GameObject& part = create_part(game);
    game.set_name(part.id(), name);
    return part.id();
}

LuaSlot read_property(engine_core::DataModel& game, engine_core::InstanceId id, const char* name) {
    engine_core::DataModel* object = game.instance(id);
    REQUIRE(object != nullptr);
    const engine_core::LuaField* field = engine_core::lua_class_find(object->class_name(), name);
    REQUIRE(field != nullptr);
    LuaSlot slot;
    REQUIRE(field->read(game, *object, slot));
    return slot;
}

double x_of(engine_core::DataModel& game, engine_core::InstanceId id) {
    return read_property(game, id, "Transform").transform.m[12];
}

double transparency_of(engine_core::DataModel& game, engine_core::InstanceId id) {
    return read_property(game, id, "Transparency").number;
}

std::vector<std::string> output(ScriptRig& rig) {
    std::vector<std::string> out = printed(rig.runtime.drain_output());
    for (const std::string& line : out) {
        UNSCOPED_INFO(line);
    }
    return out;
}

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

}  // namespace

TEST_CASE("TW1 every easing curve starts at 0 and ends at 1", "[tween]") {
    using S = aanim::EasingStyle;
    using D = aanim::EasingDirection;
    REQUIRE(engine_core::ease(S::Linear, D::In, 0.5f) == 0.5f);
    REQUIRE(near(engine_core::ease(S::Quad, D::Out, 0.5f), 0.75));
    REQUIRE(near(engine_core::ease(S::Quad, D::In, 0.5f), 0.25));
    for (std::uint8_t style = 0; style < aanim::kEasingStyleCount; ++style) {
        for (std::uint8_t direction = 0; direction < aanim::kEasingDirectionCount; ++direction) {
            INFO(int(style) << " " << int(direction));
            const auto s = static_cast<S>(style);
            const auto d = static_cast<D>(direction);
            REQUIRE(near(engine_core::ease(s, d, 0.f), 0.0, 1e-5));
            REQUIRE(near(engine_core::ease(s, d, 1.f), 1.0, 1e-5));
            for (int step = 0; step <= 20; ++step) {
                REQUIRE(std::isfinite(engine_core::ease(s, d, static_cast<float>(step) / 20.f)));
            }
        }
    }
}

TEST_CASE("TW2 tween_lerp moves numbers, vectors, colors, and matrices, and refuses the rest", "[tween]") {
    LuaSlot out;
    REQUIRE(engine_core::tween_lerp(number_slot(2), number_slot(4), 0.5, out));
    REQUIRE(out.kind == LuaSlot::Kind::Number);
    REQUIRE(out.number == 3);
    REQUIRE(engine_core::tween_lerp(number_slot(2), number_slot(4), 1.0, out));
    REQUIRE(out.number == 4);

    for (LuaSlot::Kind kind : {LuaSlot::Kind::Vec2, LuaSlot::Kind::Vec3}) {
        LuaSlot from;
        from.kind = kind;
        from.vec = engine_core::Vec3{0.f, 2.f, kind == LuaSlot::Kind::Vec3 ? 4.f : 0.f};
        LuaSlot to = from;
        to.vec = engine_core::Vec3{2.f, 4.f, kind == LuaSlot::Kind::Vec3 ? 8.f : 0.f};
        REQUIRE(engine_core::tween_lerp(from, to, 0.5, out));
        REQUIRE(out.kind == kind);
        REQUIRE(near(out.vec.x, 1));
        REQUIRE(near(out.vec.y, 3));
        REQUIRE(near(out.vec.z, kind == LuaSlot::Kind::Vec3 ? 6 : 0));
    }

    LuaSlot red;
    red.kind = LuaSlot::Kind::Color;
    red.color = rgb(1.f, 0.f, 0.f);
    LuaSlot blue = red;
    blue.color = rgb(0.f, 0.f, 1.f);
    REQUIRE(engine_core::tween_lerp(red, blue, 0.5, out));
    REQUIRE(out.kind == LuaSlot::Kind::Color);
    REQUIRE(near(out.color.r, 0.5));
    REQUIRE(near(out.color.b, 0.5));

    LuaSlot start;
    start.kind = LuaSlot::Kind::Matrix4;
    start.transform = engine_core::matrix4_identity();
    LuaSlot goal = start;
    goal.transform = engine_core::matrix4_axis_angle(engine_core::Vec3{0.f, 1.f, 0.f}, 3.14159265358979 / 2);
    goal.transform.m[12] = 2.f;
    REQUIRE(engine_core::tween_lerp(start, goal, 0.5, out));
    REQUIRE(out.kind == LuaSlot::Kind::Matrix4);
    const engine_core::Matrix4 half = engine_core::matrix4_axis_angle(engine_core::Vec3{0.f, 1.f, 0.f}, 3.14159265358979 / 4);
    REQUIRE(near(out.transform.m[12], 1));
    for (int i = 0; i < 12; ++i) {
        INFO(i);
        REQUIRE(near(out.transform.m[i], half.m[i]));
    }

    LuaSlot text;
    text.kind = LuaSlot::Kind::String;
    text.text = "a";
    REQUIRE_FALSE(engine_core::tween_lerp(text, text, 0.5, out));
    REQUIRE_FALSE(engine_core::tween_lerp(number_slot(1), red, 0.5, out));
}

TEST_CASE("TW3 a tween reaches its midpoint and its goal on time, then Completed fires true once", "[tween]") {
    ScriptRig rig;
    const engine_core::InstanceId part = named_part(rig.game, "Part");
    add_script(rig.game, "Move", R"(
        local TweenService = game:GetService("TweenService")
        local tween = TweenService:Tween(workspace.Part, {Transform = Matrix4.new(10, 0, 0)}, {Time = 1, Style = "Linear"})
        print("made", typeof(tween), tostring(tween), tween.Playing, tween == tween)
        local fired = 0
        tween.Completed:Connect(function(done)
            fired += 1
            print("completed", done, fired, tween.Playing)
        end)
    )");
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(output(rig), "made\tTween\tTween\ttrue\ttrue\n"));
    // The script starts before the first frame's tweens step.
    REQUIRE(near(x_of(rig.game, part), 10.0 / 60.0, 1e-3));
    rig.frames(29);
    REQUIRE(near(x_of(rig.game, part), 5, 1e-3));
    rig.frames(30);
    REQUIRE(x_of(rig.game, part) == 10);
    rig.frames(5);
    const std::vector<std::string> out = output(rig);
    REQUIRE(count_line(out, "completed\ttrue\t1\tfalse\n") == 1);
    REQUIRE_FALSE(has_prefix(out, "completed\ttrue\t2"));
    REQUIRE(x_of(rig.game, part) == 10);
    rig.game.stop_simulation();
}

TEST_CASE("TW4 Delay, Repeat, and Reverses shape a tween; unknown options raise", "[tween]") {
    ScriptRig rig;
    const engine_core::InstanceId late = named_part(rig.game, "Late");
    const engine_core::InstanceId back = named_part(rig.game, "Back");
    add_script(rig.game, "Shape", R"(
        local TweenService = game:GetService("TweenService")
        TweenService:Tween(workspace.Late, {Transparency = 1}, {Time = 0.5, Delay = 0.5, Style = Enum.EasingStyle.Linear})
        local back = TweenService:Tween(workspace.Back, {Transform = Matrix4.new(10, 0, 0)},
            {Time = 0.25, Repeat = 1, Reverses = true, Style = "Linear", Direction = "InOut"})
        back.Completed:Connect(function(done) print("back done", done) end)
        print("key", pcall(function() TweenService:Tween(workspace.Late, {Transparency = 1}, {Speed = 2}) end))
        print("style", pcall(function() TweenService:Tween(workspace.Late, {Transparency = 1}, {Style = "Wobbly"}) end))
        print("time", pcall(function() TweenService:Tween(workspace.Late, {Transparency = 1}, {Time = 0 / 0}) end))
        print("delay", pcall(function() TweenService:Tween(workspace.Late, {Transparency = 1}, {Delay = -1}) end))
        print("repeat", pcall(function() TweenService:Tween(workspace.Late, {Transparency = 1}, {Repeat = -2}) end))
        print("reverses", pcall(function() TweenService:Tween(workspace.Late, {Transparency = 1}, {Reverses = 1}) end))
    )");
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    std::vector<std::string> out = output(rig);
    REQUIRE(has_prefix(out, "key\tfalse\t"));
    REQUIRE(std::any_of(out.begin(), out.end(), [](const std::string& line) {
        return line.find("unknown tween option Speed") != std::string::npos;
    }));
    REQUIRE(has_prefix(out, "style\tfalse\t"));
    REQUIRE(has_prefix(out, "time\tfalse\t"));
    REQUIRE(has_prefix(out, "delay\tfalse\t"));
    REQUIRE(has_prefix(out, "repeat\tfalse\t"));
    REQUIRE(has_prefix(out, "reverses\tfalse\t"));
    rig.frames(8);
    REQUIRE(x_of(rig.game, back) > 0);
    rig.frames(20);
    // Still waiting out its delay.
    REQUIRE(transparency_of(rig.game, late) == 0);
    rig.frames(12);
    REQUIRE(transparency_of(rig.game, late) > 0);
    REQUIRE_FALSE(has_prefix(output(rig), "back done"));
    rig.frames(30);
    out = output(rig);
    REQUIRE(has_line(out, "back done\ttrue\n"));
    REQUIRE(x_of(rig.game, back) == 0);
    REQUIRE(transparency_of(rig.game, late) == 1);
    rig.game.stop_simulation();
}

TEST_CASE("TW5 a new tween on a property replaces the running one there and leaves the rest", "[tween]") {
    ScriptRig rig;
    const engine_core::InstanceId one = named_part(rig.game, "One");
    const engine_core::InstanceId two = named_part(rig.game, "Two");
    add_script(rig.game, "Replace", R"(
        local TweenService = game:GetService("TweenService")
        local first = TweenService:Tween(workspace.One, {Transform = Matrix4.new(10, 0, 0)}, {Time = 1})
        local other = TweenService:Tween(workspace.One, {Transparency = 1}, {Time = 0.5})
        local both = TweenService:Tween(workspace.Two, {Transform = Matrix4.new(10, 0, 0), Transparency = 1}, {Time = 0.5})
        first.Completed:Connect(function(done) print("first", done) end)
        other.Completed:Connect(function(done) print("other", done) end)
        both.Completed:Connect(function(done) print("both", done) end)
        task.wait(0.1)
        local second = TweenService:Tween(workspace.One, {Transform = Matrix4.new(0, 0, 0)}, {Time = 0.25})
        second.Completed:Connect(function(done) print("second", done) end)
        TweenService:Tween(workspace.Two, {Transform = Matrix4.new(-5, 0, 0)}, {Time = 0.25})
    )");
    rig.game.start_simulation();
    rig.frames(60);
    INFO(rig.runtime.last_error());
    const std::vector<std::string> out = output(rig);
    REQUIRE(has_line(out, "first\tfalse\n"));
    REQUIRE(has_line(out, "second\ttrue\n"));
    REQUIRE(has_line(out, "other\ttrue\n"));
    REQUIRE(has_line(out, "both\ttrue\n"));
    REQUIRE(x_of(rig.game, one) == 0);
    REQUIRE(transparency_of(rig.game, one) == 1);
    REQUIRE(x_of(rig.game, two) == -5);
    REQUIRE(transparency_of(rig.game, two) == 1);
    rig.game.stop_simulation();
}

TEST_CASE("TW6 Pause, Resume, Cancel, Playing, and Wait", "[tween]") {
    ScriptRig rig;
    const engine_core::InstanceId held = named_part(rig.game, "Held");
    named_part(rig.game, "Cut");
    add_script(rig.game, "Control", R"(
        local TweenService = game:GetService("TweenService")
        local held = workspace.Held
        local t = TweenService:Tween(held, {Transparency = 1}, {Time = 0.25, Style = "Linear"})
        print("playing", t.Playing)
        t:Pause()
        print("paused", t.Playing)
        task.wait(0.25)
        print("held", held.Transparency)
        t:Resume()
        print("resumed", t.Playing)
        print("result", t:Wait())
        print("again", t:Wait(), t.Playing)
        local c = TweenService:Tween(held, {Transparency = 0}, {Time = 1})
        task.wait(0.1)
        c:Cancel()
        print("cancelled", c.Playing, c:Wait())
        local w = TweenService:Tween(workspace.Cut, {Transparency = 1}, {Time = 1})
        task.delay(0.1, function() w:Cancel() end)
        print("waited", w:Wait())
        t:Pause()
        t:Resume()
        t:Cancel()
        print("ended calls", t:Wait())
    )");
    rig.game.start_simulation();
    rig.frames(90);
    INFO(rig.runtime.last_error());
    const std::vector<std::string> out = output(rig);
    REQUIRE(has_line(out, "playing\ttrue\n"));
    REQUIRE(has_line(out, "paused\tfalse\n"));
    REQUIRE(has_line(out, "held\t0\n"));
    REQUIRE(has_line(out, "resumed\ttrue\n"));
    REQUIRE(has_line(out, "result\ttrue\n"));
    REQUIRE(has_line(out, "again\ttrue\tfalse\n"));
    REQUIRE(has_line(out, "cancelled\tfalse\tfalse\n"));
    REQUIRE(has_line(out, "waited\tfalse\n"));
    REQUIRE(has_line(out, "ended calls\ttrue\n"));
    const double left = transparency_of(rig.game, held);
    REQUIRE(left > 0);
    REQUIRE(left < 1);
    rig.game.stop_simulation();
}

TEST_CASE("TW7 a tween whose instance is destroyed ends as cancelled, writing nothing more", "[tween]") {
    ScriptRig rig;
    named_part(rig.game, "Doomed");
    named_part(rig.game, "Waiting");
    add_script(rig.game, "Destroy", R"(
        local TweenService = game:GetService("TweenService")
        local doomed = workspace.Doomed
        local t = TweenService:Tween(doomed, {Transform = Matrix4.new(5, 0, 0)}, {Time = 1})
        t.Completed:Connect(function(done) print("doomed", done, t.Playing) end)
        local waiting = workspace.Waiting
        local d = TweenService:Tween(waiting, {Transparency = 1}, {Time = 1, Delay = 1})
        d.Completed:Connect(function(done) print("waiting", done) end)
        task.wait(0.1)
        doomed:Destroy()
        waiting:Destroy()
    )");
    rig.game.start_simulation();
    rig.frames(30);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    const std::vector<std::string> out = output(rig);
    REQUIRE(count_line(out, "doomed\tfalse\tfalse\n") == 1);
    REQUIRE(count_line(out, "waiting\tfalse\n") == 1);
    rig.game.stop_simulation();
}

TEST_CASE("TW8 Stop drops every tween and puts the properties back", "[tween]") {
    ScriptRig rig;
    const engine_core::InstanceId part = named_part(rig.game, "Part");
    engine_core::Script& script = add_script(rig.game, "Long", R"(
        local TweenService = game:GetService("TweenService")
        _G.tween = TweenService:Tween(workspace.Part, {Transform = Matrix4.new(10, 0, 0)}, {Time = 10, Style = "Linear"})
    )");
    rig.game.start_simulation();
    rig.frames(30);
    INFO(rig.runtime.last_error());
    REQUIRE(x_of(rig.game, part) > 0);
    rig.game.stop_simulation();
    REQUIRE(x_of(rig.game, part) == 0);
    rig.frames(5);
    REQUIRE(x_of(rig.game, part) == 0);
    // Nothing from the last Test moves the part in the next.
    script.set_enabled(false);
    rig.game.start_simulation();
    rig.frames(30);
    REQUIRE(x_of(rig.game, part) == 0);
    rig.game.stop_simulation();
    // And with the place stopped, TweenService refuses.
    rig.runtime.run_chunk(R"(
        print("stopped", pcall(function()
            game:GetService("TweenService"):Tween(workspace.Part, {Transparency = 1})
        end))
    )");
    const std::vector<std::string> out = output(rig);
    REQUIRE(std::any_of(out.begin(), out.end(), [](const std::string& line) {
        return line.rfind("stopped\tfalse\t", 0) == 0 &&
               line.find("TweenService tweens only while the place is playing") != std::string::npos;
    }));
}

TEST_CASE("TW9 Value tweens a plain value, calling back each frame and ending exactly on the goal", "[tween]") {
    ScriptRig rig;
    add_script(rig.game, "Plain", R"(
        local TweenService = game:GetService("TweenService")
        local calls, last, rising = 0, nil, true
        local t = TweenService:Value(0, 10, {Time = 1, Style = "Linear"}, function(value)
            if last ~= nil and value < last then rising = false end
            calls += 1
            last = value
        end)
        print("value", typeof(t))
        print("wait", t:Wait())
        task.wait()
        print("ended", calls >= 59 and calls <= 62, last == 10, rising)
        print("mixed", pcall(function() TweenService:Value(0, Vector3.new(), {}, function() end) end))
        print("kind", pcall(function() TweenService:Value("a", "b", {}, function() end) end))
        print("callback", pcall(function() TweenService:Value(0, 1, {}) end))
        local v
        TweenService:Value(Vector3.new(0, 0, 0), Vector3.new(2, 4, 6), {Time = 0.1}, function(value) v = value end)
        task.wait(0.3)
        print("vector", v == Vector3.new(2, 4, 6))
    )");
    rig.game.start_simulation();
    rig.frames(100);
    INFO(rig.runtime.last_error());
    const std::vector<std::string> out = output(rig);
    REQUIRE(has_line(out, "value\tTween\n"));
    REQUIRE(has_line(out, "wait\ttrue\n"));
    REQUIRE(has_line(out, "ended\ttrue\ttrue\ttrue\n"));
    REQUIRE(has_prefix(out, "mixed\tfalse\t"));
    REQUIRE(has_prefix(out, "kind\tfalse\t"));
    REQUIRE(has_prefix(out, "callback\tfalse\t"));
    REQUIRE(has_line(out, "vector\ttrue\n"));
    rig.game.stop_simulation();
}

TEST_CASE("TW10 a property that cannot tween raises, naming it", "[tween]") {
    ScriptRig rig;
    named_part(rig.game, "Part");
    engine_core::Animation& clip = rig.game.create<engine_core::Animation>();
    rig.game.set_name(clip.id(), "Clip");
    rig.game.set_parent(clip.id(), rig.game.service("Animations"));
    add_script(rig.game, "Refuse", R"(
        local TweenService = game:GetService("TweenService")
        local part = workspace.Part
        local function why(f)
            local ok, message = pcall(f)
            return tostring(ok) .. " " .. tostring(message)
        end
        print("name", why(function() TweenService:Tween(part, {Name = "x"}) end))
        print("readonly", why(function() TweenService:Tween(game.Assets.Animations.Clip, {Length = 2}) end))
        print("wrong", why(function() TweenService:Tween(part, {Transparency = Vector3.new()}) end))
        print("missing", why(function() TweenService:Tween(part, {Nothing = 1}) end))
        print("empty", why(function() TweenService:Tween(part, {}) end))
        print("instance", why(function() TweenService:Tween(5, {Transparency = 1}) end))
    )");
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    const std::vector<std::string> out = output(rig);
    auto line_has = [&](const char* prefix, const char* text) {
        return std::any_of(out.begin(), out.end(), [&](const std::string& line) {
            return line.rfind(prefix, 0) == 0 && line.find("false ") != std::string::npos &&
                   line.find(text) != std::string::npos;
        });
    };
    REQUIRE(line_has("name\t", "Name cannot be tweened"));
    REQUIRE(line_has("readonly\t", "Length cannot be tweened"));
    REQUIRE(line_has("wrong\t", "Transparency expects a number"));
    REQUIRE(line_has("missing\t", "Nothing cannot be tweened"));
    REQUIRE(line_has("empty\t", "false"));
    REQUIRE(line_has("instance\t", "false"));
    rig.game.stop_simulation();
}

TEST_CASE("TW11 strict script analysis accepts the TweenService and Tween members", "[tween]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Uses", R"(--!strict
local TweenService = game:GetService("TweenService")
local tween = TweenService:Tween(workspace, {Gravity = 1}, {Time = 1, Style = Enum.EasingStyle.Quad, Direction = Enum.EasingDirection.Out})
tween:Pause()
tween:Resume()
tween:Cancel()
local playing: boolean = tween.Playing
local done: boolean = tween:Wait()
tween.Completed:Connect(function(completed: boolean) print(completed) end)
local value = TweenService:Value(0, 1, {Time = 0.5}, function(v) print(v) end)
print(playing, done, value.Playing)
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

TEST_CASE("TW12 a Back tween that overshoots a range-checked property clamps there and completes on the goal",
          "[tween]") {
    ScriptRig rig;
    engine_core::WireframeAdornment& wire = rig.game.create<engine_core::WireframeAdornment>();
    rig.game.set_parent(wire.id(), workspace_of(rig.game));
    rig.game.set_name(wire.id(), "Wire");
    REQUIRE(!wire.set_transparency(0.5));
    add_script(rig.game, "Back", R"(
        local TweenService = game:GetService("TweenService")
        local t = TweenService:Tween(workspace.Wire, {Transparency = 1}, {Time = 0.5, Style = "Back", Direction = "Out"})
        print("done", t:Wait())
    )");
    rig.game.start_simulation();
    rig.frames(60);
    INFO(rig.runtime.last_error());
    const std::vector<std::string> out = output(rig);
    REQUIRE(has_line(out, "done\ttrue\n"));
    REQUIRE(wire.transparency() == 1.0);
    rig.game.stop_simulation();
}

TEST_CASE("TW13 a write refused inside the range cancels the tween and says so once, naming the property",
          "[tween]") {
    ScriptRig rig;
    engine_core::WireframeAdornment& wire = rig.game.create<engine_core::WireframeAdornment>();
    rig.game.set_parent(wire.id(), workspace_of(rig.game));
    rig.game.set_name(wire.id(), "Wire");
    add_script(rig.game, "Over", R"(
        local TweenService = game:GetService("TweenService")
        local t = TweenService:Tween(workspace.Wire, {Transparency = 2}, {Time = 0.5, Style = "Linear"})
        print("done", t:Wait())
    )");
    rig.game.start_simulation();
    rig.frames(60);
    const std::vector<std::string> lines = printed(rig.runtime.drain_output());
    REQUIRE(has_line(lines, "done\tfalse\n"));
    std::size_t errors = 0;
    for (const std::string& line : lines) {
        UNSCOPED_INFO(line);
        if (line.find("Transparency") != std::string::npos && line.find("from 0 to 1") != std::string::npos) {
            ++errors;
        }
    }
    REQUIRE(errors == 1);
    REQUIRE(wire.transparency() <= 1.0);
    rig.game.stop_simulation();
}

TEST_CASE("TW14 a paused tween whose instance is destroyed ends as cancelled", "[tween]") {
    ScriptRig rig;
    named_part(rig.game, "Doomed");
    add_script(rig.game, "Paused", R"(
        local TweenService = game:GetService("TweenService")
        local doomed = workspace.Doomed
        local t = TweenService:Tween(doomed, {Transparency = 1}, {Time = 1})
        t:Pause()
        task.delay(0.1, function() doomed:Destroy() end)
        print("waited", t:Wait(), t.Playing)
    )");
    rig.game.start_simulation();
    rig.frames(30);
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(output(rig), "waited\tfalse\tfalse\n"));
    rig.game.stop_simulation();
}
