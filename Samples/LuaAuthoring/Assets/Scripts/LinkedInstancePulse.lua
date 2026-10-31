--[[PulseForgeScript:1
{"version":1,"properties":[{"id":"pulse.offset","label":"Pulse offset","type":"float","default":0.4,"min":0.0,"max":2.0}],"modules":["shared/motion"]}
]]
local motion = require("shared/motion")

function OnUpdate(deltaTime)
  local _, y, z = entity:get_translation()
  local offset = motion.offset(entity:get_float_property("pulse.offset"))
  assert(entity:set_translation(offset, y, z))
end
