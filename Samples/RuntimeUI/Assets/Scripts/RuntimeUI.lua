local score = 0
local elapsed = 0

function OnCreate()
    score = 0
    elapsed = 0
end

function OnUpdate(deltaTime)
    elapsed = elapsed + deltaTime
    if input.was_action_pressed("game.interact") then
        score = score + 10
        ui.set_text("b0180000-0000-4000-8000-000000000132", "Score: " .. tostring(score))
    end
    if elapsed >= 0.5 then
        elapsed = 0
        score = score + 1
        ui.set_text("b0180000-0000-4000-8000-000000000132", "Score: " .. tostring(score))
    end
end

function OnUIAction(action)
    if action == "start" then
        scene.request_transition("b0180000-0000-4000-8000-000000000011")
    elseif action == "pause" then
        scene.request_pause(true)
    elseif action == "resume" then
        scene.request_pause(false)
    elseif action == "menu" then
        scene.request_transition("b0180000-0000-4000-8000-000000000010")
    end
end
