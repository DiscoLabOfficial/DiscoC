-- Count newly published images, not repeated PPU refreshes or host wall time.
-- build-snes.cmake supplies an explicit NTSC, normal-speed GSU profile.
local folder = emu.getRomInfo().path:match("^(.*[/\\])") or ""
local config = dofile(folder .. "fps-config.lua")
local rate = 21477270 -- MesenCE SnesConsole::UpdateRegion, NTSC.
local samples, frames, failed = {}, 0, false

local function save(name, contents)
    local file = assert(io.open(folder .. name, "wb"))
    assert(file:write(contents)); assert(file:close())
end
local function guarded(callback)
    return function(...)
        if failed then return end
        local ok, message = pcall(callback, ...)
        if not ok then
            failed = true
            save("fps-error.log", tostring(message) .. "\n")
            emu.stop(1)
        end
    end
end

emu.addMemoryCallback(guarded(function(_, value)
    if value >= 2 then error("Host failure: " .. value) end
    if value ~= 1 or #samples >= 65 then return end
    local state = emu.getState()
    local clock = state["memoryManager.masterClock"]
    assert(type(clock) == "number", "Missing master-clock state")
    assert(state["cart.coprocessor.clockSelect"] == true, "Not the 21 MHz profile")
    local gsuClock = state["cart.coprocessor.cycleCount"]
    assert(type(gsuClock) == "number" and math.abs(gsuClock-clock) <= 256,
        "GSU overclock/configuration mismatch: this measurement requires GSU=100%")
    assert(state["cart.coprocessor.highSpeedMode"] == false, "Multiply timing profile changed")
    assert(state["cart.coprocessor.sfr.running"] == false, "GSU has not stopped")
    local frame = emu.read16(0x12, emu.memType.snesWorkRam)
    local phase = emu.read16(0xC, emu.memType.snesWorkRam)
    assert(frame == #samples + 1 and phase == (#samples % 64), "Wrong publication sequence")
    samples[#samples + 1] = {clock=clock, frame=frame, phase=phase, snesFrame=frames}
    if #samples == 65 then
        local duration = samples[65].clock - samples[1].clock
        assert(duration > 0, "Invalid duration")
        local records = {}
        local repeated = 0
        for index, sample in ipairs(samples) do
            records[#records + 1] = string.format(
                '{"frame":%d,"phase":%d,"master_clock":%d,"snes_frame":%d}',
                sample.frame, sample.phase, sample.clock, sample.snesFrame)
            if index > 1 then
                local refreshes = sample.snesFrame - samples[index - 1].snesFrame
                assert(refreshes >= 1, "Multiple images published within one refresh")
                repeated = repeated + refreshes - 1
            end
        end
        save("fps.json", string.format(
            '{"profile":"NTSC CLSR=1 GSU=100%% CACHE RAM; no artificial presentation hold",' ..
            '"measurement":"64 completed-image intervals after VRAM DMA; excludes boot and first render",' ..
            '"master_clock_hz":%d,"intervals":64,"master_clocks":%d,"seconds":%.12f,' ..
            '"displayed_pose_fps":%.12f,"snes_frames":%d,"repeated_refreshes":%d,"samples":[%s]}\n',
            rate, duration, duration/rate, 64*rate/duration,
            samples[65].snesFrame-samples[1].snesFrame, repeated, table.concat(records, ",")))
        emu.log(string.format("64 completed-image intervals: %.9f seconds, %.9f new poses/s", duration/rate, 64*rate/duration))
    end
end), emu.callbackType.write, 0x7e0000, 0x7e0000, emu.cpuType.snes, emu.memType.snesMemory)

emu.addEventCallback(guarded(function()
    frames = frames + 1
    assert(frames <= 10000, "FPS measurement timed out")
end), emu.eventType.endFrame)

-- Correctness remains a requirement: timing alone never turns the run green.
dofile(config.verifier)
