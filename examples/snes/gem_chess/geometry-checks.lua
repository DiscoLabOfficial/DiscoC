-- Independent widened-integer geometry checks. No generated assembly is used.
local quarter={0,6,12,19,24,30,36,41,45,49,53,56,59,61,63,64,64}
local ringX,ringZ={64,32,-32,-64,-32,32},{0,55,55,0,-55,-55}
local radii={{5,6,3,2,4,0},{6,6,3,3,6,6},{6,6,3,2,4,2},
    {6,6,3,2,5,0},{6,6,3,2,6,3},{6,6,3,2,5,2}}
local heights={{0,2,4,10,15,21},{0,2,4,16,18,23},{0,2,4,13,18,22},
    {0,2,4,15,22,29},{0,2,4,17,23,29},{0,2,4,19,25,29}}
local masks={{36,9,36,36,0},{0,9,0,36,0},{0,9,36,36,9},
    {0,9,36,0,0},{0,9,36,36,9},{0,9,36,0,0}}
local function q(v)return (v+32+(v<0 and -1 or 0))//64 end
local function sin(phase)
    phase=phase&63
    local i=phase&15;if (phase&16)~=0 then i=16-i end
    local v=quarter[i+1];return (phase&32)~=0 and -v or v
end
local function subtract(a,b)return {a[1]-b[1],a[2]-b[2],a[3]-b[3]} end
local function cross(a,b)return {a[2]*b[3]-a[3]*b[2],a[3]*b[1]-a[1]*b[3],a[1]*b[2]-a[2]*b[1]} end
local function dot(a,b)return a[1]*b[1]+a[2]*b[2]+a[3]*b[3] end
local function area(a,b,c)return (b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]) end
local function vertex(kind,ring,side,facing)
    local offset=kind==3 and ring>=3 and (ring-3)*facing or 0
    return {q(radii[kind][ring+1]*ringX[side+1]),heights[kind][ring+1],
        offset+q(radii[kind][ring+1]*ringZ[side+1])}
end
local function project(v,yaw,elevation,zoom)
    local sy,cy,sp,cp=sin(yaw),sin(yaw+16),sin(elevation),sin(elevation+16)
    local rx=q(v[1]*cy+v[3]*sy);local rz=q(v[3]*cy-v[1]*sy)
    local vertical=q((v[2]-10)*cp-rz*sp)
    local depth=400-zoom*8-q((v[2]-10)*sp+rz*cp)
    assert(depth>=128 and depth<512,"depth table bounds")
    local factor=16384//depth
    assert(math.abs(rx*factor)<32768 and math.abs(vertical*factor)<32768,"projection word overflow")
    local p={128+q(rx*factor),84-q(vertical*factor)}
    assert(p[1]>=0 and p[1]<256 and p[2]>=0 and p[2]<192,"escaped viewport")
    return p
end
return function()
    local triangles=0
    for kind=1,6 do for _,facing in ipairs({1,-1}) do
        for band=0,4 do for side=0,5 do
            local next=(side+1)%6
            local a=vertex(kind,band,next,facing);local b=vertex(kind,band,side,facing)
            local c=vertex(kind,band+1,side,facing);local d=vertex(kind,band+1,next,facing)
            local determinant=dot(subtract(b,a),cross(subtract(c,a),subtract(d,a)))
            local bd=(masks[kind][band+1]&(1<<side))~=0
            assert(bd==(determinant>0),"mesh diagonal fixture disagrees with convex hull")
            local chosen=bd and {{a,b,d},{b,c,d}} or {{a,b,c},{a,c,d}}
            for _,tri in ipairs(chosen) do
                local normal=cross(subtract(tri[2],tri[1]),subtract(tri[3],tri[1]))
                for r=band,band+1 do for s=0,5 do
                    assert(dot(normal,subtract(vertex(kind,r,s,facing),tri[1]))<=0,"non-convex band plane")
                end end
                triangles=triangles+1
            end
        end end
    end end
    -- Regression: the former AC fan combines a visible and hidden triangle.
    local folded={}
    for _,v in ipairs({vertex(5,2,0,1),vertex(5,2,5,1),vertex(5,3,5,1),vertex(5,3,0,1)}) do
        folded[#folded+1]=project({v[1]-8,v[2],v[3]+56},61,7,16)
    end
    assert(area(folded[1],folded[2],folded[3])==-21 and
        area(folded[1],folded[3],folded[4])==18,"nonplanar culling regression fixture")
    -- A planar plinth wall also folds after integer projection at a grazing
    -- angle. Its visible second triangle must not be discarded with the first.
    local wall={}
    for _,v in ipairs({{-70,-5,70},{70,-5,70},{70,0,70},{-70,0,70}}) do
        wall[#wall+1]=project(v,11,10,13)
    end
    assert(area(wall[1],wall[2],wall[3])==106 and
        area(wall[1],wall[3],wall[4])==-94,"planar projection culling regression fixture")
    local poses,maxArea=0,0
    for yaw=0,63 do for elevation=5,10 do for zoom=6,16 do
        for _,x in ipairs({-70,70}) do for _,z in ipairs({-70,70}) do
            project({x,-5,z},yaw,elevation,zoom);project({x,0,z},yaw,elevation,zoom)
        end end
        for _,x in ipairs({-62,62}) do for _,z in ipairs({-62,62}) do
            project({x,0,z},yaw,elevation,zoom);project({x,38,z},yaw,elevation,zoom)
        end end
        local a=project({-70,0,70},yaw,elevation,zoom)
        local b=project({70,0,70},yaw,elevation,zoom)
        local c=project({70,0,-70},yaw,elevation,zoom)
        local d=project({-70,0,-70},yaw,elevation,zoom)
        local first,second=area(a,b,c),area(a,c,d)
        assert(first<0 and second<0 and first+second>=-32768,"plinth signed area overflow/culling")
        maxArea=math.max(maxArea,-first-second);poses=poses+1
    end end end
    assert(poses==4224 and maxArea==28380,"camera envelope regression")
    return string.format("%d camera envelopes, %d outward band triangles, planar/nonplanar culling regressions",poses,triangles)
end
