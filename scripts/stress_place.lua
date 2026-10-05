-- The culling and instancing stress place (docs/superpowers/plans/2026-10-05-culling-instancing.md).
-- Run once on a scratch copy of a project with at least one Prefab in game.Assets.Prefabs.
-- 10,000 GameObjects over 3 Prefabs on a 100 by 100 grid, 4 studs apart, plus 50 one-off
-- Prefabs in a row along -Z, each copied from the first so each has a slot of its own.
--
-- Prefab (and Instance) has no Clone method in this build -- get_class Prefab and
-- get_class Instance both list only Destroy, GetChildren, FindFirstChild, WaitForChild,
-- and IsA. Every sampled Prefab (Floor, Teapot, SpawnCrate1, SphereStand_Gold, ...) holds
-- exactly one Model child whose Mesh and Material are references to shared assets, not
-- owned sub-instances, so a "copy" is built by making a new Prefab with a new Model that
-- points at the same Mesh and Material. That gives each copy its own Prefab identity
-- (so the 3 "shared" copies form 3 separate instancing groups, and the 50 "one-off"
-- copies each form a group of one) without needing to duplicate geometry data.
--
-- The source for every copy must be a compact Prefab, not a floor-sized one: 10,050
-- copies of a floor slab overlap and fight for the same pixels on a 4-stud grid, which
-- makes the culling/instancing measurement meaningless. Teapot is used when present
-- (its bounding box is about 2.75 x 1.4 x 1.78 studs, under the 4-stud grid spacing, so
-- the grid is left as specified); otherwise the first Prefab whose Model's Mesh is not
-- named like a floor slab is used instead.
local function pick_source_prefab()
	local teapot = game.Assets.Prefabs:FindFirstChild("Teapot")
	if teapot then
		return teapot
	end
	for _, candidate in ipairs(game.Assets.Prefabs:GetChildren()) do
		for _, child in ipairs(candidate:GetChildren()) do
			if child.ClassName == "Model" and child.Mesh and not child.Mesh.Name:lower():match("floor") then
				return candidate
			end
		end
	end
	error("the place needs a compact Prefab (not a floor slab) in game.Assets.Prefabs")
end

local function clone_prefab(source, name, parent)
	local source_model = nil
	for _, child in ipairs(source:GetChildren()) do
		if child.ClassName == "Model" then
			source_model = child
			break
		end
	end
	assert(source_model ~= nil, "clone_prefab: " .. source.Name .. " has no Model child to copy")
	local copy = Instance.new("Prefab")
	copy.Name = name
	local model = Instance.new("Model")
	model.Name = source_model.Name
	model.Mesh = source_model.Mesh
	model.Material = source_model.Material
	model.Parent = copy
	copy.Parent = parent
	return copy
end

local prefabs = game.Assets.Prefabs:GetChildren()
assert(#prefabs >= 1, "the place needs a Prefab in game.Assets.Prefabs")
local source = pick_source_prefab()
local shared = {}
for i = 1, 3 do
	shared[i] = clone_prefab(source, "StressShared" .. i, game.Assets.Prefabs)
end
local folder = Instance.new("Folder")
folder.Name = "Stress"
folder.Parent = workspace
for x = 0, 99 do
	for z = 0, 99 do
		local object = Instance.new("GameObject")
		object.Prefab = shared[(x + z) % 3 + 1]
		object.Transform = Matrix4.new(x * 4 - 200, 0, z * 4 - 200)
		object.Color = Color3.fromHSV(((x * 7 + z * 3) % 100) / 100, 0.4, 1)
		object.Parent = folder
	end
end
for i = 1, 50 do
	local copy = clone_prefab(source, "StressOne" .. i, game.Assets.Prefabs)
	local object = Instance.new("GameObject")
	object.Prefab = copy
	object.Transform = Matrix4.new(0, 0, -210 - i * 4)
	object.Parent = folder
end
print("stress place: " .. #folder:GetChildren() .. " GameObjects")
