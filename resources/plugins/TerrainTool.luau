-- The studio's terrain tools, one group on the Home tab: Add drags a box on the
-- grid floor, Grow raises the ground under a ball at the pointer (Shift lowers
-- it), Smooth evens it out, and Paint sets its material (Shift: the default).
-- They edit the selected Terrain, else the first one in Workspace, while the
-- place is not playing. Each stroke is one undo step. The Terrain pane holds
-- the size, strength, and material. Turning on another tool, or Play, turns
-- them off.
local RunService = game:GetService("RunService")
local Selection = game:GetService("Selection")
local History = game:GetService("ChangeHistoryService")
local Core = game:GetService("Core")

local TOOLS = { "Add", "Grow", "Smooth", "Paint" }
local ICONS = {
	Add = "icons/box--plus.png",
	Grow = "icons/shovel.png",
	Smooth = "icons/layer-shape-curve.png",
	Paint = "icons/paint-brush.png",
}
local TIPS = {
	Add = "Drag a box of terrain on the grid floor",
	Grow = "Raise the terrain under the pointer (Shift: lower it)",
	Smooth = "Even out the terrain under the pointer",
	Paint = "Paint the terrain under the pointer (Shift: the default material)",
}
local SIZES = { 1, 2, 4, 6, 8, 12, 16, 24, 32, 48 }
local STRENGTHS = { 0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1 }
local RAY_LENGTH = 4000
-- Grow, Smooth, and Paint apply this often while the button is held.
local APPLY_SECONDS = 1 / 20
local CURSOR = Color3.new(1, 0.84, 0.3)

type Ray = { Origin: Vector3, Direction: Vector3 }

local active = false
local tool = "Grow"
local size = 8
local strength = 0.5
-- The chosen TerrainMaterial's Name; "" is the default material.
local materialName = ""

local mouse = plugin:GetMouse()
-- The pointer's hit on the Terrain, if any.
local hit: { position: Vector3, normal: Vector3 }? = nil
local held = false
-- Seconds since the held stroke last applied.
local sinceApply = 0
local recording: string? = nil
-- Add: the drag's corners on the grid floor (y = 0).
local boxStart: Vector3? = nil
local boxEnd = Vector3.zero

local wire = Instance.new("WireframeAdornment")
wire.Name = "TerrainToolCursor"
wire.Color3 = CURSOR
wire.Visible = false
wire.Parent = Core

-- The Terrain the tools edit: the selected one, else the first in Workspace.
local function terrain(): Terrain?
	for _, item in Selection:Get() do
		if item:IsA("Terrain") and item.Parent ~= nil then
			return item :: any
		end
	end
	for _, child in workspace:GetChildren() do
		if child:IsA("Terrain") then
			return child :: any
		end
	end
	return nil
end

-- The chosen material on t, or nil for the default.
local function material(t: Terrain): TerrainMaterial?
	for _, entry in t:GetMaterials() do
		if entry.Name == materialName then
			return entry
		end
	end
	return nil
end

-- Undo -----------------------------------------------------------------------

local function begin_stroke()
	if recording == nil then
		recording = History:TryBeginRecording("Sculpt Terrain")
	end
end

local function end_stroke()
	if recording ~= nil then
		local id = recording
		recording = nil
		History:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
	end
end

-- Picking and editing ----------------------------------------------------------

local function ray(): Ray
	local unitRay = mouse.UnitRay
	return { Origin = unitRay.Origin, Direction = unitRay.Direction }
end

-- The pointer's hit on the Terrain alone, so parts in front do not catch it.
local function pick()
	hit = nil
	local t = terrain()
	if t == nil then
		return
	end
	local params = RaycastParams.new()
	params.FilterType = Enum.RaycastFilterType.Include
	params.FilterDescendantsInstances = { t }
	local r = ray()
	local result = workspace:Raycast(r.Origin, r.Direction * RAY_LENGTH, params)
	if result ~= nil then
		hit = { position = result.Position, normal = result.Normal }
	end
end

-- Where the pointer's ray meets the grid floor, or nil.
local function on_floor(): Vector3?
	local r = ray()
	if math.abs(r.Direction.Y) < 1e-5 then
		return nil
	end
	local t = -r.Origin.Y / r.Direction.Y
	if t <= 0 then
		return nil
	end
	return r.Origin + r.Direction * t
end

local function apply_ball()
	local t = terrain()
	if t == nil or hit == nil then
		return
	end
	local radius = size / 2
	local at, normal = hit.position, hit.normal
	if tool == "Grow" then
		-- Mostly sunk below the surface, so each application raises (or, with
		-- Shift, lowers) the ground by a fraction of the radius rather than
		-- stamping a whole ball.
		local sink = radius * (1 - 0.3 * strength)
		if mouse.Shift then
			t:SubtractBall(at + normal * sink, radius)
		else
			t:FillBall(at - normal * sink, radius, material(t))
		end
	elseif tool == "Smooth" then
		t:SmoothBall(at, radius, strength)
	elseif tool == "Paint" then
		t:PaintBall(at, radius, if mouse.Shift then nil else material(t))
	end
end

local function apply_box()
	local start = boxStart
	boxStart = nil
	local t = terrain()
	if start == nil or t == nil then
		return
	end
	local width = math.abs(boxEnd.X - start.X)
	local depth = math.abs(boxEnd.Z - start.Z)
	if width < 0.5 or depth < 0.5 then
		return
	end
	local middle = Vector3.new((start.X + boxEnd.X) / 2, size / 2, (start.Z + boxEnd.Z) / 2)
	begin_stroke()
	t:FillBlock(Matrix4.new(middle), Vector3.new(width, size, depth), material(t))
	end_stroke()
end

-- The cursor: a wire ball at the hit, with a tick along the normal, or the box being dragged.
local function redraw()
	wire:Clear()
	wire.Visible = active
	if not active then
		return
	end
	if tool == "Add" then
		local start = boxStart
		if start == nil then
			return
		end
		local x0, x1 = math.min(start.X, boxEnd.X), math.max(start.X, boxEnd.X)
		local z0, z1 = math.min(start.Z, boxEnd.Z), math.max(start.Z, boxEnd.Z)
		local c = {
			Vector3.new(x0, 0, z0), Vector3.new(x1, 0, z0), Vector3.new(x1, 0, z1), Vector3.new(x0, 0, z1),
			Vector3.new(x0, size, z0), Vector3.new(x1, size, z0), Vector3.new(x1, size, z1), Vector3.new(x0, size, z1),
		}
		for i = 1, 4 do
			local j = i % 4 + 1
			wire:AddLine(c[i], c[j])
			wire:AddLine(c[4 + i], c[4 + j])
			wire:AddLine(c[i], c[4 + i])
		end
		return
	end
	if hit == nil then
		return
	end
	local segments = 32
	local r = size / 2
	local o = hit.position
	for axis = 1, 3 do
		for i = 0, segments - 1 do
			local function point(a: number): Vector3
				local u, v = math.cos(a) * r, math.sin(a) * r
				if axis == 1 then
					return o + Vector3.new(u, v, 0)
				elseif axis == 2 then
					return o + Vector3.new(u, 0, v)
				end
				return o + Vector3.new(0, u, v)
			end
			wire:AddLine(point(2 * math.pi * i / segments), point(2 * math.pi * (i + 1) / segments))
		end
	end
	wire:AddLine(o, o + hit.normal * r)
end

-- The pane ---------------------------------------------------------------------

local widget = plugin:CreateDockWidget("Terrain", {
	Title = "Terrain",
	InitialDock = "TopRight",
	Enabled = false,
	Width = 220,
	Height = 220,
	MinWidth = 180,
})

local root = Instance.new("VBox")
root.Spacing = 6
-- The pane shows through, so the studio's theme colors it.
root.BackgroundTransparency = 1
root.Style = "padding: 8px;"
root.Parent = widget

local function label(text: string, parent: Instance): Label
	local made = Instance.new("Label")
	made.Text = text
	made.FontSize = 11
	made.Parent = parent
	return made
end

local function button(parent: Instance, text: string, action: () -> ()): Button
	local made = Instance.new("Button")
	made.Text = text
	made.Parent = parent
	made.Action:Connect(action)
	return made
end

local function row(): HBox
	local box = Instance.new("HBox")
	box.Spacing = 4
	box.BackgroundTransparency = 1
	box.Parent = root
	return box
end

local terrainLabel = label("", root)
local sizeRow = row()
local sizeLabel: Label
local strengthRow = row()
local strengthLabel: Label
local materialButton: Button
local hintLabel = label("", root)
local redraw_pane: () -> ()

-- The next value of list from now, steps along, held to its ends.
local function stepped(list: { number }, now: number, steps: number): number
	local at = 1
	for i, value in list do
		if math.abs(value - now) < 1e-9 then
			at = i
		end
	end
	return list[math.clamp(at + steps, 1, #list)]
end

button(sizeRow, "-", function()
	size = stepped(SIZES, size, -1)
	redraw_pane()
	redraw()
end)
sizeLabel = label("", sizeRow)
button(sizeRow, "+", function()
	size = stepped(SIZES, size, 1)
	redraw_pane()
	redraw()
end)
button(strengthRow, "-", function()
	strength = stepped(STRENGTHS, strength, -1)
	redraw_pane()
end)
strengthLabel = label("", strengthRow)
button(strengthRow, "+", function()
	strength = stepped(STRENGTHS, strength, 1)
	redraw_pane()
end)
-- Steps through the Terrain's materials, then the default.
materialButton = button(root, "", function()
	local t = terrain()
	local names = { "" }
	if t ~= nil then
		for _, entry in t:GetMaterials() do
			table.insert(names, entry.Name)
		end
	end
	local at = table.find(names, materialName) or 1
	materialName = names[at % #names + 1]
	redraw_pane()
end)

function redraw_pane()
	local t = terrain()
	terrainLabel.Text = if t ~= nil then "Terrain: " .. t.Name else "No Terrain: add one to Workspace"
	sizeLabel.Text = string.format("Size %d units", size)
	strengthLabel.Text = string.format("Strength %d%%", math.floor(strength * 100 + 0.5))
	if t ~= nil and material(t) == nil then
		materialName = ""
	end
	materialButton.Text = "Material: " .. (if materialName == "" then "Default" else materialName)
	hintLabel.Text = TIPS[tool]
end

-- Turning on and off ----------------------------------------------------------

local toolbar = plugin:CreateToolbar("Terrain")
local cards = {}
for _, name in TOOLS do
	cards[name] = toolbar:CreateButton(name, TIPS[name], ICONS[name], name)
end

local function light()
	for name, card in cards do
		card:SetActive(active and tool == name)
	end
end

local function turn_off()
	if not active then
		return
	end
	active = false
	held = false
	boxStart = nil
	hit = nil
	end_stroke()
	light()
	widget.Enabled = false
	redraw()
end

local function turn_on(which: string)
	if RunService:IsRunning() then
		return
	end
	tool = which
	boxStart = nil
	if not active then
		active = true
		plugin:Activate(true)
	end
	widget.Enabled = true
	light()
	redraw_pane()
	redraw()
end

for name, card in cards do
	card.Click:Connect(function()
		-- The lit card turns the tools off; another one switches to it.
		if active and tool == name then
			plugin:Deactivate()
		else
			turn_on(name)
		end
	end)
end

plugin.Deactivation:Connect(turn_off)
-- Off at once, not when Deactivation is heard: Stop may drop events still queued.
RunService.Started:Connect(function()
	turn_off()
	plugin:Deactivate()
end)
-- Closing the pane turns the tools off; opening it turns the last one on.
widget.Changed:Connect(function(property)
	if property ~= "Enabled" then
		return
	end
	if widget.Enabled and not active then
		turn_on(tool)
	elseif not widget.Enabled and active then
		plugin:Deactivate()
	end
end)
Selection.SelectionChanged:Connect(function()
	if active then
		redraw_pane()
	end
end)

-- The mouse ---------------------------------------------------------------------

mouse.Move:Connect(function()
	if not active then
		return
	end
	if tool == "Add" then
		if boxStart ~= nil then
			boxEnd = on_floor() or boxEnd
		end
	else
		pick()
	end
	redraw()
end)

mouse.Button1Down:Connect(function()
	if not active then
		return
	end
	if tool == "Add" then
		local at = on_floor()
		if at ~= nil then
			boxStart = at
			boxEnd = at
		end
		redraw()
		return
	end
	pick()
	begin_stroke()
	held = true
	sinceApply = 0
	apply_ball()
	redraw()
end)

mouse.Button1Up:Connect(function()
	if not active then
		return
	end
	if tool == "Add" then
		apply_box()
	elseif held then
		held = false
		end_stroke()
	end
	redraw()
end)

-- Grow, Smooth, and Paint keep applying while the button is held.
RunService.RenderStepped:Connect(function(dt)
	if not active or not held then
		return
	end
	sinceApply += dt
	if sinceApply < APPLY_SECONDS then
		return
	end
	sinceApply = 0
	pick()
	apply_ball()
	redraw()
end)

redraw_pane()
