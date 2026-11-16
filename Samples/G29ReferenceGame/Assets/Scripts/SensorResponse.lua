function OnTriggerEnter(other)
  assert(other ~= nil and other:uuid() == "b0170000-0000-4000-8000-000000000012")
  assert(entity:destroy())
  assert(scene.request_stop())
end
