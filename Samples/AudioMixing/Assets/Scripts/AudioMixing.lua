local MusicLabel = "b0190000-0000-4000-8000-000000000302"
local EffectsLabel = "b0190000-0000-4000-8000-000000000306"
local UILabel = "b0190000-0000-4000-8000-000000000309"
local OutputLabel = "b0190000-0000-4000-8000-000000000316"
local EffectsSource = "b0190000-0000-4000-8000-000000000204"
local RefreshPending = true
local OutputRetryFailed = false

local function clamp_gain(Value)
    return math.max(0.0, math.min(1.0, Value))
end

local function update_labels()
    local Music = audio.get_gain("music")
    local Effects = audio.get_gain("effects")
    local UI = audio.get_gain("ui")
    local MusicMuted = audio.is_muted("music")
    local EffectsMuted = audio.is_muted("effects")
    local UIMuted = audio.is_muted("ui")
    assert(Music ~= nil and Effects ~= nil and UI ~= nil)
    assert(ui.set_text(MusicLabel, "Music: " .. tostring(math.floor(Music * 100 + 0.5)) .. "%" .. (MusicMuted and " (muted)" or "")))
    assert(ui.set_text(EffectsLabel, "Effects: " .. tostring(math.floor(Effects * 100 + 0.5)) .. "%" .. (EffectsMuted and " (muted)" or "")))
    assert(ui.set_text(UILabel, "UI: " .. tostring(math.floor(UI * 100 + 0.5)) .. "%" .. (UIMuted and " (muted)" or "")))
    local OutputSuffix = OutputRetryFailed and " (retry unavailable)" or ""
    assert(ui.set_text(OutputLabel, "Audio output: " .. audio.get_output_state() .. OutputSuffix))
end

local function change_gain(Channel, Delta)
    local Current = audio.get_gain(Channel)
    assert(Current ~= nil)
    assert(audio.set_gain(Channel, clamp_gain(Current + Delta)))
    RefreshPending = true
end

function OnCreate()
    -- User audio settings are intentionally unavailable during candidate startup.
    RefreshPending = true
end

function OnUpdate(deltaTime)
    if RefreshPending then
        update_labels()
        RefreshPending = false
    end
end

function OnUIAction(Action)
    assert(entity:play_audio())
    if Action == "music_down" then
        change_gain("music", -0.1)
    elseif Action == "music_up" then
        change_gain("music", 0.1)
    elseif Action == "music_mute" then
        assert(audio.set_muted("music", not audio.is_muted("music")))
        RefreshPending = true
    elseif Action == "effects_down" then
        change_gain("effects", -0.1)
    elseif Action == "effects_up" then
        change_gain("effects", 0.1)
    elseif Action == "ui_down" then
        change_gain("ui", -0.1)
    elseif Action == "ui_up" then
        change_gain("ui", 0.1)
    elseif Action == "play_effect" then
        local Effect = scene.find_entity(EffectsSource)
        assert(Effect ~= nil and Effect:play_audio())
    elseif Action == "pause" then
        assert(scene.request_pause(true))
    elseif Action == "resume" then
        assert(scene.request_pause(false))
    elseif Action == "save_settings" then
        assert(audio.save_settings())
    elseif Action == "retry_output" then
        local Recovered = audio.retry_output()
        OutputRetryFailed = not Recovered
    end
    update_labels()
    RefreshPending = false
end
