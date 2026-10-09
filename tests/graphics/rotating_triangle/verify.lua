-- Independent full-frame reference, not the compiler's clipped-span algorithm.
local frames, lastFrame, verified = 0, 0, 0
local visited = {}
local pending = {}
local quarter = { 0, 6, 12, 19, 24, 30, 36, 41, 45, 49, 53, 56, 59, 61, 63, 64, 64 }
local folder = emu.getRomInfo().path:match("^(.*[/\\])") or ""
local function sine(phase)
    phase = phase & 63
    local index = phase & 15
    if (phase & 16) ~= 0 then index = 16 - index end
    return quarter[index + 1] * ((phase & 32) ~= 0 and -1 or 1)
end
local function finish(code, message)
    emu.log(message)
    local file = io.open(folder .. "rotating-triangle-verification.log", "w")
    if file then file:write(message .. "\n"); file:close() end
    emu.stop(code)
end
local function fail(message) finish(1, "FAIL rotating triangle: " .. message) end

-- The host immediately resumes rendering after publication. Validate RAM,
-- VRAM and the mailbox synchronously before the GSU reuses that RAM; status=1
-- is deliberately not required to survive until an end-of-frame callback.
emu.addMemoryCallback(function(_, status)
    if status >= 2 then fail("host status=" .. status) return end
    if status ~= 1 or lastFrame >= 65 then return end
    local frame = emu.read16(0x12, emu.memType.snesWorkRam)
    local phase = emu.read16(0xC, emu.memType.snesWorkRam)
    if frame ~= lastFrame + 1 or phase ~= (frame - 1) % 64 then
        fail("phase/frame sequence mismatch") return
    end
    lastFrame = frame
    if emu.read16(4, emu.memType.snesWorkRam) ~= 0xFFFA or
       emu.read(6, emu.memType.snesWorkRam) ~= 0x70 or
       emu.read(7, emu.memType.snesWorkRam) ~= 0 or
       (emu.read(8, emu.memType.snesWorkRam) & 0x20) ~= 0 then
        fail("stack, program bank, RAM bank or STOP state") return
    end
    local dmaLine = emu.read16(0x1A, emu.memType.snesWorkRam)
    if dmaLine < 225 or dmaLine > 261 then fail("DMA did not finish within NTSC VBlank") return end
    local cacheBase = emu.read16(0x1C, emu.memType.snesWorkRam)
    if cacheBase < 0x6000 or cacheBase >= 0xF000 or (cacheBase & 15) ~= 0 then
        fail("CACHE did not leave an aligned base in the RAM payload") return
    end

    local framebuffer = {}
    for address = 0, 0x2FFF do
        local value = emu.read(address, emu.memType.gsuWorkRam)
        framebuffer[address + 1] = value
        if value ~= emu.read(address, emu.memType.snesVideoRam) then
            fail(string.format("RAM/VRAM mismatch at $%04X, phase=%d", address, phase)) return
        end
    end
    local s, c = sine(phase), sine(phase + 16)
    local black, white = 0, 0
    for y = 0, 191 do
        for x = 0, 255 do
            local u, v = (x - 128) * c + (y - 96) * s, -(x - 128) * s + (y - 96) * c
            local expected = 0
            if v <= 2048 and 2 * u <= v + 3072 and -2 * u <= v + 3072 then
                expected = (((u ~ v) & 512) == 0) and 1 or 2
                if expected == 1 then black = black + 1 else white = white + 1 end
            end
            local address = ((x >> 3) * 24 + (y >> 3)) * 16 + (y & 7) * 2
            local bit = 7 - (x & 7)
            local actual = ((framebuffer[address + 1] >> bit) & 1) |
                           (((framebuffer[address + 2] >> bit) & 1) << 1)
            if actual ~= expected then
                fail(string.format("phase=%d pixel=(%d,%d) expected=%d actual=%d", phase, x, y, expected, actual)) return
            end
        end
    end
    local pixels = black + white
    if emu.read16(2, emu.memType.snesWorkRam) ~= pixels or
       emu.read16(0xA, emu.memType.snesWorkRam) ~= pixels or
       emu.read16(0xE, emu.memType.snesWorkRam) ~= white or
       emu.read16(0x10, emu.memType.snesWorkRam) ~= black or
       emu.read16(0xF000, emu.memType.gsuWorkRam) ~= pixels or
       emu.read16(0xF008, emu.memType.gsuWorkRam) ~= phase then
        fail("independent pixel/color counts disagree with the CPU and GSU results") return
    end
    for row = 0, 31 do
        for column = 0, 31 do
            local expected = row < 24 and column * 24 + row or 768
            if emu.read16(0x7000 + (row * 32 + column) * 2, emu.memType.snesVideoRam) ~= expected then
                fail("invalid tilemap") return
            end
        end
    end
    for address = 0x3000, 0x300F do
        if emu.read(address, emu.memType.snesVideoRam) ~= 0 then fail("nonblank padding tile") return end
    end

    -- Mesen's endFrame hook runs before SendFrame updates its screenshot/pixel
    -- buffer. After this VBlank's DMA, allow a full scanout and that API's
    -- one-frame lag plus asynchronous decoding. The next UpdateFrame fences
    -- the previous decode; three refreshes prevent a stale screenshot buffer.
    -- The decoder can still expose the preceding pose, so the
    -- capture callback also checks the full visible reference before saving.
    -- Two publications may await capture at one pose/refresh;
    -- their RAM/VRAM checks above have already completed synchronously.
    pending[#pending + 1] = {phase=phase, frame=frame, visibleAfter=frames + 3}
    if #pending > 2 then fail("more than one publication per refresh") return end
end, emu.callbackType.write, 0x7e0000, 0x7e0000, emu.cpuType.snes, emu.memType.snesMemory)

emu.addEventCallback(function()
    frames = frames + 1
    if frames > 10000 then fail("animation timed out") return end
    local status = emu.read(0, emu.memType.snesWorkRam)
    if status >= 2 then fail("host status=" .. status) return end
    if not pending[1] or pending[1].visibleAfter > frames then return end
    local displayed = pending[1]
    local phase, frame = displayed.phase, displayed.frame
    -- Compare against a visible backdrop pixel, not the black overscan border:
    -- black is a valid foreground checker color, so (0,0) is not a reference.
    if emu.read16(0, emu.memType.snesCgRam) ~= 0x0842 or
       emu.read16(2, emu.memType.snesCgRam) ~= 0 or
       emu.read16(4, emu.memType.snesCgRam) ~= 0x7FFF or
       emu.getPixel(128, 96) == emu.getPixel(0, 96) then
        local file = io.open(folder .. "rotating-failure.png", "wb")
        if file then file:write(emu.takeScreenshot()); file:close() end
        fail(string.format("phase=%d frame=%d palette=$%04X/$%04X/$%04X center=$%08X backdrop=$%08X",
            phase, frame, emu.read16(0, emu.memType.snesCgRam), emu.read16(2, emu.memType.snesCgRam),
            emu.read16(4, emu.memType.snesCgRam), emu.getPixel(128, 96), emu.getPixel(0, 96))) return
    end
    -- Screenshot publication is asynchronous relative to endFrame. A fixed
    -- delay alone occasionally saved phase N-1 under phase N's filename even
    -- for byte-identical ROMs. Do not weaken the synchronous RAM/VRAM oracle:
    -- wait separately for all visible pixels to match this queued pose.
    local s, c = sine(phase), sine(phase + 16)
    local screen = emu.getScreenBuffer()
    local screenSize = emu.getScreenSize()
    -- This fixed NTSC host/API exposes seven top-border rows in the raw
    -- 239-line buffer; takeScreenshot crops them to its 224-line PNG.
    if screenSize.width ~= 256 or screenSize.height ~= 239 or #screen ~= 256 * 239 then
        fail("unsupported screen buffer dimensions for the NTSC pixel oracle") return
    end
    local backdrop = emu.getPixel(0, 96) & 0xFFFFFF
    local visible = true
    local mismatch
    for y = 0, 191 do
        if not visible then break end
        for x = 0, 255 do
            local u, v = (x - 128) * c + (y - 96) * s, -(x - 128) * s + (y - 96) * c
            local expected = backdrop
            if v <= 2048 and 2 * u <= v + 3072 and -2 * u <= v + 3072 then
                expected = (((u ~ v) & 512) == 0) and 0 or 0xFFFFFF
            end
            local actual = screen[(y + 7) * screenSize.width + x + 1] & 0xFFFFFF
            if actual ~= expected then
                visible = false
                mismatch = string.format("pixel=(%d,%d) expected=$%06X actual=$%06X", x, y, expected, actual)
                break
            end
        end
    end
    if not visible then
        if frames > displayed.visibleAfter + 3 then
            fail(string.format("phase=%d never presented its complete visible reference: %s", phase, mismatch))
        end
        return
    end
    table.remove(pending, 1)
    if not visited[phase] then
        local file = io.open(folder .. string.format("rotating-frame-%02d.png", phase), "wb")
        if not file then fail("cannot write screenshot") return end
        file:write(emu.takeScreenshot()); file:close()
        visited[phase] = true
        verified = verified + 1
    end
    if verified == 64 and phase == 0 and frame > 64 then
        finish(0, string.format("PASS rotating triangle: 64 poses plus phase wrap, 49152 pixels per pose, black/white checks, clean frames, VBlank DMA, CACHE and STOP; %d SNES frames", frames))
    end
end, emu.eventType.endFrame)
