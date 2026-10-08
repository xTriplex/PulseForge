local elapsed = 0
local initialX = 0
local initialY = 0
local initialZ = 0

function OnCreate()
	local x, y, z = entity:get_translation()
	assert(x ~= nil and y ~= nil and z ~= nil, "Unable to read the entity's starting translation")
	initialX, initialY, initialZ = x, y, z
end

function OnUpdate(deltaTime)
	elapsed = elapsed + deltaTime
	local updated, message = entity:set_translation(initialX + math.sin(elapsed) * 0.25, initialY, initialZ)
	assert(updated, message)
end
