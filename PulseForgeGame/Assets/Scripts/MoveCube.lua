local elapsed = 0

function OnUpdate(deltaTime)
	elapsed = elapsed + deltaTime
	local updated, message = entity:set_translation(math.sin(elapsed) * 0.25, 0, 0)
	assert(updated, message)
end
