-- The studio's Move tool: translate handles at the middle of the PVInstances
-- selected or inside what is selected, while the place is not playing, so
-- selecting a Folder moves everything in it. Dragging them moves the Dragger
-- and each of those PVInstances by the same offset, as one undo step. The
-- Dragger lives in Core, so it is never saved and its own moves are never
-- undo steps.
local RunService = game:GetService("RunService")
local Selection = game:GetService("Selection")
local Core = game:GetService("Core")

local SPACE = Enum.DraggerSpace.World
local INCREMENT = 1 -- studs

local dragger
-- Where each moved instance was when the drag began, the Dragger included.
local starts = {}
-- The selection's Changed connections, so the handles follow an edit or an undo.
local watching = {}

-- The PVInstances selected or under what is selected, each once, or none
-- while the place plays. The camera the view looks through stays out:
-- moving it would move the view the drag is measured in.
local function selected()
	local list = {}
	if RunService:IsRunning() then
		return list
	end
	local seen = {}
	local viewing = workspace.CurrentCamera
	local function gather(item)
		if seen[item] then
			return
		end
		seen[item] = true
		if item:IsA("PVInstance") and item ~= viewing then
			table.insert(list, item)
		end
		for _, child in item:GetChildren() do
			gather(child)
		end
	end
	for _, item in Selection:Get() do
		if item.Parent ~= nil then
			gather(item)
		end
	end
	return list
end

-- The middle of the box around their positions.
local function middle(list)
	local first = list[1].Transform.Position
	local minX, minY, minZ = first.X, first.Y, first.Z
	local maxX, maxY, maxZ = minX, minY, minZ
	for _, item in list do
		local p = item.Transform.Position
		minX, minY, minZ = math.min(minX, p.X), math.min(minY, p.Y), math.min(minZ, p.Z)
		maxX, maxY, maxZ = math.max(maxX, p.X), math.max(maxY, p.Y), math.max(maxZ, p.Z)
	end
	return Vector3.new((minX + maxX) / 2, (minY + maxY) / 2, (minZ + maxZ) / 2)
end

local function make_dragger()
	local made = Instance.new("Dragger")
	made.Name = "MoveTool"
	made.Space = SPACE
	made.Increment = INCREMENT
	made.Parent = Core
	made.DragBegan:Connect(function()
		starts = { [made] = made.Transform }
		for _, item in selected() do
			starts[item] = item.Transform
		end
	end)
	made.Dragged:Connect(function(_, offset)
		for item, start in starts do
			if item.Parent ~= nil then
				item.Transform = start + offset
			end
		end
	end)
	made.DragEnded:Connect(function()
		starts = {}
	end)
	return made
end

-- Puts the handles at the selection's middle, or takes them away.
local function place()
	if dragger ~= nil and dragger.Dragging then
		return
	end
	local list = selected()
	if #list == 0 then
		if dragger ~= nil then
			dragger:Destroy()
			dragger = nil
		end
		return
	end
	if dragger == nil or dragger.Parent == nil then
		dragger = make_dragger()
	end
	dragger.Transform = Matrix4.new(middle(list))
end

-- A new selection, or Play or Stop: watch what is selected now, and place.
local function update()
	for _, connection in watching do
		connection:Disconnect()
	end
	watching = {}
	for _, item in selected() do
		table.insert(watching, item.Changed:Connect(function(property)
			if property == "Transform" then
				place()
			end
		end))
	end
	place()
end

Selection.SelectionChanged:Connect(update)
RunService.Started:Connect(update)
RunService.Stopped:Connect(update)
update()
