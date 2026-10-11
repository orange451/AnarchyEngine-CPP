// Mesh:Edit and the MeshEditor scripts get from it
// (docs/superpowers/specs/2026-10-11-mesh-editor-design.md, section 1).

#include "support.hpp"

#include "AssetInstances.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

namespace {

// A game with a project folder and a Mesh called Cube, run from the command line.
struct LuaEditorRig : ScriptRig {
    std::filesystem::path resources;

    LuaEditorRig() {
        resources = std::filesystem::temp_directory_path() / ("anarchy-mesh-editor-lua-" + process_id());
        std::filesystem::remove_all(resources);
        std::filesystem::create_directories(resources);
        game.set_resources_root(resources);
        engine_core::Mesh& cube = game.create<engine_core::Mesh>();
        game.set_name(cube.id(), "Cube");
        game.set_parent(cube.id(), game.service("Meshes"));
    }
    ~LuaEditorRig() {
        std::error_code ignored;
        std::filesystem::remove_all(resources, ignored);
    }

    // What source printed, and any error, one line each.
    std::string run(const std::string& source) {
        runtime.drain_output();
        runtime.run_chunk(source);
        std::string out;
        for (const auto& line : runtime.drain_output().lines) {
            out += line.text;
        }
        return out;
    }
};

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("L1 every Edit of one Mesh is the same MeshEditor", "[mesh-editor-lua]") {
    LuaEditorRig rig;
    REQUIRE(rig.run(R"(
        local m = game.Assets.Meshes.Cube
        local a, b = m:Edit(), m:Edit()
        a:AddVertex(Vector3.new(1, 2, 3))
        print(a == b, #b:GetVertices(), typeof(a), tostring(a))
    )") == "true\t1\tMeshEditor\tMeshEditor\n");
}

TEST_CASE("L2 a shape returns the ids of the vertices it added", "[mesh-editor-lua]") {
    LuaEditorRig rig;
    REQUIRE(rig.run(R"(
        local e = game.Assets.Meshes.Cube:Edit()
        e:AddVertex(Vector3.new())
        local ids = e:AddBox(Vector3.new(2, 2, 2))
        print(#ids, ids[1], ids[24], math.abs(e:GetPosition(ids[1]).X), #e:GetTriangles())
        print(#e:AddSphere(1, 8) > 0, #e:AddCylinder(1, 2) > 0, #e:AddCone(1, 2) > 0, #e:AddPlane(1, 1),
            #e:AddTeapot(1) > 0)
    )") == "24\t1\t24\t1\t12\ntrue\ttrue\ttrue\t4\ttrue\n");
}

TEST_CASE("L3 a script rigs a tube, and poses it in the same frame", "[mesh-editor-lua]") {
    LuaEditorRig rig;
    const std::string out = rig.run(R"(
        local mesh = game.Assets.Meshes.Cube
        local e = mesh:Edit()
        local root = e:AddBone("Root")
        local tip = e:AddBone("Tip", root, Matrix4.new(0, 2, 0))
        for _, v in ipairs(e:AddCylinder(0.5, 4, 8, true, Vector3.new(0, 2, 0))) do
            local t = e:GetPosition(v).Y / 4
            e:SetVertexBones(v, {root, tip}, {1 - t, t})
        end
        local prefab = Instance.new("Prefab", game.Assets.Prefabs)
        Instance.new("Model", prefab).Mesh = mesh
        local object = Instance.new("GameObject", workspace)
        object.Prefab = prefab
        local names = object:GetBoneNames()
        local bone = object:GetBone("Tip")
        print(#names, names[2], bone.Transform.Position == Vector3.new(0, 2, 0))
        bone.Offset = Matrix4.new(1, 0, 0)
        print(bone.Transform.Position == Vector3.new(1, 2, 0), mesh.OriginOffset:FuzzyEq(Vector3.new(0, 2, 0), 1e-4),
            prefab:GetBoundingBox():FuzzyEq(Vector3.new(1, 4, 1), 1e-4))
    )");
    REQUIRE(out == "2\tTip\ttrue\ntrue\ttrue\ttrue\n");
}

TEST_CASE("L4 an editor held across Play has ended", "[mesh-editor-lua]") {
    LuaEditorRig rig;
    rig.run("shared.e = game.Assets.Meshes.Cube:Edit()");
    rig.game.start_simulation();
    INFO(rig.run("print(shared.e)"));
    CHECK(has(rig.run("shared.e:AddVertex(Vector3.new())"), "this MeshEditor has ended; call mesh:Edit() again"));
    REQUIRE(rig.run("shared.p = game.Assets.Meshes.Cube:Edit() print(#shared.p:GetVertices())") == "0\n");
    rig.game.stop_simulation();
    CHECK(has(rig.run("shared.p:GetVertices()"), "this MeshEditor has ended; call mesh:Edit() again"));
}

TEST_CASE("L5 a refused edit says why", "[mesh-editor-lua]") {
    LuaEditorRig rig;
    const std::string out = rig.run(R"(
        local e = game.Assets.Meshes.Cube:Edit()
        local v = e:AddVertex(Vector3.new())
        local function why(f, ...)
            local ok, err = pcall(f, ...)
            print(ok, err)
        end
        why(e.GetPosition, e, 99)
        why(e.AddTriangle, e, v, v, v)
        why(e.AddBone, e, "")
        e:AddBone("A")
        why(e.AddBone, e, "A")
        why(e.SetVertexBones, e, v, {0}, {1, 2})
        why(e.SetVertexBones, e, v, {0}, {-1})
        why(e.SetVertexBones, e, v, {0}, {0})
        why(e.RemoveBone, e, 5)
        why(e.RemoveTriangle, e, 0)
        why(e.AddBone, e, "B", nil, Matrix4.new(0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1))
        why(e.GetPosition, e, 1.5)
        why(e.AddSphere, e, 0)
    )");
    INFO(out);
    CHECK(has(out, "no vertex 99"));
    CHECK(has(out, "a triangle needs three different vertices"));
    CHECK(has(out, "a bone's name must not be empty"));
    CHECK(has(out, "a bone named A already exists"));
    CHECK(has(out, "bones and weights must be the same length"));
    CHECK(has(out, "a weight must be a finite number of 0 or more"));
    CHECK(has(out, "the weights must not all be 0"));
    CHECK(has(out, "no bone 5"));
    CHECK(has(out, "no triangle 0"));
    CHECK(has(out, "a bone's rest transform must have an inverse"));
    CHECK(has(out, "must be an id from this MeshEditor"));
    CHECK(has(out, "radius must be a number above 0"));
    CHECK(out.find("true") == std::string::npos);
}

TEST_CASE("L6 the Mesh's own shape methods are gone", "[mesh-editor-lua]") {
    LuaEditorRig rig;
    REQUIRE(rig.run(R"(
        local m = game.Assets.Meshes.Cube
        print(pcall(function() m:AddBox(Vector3.new(1, 1, 1)) end), (pcall(function() m:Clear() end)))
    )").rfind("false", 0) == 0);
}

TEST_CASE("L7 every getter reads what its setter wrote", "[mesh-editor-lua]") {
    LuaEditorRig rig;
    const std::string out = rig.run(R"(
        local e = game.Assets.Meshes.Cube:Edit()
        local a = e:AddVertex(Vector3.new(0, 0, 0))
        local b = e:AddVertex(Vector3.new(1, 0, 0))
        local c = e:AddVertex(Vector3.new(0, 0, -1))
        local t = e:AddTriangle(a, b, c)
        local x, y, z = e:GetTriangleVertices(t)
        print(x, y, z, e:GetNormal(a) == Vector3.new(0, 1, 0))
        e:SetNormal(a, Vector3.new(0, 0, 3))
        print(e:GetNormal(a) == Vector3.new(0, 0, 1))
        e:SetNormal(a, nil)
        print(e:GetNormal(a) == Vector3.new(0, 1, 0))
        e:SetPosition(c, Vector3.new(0, 0, -2))
        e:SetUV(b, Vector2.new(0.5, 0.25))
        print(e:GetPosition(c) == Vector3.new(0, 0, -2), e:GetUV(b) == Vector2.new(0.5, 0.25))
        e:SetColor(c, Color3.new(1, 0, 0), 0)
        local color, alpha = e:GetColor(c)
        print(color == Color3.new(1, 0, 0), alpha)
        local root = e:AddBone("Root")
        local arm = e:AddBone("Arm", root, Matrix4.new(1, 0, 0))
        print(e:FindBone("Arm"), e:FindBone("Leg"), e:GetBoneName(arm), e:GetBoneParent(arm), e:GetBoneParent(root))
        e:SetBoneName(arm, "Hand")
        e:SetBoneParent(arm, nil)
        e:SetBoneTransform(arm, Matrix4.new(2, 0, 0))
        print(e:GetBoneName(arm), e:GetBoneParent(arm), e:GetBoneTransform(arm).Position == Vector3.new(2, 0, 0), #e:GetBones())
        e:SetVertexBones(a, {root, arm})
        local bones, weights = e:GetVertexBones(a)
        print(#bones, weights[1], weights[2])
        e:SetVertexBones(a, {})
        print(#e:GetVertexBones(a))
        e:RemoveBone(root)
        print(#e:GetBones())
        e:RemoveTriangle(t)
        print(#e:GetTriangles(), #e:GetVertices())
        e:RemoveVertex(c)
        print(#e:GetVertices())
        e:Clear()
        print(#e:GetVertices(), #e:GetBones(), e:AddVertex(Vector3.new()))
    )");
    REQUIRE(out == "0\t1\t2\ttrue\n"
                   "true\n"
                   "true\n"
                   "true\ttrue\n"
                   "true\t0\n"
                   "1\tnil\tArm\t0\tnil\n"
                   "Hand\tnil\ttrue\t2\n"
                   "2\t0.5\t0.5\n"
                   "0\n"
                   "1\n"
                   "0\t3\n"
                   "2\n"
                   "0\t0\t3\n");
}
