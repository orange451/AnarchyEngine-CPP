-- The studio's terrain tools, one group on the Home tab: Add draws a box on the
-- grid as the Brushes tool does (drag the footprint, let go, move to set the
-- height, click to fill; Shift keeps it square, then a cube; Alt ends the
-- footprint early), Grow raises the ground under a ball at the pointer (Shift
-- lowers it), Smooth evens it out, and Paint sets its material (Shift: the default).
-- They edit the selected Terrain while the place is not playing; with none
-- selected, the pane offers to insert one. Each stroke is one undo step. The Terrain pane holds
-- the size, strength, grid, and material. Turning on another tool, or Play, turns
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
	Add = "Drag a footprint, let go, move to set the height, click to fill (Shift: square, then cube)",
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
-- Add draws a box as the Brushes tool does: drag its footprint on the plane
-- of the surface pressed (or the ground), let go, move to set its height,
-- and click to fill it. All on the grid.
local GRIDS = { 1, 2, 4, 8, 16 }
local grid = 1
-- nil, "Drawing" (the footprint), or "Height".
local drawing: string? = nil
-- The plane drawn on: its axis (1 X, 2 Y, 3 Z), which way it faces, and a point on it.
local planeAxis, planeSign = 2, 1
local planePoint = Vector3.zero
local drawStart = Vector3.zero
local drawEnd = Vector3.zero
local height = 0
local lastHeight = 4
-- The pane's labels again; defined with the pane, below.
local redraw_pane: () -> ()

local wire = Instance.new("WireframeAdornment")
wire.Name = "TerrainToolCursor"
wire.Color3 = CURSOR
wire.Visible = false
wire.Parent = Core
local gridWire = Instance.new("WireframeAdornment")
gridWire.Name = "TerrainToolGrid"
gridWire.Color3 = Color3.new(0.75, 0.8, 0.9)
gridWire.Transparency = 0.8
gridWire.Visible = false
gridWire.Parent = Core

-- The box's size, by the pointer, while Add draws.
local readoutAnchor = Instance.new("Attachment")
readoutAnchor.Name = "TerrainToolReadout"
readoutAnchor.Parent = Core
local readout = Instance.new("BillboardGui")
readout.AlwaysOnTop = true
-- Under the pointer, it must not take the clicks meant for the view.
readout.MouseTransparent = true
readout.Visible = false
readout.Adornee = readoutAnchor
readout.Parent = Core
local readoutText = Instance.new("Label")
readoutText.FontSize = 12
readoutText.MouseTransparent = true
readoutText.TextColor = Color3.new(1, 0.84, 0.47)
readoutText.Style = "background-color: rgba(20, 22, 26, 0.85); padding: 2px 6px; border-radius: 4px;"
readoutText.Parent = readout

-- The Terrain the tools edit: the selected one. With none, the pane offers to insert one.
local function terrain(): Terrain?
	for _, item in Selection:Get() do
		if item:IsA("Terrain") and item.Parent ~= nil then
			return item :: any
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

-- Add's box -------------------------------------------------------------------

local function axis_of(v: Vector3, axis: number): number
	return axis == 1 and v.X or (axis == 2 and v.Y or v.Z)
end

local function with_axis(v: Vector3, axis: number, value: number): Vector3
	if axis == 1 then
		return Vector3.new(value, v.Y, v.Z)
	elseif axis == 2 then
		return Vector3.new(v.X, value, v.Z)
	end
	return Vector3.new(v.X, v.Y, value)
end

local function axis_vec(axis: number, s: number?): Vector3
	return with_axis(Vector3.zero, axis, s or 1)
end

-- The world axis a normal lies most along.
local function nearest_axis(n: Vector3): number
	local ax, ay, az = math.abs(n.X), math.abs(n.Y), math.abs(n.Z)
	if ax > ay and ax > az then
		return 1
	end
	return az > ay and 3 or 2
end

local function snap(x: number): number
	return math.round(x / grid) * grid
end

local function snap_vec(v: Vector3): Vector3
	return Vector3.new(snap(v.X), snap(v.Y), snap(v.Z))
end

-- Where the ray meets the plane through point with normal; nil when parallel or behind.
local function ray_plane(r: Ray, point: Vector3, normal: Vector3): Vector3?
	local denom = r.Direction:Dot(normal)
	if math.abs(denom) < 1e-6 then
		return nil
	end
	local t = (point - r.Origin):Dot(normal) / denom
	if t <= 0 then
		return nil
	end
	return r.Origin + r.Direction * t
end

-- How far along the line (origin, unit dir) the point nearest the ray is.
local function ray_line(r: Ray, origin: Vector3, dir: Vector3): number
	local d = r.Direction.Unit
	local w = origin - r.Origin
	local b = dir:Dot(d)
	local denom = 1 - b * b
	if denom < 1e-6 then
		return 0
	end
	return (b * d:Dot(w) - dir:Dot(w)) / denom
end

-- The box's corners, as drawn so far.
local function box(): (Vector3, Vector3)
	local far = drawEnd + axis_vec(planeAxis, if drawing == "Height" then planeSign * height else 0)
	return drawStart:Min(drawEnd):Min(far), drawStart:Max(drawEnd):Max(far)
end

-- A press begins the footprint: on the plane of whatever surface it hits, else the ground.
local function begin_draw()
	local r = ray()
	-- With no physics to cast against, the ground.
	local ok, result = pcall(function()
		return workspace:Raycast(r.Origin, r.Direction * RAY_LENGTH)
	end)
	if not ok then
		result = nil
	end
	planeAxis, planeSign = 2, 1
	if result ~= nil then
		local n = result.Normal
		-- A slope draws on a level plane through the point hit.
		if math.max(math.abs(n.X), math.abs(n.Y), math.abs(n.Z)) >= 0.999 then
			planeAxis = nearest_axis(n)
			planeSign = axis_of(n, planeAxis) >= 0 and 1 or -1
		end
		planePoint = result.Position
		-- Onto the grid when it is close: terrain's surface sits a hair off it.
		local along = axis_of(planePoint, planeAxis)
		if math.abs(snap(along) - along) < 0.25 then
			planePoint = with_axis(planePoint, planeAxis, snap(along))
		end
	else
		planePoint = Vector3.zero
	end
	local start = ray_plane(r, planePoint, axis_vec(planeAxis))
	if start == nil then
		return
	end
	drawStart = with_axis(snap_vec(start), planeAxis, axis_of(planePoint, planeAxis))
	drawEnd = drawStart
	drawing = "Drawing"
end

local function update_draw()
	if mouse.Alt then
		-- Alt mid-drag: the footprint is done; the pointer sets the height now.
		drawing = "Height"
		height = lastHeight
		return
	end
	local at = ray_plane(ray(), planePoint, axis_vec(planeAxis))
	if at == nil then
		return
	end
	local stop = with_axis(snap_vec(at), planeAxis, axis_of(planePoint, planeAxis))
	if mouse.Shift then
		-- Square: the longer side wins.
		local u = planeAxis % 3 + 1
		local v = u % 3 + 1
		local du = axis_of(stop, u) - axis_of(drawStart, u)
		local dv = axis_of(stop, v) - axis_of(drawStart, v)
		local side = math.max(math.abs(du), math.abs(dv))
		stop = with_axis(stop, u, axis_of(drawStart, u) + (du < 0 and -side or side))
		stop = with_axis(stop, v, axis_of(drawStart, v) + (dv < 0 and -side or side))
	end
	drawEnd = stop
end

local function update_height()
	local h = ray_line(ray(), drawEnd, axis_vec(planeAxis, planeSign))
	local value = snap(h)
	if math.abs(value) < grid then
		value = h < 0 and -grid or grid
	end
	if mouse.Shift then
		-- A cube: as tall as the footprint's longer side.
		local u = planeAxis % 3 + 1
		local v = u % 3 + 1
		local side = math.max(
			math.abs(axis_of(drawEnd, u) - axis_of(drawStart, u)),
			math.abs(axis_of(drawEnd, v) - axis_of(drawStart, v))
		)
		value = value < 0 and -side or side
	end
	height = value
end

-- The footprint let go: the height next, or nothing when it has no area.
local function end_footprint()
	local d = drawEnd - drawStart
	local u = planeAxis % 3 + 1
	local v = u % 3 + 1
	if math.abs(axis_of(d, u)) < 1e-4 or math.abs(axis_of(d, v)) < 1e-4 then
		drawing = nil
		return
	end
	drawing = "Height"
	height = lastHeight
	update_height()
end

-- The click that fills the box, as one undo step.
local function commit_draw()
	local lo, hi = box()
	drawing = nil
	local extent = hi - lo
	if extent.X < 1e-4 or extent.Y < 1e-4 or extent.Z < 1e-4 then
		return
	end
	local t = terrain()
	if t == nil then
		return
	end
	lastHeight = math.abs(height)
	begin_stroke()
	t:FillBlock(Matrix4.new((lo + hi) / 2), extent, material(t))
	end_stroke()
end

-- The grid around the pointer, on the plane drawn on.
local function grid_patch(center: Vector3)
	local u = planeAxis % 3 + 1
	local v = u % 3 + 1
	local cells = 8
	local c = with_axis(snap_vec(center), planeAxis, axis_of(center, planeAxis))
	for i = -cells, cells do
		local a = with_axis(with_axis(c, u, axis_of(c, u) + i * grid), v, axis_of(c, v) - cells * grid)
		gridWire:AddLine(a, with_axis(a, v, axis_of(c, v) + cells * grid))
		local b = with_axis(with_axis(c, v, axis_of(c, v) + i * grid), u, axis_of(c, u) - cells * grid)
		gridWire:AddLine(b, with_axis(b, u, axis_of(c, u) + cells * grid))
	end
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

-- The cursor: a wire ball at the hit, with a tick along the normal, or the box being drawn.
local function redraw()
	wire:Clear()
	gridWire:Clear()
	readout.Visible = false
	wire.Visible = active
	gridWire.Visible = active
	if not active then
		return
	end
	if tool == "Add" then
		if drawing == nil then
			return
		end
		local lo, hi = box()
		grid_patch(drawEnd)
		local extent = hi - lo
		local function n(x: number): string
			return string.format("%d", math.round(x))
		end
		readoutText.Text = if drawing == "Height"
			then n(extent.X) .. " x " .. n(extent.Y) .. " x " .. n(extent.Z) .. "   click to fill"
			else n(extent.X) .. " x " .. n(extent.Y) .. " x " .. n(extent.Z)
		-- Up and to the right of the corner, clear of the pointer.
		local cam = workspace.CurrentCamera
		local at = hi
		if cam ~= nil and cam.ViewportSize.Y > 0 then
			local frame = cam.Transform
			local depth = math.max((at - frame.Position):Dot(frame.LookVector), 0.05)
			local step = 2 * depth * math.tan(math.rad(cam.FieldOfView) / 2) / cam.ViewportSize.Y
			at += frame.RightVector * (step * 16) + frame.UpVector * (step * 24)
		end
		readoutAnchor.Offset = Matrix4.new(at)
		readout.Visible = true
		local c = {
			Vector3.new(lo.X, lo.Y, lo.Z), Vector3.new(hi.X, lo.Y, lo.Z), Vector3.new(hi.X, lo.Y, hi.Z), Vector3.new(lo.X, lo.Y, hi.Z),
			Vector3.new(lo.X, hi.Y, lo.Z), Vector3.new(hi.X, hi.Y, lo.Z), Vector3.new(hi.X, hi.Y, hi.Z), Vector3.new(lo.X, hi.Y, hi.Z),
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

local function vbox(parent: Instance): VBox
	local box = Instance.new("VBox")
	box.Spacing = 6
	-- The pane shows through, so the studio's theme colors it.
	box.BackgroundTransparency = 1
	box.Parent = parent
	return box
end

local pane = vbox(widget)
pane.Style = "padding: 8px;"
-- The controls, shown while a Terrain is selected.
local root = vbox(pane)
-- Shown instead while none is.
local empty = vbox(pane)
empty.Visible = false

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
local gridRow = row()
local gridLabel: Label
local strengthLabel: Label
local materialButton: Button
local hintLabel = label("", root)

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
button(gridRow, "-", function()
	grid = stepped(GRIDS, grid, -1)
	redraw_pane()
end)
gridLabel = label("", gridRow)
button(gridRow, "+", function()
	grid = stepped(GRIDS, grid, 1)
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

label("No terrain selected", empty)
-- A new Terrain in Workspace, selected, as one undo step.
button(empty, "Insert terrain", function()
	local id = History:TryBeginRecording("Insert Terrain")
	local made = Instance.new("Terrain")
	made.Name = "Terrain"
	made.Parent = workspace
	if id ~= nil then
		History:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
	end
	Selection:Set({ made })
end)

function redraw_pane()
	local t = terrain()
	root.Visible = t ~= nil
	empty.Visible = t == nil
	terrainLabel.Text = if t ~= nil then "Terrain: " .. t.Name else ""
	sizeLabel.Text = string.format("Size %d units", size)
	strengthLabel.Text = string.format("Strength %d%%", math.floor(strength * 100 + 0.5))
	gridLabel.Text = string.format("Grid %d units", grid)
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
	drawing = nil
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
	drawing = nil
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
		if drawing == "Drawing" then
			update_draw()
			if drawing == "Height" then
				update_height()
			end
		elseif drawing == "Height" then
			update_height()
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
		-- The click after the height fills the box; any other press starts one.
		if drawing == "Height" then
			commit_draw()
		else
			begin_draw()
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
		if drawing == "Drawing" then
			end_footprint()
		end
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
