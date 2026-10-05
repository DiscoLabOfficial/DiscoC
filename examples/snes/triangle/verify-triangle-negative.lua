-- Negative integration oracle: the real 9409 result must show red, not BG1.
local frames, readyFrames = 0, 0
emu.addEventCallback(function()
    frames = frames + 1
    if frames < 3 then return end
    local status = emu.read(0, emu.memType.snesWorkRam)
    if status == 0 and frames < 300 then return end
    if status == 2 then
        readyFrames = readyFrames + 1
        if readyFrames < 5 then return end
    end
    local count = emu.read16(2, emu.memType.snesWorkRam)
    local cpuRamCount = emu.read16(0xA, emu.memType.snesWorkRam)
    local ramCount = emu.read16(0xF000, emu.memType.gsuWorkRam)
    local backdrop = emu.read16(0, emu.memType.snesCgRam)
    local passed = status == 2 and count == 9409 and cpuRamCount == 9409 and
                   ramCount == 9409 and backdrop == 0x001F
    local message = string.format("%s triangle negative: status=%d R0=%d RAM=%d backdrop=$%04X",
        passed and "PASS" or "FAIL", status, count, ramCount, backdrop)
    emu.log(message)
    if io and io.open then
        local folder = emu.getRomInfo().path:match("^(.*[/\\])") or ""
        local file = io.open(folder .. "triangle-negative-verification.log", "w")
        if file then file:write(message .. "\n"); file:close() end
        local screenshot = io.open(folder .. "triangle-negative-preview.png", "wb")
        if screenshot then screenshot:write(emu.takeScreenshot()); screenshot:close() end
    end
    emu.stop(passed and 0 or 1)
end, emu.eventType.endFrame)
