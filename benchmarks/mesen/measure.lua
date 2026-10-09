-- Mesen's GSU cycle counter is in the master-clock domain at 100% speed.
-- Exec callbacks occur at opcode entry, not STOP completion. Preserve that
-- observed interval and report a guarded/calibrated final fetch separately.
local folder = emu.getRomInfo().path:match("^(.*[/\\])") or ""
local config = dofile(folder .. "timing-config.lua")
local prefix = "cart.coprocessor."
local first, stopEntry, stopFetch, stopAddress
local profiler
local count, frames, seeded, completed = 0, 0, false, false
local function field(state, name, kind)
    local value = state[prefix .. name]
    assert(type(value) == (kind or "number"), "Unsupported Mesen GSU state field: " .. name)
    return value
end
local function writeFile(name, data)
    local file = assert(io.open(folder .. name, "wb"))
    assert(file:write(data)); assert(file:close())
end
local function guarded(callback)
    return function(...)
        if completed then return end
        local ok, message = pcall(callback, ...)
        if not ok then
            completed = true
            writeFile("timing-error.log", tostring(message) .. "\n")
            emu.stop(1)
        end
    end
end
guarded(function()
    if config.profile_map then profiler = dofile(config.profile_script).new(config.profile_map) end
end)()
if completed then return end

emu.addMemoryCallback(guarded(function(_, value)
    if value == 0x11 then
        assert(not seeded, "Host attempted a second run")
        local state = emu.getState()
        assert(not field(state, "sfr.running", "boolean"), "GSU already running during input seeding")
        local file = assert(io.open(folder .. "seed.bin", "rb"))
        local data = assert(file:read(131073)); file:close()
        assert(#data == 131072, "Invalid input fixture size")
        for i = 1, #data do emu.write(i - 1, data:byte(i), emu.memType.gsuWorkRam) end
        -- Only this named state field is changed; verify API support explicitly.
        emu.setState({[prefix .. "ramBank"] = 1})
        assert(field(emu.getState(), "ramBank") == 1, "Cannot set initial RAMBR")
        seeded = true
    elseif value == 0x22 then
        assert(seeded and first and stopEntry and stopFetch, "Missing start/STOP timing evidence")
        local state = emu.getState()
        assert(not field(state, "sfr.running", "boolean"), "Host completed without executing STOP")
        assert(field(state, "programReadBuffer") == 1, "STOP did not reset the pipeline")
        -- The exec hook precedes the previous instruction's PC advance; STOP
        -- subsequently prefetches the next byte and advances R15 once more.
        assert(field(state, "r15") == ((stopAddress + 2) & 0xffff), "Unexpected PC after STOP")
        assert(count == config.instructions, "Mesen/model instruction counts differ")
        assert(field(state, "cycleCount") >= stopEntry + stopFetch, "STOP fetch calibration is inconsistent")
        local bytes = {}
        for i = 0, 131071 do bytes[i + 1] = string.char(emu.read(i, emu.memType.gsuWorkRam)) end
        writeFile("ram.bin", table.concat(bytes))
        bytes = {}
        for i = 0, 15 do
            local r = field(state, "r" .. i)
            bytes[#bytes + 1] = string.char(r & 255, (r >> 8) & 255)
        end
        bytes[#bytes + 1] = string.char(field(state, "ramBank"), field(state, "programBank"))
        writeFile("registers.bin", table.concat(bytes))
        local observed = stopEntry - first
        local clocks = observed + stopFetch
        assert(clocks > 0 and clocks <= 100000000, "Timing exceeds profile bounds")
        if profiler then
            writeFile("profile.json", profiler:finish(stopFetch, clocks))
            assert(profiler.instructions == count, "Profile/opcode count differs")
        end
        writeFile("timing.json", string.format(
            '{"master_clocks_to_stop_entry":%d,"stop_fetch_master_clocks":%d,"master_clocks":%d,"gsu_cycles":%d,"instructions_executed":%d}\n',
            observed, stopFetch, clocks, clocks, count))
        completed = true
        emu.stop(0)
    else
        error("Unexpected host status")
    end
end), emu.callbackType.write, 0x7e0000, 0x7e0000, emu.cpuType.snes, emu.memType.snesMemory)

emu.addMemoryCallback(guarded(function(address, opcode)
    count = count + 1
    assert(count <= math.min(config.instructions, 10000000), "Instruction limit exceeded")
    assert(address >= 0x8000 and address < 0x8000 + config.code_bytes, "Execution outside benchmark code")
    if profiler then profiler:observe(address, opcode, emu.getState()) end
    if not first then
        assert(seeded and address == 0x8000, "Wrong GSU entry point")
        local state = emu.getState()
        assert(field(state, "programBank") == 0 and field(state, "romBank") == 0, "Wrong program/ROM bank")
        assert(field(state, "ramBank") == 1 and field(state, "r10") == 0x1234, "Wrong initial RAMBR/stack")
        assert(field(state, "clockSelect", "boolean") and not field(state, "highSpeedMode", "boolean"), "Wrong CLSR/CFGR")
        assert(field(state, "gsuRamAccess", "boolean") and field(state, "gsuRomAccess", "boolean"), "Wrong bus ownership")
        assert(field(state, "screenBase") == 0x18 and field(state, "plotBpp") == 4 and field(state, "screenHeight") == 2, "Wrong bitmap profile")
        assert(field(state, "cacheBase") == 0 and field(state, "romDelay") == 0 and field(state, "ramDelay") == 0, "Warm buffer/cache state")
        assert(field(state, "primaryCache.validBits") == 0 and field(state, "secondaryCache.validBits") == 0, "Warm pixel caches")
        for i = 0, 31 do assert(not field(state, "cacheValid" .. i, "boolean"), "Warm instruction cache") end
        first = field(state, "cycleCount")
    end
    if opcode == 0 then
        assert(not stopEntry, "Multiple STOP instructions")
        local state = emu.getState()
        assert(field(state, "programReadBuffer") == 0 and field(state, "r15") == address, "Unsupported STOP in a jump delay slot")
        assert(field(state, "programBank") == 0 and field(state, "clockSelect", "boolean"), "STOP profile changed")
        assert(field(state, "gsuRomAccess", "boolean") and field(state, "romDelay") == 0 and field(state, "ramDelay") == 0, "Unsupported pending bus operation at STOP")
        local cacheOffset = ((address + 1) - field(state, "cacheBase")) & 0xffff
        if cacheOffset < 512 then
            local valid = field(state, "cacheValid" .. (cacheOffset >> 4), "boolean")
            stopFetch = valid and 1 or 81 -- 16 ROM bytes * 5 clocks + cached fetch.
        else
            stopFetch = 5 -- Uncached ROM fetch; STOP itself has no extra Step().
        end
        stopEntry, stopAddress = field(state, "cycleCount"), address
    end
end), emu.callbackType.exec, 0, 0xffffff, emu.cpuType.gsu, emu.memType.gsuMemory)

emu.addEventCallback(guarded(function()
    frames = frames + 1
    assert(frames < 600, "GSU/host did not reach STOP before the frame limit")
end), emu.eventType.endFrame)
