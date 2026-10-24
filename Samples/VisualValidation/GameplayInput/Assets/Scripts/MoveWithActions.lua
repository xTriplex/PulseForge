local initialX = 0
local initialY = 0
local initialZ = 0
local movedX = 0
local movedZ = 0

function OnCreate()
  initialX, initialY, initialZ = entity:get_translation()
  assert(initialX ~= nil and initialY ~= nil and initialZ ~= nil)
end

function OnUpdate(deltaTime)
  local moveX, moveY = input.get_vector("Move")
  local jump = input.is_action_held("Jump")
  local interact = input.was_action_pressed("Interact")
  if moveX == nil or moveY == nil or jump == nil or interact == nil then
    error("Gameplay input actions are unavailable")
  end
  if interact then
    movedX = 0
    movedZ = 0
  end
  movedX = movedX + moveX * deltaTime * 2.0
  movedZ = movedZ - moveY * deltaTime * 2.0
  local vertical = jump and 0.8 or 0.0
  local updated, message = entity:set_translation(initialX + movedX, initialY + vertical, initialZ + movedZ)
  assert(updated, message)
end
