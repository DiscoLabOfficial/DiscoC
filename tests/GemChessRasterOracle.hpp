#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Widened midpoint oracle for the gem-chess collector. No compiler-produced
// instruction or candidate collector formula participates in this reference.
namespace GemChessRasterOracle {
using Edge = std::array<int, 4>;
struct Spans {
    std::array<int, 192> left, right;
    Spans() { left.fill(256); right.fill(-1); }
};
inline void mark(Spans& spans, int first, int last, int y) {
    if (y >= 0 && y < 192) {
        spans.left.at(static_cast<std::size_t>(y)) = std::min(spans.left.at(static_cast<std::size_t>(y)), first);
        spans.right.at(static_cast<std::size_t>(y)) = std::max(spans.right.at(static_cast<std::size_t>(y)), last);
    }
}
inline Edge canonical(Edge edge) {
    const auto dx = std::abs(edge[2] - edge[0]), dy = std::abs(edge[3] - edge[1]);
    if ((dx >= dy && edge[0] > edge[2]) || (dx < dy && edge[1] > edge[3])) {
        std::swap(edge[0], edge[2]); std::swap(edge[1], edge[3]);
    }
    return edge;
}
inline Spans reference(Edge edge) {
    edge = canonical(edge);
    const int dx = std::abs(edge[2] - edge[0]), dy = std::abs(edge[3] - edge[1]);
    const bool horizontal = dx >= dy;
    const int major = horizontal ? dx : dy, minor = horizontal ? dy : dx;
    const int direction = horizontal ? (edge[3] < edge[1] ? -1 : 1) : (edge[2] < edge[0] ? -1 : 1);
    Spans result;
    for (int k = 0; k <= major; ++k) {
        const auto numerator = static_cast<std::int64_t>(major / 2) + static_cast<std::int64_t>(k) * minor;
        const int step = major ? static_cast<int>(numerator / major) : 0;
        const int x = horizontal ? edge[0] + k : edge[0] + direction * step;
        const int y = horizontal ? edge[1] + direction * step : edge[1] + k;
        mark(result, x, x, y);
    }
    return result;
}
// Literal widened baseline port, used only to validate the independent oracle.
inline Spans frozenCollector(Edge edge) {
    edge = canonical(edge);
    int x = edge[0], y = edge[1];
    const int dx = std::abs(edge[2] - x), dy = std::abs(edge[3] - y);
    Spans result;
    if (dx >= dy) {
        const int direction = edge[3] < y ? -1 : 1;
        int first = x, error = dx / 2;
        for (int k = 0; k <= dx; ++k) {
            int next = error + dy;
            if (k == dx || next >= dx) {
                mark(result, first, x, y);
                if (k == dx) break;
                first = x + 1; y += direction; next -= dx;
            }
            error = next; ++x;
        }
    } else {
        const int direction = edge[2] < x ? -1 : 1;
        int error = dy / 2;
        for (int k = 0; k <= dy; ++k) {
            mark(result, x, x, y);
            error += dx;
            if (error >= dy) { error -= dy; x += direction; }
            ++y;
        }
    }
    return result;
}
inline std::string describe(const Edge& edge) {
    return std::to_string(edge[0]) + "," + std::to_string(edge[1]) + " -> " +
        std::to_string(edge[2]) + "," + std::to_string(edge[3]);
}
inline std::vector<Edge> runtimeEdges() {
    std::vector<Edge> result = {Edge{{0,0,0,0}}, Edge{{255,191,255,191}}, Edge{{0,0,2,1}},
        Edge{{-3,0,258,0}}, Edge{{0,-1,0,192}}, Edge{{-512,-256,767,447}}, Edge{{0,191,255,192}}};
    const int xs[] = {-512,-1,0,1,127,254,255,256,767};
    const int ys[] = {-256,-1,0,1,95,190,191,192,447};
    for (const auto x : xs) for (const auto y : ys) result.push_back(Edge{{x,y,255-x,191-y}});
    std::uint32_t seed = 0x43f1d2a5;
    const auto random = [&](unsigned limit) -> int {
        seed = seed * 1664525u + 1013904223u; return static_cast<int>(seed % limit);
    };
    for (unsigned i = 0; i < 128; ++i)
        result.push_back(Edge{{random(1280)-512,random(704)-256,random(1280)-512,random(704)-256}});
    return result;
}
inline std::vector<Edge> runtimePrivateEdges() {
    std::vector<Edge> result;
    for (const auto& edge : runtimeEdges())
        if (edge[0]>=0 && edge[0]<=255 && edge[2]>=0 && edge[2]<=255 &&
            edge[1]>=0 && edge[1]<=191 && edge[3]>=0 && edge[3]<=191) result.push_back(edge);
    std::uint32_t seed=0xa5d54edd;
    const auto random=[&](unsigned limit)->int { seed=seed*1664525u+1013904223u;return static_cast<int>(seed%limit); };
    for (unsigned i=0;i<256;++i) result.push_back(Edge{{random(256),random(192),random(256),random(192)}});
    return result;
}
using Point = std::array<int, 2>;
using Polygon = std::vector<Point>;
inline std::vector<Polygon> runtimePolygons() {
    std::vector<Polygon> result = {
        {Point{{0,0}},Point{{0,191}},Point{{255,191}},Point{{255,0}}},
        {Point{{-1,-1}},Point{{-1,192}},Point{{256,192}},Point{{256,-1}}},
        {Point{{2,7}},Point{{19,7}},Point{{11,7}}},
        {Point{{9,9}},Point{{9,9}},Point{{9,9}}},
        {Point{{0,-1}},Point{{0,192}},Point{{255,191}}},
        {Point{{255,0}},Point{{-1,191}},Point{{256,191}}},
        {Point{{-30,50}},Point{{-10,100}},Point{{-5,80}}},
        {Point{{300,50}},Point{{310,100}},Point{{320,80}}},
        {Point{{0,-30}},Point{{2,-10}},Point{{30,-20}}},
        {Point{{0,220}},Point{{2,250}},Point{{30,230}}},
        {Point{{0,0}},Point{{0,191}},Point{{0,80}}},
        {Point{{255,191}},Point{{255,0}},Point{{255,80}}},
        {Point{{-256,0}},Point{{128,191}},Point{{511,0}}},
        {Point{{0,0}},Point{{1,1}},Point{{2,0}}}
    };
    std::uint32_t seed = 0x174437fd;
    const auto random = [&](unsigned limit) -> int {
        seed = seed * 1664525u + 1013904223u; return static_cast<int>(seed % limit);
    };
    for (unsigned i=0; i<64; ++i) {
        const int x=random(384)-64, y=random(256)-32;
        const int u=random(192)+1, v=random(96)-48, w=random(96)-48, t=-random(144)-1;
        Polygon polygon{Point{{x,y}},Point{{x+u,y+v}},Point{{x+u+w,y+v+t}},Point{{x+w,y+t}}};
        if (u*t-v*w > 0) std::reverse(polygon.begin(), polygon.end());
        if (i%2) polygon.pop_back();
        result.push_back(std::move(polygon));
    }
    return result;
}
inline void rasterEdge(const Edge& original, std::array<std::uint8_t, 256*192>& pixels, std::uint8_t ink) {
    const auto edge=canonical(original);
    const int dx=std::abs(edge[2]-edge[0]), dy=std::abs(edge[3]-edge[1]);
    const bool horizontal=dx>=dy;
    const int major=horizontal?dx:dy, minor=horizontal?dy:dx;
    const int direction=horizontal?(edge[3]<edge[1]?-1:1):(edge[2]<edge[0]?-1:1);
    for (int k=0;k<=major;++k) {
        const int step=major?static_cast<int>((static_cast<std::int64_t>(major/2)+static_cast<std::int64_t>(k)*minor)/major):0;
        const int x=horizontal?edge[0]+k:edge[0]+direction*step;
        const int y=horizontal?edge[1]+direction*step:edge[1]+k;
        if (x>=0 && x<256 && y>=0 && y<192) pixels.at(static_cast<std::size_t>(y)*256+x)=ink;
    }
}
inline std::array<std::uint8_t, 24576> framebuffer(const Polygon& polygon, bool wire, std::uint8_t ink) {
    Spans combined;
    std::array<std::uint8_t, 256*192> pixels{};
    for (std::size_t i=0;i<polygon.size();++i) {
        const auto& a=polygon[i]; const auto& b=polygon[(i+1)%polygon.size()];
        const Edge edge{{a[0],a[1],b[0],b[1]}};
        if (wire) rasterEdge(edge,pixels,ink);
        else {
            const auto spans=reference(edge);
            for (std::size_t y=0;y<192;++y) {
                combined.left[y]=std::min(combined.left[y],spans.left[y]);
                combined.right[y]=std::max(combined.right[y],spans.right[y]);
            }
        }
    }
    if (!wire) for (std::size_t y=0;y<192;++y)
        for (int x=std::max(0,combined.left[y]);x<=std::min(255,combined.right[y]);++x)
            pixels.at(y*256+static_cast<std::size_t>(x))=ink;
    std::array<std::uint8_t,24576> result{};
    for (unsigned y=0;y<192;++y) for (unsigned x=0;x<256;++x) {
        const auto value=pixels[y*256+x];
        const auto address=((x/8)*24+y/8)*32+(y%8)*2;
        for (unsigned plane=0;plane<4;++plane)
            result.at(address+(plane/2)*16+plane%2)|=static_cast<std::uint8_t>(((value>>plane)&1u)<<(7-x%8));
    }
    return result;
}
inline void selfTest() {
    const auto compare = [](const Edge& edge) {
        const auto expected = reference(edge), actual = frozenCollector(edge);
        if (expected.left != actual.left || expected.right != actual.right)
            throw std::runtime_error("Midpoint reference disagrees with frozen collector: " + describe(edge));
        const Edge reverse{{edge[2],edge[3],edge[0],edge[1]}};
        const auto reversed = reference(reverse);
        if (expected.left != reversed.left || expected.right != reversed.right)
            throw std::runtime_error("Midpoint edge orientation changed coverage: " + describe(edge));
    };
    for (int a=-4; a<=4; ++a) for (int b=-4; b<=4; ++b)
        for (int c=-4; c<=4; ++c) for (int d=-4; d<=4; ++d) compare(Edge{{a,b,c,d}});
    for (const auto& edge : runtimeEdges()) compare(edge);
    const auto tie = reference(Edge{{0,0,2,1}}), point = reference(Edge{{4,7,4,7}});
    if (tie.left[0] != 0 || tie.right[0] != 0 || tie.left[1] != 1 || tie.right[1] != 2 ||
        point.left[7] != 4 || point.right[7] != 4 || point.left[6] != 256 || point.right[8] != -1)
        throw std::runtime_error("Midpoint tie, endpoint inclusion or degenerate-point convention changed");
    const auto full=framebuffer(runtimePolygons().front(),false,15);
    if (!std::all_of(full.begin(),full.end(),[](std::uint8_t value){return value==255;}))
        throw std::runtime_error("Planar oracle missed a full-viewport pixel or final bitplane byte");
    const Polygon lastPixel{Point{{255,191}},Point{{255,191}},Point{{255,191}}};
    const auto last=framebuffer(lastPixel,false,8);
    if (last.back()!=1 || std::count(last.begin(),last.end(),static_cast<std::uint8_t>(0))!=24575)
        throw std::runtime_error("Planar oracle's last-pixel golden byte is incorrect");
}
} // namespace GemChessRasterOracle
