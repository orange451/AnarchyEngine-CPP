-- The studio's Move tool: translate handles on the first selected PVInstance
-- while the place is not playing. Its one Dragger lives in Core, so it is
-- never saved and never an undo step; dragging its handles is.
local RunService = game:GetService("RunService")
local Selection = game:GetService("Selection")
local Core = game:GetService("Core")

local SPACE = Enum.DraggerSpace.World
local INCREMENT = 1 -- studs

local dragger

-- The instance the handles belong on now, or nil.
local function wanted()
	if RunService:IsRunning() then
		return nil
	end
	local first = Selection:Get()[1]
	if first ~= nil and first:IsA("PVInstance") then
		return first
	end
	return nil
end

local function update()
	local target = wanted()
	if target == nil then
		if dragger ~= nil and dragger.Parent ~= nil then
			dragger.Adornee = nil
		end
		return
	end
	-- Made on first need, and again if something destroyed it.
	if dragger == nil or dragger.Parent == nil then
		dragger = Instance.new("Dragger")
		dragger.Name = "MoveTool"
		dragger.Space = SPACE
		dragger.Increment = INCREMENT
		dragger.Parent = Core
	end
	dragger.Adornee = target
end

Selection.SelectionChanged:Connect(update)
RunService.Started:Connect(update)
RunService.Stopped:Connect(update)
update()
