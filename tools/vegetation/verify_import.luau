-- Prints what import_vegetation.py checks: each vegetation Prefab's bounding-box
-- size and Model count, the place's Texture count, the shared Material count,
-- and how many other Materials use a vegetation texture (should be 0).
local assets = game:GetService("Assets")

local function descendants(root, out)
	out = out or {}
	for _, child in ipairs(root:GetChildren()) do
		table.insert(out, child)
		descendants(child, out)
	end
	return out
end

for _, p in ipairs(descendants(assets.Prefabs.Vegetation)) do
	if p.ClassName == "Prefab" then
		local s = p:GetBoundingBox()
		print("BOUNDS", p.Name, s.X, s.Y, s.Z, #p:GetChildren())
	end
end

-- Vegetation textures only: the place already holds its terrain and prop textures.
print("TEXTURES", #assets.Textures.Vegetation:GetChildren())

local veg = assets.Materials.Vegetation
print("MATERIALS", #veg:GetChildren())

-- Compared by name: an Instance read twice need not be the same Luau value.
local vegTex = {}
for _, t in ipairs(assets.Textures.Vegetation:GetChildren()) do
	vegTex[t.Name] = true
end
local stray = 0
for _, m in ipairs(descendants(assets.Materials)) do
	if m.ClassName == "Material" and m.Parent.Name ~= "Vegetation" then
		for _, slot in ipairs({ "DiffuseTexture", "NormalTexture", "RoughnessTexture", "MetalnessTexture" }) do
			local t = m[slot]
			if t ~= nil and vegTex[t.Name] then
				stray += 1
			end
		end
	end
end
print("STRAY", stray)
