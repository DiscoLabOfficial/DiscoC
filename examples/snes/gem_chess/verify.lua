-- Real port-one input, actual GSU STOPs, complete VRAM and visible-image checks.
-- This is a presentation/ABI/control test, not a second polygon renderer.
local folder=emu.getRomInfo().path:match("^(.*[/\\])") or ""
local config=dofile(folder.."scene-test-config.lua")
local geometry=dofile(folder.."geometry-checks.lua")()
local palette={0x0C41,0x2CE5,0x5692,0x1616,0x20A0,0x4580,0x6282,0x7B88,
    0x7FF8,0x1027,0x206F,0x34B8,0x499F,0x66FF,0x7FFF,0x0841}
local goals={
    {yaw=8,pitch=7,zoom=12,mode=0},
    {yaw=16,pitch=10,zoom=16,mode=0},
    {yaw=0,pitch=5,zoom=6,mode=0},
    {yaw=32,pitch=7,zoom=12,mode=0},
    {yaw=56,pitch=10,zoom=16,mode=0},
    {yaw=61,pitch=7,zoom=16,mode=0},
    {yaw=2,pitch=10,zoom=16,mode=0},
    {yaw=11,pitch=10,zoom=13,mode=0},
    {yaw=8,pitch=7,zoom=12,mode=1},
    {yaw=8,pitch=7,zoom=12,mode=0,reset=true}
}
local expected={yaw=8,pitch=7,zoom=12,mode=0,previous=0}
local stage,frames,polls,publications=1,0,0,0
local failed,latest=false,nil
local captureAfter=0
local active,pending=nil,nil
local timing={"publication,yaw,elevation,zoom,wire,gsu_cycles,gsu_seconds,host_publication_clock"}
local function save(name,bytes)
    local file=assert(io.open(folder..name,"wb"))
    assert(file:write(bytes));assert(file:close())
end
local function finish(code,message)
    save("gem-chess-verification.log",message.."\n")
    emu.log(message);emu.stop(code)
end
local function guard(fn)
    return function(...)
        if failed then return end
        local ok,why=pcall(fn,...)
        if not ok then failed=true;finish(1,"FAIL gem chess: "..tostring(why)) end
    end
end
local function same(a,b)
    return a.yaw==b.yaw and a.pitch==b.pitch and a.zoom==b.zoom and a.mode==b.mode
end
emu.addEventCallback(guard(function()
    local goal=goals[stage]
    local input={a=false,start=false,left=false,right=false,up=false,down=false,select=false}
    if goal then
        if goal.reset then input.start=true
        else
            local delta=(goal.yaw-expected.yaw)&63
            if delta~=0 then if delta<=32 then input.right=true else input.left=true end end
            if expected.pitch~=goal.pitch then
                if expected.pitch<goal.pitch then input.up=true else input.down=true end
            elseif expected.zoom~=goal.zoom then
                input.select=true
                if expected.zoom<goal.zoom then input.up=true else input.down=true end
            end
            if expected.mode~=goal.mode and (expected.previous&0x80)==0 then input.a=true end
        end
    end
    emu.setInput(input,0,0)
end),emu.eventType.inputPolled)

emu.addMemoryCallback(guard(function()
    if (emu.read16(0x20,emu.memType.snesWorkRam)&1)==0 then return end
    local held=emu.read16(0x22,emu.memType.snesWorkRam)
    local e=expected
    if (held&0x1000)~=0 then e.yaw=8;e.pitch=7;e.zoom=12;e.mode=0
    else
        if ((held~e.previous)&held&0x80)~=0 then e.mode=e.mode~1 end
        local horizontal=held&0x300
        if horizontal==0x100 then e.yaw=(e.yaw+1)&63 end
        if horizontal==0x200 then e.yaw=(e.yaw-1)&63 end
        local vertical=held&0xC00
        if (held&0x2000)~=0 then
            if vertical==0x800 then e.zoom=math.min(16,e.zoom+1) end
            if vertical==0x400 then e.zoom=math.max(6,e.zoom-1) end
        else
            if vertical==0x800 then e.pitch=math.min(10,e.pitch+1) end
            if vertical==0x400 then e.pitch=math.max(5,e.pitch-1) end
        end
    end
    e.previous=held;polls=polls+1
    for i,key in ipairs({"yaw","pitch","zoom","mode"}) do
        assert(emu.read16(0x24+(i-1)*2,emu.memType.snesWorkRam)==e[key],"input transition "..key)
    end
end),emu.callbackType.write,0x2E,0x2E,emu.cpuType.snes,emu.memType.snesMemory)

local function rgb(value)
    local function channel(v)return (v<<3)|(v>>2) end
    return (channel(value&31)<<16)|(channel((value>>5)&31)<<8)|channel((value>>10)&31)
end
local function capture(screen,name)
    local rows={"BM"..string.pack("<I4I2I2I4I4i4i4I2I2I4I4i4i4I4I4",54+256*224*3,0,0,54,40,256,224,1,24,0,256*224*3,0,0,0,0)}
    for y=223,0,-1 do
        local row={}
        for x=0,255 do
            local value=screen[(y+7)*256+x+1]
            row[#row+1]=string.char(value&255,(value>>8)&255,(value>>16)&255)
        end
        rows[#rows+1]=table.concat(row)
    end
    save(name,table.concat(rows))
end
emu.addMemoryCallback(guard(function()
    assert(not active and not pending,"reentered payload")
    active=assert(emu.getState()["cart.coprocessor.cycleCount"],"missing GSU clock")
end),emu.callbackType.exec,0x706000,0x706000,emu.cpuType.gsu,emu.memType.gsuMemory)
emu.addMemoryCallback(guard(function(address,opcode)
    local state=emu.getState()
    assert(active and opcode==0 and state["cart.coprocessor.r6"]==0,"unexpected STOP")
    local offset=((address+1)-state["cart.coprocessor.cacheBase"])&0xFFFF
    local tail=5
    if offset<512 then tail=state["cart.coprocessor.cacheValid"..(offset>>4)] and 1 or 81 end
    pending=state["cart.coprocessor.cycleCount"]-active+tail;active=nil
end),emu.callbackType.exec,config.stop,config.stop,emu.cpuType.gsu,emu.memType.gsuMemory)
emu.addMemoryCallback(guard(function(_,status)
    assert(status<2,"host failure status="..status)
    if status~=1 then return end
    local machine=emu.getState()
    assert(machine["cart.coprocessor.clockSelect"]==true and machine["cart.coprocessor.highSpeedMode"]==false,"GSU clock profile")
    assert(emu.read16(4,emu.memType.snesWorkRam)==0xEFFA and emu.read(6,emu.memType.snesWorkRam)==0x70 and
        emu.read(7,emu.memType.snesWorkRam)==0 and (emu.read(8,emu.memType.snesWorkRam)&0x20)==0,"STOP/ABI/banks")
    local count=emu.read16(2,emu.memType.snesWorkRam)
    assert(count>64 and count==emu.read16(0xF000,emu.memType.gsuWorkRam) and
        emu.read16(0xF018,emu.memType.gsuWorkRam)==0,"primitive result/projection bounds")
    assert(emu.read16(0x32,emu.memType.snesWorkRam)==6,"incomplete DMA chunks")
    local line=emu.read16(0x1A,emu.memType.snesWorkRam)
    assert(line>=225 and line<=261,"DMA outside NTSC VBlank: "..line)
    assert(emu.read16(0x1C,emu.memType.snesWorkRam)>=0x6000,"CACHE outside code")
    local buffer=emu.read16(0x30,emu.memType.snesWorkRam)
    assert(buffer==0 or buffer==1,"invalid VRAM buffer")
    local ram={}
    for address=0,0x5FFF do
        local value=emu.read(address,emu.memType.gsuWorkRam);ram[address+1]=value
        assert(emu.read(buffer*0x6000+address,emu.memType.snesVideoRam)==value,string.format("VRAM byte $%04X",address))
    end
    for address=1,32 do assert(ram[address]==0,"shared blank tile was drawn") end
    for y=0,31 do for x=0,31 do
        assert(emu.read16(0xE000+(y*32+x)*2,emu.memType.snesVideoRam)==(y<24 and x*24+y or 0),"tilemap")
    end end
    for i,value in ipairs(palette) do assert(emu.read16((i-1)*2,emu.memType.snesCgRam)==value,"palette") end
    local image,histogram={},{}
    for i=0,15 do histogram[i]=0 end
    for y=0,191 do for x=0,255 do
        local address=((x>>3)*24+(y>>3))*32+(y&7)*2
        local bit=7-(x&7)
        local ink=0
        for plane=0,3 do ink=ink|(((ram[address+(plane>>1)*16+(plane&1)+1]>>bit)&1)<<plane) end
        image[y*256+x+1]=ink;histogram[ink]=histogram[ink]+1
    end end
    local cyan,ruby=0,0
    for i=4,8 do cyan=cyan+histogram[i] end
    for i=9,13 do ruby=ruby+histogram[i] end
    assert(cyan>30 and ruby>30 and histogram[1]>1000 and histogram[2]>1000,"missing board or army")
    latest={yaw=emu.read16(0xC,emu.memType.snesWorkRam),pitch=emu.read16(0xE,emu.memType.snesWorkRam),
        zoom=emu.read16(0x10,emu.memType.snesWorkRam),mode=emu.read16(0x12,emu.memType.snesWorkRam),image=image,count=count}
    publications=publications+1;captureAfter=frames+3
    assert(pending,"missing GSU timing sample")
    timing[#timing+1]=string.format("%d,%d,%d,%d,%d,%d,%.9f,%d",publications,latest.yaw,latest.pitch,latest.zoom,
        latest.mode,pending,pending/21477270,machine["memoryManager.masterClock"])
    save("gem-chess-timings.csv",table.concat(timing,"\n").."\n");pending=nil
end),emu.callbackType.write,0x7E0000,0x7E0000,emu.cpuType.snes,emu.memType.snesMemory)

emu.addEventCallback(guard(function()
    frames=frames+1
    assert(frames<7200,"scene/controller timeout")
    local goal=goals[stage]
    if not goal or not latest or frames<captureAfter or not same(latest,goal) then return end
    local screen,size=emu.getScreenBuffer(),emu.getScreenSize()
    assert(size.width==256 and size.height==239,"screen size")
    for y=0,223 do for x=0,255 do
        local ink=y<192 and latest.image[y*256+x+1] or 0
        assert((screen[(y+7)*256+x+1]&0xFFFFFF)==rgb(palette[ink+1]),string.format("displayed pixel %d,%d",x,y))
    end end
    capture(screen,string.format("gem-chess-view-%02d.bmp",stage))
    if stage==1 then save("gem-chess-preview.png",emu.takeScreenshot()) end
    emu.log(string.format("checked view %d: yaw=%d elevation=%d zoom=%d wire=%d faces=%d",stage,latest.yaw,latest.pitch,latest.zoom,latest.mode,latest.count))
    stage=stage+1
    if stage>#goals then finish(0,string.format("PASS gem chess: %d camera views, %d publications, %d real controller polls; full framebuffer/VRAM/PPU, both buffers, STOP/ABI, wireframe, reset, zoom/elevation and VBlank DMA; %s",#goals,publications,polls,geometry)) end
end),emu.eventType.endFrame)
