-- Independent analytic edge oracle plus actual controller injection. No host
-- variables are modified to simulate controls: only port 1 buttons are used.
local folder = emu.getRomInfo().path:match("^(.*[/\\])") or ""
local frames, publications, polls, stage, stagePolls = 0, 0, 0, 1, 0
local failed, latest = false, {}
local captureCount,lastCapture=0,nil
local quarter = {0,6,12,19,24,30,36,41,45,49,53,56,59,61,63,64,64}
local palette = {0,0x001F,0x7C00,0x03E0,0x03FF,0x7C1F,0x7FE0,0x02FF,0x7FFF,0x7C1F}
local stages = {
    {n=4}, {n=5,right=true}, {n=5,left=true},
    {n=5,down=true}, {n=5,up=true},
    {n=10,a=true}, {n=4}, {n=10,a=true}, {n=4},
    {n=12,select=true,up=true}, {n=18,select=true,down=true},
    {n=6,select=true,up=true},
    {n=5,left=true,right=true,up=true,down=true},
    {n=4,right=true,down=true}, {n=1,a=true}, {n=30},
    {n=18,select=true,down=true}, {n=30},
    {n=18,select=true,up=true}, {n=30},
    {n=1,a=true}, {n=30}
}
local expectedInput = {yaw=8,pitch=5,size=12,mode=0,previous=0}
local seen = {wire=false,filled=false,min=false,max=false,screen=false}
local function save(name, contents)
    local file = assert(io.open(folder .. name, "wb"))
    assert(file:write(contents)); assert(file:close())
end
local function finish(code, message)
    save("interactive-shapes-verification.log", message .. "\n")
    emu.log(message); emu.stop(code)
end
local function guard(fn)
    return function(...)
        if failed then return end
        local ok, why = pcall(fn, ...)
        if not ok then failed=true; finish(1,"FAIL interactive shapes: "..tostring(why)) end
    end
end
local function sin(phase)
    phase=phase&63
    local i=phase&15
    if (phase&16)~=0 then i=16-i end
    local v=quarter[i+1]
    return (phase&32)~=0 and -v or v
end
local function q(value)
    return value<0 and -math.floor((-value+32)/64) or math.floor((value+32)/64)
end
local function mask(input)
    return (input.a and 0x80 or 0)|(input.select and 0x2000 or 0)|
        (input.up and 0x800 or 0)|(input.down and 0x400 or 0)|
        (input.left and 0x200 or 0)|(input.right and 0x100 or 0)
end
emu.addEventCallback(guard(function()
    local s=stages[stage] or {}
    emu.setInput({a=s.a or false,select=s.select or false,up=s.up or false,
        down=s.down or false,left=s.left or false,right=s.right or false},0,0)
end),emu.eventType.inputPolled)

emu.addMemoryCallback(guard(function()
    if (emu.read16(0x20,emu.memType.snesWorkRam)&1)==0 then return end -- reset clear, not a poll
    local held=emu.read16(0x22,emu.memType.snesWorkRam)
    assert(held==mask(stages[stage] or {}),string.format("controller stage%d: $%04X",stage,held))
    local e=expectedInput
    if ((held~e.previous)&held&0x80)~=0 then e.mode=e.mode~1 end
    local horizontal=held&0x300
    if horizontal==0x100 then e.yaw=(e.yaw+1)&63 end
    if horizontal==0x200 then e.yaw=(e.yaw-1)&63 end
    local vertical=held&0xC00
    if (held&0x2000)~=0 then
        if vertical==0x800 then e.size=math.min(16,e.size+1) end
        if vertical==0x400 then e.size=math.max(6,e.size-1) end
    else
        if vertical==0x800 then e.pitch=(e.pitch-1)&63 end
        if vertical==0x400 then e.pitch=(e.pitch+1)&63 end
    end
    e.previous=held
    for i,field in ipairs({"yaw","pitch","size","mode"}) do
        local actual=emu.read16(0x24+(i-1)*2,emu.memType.snesWorkRam)
        assert(actual==e[field],string.format("input transition %s: %d != %d",field,actual,e[field]))
    end
    polls=polls+1
    stagePolls=stagePolls+1
    if stage<=#stages and stagePolls==stages[stage].n then stage=stage+1;stagePolls=0 end
end),emu.callbackType.write,0x2E,0x2E,emu.cpuType.snes,emu.memType.snesMemory)

-- Unlike the production error accumulator, this evaluates each boundary point
-- directly with a rational major-axis displacement and explicit midpoint rule.
local function edge_points(a,b,visit)
    local dx,dy=math.abs(b.x-a.x),math.abs(b.y-a.y)
    local horizontal=dx>=dy
    if (horizontal and a.x>b.x) or (not horizontal and a.y>b.y) then a,b=b,a end
    local major,minor=horizontal and dx or dy,horizontal and dy or dx
    local direction=(horizontal and b.y<a.y or not horizontal and b.x<a.x) and -1 or 1
    for k=0,major do
        local d=major==0 and 0 or math.floor((k*minor+math.floor(major/2))/major)
        visit(a.x+(horizontal and k or direction*d),a.y+(horizontal and direction*d or k))
    end
end
local faces={{0,2,3,1},{4,5,7,6},{0,4,6,2},{1,3,7,5},{0,1,5,4},{2,6,7,3}}
local sphereFaces={{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
    {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
    {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
    {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}}
local function oracle(state)
    local image={}
    for i=1,65536 do image[i]=0 end
    local function point(x,y,ink,origin)
        assert(x>=0 and x<64 and y>=0 and y<64,"geometry escaped panel")
        image[(y+64)*256+origin+x+1]=ink
    end
    local function contour(vertices,origin,ink,filled)
        local left,right={},{}
        for i=1,#vertices do
            edge_points(vertices[i],vertices[i%#vertices+1],function(x,y)
                if filled then
                    left[y]=math.min(left[y] or 64,x);right[y]=math.max(right[y] or -1,x)
                else point(x,y,ink,origin) end
            end)
        end
        if filled then
            for y=0,63 do
                for x=left[y] or 64,right[y] or -1 do point(x,y,ink,origin) end
            end
        end
    end
    local sy,cy,sx,cx=sin(state.yaw),sin(state.yaw+16),sin(state.pitch),sin(state.pitch+16)
    local function project(x,y,z)
        local xx=q(x*cy+z*sy);local zz=q(z*cy-x*sy)
        return {x=32+xx,y=32-q(y*cx-zz*sx)}
    end
    local cube={}
    for i=0,7 do
        cube[i+1]=project((i&1)~=0 and state.size or -state.size,
            (i&2)~=0 and state.size or -state.size,(i&4)~=0 and state.size or -state.size)
    end
    local primitives=0
    if state.mode==0 then
        for i=0,7 do
            for _,axis in ipairs({1,2,4}) do
                if (i&axis)==0 then
                    edge_points(cube[i+1],cube[(i|axis)+1],function(x,y) point(x,y,8,32) end)
                    primitives=primitives+1
                end
            end
        end
    else
        for ink,face in ipairs(faces) do
            local vertices={cube[face[1]+1],cube[face[2]+1],cube[face[3]+1],cube[face[4]+1]}
            local a,b,c=vertices[1],vertices[2],vertices[3]
            local cross=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)
            if cross<0 then contour(vertices,32,ink,true);primitives=primitives+1 end
        end
    end
    local radius=state.size*2-2
    local a,b=q(radius*34),q(radius*55)
    local sphere={project(-a,b,0),project(a,b,0),project(-a,-b,0),project(a,-b,0),
        project(0,-a,b),project(0,a,b),project(0,-a,-b),project(0,a,-b),
        project(b,0,-a),project(b,0,a),project(-b,0,-a),project(-b,0,a)}
    local unique={}
    for i,face in ipairs(sphereFaces) do
        if state.mode==0 then
            for vertex=1,3 do
                local from,to=face[vertex],face[vertex%3+1]
                local lo,hi=math.min(from,to),math.max(from,to)
                local key=lo*12+hi
                if not unique[key] then
                    unique[key]=true
                    edge_points(sphere[lo+1],sphere[hi+1],function(x,y) point(x,y,9,160) end)
                    primitives=primitives+1
                end
            end
        else
            local vertices={sphere[face[1]+1],sphere[face[2]+1],sphere[face[3]+1]}
            local p,r,s=vertices[1],vertices[2],vertices[3]
            if (r.x-p.x)*(s.y-p.y)-(r.y-p.y)*(s.x-p.x)<0 then
                contour(vertices,160,1+(i-1)%6,true);primitives=primitives+1
            end
        end
    end
    local planar={}
    for i=1,32768 do planar[i]=0 end
    for y=0,255 do
        for x=0,255 do
            local color=image[y*256+x+1]
            if color~=0 then
                local tile=(y//128)*512+(x//128)*256+((y%128)//8)*16+(x%128)//8
                local base=tile*32+(y%8)*2
                for plane=0,3 do
                    local index=base+(plane//2)*16+(plane%2)+1
                    planar[index]=planar[index]|(((color>>plane)&1)<<(7-x%8))
                end
            end
        end
    end
    return image,planar,primitives
end
local function rgb(value)
    local function c(v) return (v<<3)|(v>>2) end
    return (c(value&31)<<16)|(c((value>>5)&31)<<8)|c((value>>10)&31)
end
local function capture(screen,name)
    local bytes=256*224*3
    local rows={"BM"..string.pack("<I4I2I2I4I4i4i4I2I2I4I4i4i4I4I4",54+bytes,0,0,54,40,256,224,1,24,0,bytes,0,0,0,0)}
    for y=223,0,-1 do
        local pixels={}
        for x=0,255 do
            local v=screen[(y+7)*256+x+1]
            pixels[#pixels+1]=string.char(v&255,(v>>8)&255,(v>>16)&255)
        end
        rows[#rows+1]=table.concat(pixels)
    end
    save(name,table.concat(rows))
end
emu.addMemoryCallback(guard(function(_,status)
    assert(status<2,"host failure status="..status)
    if status~=1 then return end
    local machine=emu.getState()
    assert(machine["cart.coprocessor.clockSelect"]==true and
        machine["cart.coprocessor.highSpeedMode"]==false,"21 MHz/normal-multiply profile")
    local state={yaw=emu.read16(0xC,emu.memType.snesWorkRam),pitch=emu.read16(0xE,emu.memType.snesWorkRam),
        size=emu.read16(0x10,emu.memType.snesWorkRam),mode=emu.read16(0x12,emu.memType.snesWorkRam)}
    local image,planar,primitives=oracle(state)
    assert(emu.read16(2,emu.memType.snesWorkRam)==primitives and
        emu.read16(0xF000,emu.memType.gsuWorkRam)==primitives,"primitive count")
    assert(emu.read16(4,emu.memType.snesWorkRam)==0xEFFA and
        emu.read(6,emu.memType.snesWorkRam)==0x70 and emu.read(7,emu.memType.snesWorkRam)==0 and
        (emu.read(8,emu.memType.snesWorkRam)&0x20)==0,"ABI/STOP/banks")
    local line=emu.read16(0x1A,emu.memType.snesWorkRam)
    if line<225 or line>261 then
        local parts={"line="..line,"frames="..frames,"polls="..polls}
        for k,v in pairs(emu.getState()) do
            if k:match("ppu.*[Cc]ycle") or k:match("ppu.*[Ss]canline") or k:match("cpu.*[Pp]rogramCounter") then
                parts[#parts+1]=k.."="..tostring(v)
            end
        end
        error("DMA outside VBlank: "..table.concat(parts,", "))
    end
    assert(emu.read16(0x1C,emu.memType.snesWorkRam)>=0x8000,"CACHE outside payload")
    for address=0,32767 do
        local value=planar[address+1]
        assert(emu.read(address,emu.memType.gsuWorkRam)==value,string.format("framebuffer $%04X state=%d/%d/%d/%d",address,state.yaw,state.pitch,state.size,state.mode))
        assert(emu.read(address,emu.memType.snesVideoRam)==value,string.format("VRAM $%04X",address))
    end
    assert(emu.read(0,emu.memType.snesSpriteRam)==32 and
        emu.read(4,emu.memType.snesSpriteRam)==160 and emu.read(512,emu.memType.snesSpriteRam)==0x5A,"OAM")
    for i,value in ipairs(palette) do
        assert(emu.read16(256+(i-1)*2,emu.memType.snesCgRam)==value,"OBJ palette")
    end
    publications=publications+1
    state.image=image
    latest[#latest+1]=state
    if #latest>5 then table.remove(latest,1) end
    seen.wire=seen.wire or state.mode==0;seen.filled=seen.filled or state.mode==1
    seen.min=seen.min or state.size==6;seen.max=seen.max or state.size==16
end),emu.callbackType.write,0x7E0000,0x7E0000,emu.cpuType.snes,emu.memType.snesMemory)

emu.addEventCallback(guard(function()
    frames=frames+1
    assert(frames<=1800,"controller/demo timeout")
    if publications<2 then return end
    local screen,size=emu.getScreenBuffer(),emu.getScreenSize()
    assert(size.width==256 and size.height==239,"screen dimensions")
    local match
    for i=#latest,1,-1 do
        local candidate=latest[i];local good=true
        for y=0,223 do
            for x=0,255 do
                local ink=candidate.image[y*256+x+1]
                local expected=rgb(ink==0 and 0x0842 or palette[ink+1])
                if (screen[(y+7)*256+x+1]&0xFFFFFF)~=expected then good=false;break end
            end
            if not good then break end
        end
        if good then match=candidate;break end
    end
    assert(match,"visible PPU OBJ image does not match any checked completed pose")
    seen.screen=true
    local pose=string.format("%d/%d/%d/%d",match.yaw,match.pitch,match.size,match.mode)
    if pose~=lastCapture then
        captureCount=captureCount+1
        assert(captureCount<=512,"capture limit")
        capture(screen,string.format("interactive-frame-%03d.bmp",captureCount))
        lastCapture=pose
    end
    local key=string.format("%d-%d",match.mode,match.size)
    if not seen[key] then
        capture(screen,"interactive-"..(match.mode==0 and "wire" or "filled").."-"..match.size..".bmp")
        seen[key]=true
    end
    if stage>#stages and seen.wire and seen.filled and seen.min and seen.max and seen.screen then
        finish(0,string.format("PASS interactive shapes: %d full OBJ framebuffers, %d controller polls, A edge/hold/repress, both rotation axes, Select resize/clamps, opposing directions, PPU sprites and VBlank DMA",publications,polls))
    end
end),emu.eventType.endFrame)
