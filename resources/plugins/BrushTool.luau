-- The studio's Brush tool: draw boxes, move and resize brushes, push and pull
-- their faces, extrude, drag corners, clip, and loop cut, while the place is
-- not playing. The Brushes button on the Plugins tab turns it on, and the
-- Brushes pane holds its modes and actions. Every edit is one undo step.
--
-- Draw mode
--   Drag on a surface or the ground: the footprint, then move to set the
--   height and click to place it. Shift keeps it square, then a cube.
--   Alt mid-drag ends the footprint early.
--   Click a brush: select it (Ctrl adds or removes). Click nothing: deselect.
--   Drag a selected brush: move it on the ground (Alt: up and down, Shift:
--   one axis, Ctrl: a copy).
--   Shift-drag a selected brush's face: push or pull that face of every
--   selected brush. Ctrl+Shift-drag: extrude a new brush from it instead.
--   Alt-click a face: paint it the pane's Material (Alt+Shift: the whole brush).
-- Vertex mode: drag a selected brush's corner on the plane facing the camera
--   (Alt: up and down). Red means it would not stay convex.
-- Clip mode: click two or three points, then Cut; Side picks the half kept.
-- Loop mode: point near an edge and click to cut the brush in two across it,
--   on the grid (Shift: at the middle).

local RunService = game:GetService("RunService")
local Selection = game:GetService("Selection")
local History = game:GetService("ChangeHistoryService")
local Core = game:GetService("Core")
local Assets = game:GetService("Assets")

local GRIDS = { 0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256 }
local RAY_LENGTH = 8000
-- A press that moves this far, in points, is a drag; less is a click.
local DRAG_POINTS = 4
-- Corners are picked this close, in points.
local CORNER_POINTS = 10

local DRAW = Color3.new(1, 0.62, 0.15)
local FACE = Color3.new(1, 0.85, 0.2)
local CORNER = Color3.new(0.3, 0.85, 1)
local CORNER_HOT = Color3.new(1, 0.9, 0.2)
local INVALID = Color3.new(1, 0.25, 0.25)
local KEEP = Color3.new(1, 1, 1)
local CLIP = Color3.new(1, 0.35, 0.9)
local GRID_LINE = Color3.new(0.75, 0.8, 0.9)

local MODES = { "Draw", "Vertex", "Clip", "Loop" }
local HINTS = {
	Draw = "Drag: draw, move up, click\nClick: select   Drag it: move\nShift-drag a face: push/pull\nCtrl+Shift-drag: extrude\nAlt-click: paint",
	Vertex = "Drag a corner (Alt: up/down)\nRed: would not stay convex",
	Clip = "Click 2 or 3 points, then Cut\nSide: which half stays",
	Loop = "Point near an edge, click:\ncut across it, on the grid\nShift: at the middle",
}
local SIDES = { "Back", "Front", "Both" }

type Ray = { Origin: Vector3, Direction: Vector3 }
type Mods = { shift: boolean, ctrl: boolean, alt: boolean }
-- What a ray hit; brush is the Brush when it was one.
type Hit = { instance: Instance, brush: Brush?, face: number?, position: Vector3, normal: Vector3 }
type Press = { x: number, y: number, ray: Ray, mods: Mods, hit: Hit? }
-- A selected brush at a drag's start; good is the last shape an edit made.
type Held = { brush: Brush, transform: Matrix4, faces: { BrushFace }, good: { BrushFace }, face: number? }
type Corner = { brush: Brush, world: Vector3 }

-- Tool state -----------------------------------------------------------------

local active = false
local mode = "Draw"
local grid = 1
local lastHeight = 4
local material: Material? = nil

local mouse = plugin:GetMouse()
local ray: Ray = { Origin = Vector3.zero, Direction = -Vector3.zAxis }
-- Whether the pointer has been over the view since the tool turned on.
local hasRay = false
local mods: Mods = { shift = false, ctrl = false, alt = false }

-- nil, or "Pending", "Drawing", "Height", "Moving", "Resizing", "Vertex".
local drag: string? = nil
local press: Press? = nil
local recording: string? = nil
local held: { Held } = {}
local dragOrigin = Vector3.zero
local dragNormal = Vector3.yAxis
local dragAmount = 0
local dragDelta = Vector3.zero
local dragInvalid = false

-- Drawing: a box on the plane through planePoint, normal along planeAxis.
local planeAxis = 2
local planeSign = 1
local planePoint = Vector3.zero
local drawStart = Vector3.zero
local drawEnd = Vector3.zero
local height = 0

local clipPoints: { Vector3 } = {}
local clipNormal = Vector3.yAxis
local clipSide = 1

-- Vertex mode: which corner is under the pointer and which is held.
local corners: { Corner } = {}
local hoverCorner: number? = nil
local dragCorner: number? = nil

-- What the tool draws: lines in three strengths, and a readout by the pointer.
local function make_wire(name: string, transparency: number): WireframeAdornment
	local wire = Instance.new("WireframeAdornment")
	wire.Name = name
	wire.Transparency = transparency
	wire.Visible = false
	wire.Parent = Core
	return wire
end
local solid = make_wire("BrushToolLines", 0)
local faint = make_wire("BrushToolHover", 0.55)
local gridWire = make_wire("BrushToolGrid", 0.8)

local readoutAnchor = Instance.new("Attachment")
readoutAnchor.Name = "BrushToolReadout"
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

-- Vector helpers -------------------------------------------------------------

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

-- The world axis (1 X, 2 Y, 3 Z) a normal lies most along.
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

local function unit(v: Vector3): Vector3
	return v.Magnitude > 1e-12 and v.Unit or Vector3.zero
end

local function number(x: number): string
	if math.abs(x - math.round(x)) < 1e-6 then
		return string.format("%d", math.round(x))
	end
	return string.format("%.3g", x)
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
	local d = unit(r.Direction)
	local w = origin - r.Origin
	local b = dir:Dot(d)
	local denom = 1 - b * b
	if denom < 1e-6 then
		return 0
	end
	return (b * d:Dot(w) - dir:Dot(w)) / denom
end

local function camera(): Camera?
	return workspace.CurrentCamera
end

-- World units per point at p, so marks keep their size on screen.
local function world_per_point(p: Vector3): number
	local cam = camera()
	if cam == nil or cam.ViewportSize.Y <= 0 then
		return 0.05
	end
	local frame = cam.Transform
	local depth = math.max((p - frame.Position):Dot(frame.LookVector), 0.05)
	return 2 * depth * math.tan(math.rad(cam.FieldOfView) / 2) / cam.ViewportSize.Y
end

-- The camera's back (+Z) direction, the way it looks from.
local function camera_back(): Vector3
	local cam = camera()
	return if cam ~= nil then cam.Transform.ZVector else Vector3.zAxis
end

-- The world axis (1 or 3) nearest the camera's right, or its forward, on the ground.
local function camera_axis(right: boolean): (number, number)
	local cam = camera()
	local dir = cam and (right and cam.Transform.RightVector or cam.Transform.LookVector) or Vector3.xAxis
	if math.abs(dir.X) >= math.abs(dir.Z) then
		return 1, dir.X >= 0 and 1 or -1
	end
	return 3, dir.Z >= 0 and 1 or -1
end

-- Picking and selection --------------------------------------------------------

local function pick(r: Ray?, brushesOnly: boolean): Hit?
	if r == nil then
		return nil
	end
	local hit = workspace:Raycast(r.Origin, r.Direction * RAY_LENGTH)
	if hit == nil then
		return nil
	end
	local brush: Brush? = if hit.Instance:IsA("Brush") then hit.Instance :: any else nil
	if brushesOnly and brush == nil then
		return nil
	end
	return { instance = hit.Instance, brush = brush, face = hit.Face, position = hit.Position, normal = hit.Normal }
end

local function selected_brushes(): { Brush }
	local list: { Brush } = {}
	for _, item in Selection:Get() do
		if item:IsA("Brush") and item.Parent ~= nil then
			table.insert(list, item :: any)
		end
	end
	return list
end

local function select(list: { Brush })
	Selection:Set(list :: any)
end

local function is_selected(brush: Instance): boolean
	for _, item in Selection:Get() do
		if item == brush then
			return true
		end
	end
	return false
end

-- Undo -----------------------------------------------------------------------

local function begin_step(name: string)
	if recording == nil then
		recording = History:TryBeginRecording(name)
	end
end

local function end_step(commit: boolean)
	if recording ~= nil then
		local id = recording
		recording = nil
		History:FinishRecording(
			id,
			commit and Enum.FinishRecordingOperation.Commit or Enum.FinishRecordingOperation.Cancel
		)
	end
end

-- One undo step around fn.
local function step(name: string, fn: () -> ())
	begin_step(name)
	local ok, err = pcall(fn)
	end_step(true)
	if not ok then
		print("Brushes: " .. tostring(err))
	end
end

-- Brushes ---------------------------------------------------------------------

-- A new brush like source, with faces (source's own when nil), beside it.
local function copy_brush(source: Brush, faces: { BrushFace }?): Brush
	local copy = Instance.new("Brush")
	copy.Name = source.Name
	copy.Transform = source.Transform
	copy:SetFaces(faces or source:GetFaces())
	copy.Anchored = source.Anchored
	copy.Color = source.Color
	copy.Transparency = source.Transparency
	copy.CanCollide = source.CanCollide
	copy.Friction = source.Friction
	copy.Bounciness = source.Bounciness
	copy.Parent = source.Parent
	return copy
end

local function capture_held()
	held = {}
	for _, brush in selected_brushes() do
		local faces = brush:GetFaces()
		table.insert(held, { brush = brush, transform = brush.Transform, faces = faces, good = faces, face = nil })
	end
end

-- Puts item's brush back to the faces it had at the drag's start, then edits
-- it; a refused edit leaves the last shape that worked. True when it worked.
local function edit_held(item: Held, fn: (Brush) -> ()): boolean
	local ok = pcall(function()
		item.brush:SetFaces(item.faces)
		fn(item.brush)
	end)
	if ok then
		item.good = item.brush:GetFaces()
	else
		pcall(function()
			item.brush:SetFaces(item.good)
		end)
	end
	return ok
end

local function refresh_corners()
	corners = {}
	for _, brush in selected_brushes() do
		local m = brush.Transform
		for _, v in brush:GetVertices() do
			table.insert(corners, { brush = brush, world = m * v })
		end
	end
end

-- Drawing ---------------------------------------------------------------------

local function clear_marks()
	solid:Clear()
	faint:Clear()
	gridWire:Clear()
	readout.Visible = false
end

local function label(at: Vector3, text: string)
	-- Up and to the right of the point, clear of the pointer.
	local cam = camera()
	if cam ~= nil then
		local step = world_per_point(at)
		at += cam.Transform.RightVector * (step * 16) + cam.Transform.UpVector * (step * 24)
	end
	readoutAnchor.Offset = Matrix4.new(at)
	readoutText.Text = text
	readout.Visible = true
end

local function cross(wire: WireframeAdornment, p: Vector3, size: number, color: Color3)
	for axis = 1, 3 do
		local d = axis_vec(axis, size)
		wire:AddLine(p - d, p + d, color)
	end
end

local function box_lines(wire: WireframeAdornment, lo: Vector3, hi: Vector3, color: Color3)
	local p = {
		Vector3.new(lo.X, lo.Y, lo.Z),
		Vector3.new(hi.X, lo.Y, lo.Z),
		Vector3.new(hi.X, lo.Y, hi.Z),
		Vector3.new(lo.X, lo.Y, hi.Z),
		Vector3.new(lo.X, hi.Y, lo.Z),
		Vector3.new(hi.X, hi.Y, lo.Z),
		Vector3.new(hi.X, hi.Y, hi.Z),
		Vector3.new(lo.X, hi.Y, hi.Z),
	}
	for i = 1, 4 do
		local j = i % 4 + 1
		wire:AddLine(p[i], p[j], color)
		wire:AddLine(p[4 + i], p[4 + j], color)
		wire:AddLine(p[i], p[4 + i], color)
	end
end

-- A brush's edges, in world space.
local function brush_lines(wire: WireframeAdornment, brush: Brush, color: Color3, frame: Matrix4?)
	local m = frame or brush.Transform
	local vertices = brush:GetVertices()
	local points: { Vector3 } = {}
	for _, edge in brush:GetEdges() do
		table.insert(points, m * vertices[edge[1]])
		table.insert(points, m * vertices[edge[2]])
	end
	if #points > 0 then
		wire:AddLines(points, color)
	end
end

-- One face's outline, an X across it so it reads as picked, and a tick along its normal.
local function face_lines(brush: Brush, face: number?, color: Color3)
	if face == nil then
		return
	end
	local m = brush.Transform
	local points: { Vector3 } = {}
	local middle = Vector3.zero
	for _, p in brush:GetFaceVertices(face) do
		local w = m * p
		table.insert(points, w)
		middle += w
	end
	if #points < 3 then
		return
	end
	solid:AddPath(points, true, color)
	middle /= #points
	local a, b = points[1], points[#points // 2 + 1]
	faint:AddLine(a, b, color)
	local n = unit(m:VectorToWorldSpace(brush:GetFace(face).Normal))
	solid:AddLine(middle, middle + n * math.max(0.5, (a - b).Magnitude * 0.25), color)
end

-- A fading patch of grid around center on the axis plane.
local function grid_patch(center: Vector3, axis: number)
	local u = axis % 3 + 1
	local v = u % 3 + 1
	local cells = 8
	local c = with_axis(snap_vec(center), axis, axis_of(center, axis))
	local points: { Vector3 } = {}
	for i = -cells, cells do
		local a = with_axis(with_axis(c, u, axis_of(c, u) + i * grid), v, axis_of(c, v) - cells * grid)
		table.insert(points, a)
		table.insert(points, with_axis(a, v, axis_of(c, v) + cells * grid))
		local b = with_axis(with_axis(c, v, axis_of(c, v) + i * grid), u, axis_of(c, u) - cells * grid)
		table.insert(points, b)
		table.insert(points, with_axis(b, u, axis_of(c, u) + cells * grid))
	end
	gridWire:AddLines(points, GRID_LINE)
end

-- A spare Brush, never parented, for previews: the clip's halves and the loop's ring.
local scratch = Instance.new("Brush")

-- The clip plane's three points, world space; nil until two are clicked.
local function clip_plane(): (Vector3?, Vector3, Vector3)
	if #clipPoints < 2 then
		return nil, Vector3.zero, Vector3.zero
	end
	local a, b = clipPoints[1], clipPoints[2]
	local c = clipPoints[3] or (a + clipNormal)
	if (b - a):Cross(c - a).Magnitude < 1e-6 then
		return nil, Vector3.zero, Vector3.zero
	end
	return a, b, c
end

-- The loop cut on a hit brush, in its space: normal, point, edge's ends; or nil.
local function loop_at(hit: Hit?): (Vector3?, Vector3, Vector3, Vector3)
	local brush = hit and hit.brush
	if hit == nil or brush == nil or hit.face == nil then
		return nil, Vector3.zero, Vector3.zero, Vector3.zero
	end
	local localPoint = brush.Transform:PointToObjectSpace(hit.position)
	local n, p, from, to = brush:GetLoopCut(hit.face, localPoint, grid, mods.shift)
	if n == nil then
		return nil, Vector3.zero, Vector3.zero, Vector3.zero
	end
	return n, p :: Vector3, from :: Vector3, to :: Vector3
end

local function redraw()
	clear_marks()
	if not active or not hasRay then
		return
	end
	local r = ray

	if drag == "Drawing" or drag == "Height" then
		local h = drag == "Height" and planeSign * height or 0
		local far = drawEnd + axis_vec(planeAxis, h)
		local lo = drawStart:Min(drawEnd):Min(far)
		local hi = drawStart:Max(drawEnd):Max(far)
		grid_patch(drawEnd, planeAxis)
		box_lines(solid, lo, hi, DRAW)
		local size = hi - lo
		if drag == "Height" then
			label(hi, number(size.X) .. " x " .. number(size.Y) .. " x " .. number(size.Z) .. "   click to place")
		else
			local u = planeAxis % 3 + 1
			local v = u % 3 + 1
			label(hi, number(axis_of(size, u)) .. " x " .. number(axis_of(size, v)))
		end
		return
	end

	if mode == "Loop" then
		local hit = pick(r, true)
		local brush = hit and hit.brush
		if brush == nil then
			return
		end
		brush_lines(faint, brush, KEEP)
		local n, p, from, to = loop_at(hit)
		if n == nil then
			return
		end
		local m = brush.Transform
		solid:AddLine(m * from, m * to, FACE)
		-- The new face: the back half's polygon on the cut plane.
		local ok = pcall(function()
			scratch:SetFaces(brush:GetFaces())
			scratch:Split(n, p)
		end)
		if ok then
			local face = scratch:GetFaceAt(p, n)
			if face ~= nil then
				local ring: { Vector3 } = {}
				for _, q in scratch:GetFaceVertices(face) do
					table.insert(ring, m * q)
				end
				solid:AddPath(ring, true, CLIP)
			end
		end
		local at = m * p
		cross(solid, at, world_per_point(at) * 5, CLIP)
		local t = (p - from).Magnitude
		label(at, number(t) .. " | " .. number((to - from).Magnitude - t))
		return
	end

	if mode == "Vertex" then
		hoverCorner = nil
		local cam = camera()
		if cam ~= nil and drag == nil then
			local best = CORNER_POINTS
			for i, corner in corners do
				local at, onScreen = cam:WorldToViewportPoint(corner.world)
				if onScreen then
					local d = math.sqrt((at.X - mouse.X) ^ 2 + (at.Y - mouse.Y) ^ 2)
					if d < best then
						best = d
						hoverCorner = i
					end
				end
			end
		end
		for i, corner in corners do
			local hot = i == hoverCorner or i == dragCorner
			local color = hot and (dragInvalid and INVALID or CORNER_HOT) or CORNER
			cross(solid, corner.world, world_per_point(corner.world) * 7, color)
		end
		if drag == "Vertex" then
			label(
				dragDelta,
				number(dragDelta.X) .. ", " .. number(dragDelta.Y) .. ", " .. number(dragDelta.Z)
					.. (dragInvalid and "   not convex" or "")
			)
		end
		return
	end

	if mode == "Clip" then
		for i, p in clipPoints do
			cross(solid, p, world_per_point(p) * 6, CLIP)
			if i > 1 then
				solid:AddLine(clipPoints[i - 1], p, CLIP)
			end
		end
		local a, b, c = clip_plane()
		if a ~= nil then
			for _, brush in selected_brushes() do
				local m = brush.Transform
				local la, lb, lc = m:PointToObjectSpace(a), m:PointToObjectSpace(b), m:PointToObjectSpace(c)
				local function half(face: BrushFace, color: Color3)
					local ok = pcall(function()
						scratch:SetFaces(brush:GetFaces())
						scratch:Clip(face)
					end)
					if ok then
						brush_lines(solid, scratch, color, m)
					end
				end
				local okBack, back = pcall(BrushFace.new, la, lb, lc)
				local okFront, front = pcall(BrushFace.new, la, lc, lb)
				if okBack and okFront then
					half(back, clipSide == 2 and INVALID or KEEP)
					half(front, clipSide == 1 and INVALID or KEEP)
				end
			end
		end
		local hit = pick(r, false)
		if hit ~= nil then
			cross(faint, snap_vec(hit.position), world_per_point(hit.position) * 4, CLIP)
		end
		return
	end

	if drag == "Moving" then
		solid:AddLine(dragOrigin, dragOrigin + dragDelta, FACE)
		label(
			dragOrigin + dragDelta,
			"moved " .. number(dragDelta.X) .. ", " .. number(dragDelta.Y) .. ", " .. number(dragDelta.Z)
		)
		return
	end

	if drag == "Resizing" then
		for _, item in held do
			if item.brush.Parent ~= nil and item.face ~= nil then
				face_lines(item.brush, item.face, dragInvalid and INVALID or FACE)
				local lo, hi = item.brush:GetBounds()
				local size = hi - lo
				label(
					dragOrigin + dragNormal * dragAmount,
					(dragAmount >= 0 and "+" or "") .. number(dragAmount) .. "   " .. number(size.X) .. " x "
						.. number(size.Y) .. " x " .. number(size.Z)
				)
			end
		end
		if press ~= nil and press.mods.ctrl then
			-- The extruded slab, as it would be made.
			local offset = dragNormal * dragAmount
			for _, item in held do
				if item.brush.Parent ~= nil and item.face ~= nil then
					local m = item.brush.Transform
					local ring: { Vector3 } = {}
					for _, p in item.brush:GetFaceVertices(item.face) do
						local w = m * p
						table.insert(ring, w + offset)
						solid:AddLine(w, w + offset, DRAW)
					end
					solid:AddPath(ring, true, DRAW)
				end
			end
		end
		return
	end

	if drag ~= nil then
		return
	end
	-- Idle in Draw: show what a press would do.
	local hit = pick(r, false)
	if hit == nil then
		local at = ray_plane(r, Vector3.zero, Vector3.yAxis)
		if at ~= nil then
			cross(gridWire, snap_vec(at), world_per_point(at) * 5, GRID_LINE)
		end
		return
	end
	local brush = hit.brush
	if brush == nil then
		return
	end
	if is_selected(brush) then
		if mods.shift then
			face_lines(brush, hit.face, FACE)
		end
	else
		brush_lines(faint, brush, KEEP)
	end
end

-- Dragging --------------------------------------------------------------------

local function update_draw()
	if mods.alt then
		-- Alt mid-drag: the footprint is done; the pointer sets the height now.
		drag = "Height"
		height = lastHeight
		return
	end
	local at = ray_plane(ray, planePoint, axis_vec(planeAxis))
	if at == nil then
		return
	end
	local stop = with_axis(snap_vec(at), planeAxis, axis_of(planePoint, planeAxis))
	if mods.shift then
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
	local h = ray_line(ray, drawEnd, axis_vec(planeAxis, planeSign))
	local value = snap(h)
	if math.abs(value) < grid then
		value = h < 0 and -grid or grid
	end
	if mods.shift then
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

local function commit_draw()
	drag = nil
	local far = drawEnd + axis_vec(planeAxis, planeSign * height)
	local lo = drawStart:Min(drawEnd):Min(far)
	local hi = drawStart:Max(drawEnd):Max(far)
	local size = hi - lo
	if size.X < 1e-4 or size.Y < 1e-4 or size.Z < 1e-4 then
		return
	end
	lastHeight = math.abs(height)
	local made: Brush? = nil
	step("Draw Brush", function()
		local brush = Instance.new("Brush")
		brush.Name = "Brush"
		brush:MakeBox(size)
		brush.Transform = Matrix4.new((lo + hi) / 2)
		if material ~= nil then
			brush:SetMaterial(material)
		end
		brush.Parent = workspace
		made = brush
	end)
	if made ~= nil and made.Parent ~= nil then
		select({ made })
	end
end

local function update_move()
	local delta: Vector3
	if mods.alt then
		-- Up and down: a vertical plane through the press, facing the camera.
		local back = camera_back()
		local facing = Vector3.new(back.X, 0, back.Z)
		facing = facing.Magnitude > 1e-4 and facing.Unit or Vector3.zAxis
		local at = ray_plane(ray, dragOrigin, facing)
		if at == nil then
			return
		end
		delta = Vector3.new(0, at.Y - dragOrigin.Y, 0)
	else
		local at = ray_plane(ray, dragOrigin, Vector3.yAxis)
		if at == nil then
			return
		end
		delta = Vector3.new(at.X - dragOrigin.X, 0, at.Z - dragOrigin.Z)
	end
	if mods.shift then
		-- One axis: whichever the pointer has gone furthest along.
		if math.abs(delta.X) >= math.abs(delta.Z) then
			delta = Vector3.new(delta.X, delta.Y, 0)
		else
			delta = Vector3.new(0, delta.Y, delta.Z)
		end
	end
	delta = snap_vec(delta)
	dragDelta = delta
	for _, item in held do
		if item.brush.Parent ~= nil then
			item.brush.Transform = item.transform + delta
		end
	end
end

local function update_resize()
	dragAmount = snap(ray_line(ray, dragOrigin, dragNormal))
	if press ~= nil and press.mods.ctrl then
		return -- extruding: nothing moves until the release
	end
	local invalid = false
	for _, item in held do
		local face = item.face
		if item.brush.Parent ~= nil and face ~= nil then
			-- Distance in the brush's own space (its Transform may scale).
			local localMove = item.transform:VectorToObjectSpace(dragNormal * dragAmount).Magnitude
			local distance = dragAmount >= 0 and localMove or -localMove
			if not edit_held(item, function(brush)
				brush:MoveFace(face, distance)
			end) then
				invalid = true
			end
		end
	end
	dragInvalid = invalid
end

local function update_vertex()
	local corner = if dragCorner ~= nil then corners[dragCorner] else nil
	if corner == nil then
		return
	end
	local target: Vector3
	if mods.alt then
		local back = camera_back()
		local facing = Vector3.new(back.X, 0, back.Z)
		facing = facing.Magnitude > 1e-4 and facing.Unit or Vector3.zAxis
		local at = ray_plane(ray, dragOrigin, facing)
		if at == nil then
			return
		end
		target = Vector3.new(dragOrigin.X, snap(at.Y), dragOrigin.Z)
	else
		-- On the camera-facing world plane through the corner.
		local axis = nearest_axis(camera_back())
		local at = ray_plane(ray, dragOrigin, axis_vec(axis))
		if at == nil then
			return
		end
		target = with_axis(snap_vec(at), axis, axis_of(dragOrigin, axis))
	end
	local invalid = false
	for _, item in held do
		if item.brush == corner.brush and item.brush.Parent ~= nil then
			-- From where the corner was when the drag began: item.faces are the brush then.
			local from = item.transform:PointToObjectSpace(dragOrigin)
			local to = item.transform:PointToObjectSpace(target)
			if not edit_held(item, function(brush)
				brush:MoveVertex(from, to)
			end) then
				invalid = true
			end
		end
	end
	dragInvalid = invalid
	if not invalid then
		corner.world = target
	end
	dragDelta = target
end

local function duplicate()
	local copies: { Brush } = {}
	for _, brush in selected_brushes() do
		table.insert(copies, copy_brush(brush))
	end
	if #copies > 0 then
		select(copies)
	end
end

local function extrude()
	-- A new brush: each held face's polygon swept along its normal.
	local offset = dragNormal * dragAmount
	local made: { Brush } = {}
	for _, item in held do
		local brush = item.brush
		local face = item.face
		if brush.Parent ~= nil and face ~= nil then
			local m = brush.Transform
			local origin = m.Position
			local points: { Vector3 } = {}
			for _, p in brush:GetFaceVertices(face) do
				local w = m * p - origin
				table.insert(points, w)
				table.insert(points, w + offset)
			end
			local faceMaterial = brush:GetFace(face).Material
			local ok, copy = pcall(function()
				local b = Instance.new("Brush")
				b.Name = brush.Name
				b:MakeHull(points)
				b.Transform = Matrix4.new(origin)
				b:SetMaterial(faceMaterial)
				b.Parent = brush.Parent
				return b
			end)
			if ok then
				table.insert(made, copy)
			end
		end
	end
	if #made > 0 then
		select(made)
	end
end

local function start_drag()
	local pressed = press :: Press
	local hit = pressed.hit
	local pressedBrush = if hit ~= nil then hit.brush else nil
	local onSelected = pressedBrush ~= nil and is_selected(pressedBrush)
	local pressedFace = if hit ~= nil then hit.face else nil
	if hit ~= nil and pressedBrush ~= nil and onSelected and pressed.mods.shift and pressedFace ~= nil then
		-- Push or pull the face (and the same face of every selected brush).
		capture_held()
		dragOrigin = hit.position
		dragAmount = 0
		dragInvalid = false
		local m = pressedBrush.Transform
		dragNormal = unit(m:VectorToWorldSpace(pressedBrush:GetFace(pressedFace).Normal))
		local point = m * pressedBrush:GetFaceVertices(pressedFace)[1]
		for _, item in held do
			local n = unit(item.transform:VectorToObjectSpace(dragNormal))
			local p = item.transform:PointToObjectSpace(point)
			local face = item.brush:GetFaceAt(p, n)
			if face ~= nil and item.brush:GetFace(face).Normal:Dot(n) > 0.9999 then
				item.face = face
			end
		end
		begin_step(if pressed.mods.ctrl then "Extrude Brush" else "Resize Brush")
		drag = "Resizing"
		return
	end
	if hit ~= nil and onSelected then
		capture_held()
		dragOrigin = hit.position
		dragDelta = Vector3.zero
		begin_step(if pressed.mods.ctrl then "Copy Brushes" else "Move Brushes")
		if pressed.mods.ctrl then
			duplicate()
			capture_held()
		end
		drag = "Moving"
		return
	end
	if mode ~= "Draw" then
		drag = nil
		return
	end
	-- Draw on the surface pressed (its nearest axis plane), else on the
	-- ground: the horizontal plane through the origin.
	planeAxis, planeSign = 2, 1
	if hit ~= nil then
		local n = hit.normal
		planeAxis = nearest_axis(n)
		planeSign = axis_of(n, planeAxis) >= 0 and 1 or -1
		-- A slope draws on a level plane through the point hit.
		if math.max(math.abs(n.X), math.abs(n.Y), math.abs(n.Z)) < 0.999 then
			planeAxis, planeSign = 2, 1
		end
		planePoint = hit.position
		-- Snap the plane onto the grid only when it is close: a face off the
		-- grid keeps its own height, so a box drawn on it sits on it.
		local along = axis_of(planePoint, planeAxis)
		if math.abs(snap(along) - along) < 1e-3 then
			planePoint = with_axis(planePoint, planeAxis, snap(along))
		end
	else
		local at = ray_plane(pressed.ray, Vector3.zero, Vector3.yAxis)
		if at == nil then
			drag = nil
			return
		end
		planePoint = Vector3.new(at.X, 0, at.Z)
	end
	local start = ray_plane(pressed.ray, planePoint, axis_vec(planeAxis))
	if start == nil then
		drag = nil
		return
	end
	drawStart = with_axis(snap_vec(start), planeAxis, axis_of(planePoint, planeAxis))
	drawEnd = drawStart
	drag = "Drawing"
	update_draw()
end

-- Clicks ----------------------------------------------------------------------

local function paint(hit: Hit, whole: boolean)
	step("Paint Brush", function()
		local brush = hit.brush
		if brush == nil then
			return
		end
		if whole then
			brush:SetMaterial(material)
		elseif hit.face ~= nil then
			brush:SetFaceMaterial(hit.face, material)
		end
	end)
end

local function click_clip()
	local hit = pick(ray, false)
	if hit == nil then
		return
	end
	if #clipPoints >= 3 then
		clipPoints = {}
	end
	if #clipPoints == 0 then
		clipNormal = hit.normal
	end
	-- Stay on the face clicked: only the in-face axes snap.
	local axis = nearest_axis(hit.normal)
	table.insert(clipPoints, with_axis(snap_vec(hit.position), axis, axis_of(hit.position, axis)))
end

local function apply_clip()
	local a, b, c = clip_plane()
	if a == nil then
		return
	end
	local made: { Brush } = {}
	local brushes = selected_brushes()
	step("Clip Brushes", function()
		for _, brush in brushes do
			local m = brush.Transform
			local la, lb, lc = m:PointToObjectSpace(a), m:PointToObjectSpace(b), m:PointToObjectSpace(c)
			pcall(function()
				if clipSide == 3 then
					local back = BrushFace.new(la, lb, lc)
					local front = brush:Split(back.Normal, la)
					table.insert(made, copy_brush(brush, front))
				elseif clipSide == 2 then
					brush:Clip(BrushFace.new(la, lc, lb))
				else
					brush:Clip(BrushFace.new(la, lb, lc))
				end
			end)
		end
	end)
	clipPoints = {}
	if #made > 0 then
		local all = brushes
		for _, brush in made do
			table.insert(all, brush)
		end
		select(all)
	end
end

local function apply_loop()
	local hit = pick(ray, true)
	local brush = if hit ~= nil then hit.brush else nil
	local n, p = loop_at(hit)
	if n == nil or brush == nil then
		return
	end
	local halves: { Brush }? = nil
	step("Loop Cut", function()
		local front = brush:Split(n, p)
		halves = { brush, copy_brush(brush, front) }
	end)
	if halves ~= nil then
		select(halves)
	end
end

-- The pane --------------------------------------------------------------------

local widget = plugin:CreateDockWidget("Brushes", {
	Title = "Brushes",
	InitialDock = "BottomLeft",
	Enabled = false,
	Width = 240,
	Height = 420,
	MinWidth = 200,
})

local SELECTED_STYLE = "background-color: var(--ide-find-button-checked-color); border-color: var(--ide-find-button-checked-border-color);"

local root = Instance.new("VBox")
root.Spacing = 6
-- The pane shows through, so the studio's theme colors it.
root.BackgroundTransparency = 1
root.Style = "padding: 8px;"
root.Parent = widget

local function row(parent: Instance?): HBox
	local box = Instance.new("HBox")
	box.Spacing = 4
	box.BackgroundTransparency = 1
	box.Parent = parent or root
	return box
end

local function caption(text: string, parent: Instance?): Label
	local made = Instance.new("Label")
	made.Text = text
	made.FontSize = 11
	made.Parent = parent or root
	return made
end

local function button(parent: Instance, text: string, action: () -> ()): Button
	local made = Instance.new("Button")
	made.Text = text
	made.Parent = parent
	made.Action:Connect(action)
	return made
end

local modeButtons: { [string]: Button } = {}
local gridLabel: Label
local gridSlider: Slider
local materialPicker: AssetPicker
local sideButton: Button
local clipRow: HBox
local hintLabel: Label
local redraw_pane: () -> ()

local function set_mode(next: string)
	mode = next
	clipPoints = {}
	hoverCorner = nil
	if mode == "Vertex" then
		refresh_corners()
	end
	redraw_pane()
	redraw()
end

-- Every selected brush, as one undo step, then the marks again.
local function on_selection(name: string, fn: ({ Brush }) -> ())
	local brushes = selected_brushes()
	if #brushes == 0 then
		return
	end
	step(name, function()
		fn(brushes)
	end)
	if mode == "Vertex" then
		refresh_corners()
	end
	redraw()
end

-- Turns the brushes a quarter turn about a world axis, about their middle on the grid.
local function turn(axis: number, quarterTurns: number)
	on_selection("Rotate Brushes", function(brushes)
		local lo = Vector3.new(math.huge, math.huge, math.huge)
		local hi = -lo
		for _, brush in brushes do
			local m = brush.Transform
			for _, v in brush:GetVertices() do
				local p = m * v
				lo = lo:Min(p)
				hi = hi:Max(p)
			end
		end
		local pivot = snap_vec((lo + hi) / 2)
		local q = quarterTurns % 4
		local c = q == 0 and 1 or (q == 2 and -1 or 0)
		local s = q == 1 and 1 or (q == 3 and -1 or 0)
		local rows = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }
		local a = axis % 3 + 1
		local b = a % 3 + 1
		rows[a][a], rows[a][b], rows[b][a], rows[b][b] = c, -s, s, c
		for _, brush in brushes do
			-- Turn the brush's faces about the pivot, in its space; Transform stays.
			local p = pivot - brush.Transform.Position
			local turned = Vector3.new(
				rows[1][1] * p.X + rows[1][2] * p.Y + rows[1][3] * p.Z,
				rows[2][1] * p.X + rows[2][2] * p.Y + rows[2][3] * p.Z,
				rows[3][1] * p.X + rows[3][2] * p.Y + rows[3][3] * p.Z
			)
			local t = p - turned
			brush:TransformShape(Matrix4.new(
				t.X, t.Y, t.Z,
				rows[1][1], rows[1][2], rows[1][3],
				rows[2][1], rows[2][2], rows[2][3],
				rows[3][1], rows[3][2], rows[3][3]
			))
		end
	end)
end

-- Mirrors each brush along a world axis, about its own middle.
local function flip(axis: number)
	on_selection("Flip Brushes", function(brushes)
		for _, brush in brushes do
			local lo, hi = brush:GetBounds()
			local middle = (lo + hi) / 2
			local scale = Vector3.new(axis == 1 and -1 or 1, axis == 2 and -1 or 1, axis == 3 and -1 or 1)
			local t = middle - middle * scale
			brush:TransformShape(Matrix4.new(t.X, t.Y, t.Z, scale.X, 0, 0, 0, scale.Y, 0, 0, 0, scale.Z))
		end
	end)
end

-- Replaces each brush with a shape filling its bounds, keeping its first face's Material.
local function reshape(kind: string, sides: number?)
	on_selection("Reshape Brushes", function(brushes)
		for _, brush in brushes do
			local lo, hi = brush:GetBounds()
			local keep = brush:GetFace(1).Material
			if kind == "Cylinder" then
				brush:MakeCylinder(hi - lo, sides or 8)
			elseif kind == "Cone" then
				brush:MakeCone(hi - lo, sides or 12)
			elseif kind == "Sphere" then
				brush:MakeSphere(hi - lo, sides or 2)
			else
				brush:MakeBox(hi - lo)
			end
			brush:TransformShape(Matrix4.new((lo + hi) / 2))
			brush:SetMaterial(keep)
		end
	end)
end

caption("Mode")
local modeRow = row()
for _, name in MODES do
	modeButtons[name] = button(modeRow, name, function()
		set_mode(name)
	end)
end

clipRow = row()
button(clipRow, "Cut", function()
	apply_clip()
	redraw()
end)
sideButton = button(clipRow, "", function()
	clipSide = clipSide % #SIDES + 1
	redraw_pane()
	redraw()
end)
button(clipRow, "Undo point", function()
	table.remove(clipPoints)
	redraw()
end)

gridLabel = caption("")
-- One notch per entry of GRIDS.
gridSlider = Instance.new("Slider")
gridSlider.Min = 1
gridSlider.Max = #GRIDS
gridSlider.Step = 1
gridSlider.Style = "width: 100%;"
gridSlider.Parent = root
gridSlider.Changed:Connect(function(name)
	if name == "Value" and GRIDS[gridSlider.Value] ~= grid then
		grid = GRIDS[gridSlider.Value]
		redraw_pane()
		redraw()
	end
end)

caption("Shape of the selection")
local shapeRow = row()
button(shapeRow, "Box", function()
	reshape("Box")
end)
button(shapeRow, "Cyl 8", function()
	reshape("Cylinder", 8)
end)
button(shapeRow, "Cyl 16", function()
	reshape("Cylinder", 16)
end)
button(shapeRow, "Cone", function()
	reshape("Cone", 12)
end)
button(shapeRow, "Ball", function()
	reshape("Sphere", 2)
end)

caption("Material")
local materialRow = row()
-- The Material new and painted brushes get; None is the default.
materialPicker = Instance.new("AssetPicker")
materialPicker.AssetType = Enum.AssetType.Material
materialPicker.Parent = materialRow
materialPicker.Changed:Connect(function(name)
	if name == "Value" then
		material = materialPicker.Value :: any
		redraw_pane()
	end
end)
button(materialRow, "Apply", function()
	on_selection("Paint Brushes", function(brushes)
		for _, brush in brushes do
			brush:SetMaterial(material)
		end
	end)
end)

caption("Selection")
local editRow = row()
button(editRow, "Duplicate", function()
	on_selection("Duplicate Brushes", function()
		duplicate()
		local axis, sign = camera_axis(true)
		for _, brush in selected_brushes() do
			brush.Transform = brush.Transform + axis_vec(axis, grid * sign)
		end
	end)
end)
button(editRow, "Delete", function()
	on_selection("Delete Brushes", function(brushes)
		for _, brush in brushes do
			brush:Destroy()
		end
	end)
end)
local turnRow = row()
button(turnRow, "Turn left", function()
	turn(2, 1)
end)
button(turnRow, "Turn right", function()
	turn(2, -1)
end)
local flipRow = row()
button(flipRow, "Flip across", function()
	flip((camera_axis(true)))
end)
button(flipRow, "Flip up", function()
	flip(2)
end)

hintLabel = caption("")

redraw_pane = function()
	for name, made in modeButtons do
		made.Style = name == mode and SELECTED_STYLE or ""
	end
	clipRow.Visible = mode == "Clip"
	sideButton.Text = "Side: " .. SIDES[clipSide]
	gridLabel.Text = "Grid " .. number(grid)
	gridSlider.Value = table.find(GRIDS, grid) or 1
	hintLabel.Text = HINTS[mode]
end
redraw_pane()

-- Turning on and off ----------------------------------------------------------

local toolbar = plugin:CreateToolbar("Brushes")
local toggle = toolbar:CreateButton("Brushes", "Draw and edit brushes", "icons/layer-shape-polygon.png", "Brushes")

local function turn_off()
	if not active then
		return
	end
	active = false
	end_step(drag ~= "Height")
	drag = nil
	press = nil
	held = {}
	clipPoints = {}
	corners = {}
	solid.Visible = false
	faint.Visible = false
	gridWire.Visible = false
	clear_marks()
	toggle:SetActive(false)
	widget.Enabled = false
end

local function turn_on()
	if active or RunService:IsRunning() then
		return
	end
	active = true
	mode = "Draw"
	solid.Visible = true
	faint.Visible = true
	gridWire.Visible = true
	toggle:SetActive(true)
	widget.Enabled = true
	plugin:Activate(true)
	redraw_pane()
end

toggle.Click:Connect(function()
	if active then
		plugin:Deactivate()
	else
		turn_on()
	end
end)

plugin.Deactivation:Connect(turn_off)
RunService.Started:Connect(function()
	plugin:Deactivate()
end)
-- Opening the pane, from the Window menu or as a restored layout does, turns
-- the tool on, and closing it turns the tool off.
widget.Changed:Connect(function(property)
	if property ~= "Enabled" then
		return
	end
	if widget.Enabled and not active then
		turn_on()
	elseif not widget.Enabled and active then
		plugin:Deactivate()
	end
end)
Selection.SelectionChanged:Connect(function()
	if active and mode == "Vertex" and drag == nil then
		refresh_corners()
		redraw()
	end
end)

-- The mouse ---------------------------------------------------------------------

local function read_mouse()
	local unitRay = mouse.UnitRay
	ray = { Origin = unitRay.Origin, Direction = unitRay.Direction }
	hasRay = true
	mods = { shift = mouse.Shift, ctrl = mouse.Ctrl, alt = mouse.Alt }
end

mouse.Move:Connect(function()
	if not active then
		return
	end
	read_mouse()
	if drag == "Pending" and press ~= nil then
		if math.sqrt((mouse.X - press.x) ^ 2 + (mouse.Y - press.y) ^ 2) >= DRAG_POINTS then
			start_drag()
		end
	elseif drag == "Drawing" then
		update_draw()
		if drag == "Height" then
			update_height()
		end
	elseif drag == "Height" then
		update_height()
	elseif drag == "Moving" then
		update_move()
	elseif drag == "Resizing" then
		update_resize()
	elseif drag == "Vertex" then
		update_vertex()
	end
	redraw()
end)

mouse.Button1Down:Connect(function()
	if not active then
		return
	end
	read_mouse()
	if drag == "Height" then
		commit_draw()
		redraw()
		return
	end
	if mode == "Clip" then
		click_clip()
		redraw()
		return
	end
	if mode == "Loop" then
		apply_loop()
		redraw()
		return
	end
	if mode == "Vertex" and hoverCorner ~= nil then
		dragCorner = hoverCorner
		capture_held()
		dragOrigin = corners[hoverCorner].world
		dragInvalid = false
		begin_step("Move Vertex")
		drag = "Vertex"
		return
	end
	local hit = pick(ray, false)
	press = { x = mouse.X, y = mouse.Y, ray = ray, mods = mods, hit = hit }
	if mods.alt and hit ~= nil and hit.brush ~= nil then
		paint(hit, mods.shift)
		press = nil
		return
	end
	drag = "Pending"
end)

mouse.Button1Up:Connect(function()
	if not active then
		return
	end
	read_mouse()
	local pressed = press
	if drag == "Pending" and pressed ~= nil then
		-- A click: select what is under it.
		drag = nil
		if mode == "Draw" then
			local hit = pressed.hit
			local brush = if hit ~= nil then hit.brush else nil
			if brush ~= nil then
				local list: { Brush } = if pressed.mods.ctrl then selected_brushes() else {}
				local found = table.find(list, brush)
				if found then
					table.remove(list, found)
				else
					table.insert(list, brush)
				end
				select(list)
			elseif not pressed.mods.ctrl then
				Selection:Set({})
			end
		end
	elseif drag == "Drawing" then
		local d = drawEnd - drawStart
		local u = planeAxis % 3 + 1
		local v = u % 3 + 1
		if math.abs(axis_of(d, u)) < 1e-4 or math.abs(axis_of(d, v)) < 1e-4 then
			drag = nil -- no footprint: nothing drawn
		else
			drag = "Height" -- now the height, until a click
			height = lastHeight
			update_height()
		end
	elseif drag == "Resizing" or drag == "Moving" or drag == "Vertex" then
		if drag == "Resizing" and pressed ~= nil and pressed.mods.ctrl and math.abs(dragAmount) > 1e-6 then
			extrude()
		end
		drag = nil
		end_step(true)
		held = {}
		dragCorner = nil
		if mode == "Vertex" then
			refresh_corners()
		end
	end
	redraw()
end)

plugin.Unloading:Connect(function()
	turn_off()
	local made: { Instance } = { solid, faint, gridWire, readout, readoutAnchor, scratch }
	for _, item in made do
		item:Destroy()
	end
end)
