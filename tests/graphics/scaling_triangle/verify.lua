-- Independent analytic image, not the drawing code's edge/color accumulators.
-- Publication is checked synchronously; the host immediately reuses GSU RAM.
local folder = emu.getRomInfo().path:match("^(.*[/\\])") or ""
local frames, publications, failed = 0, 0, false
local shown = {}
local palette = {0x0842, 0x001F, 0x021F, 0x03FF, 0x03E0, 0x7FE0, 0x7C00, 0x7C10, 0x7C1F}
local function save(name, contents)
    local file = assert(io.open(folder .. name, "wb"))
    assert(file:write(contents)); assert(file:close())
end
local function finish(code, message)
    save("scaling-triangle-verification.log", message .. "\n")
    emu.log(message); emu.stop(code)
end
local function guarded(callback)
    return function(...)
        if failed then return end
        local ok, message = pcall(callback, ...)
        if not ok then
            failed = true
            finish(1, "FAIL scaling triangle: " .. tostring(message))
        end
    end
end
local function height_for(phase) return 24 + math.min(phase, 64 - phase) end
local function expected_pixel(x, y, height)
    local row = y - (128 - height)
    if row < 0 or row > height or math.abs(x - 128) > row then return 0 end
    return 1 + math.floor(8 * row / (height + 1))
end
local function rgb(value)
    local function channel(v) return (v << 3) | (v >> 2) end
    return (channel(value & 31) << 16) | (channel((value >> 5) & 31) << 8) |
        channel((value >> 10) & 31)
end

-- takeScreenshot can still return the previous asynchronously decoded pose
-- even after getScreenBuffer is verified. Encode the owned, checked snapshot
-- losslessly as a 24-bit BMP; crop only this NTSC profile's seven border rows.
local function capture(screen, name)
    local width, height = 256, 224
    local bytes = width * height * 3
    local header = "BM" .. string.pack("<I4I2I2I4I4i4i4I2I2I4I4i4i4I4I4",
        54 + bytes, 0, 0, 54, 40, width, height, 1, 24, 0, bytes, 0, 0, 0, 0)
    local rows = {}
    for y = height - 1, 0, -1 do
        local pixels = {}
        for x = 0, width - 1 do
            local value = screen[(y + 7) * width + x + 1]
            pixels[#pixels + 1] = string.char(value & 255, (value >> 8) & 255, (value >> 16) & 255)
        end
        rows[#rows + 1] = table.concat(pixels)
    end
    save(name, header .. table.concat(rows))
end

emu.addMemoryCallback(guarded(function(_, status)
    assert(status < 2, "host status=" .. status)
    if status ~= 1 or publications >= 65 then return end
    local frame = emu.read16(0x12, emu.memType.snesWorkRam)
    local phase = emu.read16(0xC, emu.memType.snesWorkRam)
    assert(frame == publications + 1 and phase == publications % 64, "publication sequence")
    local height = height_for(phase)
    local rows, pixels = height + 1, (height + 1)^2
    assert(emu.read16(2, emu.memType.snesWorkRam) == pixels and
        emu.read16(0xA, emu.memType.snesWorkRam) == pixels and
        emu.read16(0xE, emu.memType.snesWorkRam) == height and
        emu.read16(0x10, emu.memType.snesWorkRam) == rows, "independent counts/size")
    assert(emu.read16(0xF000, emu.memType.gsuWorkRam) == pixels and
        emu.read16(0xF004, emu.memType.gsuWorkRam) == height and
        emu.read16(0xF006, emu.memType.gsuWorkRam) == rows and
        emu.read16(0xF008, emu.memType.gsuWorkRam) == phase, "GSU mailbox")
    assert(emu.read16(4, emu.memType.snesWorkRam) == 0xFFFA and
        emu.read(6, emu.memType.snesWorkRam) == 0x70 and
        emu.read(7, emu.memType.snesWorkRam) == 0 and
        (emu.read(8, emu.memType.snesWorkRam) & 0x20) == 0, "stack/banks/STOP")
    local dmaLine = emu.read16(0x1A, emu.memType.snesWorkRam)
    assert(dmaLine >= 225 and dmaLine <= 261, "DMA finished outside VBlank")
    local cbr = emu.read16(0x1C, emu.memType.snesWorkRam)
    assert(cbr >= 0x6000 and cbr < 0xF000 and (cbr & 15) == 0, "CACHE base")
    local framebuffer = {}
    for address = 0, 0x5FFF do
        local value = emu.read(address, emu.memType.gsuWorkRam)
        framebuffer[address + 1] = value
        assert(value == emu.read(address, emu.memType.snesVideoRam),
            string.format("RAM/VRAM mismatch at $%04X", address))
    end
    local counted, bands = 0, {}
    for y = 0, 191 do
        for x = 0, 255 do
            local address = ((x >> 3) * 24 + (y >> 3)) * 32 + (y & 7) * 2
            local bit = 7 - (x & 7)
            local actual = 0
            for plane = 0, 3 do
                local offset = (plane >> 1) * 16 + (plane & 1)
                actual = actual | (((framebuffer[address + offset + 1] >> bit) & 1) << plane)
            end
            local expected = expected_pixel(x, y, height)
            assert(actual == expected, string.format("phase=%d pixel=(%d,%d): %d != %d",
                phase, x, y, actual, expected))
            if actual ~= 0 then counted = counted + 1; bands[actual] = true end
        end
    end
    assert(counted == pixels, "full-frame pixel count")
    for band = 1, 8 do assert(bands[band], "missing rainbow band " .. band) end
    for index, value in ipairs(palette) do
        assert(emu.read16((index - 1) * 2, emu.memType.snesCgRam) == value, "palette")
    end
    for row = 0, 31 do
        for column = 0, 31 do
            local tile = row < 24 and column * 24 + row or 768
            assert(emu.read16(0x7000 + (row * 32 + column) * 2, emu.memType.snesVideoRam) == tile,
                "tilemap")
        end
    end
    for address = 0x6000, 0x601F do
        assert(emu.read(address, emu.memType.snesVideoRam) == 0, "blank padding tile")
    end
    publications = publications + 1
end), emu.callbackType.write, 0x7e0000, 0x7e0000, emu.cpuType.snes, emu.memType.snesMemory)

emu.addEventCallback(guarded(function()
    frames = frames + 1
    assert(frames <= 1000, "animation timed out")
    assert(emu.read(0, emu.memType.snesWorkRam) < 2, "host failure")
    if publications < 4 then return end
    -- At one new pose per refresh, the asynchronous screenshot buffer cannot
    -- be matched to the latest mailbox. Identify its actual height, then check
    -- every visible pixel against that pose. Never slow the ROM for capture.
    local screen, size = emu.getScreenBuffer(), emu.getScreenSize()
    assert(size.width == 256 and size.height == 239 and #screen == 256 * 239, "screen dimensions")
    local backdrop = rgb(palette[1])
    local top
    for y = 72, 104 do
        if (screen[(y + 7) * 256 + 129] & 0xFFFFFF) ~= backdrop then top = y; break end
    end
    if not top then return end
    local height = 128 - top
    assert(height >= 24 and height <= 56, "visible height")
    for y = 0, 191 do
        for x = 0, 255 do
            local expected = rgb(palette[expected_pixel(x, y, height) + 1])
            local actual = screen[(y + 7) * 256 + x + 1] & 0xFFFFFF
            assert(actual == expected, string.format("visible h=%d pixel=(%d,%d): $%06X != $%06X",
                height, x, y, actual, expected))
        end
    end
    if not shown[height] then
        capture(screen, string.format("scaling-height-%02d.bmp", height))
        if height == 24 or height == 56 then
            capture(screen, height == 24 and "scaling-minimum.bmp" or "scaling-maximum.bmp")
        end
    end
    shown[height] = true
    if publications == 65 then
        for h = 24, 56 do assert(shown[h], "size was never presented: " .. h) end
        assert(io.open(folder .. "fps.json", "rb"), "missing completed-frame FPS evidence"):close()
        finish(0, string.format("PASS scaling triangle: 64 phases plus wrap, 49152 pixels per phase, eight colors, 33 visible sizes, VBlank DMA, CACHE and STOP; %d SNES frames", frames))
    end
end), emu.eventType.endFrame)
