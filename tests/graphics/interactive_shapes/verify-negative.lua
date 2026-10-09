local folder=emu.getRomInfo().path:match("^(.*[/\\])") or ""
local frames,red=0,0
emu.addEventCallback(function()
    frames=frames+1
    local status=emu.read(0,emu.memType.snesWorkRam)
    if status==0 and frames<1000 then return end
    if status==2 then red=red+1 end
    if status==2 and red<5 then return end
    local pass=status==2 and emu.read16(2,emu.memType.snesWorkRam)==42 and
        emu.read16(0,emu.memType.snesCgRam)==0x001F and
        (emu.getPixel(128,96)&0xFFFFFF)==0xFF0000
    local message=(pass and "PASS" or "FAIL").." interactive shapes negative: deliberate result mismatch gives a red screen"
    local file=assert(io.open(folder.."interactive-shapes-negative-verification.log","w"))
    file:write(message.."\n");file:close()
    emu.log(message);emu.stop(pass and 0 or 1)
end,emu.eventType.endFrame)
