-- Builds the shared vegetation Materials and points every imported Model at them.
-- import_vegetation.py substitutes the three tokens below before running this
-- through the run_lua MCP tool. Prints FIX_DONE <materials> <models> <folders>.
local TEX = __TEXTURES__            -- {["Bark_Trunk27"]={Color="Veg_Trunk27_Color.png", Normal=...}, ...}
local ASSET_MATS = __ASSET_MATERIALS__ -- {["Oak-Old-Var01"]={"Bark_Trunk27","Leaf_Leaves3"}, ...}
local FAMILY = __FAMILIES__         -- {["Oak-Old-Var01"]="Tree", ...}

local assets = game:GetService("Assets")
local history = game:GetService("ChangeHistoryService")
local recording = history:TryBeginRecording("Vegetation materials")

local function folder(parent, name)
	local f = parent:FindFirstChild(name)
	if f == nil then
		f = Instance.new("Folder", parent)
		f.Name = name
	end
	return f
end

local function descendants(root, out)
	out = out or {}
	for _, child in ipairs(root:GetChildren()) do
		table.insert(out, child)
		descendants(child, out)
	end
	return out
end

-- 1. Imported textures are named after their file's stem; all vegetation ones start with Veg_.
local texFolder = folder(assets.Textures, "Vegetation")
local texByName = {}
for _, t in ipairs(descendants(assets.Textures)) do
	if t.ClassName == "Texture" and string.sub(t.Name, 1, 4) == "Veg_" then
		texByName[t.Name] = t
		t.Parent = texFolder
	end
end
local function tex(file)
	if file == nil then
		return nil
	end
	local stem = string.gsub(file, "%.png$", "")
	local t = texByName[stem]
	if t == nil then
		error("texture not imported: " .. stem)
	end
	return t
end

-- 2. One shared Material per albedo.
local matFolder = folder(assets.Materials, "Vegetation")
local shared, nMaterials = {}, 0
for key, files in pairs(TEX) do
	local m = Instance.new("Material", matFolder)
	m.Name = key
	m.DiffuseTexture = tex(files.Color)
	m.NormalTexture = tex(files.Normal)
	m.Metalness = 0
	if string.sub(key, 1, 5) == "Leaf_" then
		m.Roughness = 0.85
	else
		m.Roughness = 0.9
		if files.Roughness then
			m.RoughnessTexture = tex(files.Roughness)
		end
		if files.Metalness then
			m.MetalnessTexture = tex(files.Metalness)
		end
	end
	shared[key] = m
	nMaterials += 1
end

-- 3. Repoint each imported Model, file the Prefab by family, drop the importer's per-asset Materials.
local prefabRoot = folder(assets.Prefabs, "Vegetation")
local familyFolder = {
	Tree = folder(prefabRoot, "Trees"),
	Bush = folder(prefabRoot, "Bushes"),
	BerryBush = folder(prefabRoot, "BerryBushes"),
	Card = folder(prefabRoot, "Cards"),
	GroundLog = folder(prefabRoot, "GroundLogs"),
}
local meshRoot = folder(assets.Meshes, "Vegetation")
local nModels, nFolders = 0, 0
for name, _ in pairs(ASSET_MATS) do
	local prefab = assets.Prefabs:FindFirstChild(name)
	if prefab == nil or prefab.ClassName ~= "Prefab" then
		error("prefab missing: " .. name)
	end
	for _, model in ipairs(prefab:GetChildren()) do
		local old = model.Material
		local key = old and old.Name or "nil"
		if shared[key] == nil then
			error(name .. ": model material " .. key .. " has no shared material")
		end
		model.Material = shared[key]
		nModels += 1
	end
	prefab.Parent = familyFolder[FAMILY[name]]
	local oldMaterials = assets.Materials:FindFirstChild(name)
	if oldMaterials then
		oldMaterials:Destroy()
		nFolders += 1
	end
	local meshes = assets.Meshes:FindFirstChild(name)
	if meshes then
		meshes.Parent = meshRoot
	end
end

if recording then
	history:FinishRecording(recording, Enum.FinishRecordingOperation.Commit)
end
print("FIX_DONE", nMaterials, nModels, nFolders)
