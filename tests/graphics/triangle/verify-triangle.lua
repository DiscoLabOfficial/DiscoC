-- Check the actual 65816 + GSU + PPU integration, not a mocked graphics call.
local frames, readyFrames = 0, 0
local function outputPath(name)
    local folder = emu.getRomInfo().path:match("^(.*[/\\])") or ""
    return folder .. name
end
local function finish(code, message)
    emu.log(message)
    if io and io.open then
        local file = io.open(outputPath("triangle-verification.log"), "w")
        if file then file:write(message .. "\n"); file:close() end
    end
    emu.stop(code)
end

emu.addEventCallback(function()
    frames = frames + 1
    if frames < 3 then return end
    local status = emu.read(0, emu.memType.snesWorkRam)
    if status == 0 and frames < 300 then return end
    if status ~= 1 then
        finish(1, string.format("FAIL triangle: host status=%d after %d frames", status, frames))
        return
    end
    -- Wait for complete displayed frames after the CPU has finished VRAM DMA.
    readyFrames = readyFrames + 1
    if readyFrames < 5 then return end

    local count = emu.read16(2, emu.memType.snesWorkRam)
    local cpuRamCount = emu.read16(0xA, emu.memType.snesWorkRam)
    local ramCount = emu.read16(0xF000, emu.memType.gsuWorkRam)
    local sp = emu.read16(4, emu.memType.snesWorkRam)
    local pbr = emu.read(6, emu.memType.snesWorkRam)
    local rambr = emu.read(7, emu.memType.snesWorkRam)
    local sfr = emu.read(8, emu.memType.snesWorkRam)
    if count ~= 9409 or cpuRamCount ~= 9409 or ramCount ~= 9409 or
       sp ~= 0xFFFA or pbr ~= 0x70 or rambr ~= 0 or (sfr & 0x20) ~= 0 then
        finish(1, string.format(
            "FAIL triangle: R0=%d CPU-RAM=%d RAM=%d SP=$%04X PBR=$%02X RAMBR=%d SFR=$%02X",
            count, cpuRamCount, ramCount, sp, pbr, rambr, sfr))
        return
    end

    local framebuffer = {}
    for address = 0, 0x5FFF do
        local value = emu.read(address, emu.memType.gsuWorkRam)
        framebuffer[address + 1] = value
        if emu.read(address, emu.memType.snesVideoRam) ~= value then
            finish(1, string.format("FAIL triangle: framebuffer/VRAM differ at $%04X", address))
            return
        end
    end

    -- Independently decode every logical pixel from four SNES bitplanes.
    -- GSU bitmap layout: 24 vertical tiles in each of the 32 tile columns.
    local plotted = 0
    for y = 0, 191 do
        for x = 0, 255 do
            local tile = (x >> 3) * 24 + (y >> 3)
            local address = tile * 32 + (y & 7) * 2
            local bit = 7 - (x & 7)
            local actual = 0
            for plane = 0, 3 do
                local offset = address + (plane >> 1) * 16 + (plane & 1)
                actual = actual | (((framebuffer[offset + 1] >> bit) & 1) << plane)
            end
            local expected = 0
            if y >= 48 and y <= 144 and math.abs(x - 128) <= y - 48 then
                expected = 1 + ((y - 48) >> 3)
                plotted = plotted + 1
            end
            if actual ~= expected then
                finish(1, string.format("FAIL triangle: pixel (%d,%d), expected %d got %d", x, y, expected, actual))
                return
            end
        end
    end
    if plotted ~= 9409 then finish(1, "FAIL triangle: invalid geometric expectation") return end

    -- Check the PPU's row-major map and its separate cleared blank tile.
    for row = 0, 31 do
        for column = 0, 31 do
            local address = 0x7000 + (row * 32 + column) * 2
            local expected = row < 24 and column * 24 + row or 768
            if emu.read16(address, emu.memType.snesVideoRam) ~= expected then
                finish(1, string.format("FAIL triangle: tilemap column=%d row=%d", column, row))
                return
            end
        end
    end
    for address = 0x6000, 0x601F do
        if emu.read(address, emu.memType.snesVideoRam) ~= 0 then
            finish(1, "FAIL triangle: padding tile is not blank") return
        end
    end
    if emu.read16(0, emu.memType.snesCgRam) ~= 0x0842 then
        finish(1, "FAIL triangle: wrong backdrop palette") return
    end

    local size = emu.getScreenSize()
    local scale = size.width / 256
    local background = emu.getPixel(0, 0)
    local center = emu.getPixel(math.floor(128 * scale), math.floor(96 * scale))
    if center == background then
        finish(1, "FAIL triangle: displayed center is indistinguishable from background") return
    end
    -- Optional capture. build-snes.cmake enables I/O only in this subprocess,
    -- without saving settings; output files are next to this demo's ROM.
    if io and io.open then
        local file = io.open(outputPath("triangle-preview.png"), "wb")
        if file then file:write(emu.takeScreenshot()); file:close() end
    end
    finish(0, string.format(
        "PASS triangle: all 49152 pixels and 1024 tilemap entries match; count=%d R10=$%04X PBR=$%02X RAMBR=%d screen=%dx%d",
        plotted, sp, pbr, rambr, size.width, size.height))
end, emu.eventType.endFrame)
