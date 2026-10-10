-- The studio's Select, Move, and Rotate tools, one group on the Home tab. While
-- any is on, clicking in the Scene View selects what is under the pointer
-- (Shift or Ctrl adds it or takes it away, and clicking nothing clears the
-- selection). With Move, translate handles also sit at the middle of the
-- PVInstances selected or inside what is selected, while the place is not
-- playing, so selecting a Folder moves everything in it. Dragging them moves
-- the Dragger and each of those PVInstances by the same offset, as one undo
-- step. With Rotate the handles are rings instead, and dragging one turns each
-- of them about that middle, in steps of 15 degrees. The Dragger lives in
-- Core, so it is never saved and its own moves are never undo steps. Turning
-- on another tool, or Play, turns it off.
local RunService = game:GetService("RunService")
local Selection = game:GetService("Selection")
local Core = game:GetService("Core")

local SPACE = Enum.TransformSpace.World
-- Each card's snap step: units for Move, degrees for Rotate.
local INCREMENT = { Move = 1, Rotate = 15 }
local TRANSFORM_MODE = { Move = Enum.TransformMode.Translation, Rotate = Enum.TransformMode.Rotation }

local active = false
-- Which card is on: "Select", which only selects, "Move", which shows the
-- arrows too, or "Rotate", which shows the rings.
local mode = "Move"
local dragger
-- Where each moved instance was when the drag began, the Dragger included.
local starts = {}
-- The selection's Changed connections, so the handles follow an edit or an undo.
local watching = {}

-- The PVInstances selected or under what is selected, each once, or none
-- while the place plays. The camera the view looks through stays out:
-- moving it would move the view the drag is measured in. An Attachment whose
-- parent is in the list stays out: it moves with its parent.
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
	-- An Attachment (a Bone among them) holds only an Offset from its parent,
	-- so a parent that moves carries it: written as well, it would move twice.
	local moving = {}
	for _, item in list do
		moving[item] = true
	end
	local carried = {}
	for _, item in list do
		if not (item:IsA("Attachment") and moving[item.Parent]) then
			table.insert(carried, item)
		end
	end
	return carried
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
	made.Parent = Core
	made.DragBegan:Connect(function()
		starts = { [made] = made.Transform }
		for _, item in selected() do
			starts[item] = item.Transform
		end
	end)
	-- Move's offset is a Vector3 to add; Rotate's a Matrix4, the turn about the middle.
	made.Dragged:Connect(function(_, offset)
		local turning = typeof(offset) == "Matrix4"
		for item, start in starts do
			if item.Parent ~= nil then
				item.Transform = if turning then offset * start else start + offset
			end
		end
	end)
	made.DragEnded:Connect(function()
		starts = {}
	end)
	return made
end

local function drop_dragger()
	if dragger ~= nil then
		dragger:Destroy()
		dragger = nil
	end
end

-- Puts the handles at the selection's middle while the tool is on, or takes them away.
local function place()
	if dragger ~= nil and dragger.Dragging then
		return
	end
	local list = if active and mode ~= "Select" then selected() else {}
	if #list == 0 then
		drop_dragger()
		return
	end
	if dragger == nil or dragger.Parent == nil then
		dragger = make_dragger()
	end
	dragger.TransformMode = TRANSFORM_MODE[mode]
	dragger.Increment = INCREMENT[mode]
	dragger.Transform = Matrix4.new(middle(list))
end

-- A new selection, or Play or Stop: watch what is selected now, and place.
local function update()
	for _, connection in watching do
		connection:Disconnect()
	end
	watching = {}
	if active and mode ~= "Select" then
		for _, item in selected() do
			table.insert(watching, item.Changed:Connect(function(property)
				if property == "Transform" then
					place()
				end
			end))
		end
	end
	place()
end

-- Clicking in the Scene View ---------------------------------------------------

local mouse = plugin:GetMouse()

mouse.Button1Down:Connect(function()
	-- A press on a handle began a drag; it is not a click on the world.
	if not active or (dragger ~= nil and dragger.Dragging) then
		return
	end
	local target = mouse.Target
	if mouse.Shift or mouse.Ctrl then
		if target == nil then
			return
		end
		local list = Selection:Get()
		for index, item in list do
			if item == target then
				table.remove(list, index)
				Selection:Set(list)
				return
			end
		end
		table.insert(list, target)
		Selection:Set(list)
	elseif target ~= nil then
		Selection:Set({ target })
	else
		Selection:Set({})
	end
end)

-- Turning on and off ----------------------------------------------------------

-- Three cards in one group: Select picks with no handles, Move picks and
-- moves, Rotate picks and turns.
local toolbar = plugin:CreateToolbar("Move")
local cards = {
	Select = toolbar:CreateButton("Select", "Select anything in the Scene View", "icons/cursor.png", "Select"),
	Move = toolbar:CreateButton("Move", "Select and move parts in the Scene View", "icons/Dragger.png", "Move"),
	Rotate = toolbar:CreateButton(
		"Rotate",
		"Select and rotate parts in the Scene View",
		"icons/edit-rotate.png",
		"Rotate"
	),
}

local function light()
	for name, card in cards do
		card:SetActive(active and mode == name)
	end
end

local function turn_off()
	if not active then
		return
	end
	active = false
	light()
	update()
end

local function turn_on(which)
	if RunService:IsRunning() then
		return
	end
	mode = which
	if not active then
		active = true
		plugin:Activate(true)
	end
	light()
	update()
end

for name, card in cards do
	card.Click:Connect(function()
		-- The lit card turns the tool off; the other one switches to it.
		if active and mode == name then
			plugin:Deactivate()
		else
			turn_on(name)
		end
	end)
end

plugin.Deactivation:Connect(turn_off)
Selection.SelectionChanged:Connect(update)
-- Off at once, not when Deactivation is heard: Stop may drop events still queued.
RunService.Started:Connect(function()
	turn_off()
	plugin:Deactivate()
end)
RunService.Stopped:Connect(update)
update()
