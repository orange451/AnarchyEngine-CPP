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
local function clone_prefab(source, name, parent)
	local copy = Instance.new("Prefab")
	copy.Name = name
	for _, child in ipairs(source:GetChildren()) do
		if child.ClassName == "Model" then
			local model = Instance.new("Model")
			model.Name = child.Name
			model.Mesh = child.Mesh
			model.Material = child.Material
			model.Parent = copy
		end
	end
	copy.Parent = parent
	return copy
end

local prefabs = game.Assets.Prefabs:GetChildren()
assert(#prefabs >= 1, "the place needs a Prefab in game.Assets.Prefabs")
local shared = {}
for i = 1, 3 do
	shared[i] = clone_prefab(prefabs[1], "StressShared" .. i, game.Assets.Prefabs)
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
	local copy = clone_prefab(prefabs[1], "StressOne" .. i, game.Assets.Prefabs)
	local object = Instance.new("GameObject")
	object.Prefab = copy
	object.Transform = Matrix4.new(0, 0, -210 - i * 4)
	object.Parent = folder
end
print("stress place: " .. #folder:GetChildren() .. " GameObjects")
