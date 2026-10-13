-- Lines up one row of vegetation prefabs on the terrain and aims the camera at it.
-- gallery.py substitutes __ROW__ (names), __SPACING__ (per-name widths), __ORIGIN__
-- (x, y, z) and __TALLEST__ (units). Prints GALLERY_READY <row length>.
local NAMES = __ROW__
local SPACING = __SPACING__
local origin = Vector3.new(__ORIGIN__)
local tallest = __TALLEST__

local root = game:GetService("Assets").Prefabs.Vegetation
local byName = {}
for _, family in ipairs(root:GetChildren()) do
	for _, p in ipairs(family:GetChildren()) do
		byName[p.Name] = p
	end
end

local old = workspace:FindFirstChild("VegetationGallery")
if old then
	old:Destroy()
end
local gallery = Instance.new("Folder", workspace)
gallery.Name = "VegetationGallery"

local params = RaycastParams.new()
params.FilterType = Enum.RaycastFilterType.Include
params.FilterDescendantsInstances = { workspace.Z1 }

local x = 0
for i, name in ipairs(NAMES) do
	x += SPACING[i] / 2
	local at = origin + Vector3.new(x, 0, 0)
	local hit = workspace:Raycast(at + Vector3.new(0, 500, 0), Vector3.new(0, -1000, 0), params)
	local go = Instance.new("GameObject")
	go.Name = name
	go.Prefab = byName[name] or error("no prefab " .. name)
	go.Transform = Matrix4.new(at.X, hit and hit.Position.Y or origin.Y, at.Z)
	go.Parent = gallery
	x += SPACING[i] / 2
end

local mid = origin + Vector3.new(x / 2, 0, 0)
local hit = workspace:Raycast(mid + Vector3.new(0, 500, 0), Vector3.new(0, -1000, 0), params)
if hit then
	mid = hit.Position
end
local distance = math.max(0.75 * x, 2.2 * tallest) + 6
local eye = mid + Vector3.new(0, 0.25 * distance + 0.3 * tallest, distance)
workspace.CurrentCamera.Transform = Matrix4.lookAt(eye, mid + Vector3.new(0, 0.4 * tallest, 0))
print("GALLERY_READY", x)
