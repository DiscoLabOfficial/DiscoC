local frames, readyFrames = 0, 0
local folder = emu.getRomInfo().path:match("^(.*[/\\])") or ""
emu.addEventCallback(function()
    frames = frames + 1
    local status = emu.read(0, emu.memType.snesWorkRam)
    if status == 0 and frames < 1000 then return end
    if status == 2 then readyFrames = readyFrames + 1 end
    if status == 2 and readyFrames < 5 then return end
    local result = emu.read16(2, emu.memType.snesWorkRam)
    local passed = status == 2 and result == 3281 and
        emu.read16(0xF000, emu.memType.gsuWorkRam) == 3281 and
        emu.read16(0, emu.memType.snesCgRam) == 0x001F and
        (emu.getPixel(128, 96) & 0xFFFFFF) == 0xFF0000 and
        emu.getPixel(128, 96) == emu.getPixel(0, 96)
    local message = string.format("%s rotating triangle negative: status=%d result=%d red backdrop",
        passed and "PASS" or "FAIL", status, result)
    emu.log(message)
    local file = io.open(folder .. "rotating-triangle-negative-verification.log", "w")
    if file then file:write(message .. "\n"); file:close() end
    local screenshot = io.open(folder .. "rotating-negative-preview.png", "wb")
    if screenshot then screenshot:write(emu.takeScreenshot()); screenshot:close() end
    emu.stop(passed and 0 or 1)
end, emu.eventType.endFrame)
