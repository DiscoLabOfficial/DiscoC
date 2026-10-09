-- Synthetic timelines test the profiler independently of opcode timing/codegen.
local folder = emu.getRomInfo().path:match("^(.*[/\\])") or ""
local function write(name, data)
    local f = assert(io.open(folder..name,"wb")); assert(f:write(data)); assert(f:close())
end
local ok, failure = pcall(function()
    local config = dofile(folder.."profile-test-config.lua")
    local Profile = dofile(config.script)
    local map_path = folder.."synthetic-map.lua"
    local good = "return {origin=32768,finish=32772,regions={{start=32768,finish=32770,name='entry',owner='main'},{start=32770,finish=32772,name='tail',owner='helper'}}}"
    write("synthetic-map.lua", good)
    local function state(clocks, a, b)
        return {["cart.coprocessor.cycleCount"]=clocks,["cart.coprocessor.sfr.alt1"]=a or false,["cart.coprocessor.sfr.alt2"]=b or false}
    end
    local function rejects(callback, message)
        local succeeded = pcall(callback); assert(not succeeded, message)
    end
    local p = Profile.new(map_path)
    p:observe(32768,0x4c,state(100))       -- PLOT, changes cursor x in hardware.
    p:observe(32769,0x4c,state(103,true))  -- RPIX, same opcode under ALT1.
    p:observe(32770,0xdf,state(109))      -- GETC, not a RAM color load.
    p:observe(32771,0,state(116))
    local result = p:finish(1,17)
    assert(p.instructions == 4 and p.clocks == 17)
    assert(p.categories.plot.clocks == 3 and p.categories.rpix.clocks == 6 and p.categories.getc.clocks == 7 and p.categories.stop.clocks == 1)
    assert(p.functions.main.clocks == 9 and p.functions.helper.clocks == 8)
    write("opcode-profile.json",result)
    rejects(function() p:finish(1,17) end,"Finished profile was reusable")
    local block = Profile.new(map_path,true)
    local loads = 0
    local function loader(clock) return function() loads=loads+1; return state(clock) end end
    block:observeBlock(32768,1,loader(100))
    block:observeBlock(32769,1,loader(104))
    block:observeBlock(32770,1,loader(110))
    block:observeBlock(32771,0,loader(116))
    result = block:finish(1,17)
    assert(loads == 3 and block.instructions == 4 and block.blocks[1].clocks == 10 and block.blocks[2].clocks == 7)
    write("region-profile.json",result)
    local choices = {{0x4e,false,false,"color"},{0x4e,true,false,"cmode"},{0xdf,false,true,"bank_select"},
        {0x90,true,false,"ram_store"},{0xa0,true,false,"ram_load"},{0xf0,false,true,"ram_store"},
        {0xa0,false,false,"immediate"},{0xef,true,true,"getb"},{0x02,false,false,"cache"},
        {0x3c,false,false,"branch_jump"},{0x94,false,false,"link"},{0x9f,true,false,"multiply"},
        {0x3e,false,false,"prefix"},{0x25,false,false,"register_select_copy"}}
    for _, c in ipairs(choices) do
        local sample = Profile.new(map_path)
        sample:observe(32768,c[1],state(0,c[2],c[3])); sample:observe(32769,0,state(5)); sample:finish(5,10)
        assert(sample.categories[c[4]].instructions == 1,"ALT/category selection changed")
    end
    local bad = Profile.new(map_path)
    rejects(function() bad:observe(32767,1,state(0)) end,"Out-of-CODE execution accepted")
    rejects(function() bad:finish(1,1) end,"Missing STOP accepted")
    bad = Profile.new(map_path); bad:observe(32768,1,state(2))
    rejects(function() bad:observe(32769,0,state(1)) end,"Clock counter went backwards")
    bad = Profile.new(map_path); bad:observe(32768,0,state(0))
    rejects(function() bad:observe(32769,1,state(5)) end,"Post-STOP host time counted")
    rejects(function() bad:finish(5,6) end,"Boundary/profile mismatch accepted")
    rejects(function() Profile.new(map_path,true):observe(32768,1,state(0)) end,"Wrong granularity accepted")
    write("synthetic-map.lua",good:gsub("start=32770","start=32771"))
    rejects(function() Profile.new(map_path) end,"Map gap accepted")
    write("synthetic-map.lua","return {origin=65535,finish=65537,regions={{start=65535,finish=65537,name='x',owner='main'}}}")
    rejects(function() Profile.new(map_path) end,"Cross-bank map accepted")
    write("profile-test-pass.log","PASS GSU opcode/region intervals, ALT effects, STOP, boundaries and malformed maps\n")
end)
if not ok then write("timing-error.log",tostring(failure).."\n"); emu.stop(1) else emu.stop(0) end
