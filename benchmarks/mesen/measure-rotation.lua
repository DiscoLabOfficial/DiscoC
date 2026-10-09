-- Boundary GSU timing plus the existing complete-frame/FPS verifier. Optional
-- profiling snapshots only machine-region transitions in the first pose.
-- Extra renders are allowed while the PPU screenshot for pose 64 settles.
local folder = emu.getRomInfo().path:match("^(.*[/\\])") or ""
local config = dofile(folder .. "timing-config.lua")
local prefix = "cart.coprocessor."
local samples, active, pending, failed = {}, nil, nil, false
local profiler
local profile_callback
local function field(state, name, kind)
    local value = state[prefix .. name]
    assert(type(value) == (kind or "number"), "Unsupported GSU field: " .. name)
    return value
end
local function save(name, text)
    local file = assert(io.open(folder .. name, "wb"))
    assert(file:write(text)); assert(file:close())
end
local function guarded(callback)
    return function(...)
        if failed then return end
        local ok, message = pcall(callback, ...)
        if not ok then failed = true; save("timing-error.log", tostring(message)); emu.stop(1) end
    end
end
guarded(function()
    local profile_file = io.open(folder .. "profile-config.lua", "rb")
    if profile_file then
        assert(profile_file:close())
        local options = dofile(folder .. "profile-config.lua")
        profiler = dofile(options.script).new(options.map, true)
    end
end)()
if failed then return end
emu.addMemoryCallback(guarded(function(address)
    if #samples >= 65 then return end
    assert(not active and not pending and address == config.origin, "Duplicate/wrong entry")
    local state = emu.getState()
    assert(field(state, "programBank") == 0x70 and field(state, "gsuRamAccess", "boolean"), "Wrong RAM profile")
    assert(field(state, "clockSelect", "boolean") and not field(state, "highSpeedMode", "boolean"), "Wrong clock profile")
    assert(field(state, "romDelay") == 0 and field(state, "ramDelay") == 0, "Pending bus operation on entry")
    for i = 0, 31 do assert(not field(state, "cacheValid" .. i, "boolean"), "Warm instruction cache") end
    local phase = emu.read16(0xF002, emu.memType.gsuWorkRam)
    assert(phase == #samples % 64, "Wrong pose sequence")
    active = {phase=phase, first=field(state, "cycleCount")}
end), emu.callbackType.exec, config.origin, config.origin, emu.cpuType.gsu, emu.memType.gsuMemory)
emu.addMemoryCallback(guarded(function(address, opcode)
    if #samples >= 65 then return end
    assert(active and not pending and opcode == 0 and address == config.stop_address, "Wrong normal STOP")
    local state = emu.getState()
    if profiler and #samples == 0 then profiler:observeBlock(address, opcode, function() return state end) end
    assert(field(state, "programReadBuffer") == 0 and field(state, "r15") == (address & 0xffff), "STOP in delay slot")
    assert(field(state, "romDelay") == 0 and field(state, "ramDelay") == 0, "Pending bus operation at STOP")
    assert(field(state, "ramBank") == 0 and field(state, "r6") == 0 and field(state, "r10") == 0xFFFA, "Fault/ABI state")
    local base = field(state, "cacheBase")
    local offset = ((address + 1) - base) & 0xffff
    local tail = offset < 512 and (field(state, "cacheValid" .. (offset >> 4), "boolean") and 1 or 81) or 5
    local stop = field(state, "cycleCount")
    assert(stop > active.first and stop-active.first < 100000000, "Invalid cycle window")
    pending = {phase=active.phase, observed=stop-active.first, tail=tail, stop=stop, base=base}
    active = nil
end), emu.callbackType.exec, config.stop_address, config.stop_address, emu.cpuType.gsu, emu.memType.gsuMemory)
emu.addMemoryCallback(guarded(function(_, status)
    if #samples >= 65 then return end
    if status >= 2 then error("Host failure: " .. status) end
    if status ~= 1 then return end
    assert(pending and not active, "Publication without STOP evidence")
    local state = emu.getState()
    assert(not field(state, "sfr.running", "boolean") and field(state, "programReadBuffer") == 1, "STOP incomplete")
    assert(field(state, "r15") == ((config.stop_address + 2) & 0xffff), "Wrong post-STOP PC")
    assert(field(state, "cycleCount") >= pending.stop + pending.tail, "STOP calibration inconsistent")
    assert(emu.read16(0x12, emu.memType.snesWorkRam) == #samples+1, "Wrong publication frame")
    if profiler and #samples == 0 then
        save("profile.json", profiler:finish(pending.tail, pending.observed+pending.tail))
        emu.removeMemoryCallback(profile_callback, emu.callbackType.exec, config.origin, config.origin+config.payload_bytes-1, emu.cpuType.gsu, emu.memType.gsuMemory)
    end
    samples[#samples+1] = pending; pending = nil
    local records = {}
    for _, s in ipairs(samples) do
        records[#records+1] = string.format('{"phase":%d,"master_clocks_to_stop_entry":%d,"stop_fetch_master_clocks":%d,"gsu_cycles":%d,"cache_base_at_stop":%d}',
            s.phase, s.observed, s.tail, s.observed+s.tail, s.base)
    end
    save("timing.json", string.format('{"optimization":"%s","profile":"NTSC CLSR=1 GSU=100%% CFGR.MSO=0 RAM-execution CACHE cold","payload_bytes":%d,"rom_bytes":%d,"payload_sha256":"%s","source_sha256":"%s","samples":[%s]}\n',
        config.level, config.payload_bytes, config.rom_bytes, config.payload_sha256, config.source_sha256, table.concat(records, ",")))
end), emu.callbackType.write, 0x7e0000, 0x7e0000, emu.cpuType.snes, emu.memType.snesMemory)
if profiler then
    profile_callback = emu.addMemoryCallback(guarded(function(address, opcode)
        if #samples == 0 and active and opcode ~= 0 then profiler:observeBlock(address, opcode, emu.getState) end
    end), emu.callbackType.exec, config.origin, config.origin+config.payload_bytes-1, emu.cpuType.gsu, emu.memType.gsuMemory)
end
dofile(config.fps_probe)
