local queryChecks = 0

function OnUpdate()
  local hit = physics.raycast(0, 0, 3, 0, 0, -1, 8, 2, false)
  assert(hit ~= nil and hit.uuid == "b0170000-0000-4000-8000-000000000011")
  assert(hit.entity:uuid() == hit.uuid and hit.fraction > 0)

  local overlap = physics.overlap_box(1.0, 1.0, 1.0, 0, 0, 0, 2, 2, false)
  assert(overlap ~= nil and #overlap.hits > 0 and overlap.truncated == false)

  local cast = physics.cast_capsule(0.25, 1.2, -2, 0, 0, 1, 0, 0, 8, 2, false)
  assert(cast ~= nil and cast.uuid == "b0170000-0000-4000-8000-000000000011")
  queryChecks = queryChecks + 1

  if entity:uuid() == "b0170000-0000-4000-8000-000000000012" then
    local applied, message = entity:apply_force(18, 0, 0)
    assert(applied, message)
  end
end
