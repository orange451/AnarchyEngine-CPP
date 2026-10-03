-- Flies the scene view's camera in edit mode. Hold the right mouse button in a
-- scene view to lock the pointer and turn. W, A, S, and D move where the camera
-- faces; E and Q move straight up and down. In play the game owns the camera,
-- so this does nothing.
local RunService = game:GetService("RunService")
local UserInputService = game:GetService("UserInputService")

local MOVE_SPEED = 16 -- studs per second
local TURN_SPEED = 0.004 -- radians per point of mouse motion
local MAX_PITCH = math.rad(89)

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
		position += move.Unit * MOVE_SPEED * dt
	end
	camera.Transform = Matrix4.new(position) * rotation
end)
