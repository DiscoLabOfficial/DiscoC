-- Opcode-entry intervals, not independent ISA latencies. The prefetched byte,
-- waits and cache fill between hooks are charged to the preceding opcode.
-- STOP uses the boundary timer's calibrated fetch; host idle time is excluded.
local Profile = {}
Profile.__index = Profile
local prefix = "cart.coprocessor."
local function number(state, name)
    local value = state[prefix .. name]
    assert(type(value) == "number", "Missing GSU profile field: " .. name)
    return value
end
local function flag(state, name)
    local value = state[prefix .. "sfr." .. name]
    assert(type(value) == "boolean", "Missing GSU profile flag: " .. name)
    return value
end
local function category(op, state)
    if op == 0 then return "stop" end
    if op >= 0x3d and op <= 0x3f then return "prefix" end
    if op >= 0x40 and op <= 0x4b then return "ram_load" end
    if (op >= 0x30 and op <= 0x3b) or op == 0x90 then return "ram_store" end
    if op == 0x4c then return flag(state,"alt1") and "rpix" or "plot" end
    if op == 0x4e then return flag(state,"alt1") and "cmode" or "color" end
    if op == 0xdf then return flag(state,"alt2") and "bank_select" or "getc" end
    if op == 0xef then return "getb" end
    if op == 0x02 then return "cache" end
    if (op >= 5 and op <= 15) or op == 0x3c or (op >= 0x98 and op <= 0x9d) then return "branch_jump" end
    if op >= 0x91 and op <= 0x94 then return "link" end
    if (op >= 0x80 and op <= 0x8f) or op == 0x9f then return "multiply" end
    if (op >= 0xa0 and op <= 0xaf) or op >= 0xf0 then
        if flag(state,"alt1") then return "ram_load" end
        if flag(state,"alt2") then return "ram_store" end
        return "immediate"
    end
    if (op >= 0x10 and op <= 0x2f) or (op >= 0xb0 and op <= 0xbf) then return "register_select_copy" end
    if op == 1 then return "nop" end
    return "alu"
end
local function tally(table_, key, clocks)
    local item = table_[key]
    if not item then item = {instructions=0, clocks=0}; table_[key] = item end
    item.instructions = item.instructions + 1
    item.clocks = item.clocks + clocks
end
function Profile.new(map_path, blocks_only)
    local map = dofile(map_path)
    assert(type(map) == "table" and type(map.regions) == "table" and #map.regions > 0 and #map.regions <= 4096, "Invalid GSU profile map")
    assert(math.type(map.origin) == "integer" and math.type(map.finish) == "integer" and
        map.origin >= 0 and map.finish > map.origin and map.finish <= 0x1000000 and
        (map.origin >> 16) == ((map.finish-1) >> 16), "Invalid GSU profile origin/span")
    local finish = map.origin
    for _, r in ipairs(map.regions) do
        assert(r.start == finish and r.finish > r.start and r.finish <= map.finish, "Profile map has gap/overlap")
        assert(r.name:match("^[%w_]+$") and r.owner:match("^[%w_]+$"), "Unsafe profile name")
        finish = r.finish
    end
    assert(finish == map.finish, "Incomplete GSU profile map")
    return setmetatable({map=map, previous=nil, instructions=0, clocks=0, categories={}, functions={}, blocks={}, pcs={}, blocks_only=blocks_only}, Profile)
end
function Profile:chargeBlock(clocks)
    assert(clocks >= 0 and clocks < 100000000, "Invalid region interval")
    local p = assert(self.previous)
    self.clocks = self.clocks + clocks
    self.blocks[p.block].clocks = self.blocks[p.block].clocks + clocks
    self.functions[p.owner].clocks = self.functions[p.owner].clocks + clocks
end
function Profile:observeBlock(address, opcode, state_loader)
    assert(self.blocks_only and (not self.previous or not self.previous.stopped), "Wrong/finished profile mode")
    local block, r = self:region(address)
    if not self.previous or self.previous.block ~= block or opcode == 0 then
        local now = number(state_loader(),"cycleCount")
        if self.previous then self:chargeBlock(now-self.previous.clock) end
        self.previous = {block=block, owner=r.owner, clock=now, stopped=opcode==0}
    end
    self.instructions = self.instructions + 1
    assert(self.instructions <= 10000000, "GSU profiling instruction limit")
    tally(self.blocks,block,0); tally(self.functions,r.owner,0)
    local pc = self.pcs[address]
    if not pc then pc = {instructions=0}; self.pcs[address] = pc end
    pc.instructions = pc.instructions+1
end
function Profile:region(address)
    local low, high = 1, #self.map.regions
    while low <= high do
        local middle = (low + high) // 2
        local r = self.map.regions[middle]
        if address < r.start then high = middle - 1
        elseif address >= r.finish then low = middle + 1
        else return middle, r end
    end
    error("GSU executed outside profile CODE")
end
function Profile:charge(clocks)
    local p = assert(self.previous, "Profile has no preceding opcode")
    assert(clocks >= 0 and clocks < 100000000, "Invalid opcode interval")
    self.instructions = self.instructions + 1
    assert(self.instructions <= 10000000, "GSU profiling instruction limit")
    self.clocks = self.clocks + clocks
    tally(self.categories, p.category, clocks)
    tally(self.functions, p.owner, clocks)
    tally(self.blocks, p.block, clocks)
    tally(self.pcs, p.pc, clocks)
end
function Profile:observe(address, opcode, state)
    assert(not self.blocks_only and (not self.previous or self.previous.category ~= "stop"), "Wrong/finished profile mode")
    local now = number(state,"cycleCount")
    if self.previous then self:charge(now - self.previous.clock) end
    local block, r = self:region(address)
    self.previous = {pc=address, block=block, owner=r.owner, category=category(opcode,state), clock=now}
end
local function records(table_, key_name, regions)
    local keys, output = {}, {}
    for key in pairs(table_) do keys[#keys+1] = key end
    table.sort(keys)
    for _, key in ipairs(keys) do
        local value = table_[key]
        local encoded = type(key) == "number" and tostring(key) or ('"'..key..'"')
        local clocks = value.clocks and (',"master_clocks":'..value.clocks) or ''
        local metadata = ''
        if regions then
            local r = regions[key]
            metadata = string.format(',"label":"%s","function":"%s","start":%d,"finish":%d',r.name,r.owner,r.start,r.finish)
        end
        output[#output+1] = string.format('{"%s":%s,"instructions":%d%s%s}', key_name,encoded,value.instructions,clocks,metadata)
    end
    return '['..table.concat(output,',')..']'
end
function Profile:finish(tail, expected)
    assert(self.previous and (self.previous.stopped or self.previous.category == "stop"), "Profile must finish at STOP")
    if self.blocks_only then self:chargeBlock(tail) else self:charge(tail) end
    assert(self.clocks == expected, "Profile/boundary clock sum differs")
    self.previous = nil
    return string.format('{"measurement":"gsu-entry-interval-profile-v1","granularity":"%s","master_clocks":%d,"instructions":%d,"categories":%s,"functions":%s,"blocks":%s,"pcs":%s}\n',
        self.blocks_only and "machine_label_regions" or "opcode",self.clocks,self.instructions,records(self.categories,"category"),records(self.functions,"function"),records(self.blocks,"region",self.map.regions),records(self.pcs,"address"))
end
return Profile
