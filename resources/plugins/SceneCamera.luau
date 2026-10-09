-- Flies the scene view's camera in edit mode. Hold the right mouse button in a
-- scene view to lock the pointer and turn. W, A, S, and D move where the camera
-- faces; E and Q move straight up and down. The wheel jumps toward the pointer:
-- up forward, down back. Shift slows both. In play the game owns the camera, so
-- this does nothing.
local RunService = game:GetService("RunService")
local UserInputService = game:GetService("UserInputService")

local MOVE_SPEED = 16 -- units per second
local SCROLL_STEP = 8 -- units per notch of the wheel
local SLOW = 0.25 -- of both, while Shift is held
local TURN_SPEED = 0.004 -- radians per point of mouse motion
local MAX_PITCH = math.rad(89)

local function slow(): number
	if UserInputService:IsKeyDown(Enum.KeyCode.LeftShift) or UserInputService:IsKeyDown(Enum.KeyCode.RightShift) then
		return SLOW
	end
	return 1
end
UserInputService.InputBegan:Connect(function(input)
	if input.UserInputType == Enum.UserInputType.MouseButton2 and not RunService:IsRunning() then
		UserInputService.MouseBehavior = Enum.MouseBehavior.LockCurrentPosition
	end
end)

UserInputService.InputEnded:Connect(function(input)
	if input.UserInputType == Enum.UserInputType.MouseButton2 and not RunService:IsRunning() then
		UserInputService.MouseBehavior = Enum.MouseBehavior.Default
	end
end)

-- The wheel: a jump along the ray from the camera through the pointer.
UserInputService.InputChanged:Connect(function(input)
	if input.UserInputType ~= Enum.UserInputType.MouseWheel or RunService:IsRunning() then
		return
	end
	local camera = workspace.CurrentCamera
	local notches = input.Position.Z
	if camera == nil or notches == 0 then
		return
	end
	-- Errors until a view shows the camera; then there is nowhere to jump toward.
	local ok, _, direction = pcall(function()
		return camera:ViewportPointToRay(input.Position.X, input.Position.Y)
	end)
	if not ok or direction == nil then
		return
	end
	local transform = camera.Transform
	camera.Transform = Matrix4.new(transform.Position + direction * (notches * SCROLL_STEP * slow())) * transform.Rotation
end)

-- 1, -1, or 0, from a pair of keys.
local function axis(positive, negative)
	local value = 0
	if UserInputService:IsKeyDown(positive) then
		value += 1
	end
	if UserInputService:IsKeyDown(negative) then
		value -= 1
	end
	return value
end

RunService.RenderStepped:Connect(function(dt)
	local camera = workspace.CurrentCamera
	if camera == nil or RunService:IsRunning() then
		return
	end
	local transform = camera.Transform
	local rotation = transform.Rotation
	local turned = false
	if UserInputService.MouseBehavior ~= Enum.MouseBehavior.Default then
		local delta = UserInputService:GetMouseDelta()
		if delta.Magnitude > 0 then
			local pitch, yaw = transform:ToOrientation()
			yaw -= delta.X * TURN_SPEED
			pitch = math.clamp(pitch - delta.Y * TURN_SPEED, -MAX_PITCH, MAX_PITCH)
			rotation = Matrix4.fromOrientation(pitch, yaw, 0)
			turned = true
		end
	end
	local move = rotation.LookVector * axis(Enum.KeyCode.W, Enum.KeyCode.S)
		+ rotation.RightVector * axis(Enum.KeyCode.D, Enum.KeyCode.A)
		+ Vector3.new(0, axis(Enum.KeyCode.E, Enum.KeyCode.Q), 0)
	local moved = move.Magnitude > 0
	-- A still camera is not written, so an idle view does not mark the place changed.
	if not turned and not moved then
		return
	end
	local position = transform.Position
	if moved then
		position += move.Unit * MOVE_SPEED * slow() * dt
	end
	camera.Transform = Matrix4.new(position) * rotation
end)
