-- First-opcode to normal STOP, excluding controller/CPU/DMA idle time.
-- Uses the same fixed pose in wire and filled modes on both compared builds.
local folder=emu.getRomInfo().path:match("^(.*[/\\])") or ""
local config=dofile(folder.."benchmark-config.lua")
local samples={wire={},filled={}}
local active,pending,failed=nil,nil,false
local function save(name,value)
    local file=assert(io.open(folder..name,"wb"));file:write(value);file:close()
end
local function guard(fn)
    return function(...)
        if failed then return end
        local ok,why=pcall(fn,...)
        if not ok then failed=true;save("benchmark-error.log",tostring(why));emu.stop(1) end
    end
end
local function state() return emu.getState() end
local function value(s,key) return assert(s["cart.coprocessor."..key],key) end
emu.addEventCallback(guard(function()
    emu.setInput({a=#samples.wire>=4},0,0)
end),emu.eventType.inputPolled)
emu.addMemoryCallback(guard(function()
    assert(not active and not pending,"reentered payload")
    local s=state()
    assert(value(s,"clockSelect")==true and s["cart.coprocessor.highSpeedMode"]==false,"wrong clock")
    for i=0,31 do assert(s["cart.coprocessor.cacheValid"..i]==false,"warm instruction CACHE on entry") end
    assert(emu.read16(0xF002,emu.memType.gsuWorkRam)==8 and
        emu.read16(0xF004,emu.memType.gsuWorkRam)==5 and
        emu.read16(0xF006,emu.memType.gsuWorkRam)==12,"pose changed")
    active={first=value(s,"cycleCount"),mode=emu.read16(0xF008,emu.memType.gsuWorkRam)}
end),emu.callbackType.exec,0x708000,0x708000,emu.cpuType.gsu,emu.memType.gsuMemory)
emu.addMemoryCallback(guard(function(address,opcode)
    assert(active and opcode==0,"unexpected STOP")
    local s=state()
    assert(value(s,"r6")==0 and value(s,"r10")==0xEFFA,"runtime fault")
    local offset=((address+1)-value(s,"cacheBase"))&0xFFFF
    local tail=5
    if offset<512 then tail=s["cart.coprocessor.cacheValid"..(offset>>4)] and 1 or 81 end
    pending={cycles=value(s,"cycleCount")-active.first+tail,mode=active.mode}
    active=nil
end),emu.callbackType.exec,config.stop,config.stop,emu.cpuType.gsu,emu.memType.gsuMemory)
emu.addMemoryCallback(guard(function(_,status)
    assert(status<2,"host failure")
    if status~=1 then return end
    assert(pending,"no STOP sample")
    local key=pending.mode==0 and "wire" or "filled"
    if #samples[key]<4 then samples[key][#samples[key]+1]=pending.cycles end
    pending=nil
    if #samples.wire==4 and #samples.filled==4 then
        local function encode(values) local out={} for _,v in ipairs(values) do out[#out+1]=tostring(v) end return table.concat(out,",") end
        save("benchmark.json",string.format('{"profile":"NTSC CLSR=1 GSU=100%% normal multiply RAM CACHE cold","pose":{"yaw":8,"pitch":5,"size":12},"payload_bytes":%d,"wire_cycles":[%s],"filled_cycles":[%s]}\n',config.bytes,encode(samples.wire),encode(samples.filled)))
        emu.stop(0)
    end
end),emu.callbackType.write,0x7E0000,0x7E0000,emu.cpuType.snes,emu.memType.snesMemory)
