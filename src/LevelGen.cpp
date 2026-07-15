// Graal Level Generator
// Original software by Stefan Knorr.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <gdiplus.h>
#else
#include <png.h>
#include <cstdint>
#include <filesystem>
using BYTE = std::uint8_t;
using COLORREF = std::uint32_t;
static constexpr COLORREF RGB(BYTE r, BYTE g, BYTE b) { return (COLORREF)r | ((COLORREF)g << 8) | ((COLORREF)b << 16); }
static constexpr BYTE GetRValue(COLORREF color) { return (BYTE)(color & 0xff); }
static constexpr BYTE GetGValue(COLORREF color) { return (BYTE)((color >> 8) & 0xff); }
static constexpr BYTE GetBValue(COLORREF color) { return (BYTE)((color >> 16) & 0xff); }
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <list>
#include <memory>
#include <algorithm>
#include <cctype>
#include <filesystem>

#ifdef _WIN32
#pragma comment(lib, "gdiplus.lib")
using namespace Gdiplus;
#endif

// -----------------------------------------------------------------------------
//  Graal .nw level model
// -----------------------------------------------------------------------------
//
//  A Graal level is a 64x64 tile grid. Each tile is a 16-bit index into the
//  tileset (a tiles pic). Levels also carry LINKs (exits to other levels),
//  SIGNS, NPCs and BADDIES.
//
//  .nw on-disk formats:
//    "GLEVNW01"  - text format:  "BOARD 0 <row> 64 0 <tiles...>"
//    "GR-V1.00"  - binary, board = raw shorts
//    "GR-V1.01"  - binary + RLE
//    "GR-V1.02"  - binary + RLE, more sections
//    "GR-V1.03"  - binary + RLE

static const int  TILES_PER_LEVEL = 64;          // 64x64 board

// A placed link/warp between levels.
struct GraalLink {
    std::string destLevel;   // e.g. "onlinestartlevel"
    double destX = 0.0;      // warp target (in tiles, float)
    double destY = 0.0;
    double width  = 2.0;     // 0x40000000 -> 2.0
    double height = 2.0;     // 0x40000000 -> 2.0
    std::string destXVar;    // "playerx" or ""
    std::string destYVar;    // "playery" or ""
};

struct GraalSign {
    int  x = 0, y = 0;       // tile coords
    std::string text;
};

struct GraalNpc {
    double x = 0.0, y = 0.0; // tile coords (float)
    std::string image;       // gif filename
    std::string script;
};

struct GraalBaddy {
    double x = 0.0, y = 0.0;
    int    id = 0;
    int    props = 0;
    std::string image;
    std::string script;
};

// One full level.
struct GraalLevel {
    unsigned short tiles[TILES_PER_LEVEL * TILES_PER_LEVEL] = { 0 };
    std::string    name;          // e.g. "world_a-01"
    bool existing = false;
    std::vector<GraalLink> links;
    std::vector<GraalSign> signs;
    std::vector<GraalNpc>  npcs;
    std::vector<GraalBaddy> baddies;
};

inline unsigned short& TileAt(GraalLevel& L, int x, int y) {
    return L.tiles[y * TILES_PER_LEVEL + x];
}
inline unsigned short  TileAt(const GraalLevel& L, int x, int y) {
    return L.tiles[y * TILES_PER_LEVEL + x];
}

// forward declarations (definitions below)
static bool LoadLevel(GraalLevel& L, const std::string& path);
static void RenderTerrainLayer();


// -----------------------------------------------------------------------------
//  Tile blitter
//    Stamps a rectangular tile pattern onto the GLOBAL grid (spanning many
//    levels). Global coords are split into (level, local-x, local-y) by /64
//    and %64. Out-of-range writes are dropped, matching the original's bounds
//    checks.
// -----------------------------------------------------------------------------
struct TilePattern {
    int w, h;
    std::vector<unsigned short> tiles;  // w*h entries
};

static int g_levelCols = 0;
static int g_levelRows = 0;
static std::vector<GraalLevel> g_levels;

static std::string JoinPath(const std::string& folder, const std::string& filename)
{
    return (std::filesystem::path(folder) / filename).string();
}

// Stamp `pattern` so its top-left lands at global tile (gx, gy).
static void BlitPattern(const TilePattern& pat, int gx, int gy)
{
    for (int py = 0; py < pat.h; ++py) {
        for (int px = 0; px < pat.w; ++px) {
            unsigned short tile = pat.tiles[py * pat.w + px];
            if (tile == 0xffff) continue;
            int tx = gx + px;
            int ty = gy + py;
            if (tx < 0 || tx >= g_levelCols * TILES_PER_LEVEL) continue;
            if (ty < 0 || ty >= g_levelRows * TILES_PER_LEVEL) continue;
            int lvlIdx = (tx / TILES_PER_LEVEL) + (ty / TILES_PER_LEVEL) * g_levelCols;
            int lx = tx % TILES_PER_LEVEL;
            int ly = ty % TILES_PER_LEVEL;
            TileAt(g_levels[lvlIdx], lx, ly) = tile;
        }
    }
}

static bool CanStampOnGrass(const TilePattern& pat, int gx, int gy)
{
    for (int py = 0; py < pat.h; ++py) for (int px = 0; px < pat.w; ++px) {
        if (pat.tiles[py * pat.w + px] == 0xffff) continue;
        int tx = gx + px, ty = gy + py;
        if (tx < 0 || ty < 0 || tx >= g_levelCols * TILES_PER_LEVEL || ty >= g_levelRows * TILES_PER_LEVEL) return false;
        if (TileAt(g_levels[(tx / TILES_PER_LEVEL) + (ty / TILES_PER_LEVEL) * g_levelCols], tx % TILES_PER_LEVEL, ty % TILES_PER_LEVEL) != 0x7ff) return false;
    }
    return true;
}


// -----------------------------------------------------------------------------
//  Terrain edge detector
//    Scans a boolean mask (one byte per pixel, the thresholded colour layer)
//    and, for each set pixel, examines its 8 neighbours to choose which cliff
//    / edge tile patterns to stamp. This auto-generates mountain & water edges
//    from a filled outline - the "grid-based algorithm" described in the docs.
//
//    The mask is stored row by row.
// -----------------------------------------------------------------------------
static int g_maskW = 0, g_maskH = 0;
static std::vector<std::vector<char>> g_mask;  // [y][x]

static inline bool MaskAt(int x, int y)
{
    if (x < 0 || y < 0 || x >= g_maskW || y >= g_maskH) return false;
    return g_mask[y][x] != 0;
}

struct MapImage {
    int width = 0;
    int height = 0;
    std::vector<BYTE> rgb;
    COLORREF At(int x, int y) const {
        size_t p = ((size_t)y * width + x) * 3;
        return RGB(rgb[p], rgb[p + 1], rgb[p + 2]);
    }
};

static bool IsColourNear(COLORREF color, BYTE r, BYTE g, BYTE b)
{
    return std::abs((int)GetRValue(color) - r) + std::abs((int)GetGValue(color) - g) + std::abs((int)GetBValue(color) - b) < 32;
}

// helper to build a pattern from a width + raw tile ids
static TilePattern MakePat(int w, std::initializer_list<unsigned short> ids)
{
    TilePattern p; p.w = w; p.h = (int)ids.size() / w;
    p.tiles.assign(ids);
    return p;
}

static void RenderMountainTerrain()
{
    static const TilePattern base = MakePat(6, { 0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x59,0x5a,0x5b,0x5c,0x5d,0x5e });
    static const TilePattern urJoined = MakePat(3, { 0x0c,0xc2,0xc3 }), urOpen = MakePat(3, { 0x0c,0x0d,0x0e });
    static const TilePattern urCorner = MakePat(1, { 0xd1,0xe1 }), urGap = MakePat(1, { 0x1f,0x2f }), rightEdge = MakePat(1, { 0x3f,0x4f,0x5f }), rightCorner = MakePat(1, { 0x6f,0x7f });
    static const TilePattern rightWall = MakePat(2, { 0x6d,0x6e,0x7d,0x7e,0x8d,0x8e,0x9d,0xffff }), rightJoin = MakePat(1, { 0x6c });
    static const TilePattern ulJoined = MakePat(3, { 0xc0,0xc1,0x0b }), ulOpen = MakePat(3, { 0x09,0x0a,0x0b });
    static const TilePattern ulCorner = MakePat(1, { 0xd0,0xe0 }), ulGap = MakePat(1, { 0x18,0x28 }), leftEdge = MakePat(1, { 0x38,0x48,0x58 }), leftCorner = MakePat(1, { 0x68,0x78 });
    static const TilePattern leftWall = MakePat(2, { 0x69,0x6a,0x79,0x7a,0x89,0x8a,0xffff,0x9a }), leftJoin = MakePat(1, { 0x6b }), centerWall = MakePat(2, { 0x6b,0x6c,0x7b,0x7c,0x8b,0x8c,0x9b,0x9c });
    for (int x = 0; x < g_maskW; ++x) for (int y = 0; y < g_maskH; ++y) {
        if (!MaskAt(x, y)) continue;
        bool left = MaskAt(x - 1, y), right = MaskAt(x + 1, y);
        int half = (y & 1) ? -1 : 0;
        bool ul = MaskAt(x + half, y - 1), ur = MaskAt(x + half + 1, y - 1), dl = MaskAt(x + half, y + 1), dr = MaskAt(x + half + 1, y + 1);
        int bx = x * 6 - 1 + half * 3, by = y * 6;
        BlitPattern(base, bx + 1, by + 1);
        BlitPattern(ur ? urJoined : urOpen, bx + 4, by);
        if (ur && !right) BlitPattern(urCorner, bx + 7, by + 1);
        if (!ur && !right) BlitPattern(urGap, bx + 7, by + 1);
        if (!right) { BlitPattern(rightEdge, bx + 7, by + 3); if (!dr) BlitPattern(rightCorner, bx + 7, by + 6); }
        if (!dr) BlitPattern(rightWall, bx + 5, by + 6);
        if (dl && !dr) BlitPattern(rightJoin, bx + 4, by + 6);
        BlitPattern(ul ? ulJoined : ulOpen, bx + 1, by);
        if (ul && !left) BlitPattern(ulCorner, bx, by + 1);
        if (!ul && !left) BlitPattern(ulGap, bx, by + 1);
        if (!left) { BlitPattern(leftEdge, bx, by + 3); if (!dl) BlitPattern(leftCorner, bx, by + 6); }
        if (!dl) BlitPattern(leftWall, bx + 1, by + 6);
        if (!dl && dr) BlitPattern(leftJoin, bx + 3, by + 6);
        if (!dl && !dr) BlitPattern(centerWall, bx + 3, by + 6);
    }
}

static void RenderSandTerrain()
{
    static const TilePattern base = MakePat(6, { 0x491,0x492,0x493,0x494,0x495,0x496,0x4a1,0x4a2,0x4a3,0x4a4,0x4a5,0x4a6,0x4b1,0x4b2,0x4b3,0x4b4,0x4b5,0x4b6,0x4c1,0x4c2,0x4c3,0x4c4,0x4c5,0x4c6,0x4d1,0x4d2,0x4d3,0x4d4,0x4d5,0x4d6 });
    static const TilePattern urJoined = MakePat(3, { 0x484,0xe0e,0xe0f }), urOpen = MakePat(3, { 0x484,0x485,0x486 });
    static const TilePattern urCorner = MakePat(1, { 0xe1f,0xe2f }), urGap = MakePat(1, { 0x497,0x4a7 }), rightEdge = MakePat(1, { 0x4b7,0x4c7,0x4d7 }), rightCorner = MakePat(1, { 0x6f,0x7f });
    static const TilePattern rightWall = MakePat(2, { 0x4e5,0x4e6,0x7d,0x7e,0x8d,0x8e,0x9d,0xffff }), rightJoin = MakePat(1, { 0x4e4 });
    static const TilePattern ulJoined = MakePat(3, { 0xe0c,0xe0d,0x483 }), ulOpen = MakePat(3, { 0x481,0x482,0x483 });
    static const TilePattern ulCorner = MakePat(1, { 0xe1e,0xe2e }), ulGap = MakePat(1, { 0x490,0x4a0 }), leftEdge = MakePat(1, { 0x4b0,0x4c0,0x4d0 }), leftCorner = MakePat(1, { 0x68,0x78 });
    static const TilePattern leftWall = MakePat(2, { 0x4e1,0x4e2,0x79,0x7a,0x89,0x8a,0xffff,0x9a }), leftJoin = MakePat(1, { 0x4e3 }), centerWall = MakePat(2, { 0x4e3,0x4e4,0x5b5,0x5b6,0x8b,0x8c,0x9b,0x9c });
    for (int x = 0; x < g_maskW; ++x) for (int y = 0; y < g_maskH; ++y) {
        if (!MaskAt(x, y)) continue;
        bool left = MaskAt(x - 1, y), right = MaskAt(x + 1, y);
        int half = (y & 1) ? -1 : 0;
        bool ul = MaskAt(x + half, y - 1), ur = MaskAt(x + half + 1, y - 1), dl = MaskAt(x + half, y + 1), dr = MaskAt(x + half + 1, y + 1);
        int bx = x * 6 - 1 + half * 3, by = y * 6;
        BlitPattern(base, bx + 1, by + 1);
        BlitPattern(ur ? urJoined : urOpen, bx + 4, by);
        if (ur && !right) BlitPattern(urCorner, bx + 7, by + 1);
        if (!ur && !right) BlitPattern(urGap, bx + 7, by + 1);
        if (!right) { BlitPattern(rightEdge, bx + 7, by + 3); if (!dr) BlitPattern(rightCorner, bx + 7, by + 6); }
        if (!dr) BlitPattern(rightWall, bx + 5, by + 6);
        if (dl && !dr) BlitPattern(rightJoin, bx + 4, by + 6);
        BlitPattern(ul ? ulJoined : ulOpen, bx + 1, by);
        if (ul && !left) BlitPattern(ulCorner, bx, by + 1);
        if (!ul && !left) BlitPattern(ulGap, bx, by + 1);
        if (!left) { BlitPattern(leftEdge, bx, by + 3); if (!dl) BlitPattern(leftCorner, bx, by + 6); }
        if (!dl) BlitPattern(leftWall, bx + 1, by + 6);
        if (!dl && dr) BlitPattern(leftJoin, bx + 3, by + 6);
        if (!dl && !dr) BlitPattern(centerWall, bx + 3, by + 6);
    }
}

static void RenderBlueInterior()
{
    static const TilePattern interior = MakePat(6, { 0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142 });
    for (int x = 0; x < g_maskW; ++x) for (int y = 0; y < g_maskH; ++y) {
        if (MaskAt(x, y) && MaskAt(x - 1, y - 1) && MaskAt(x, y - 1) && MaskAt(x + 1, y - 1) && MaskAt(x - 1, y) && MaskAt(x + 1, y) && MaskAt(x - 1, y + 1) && MaskAt(x, y + 1) && MaskAt(x + 1, y + 1)) BlitPattern(interior, x * 6, y * 6);
    }
}
static void RenderComplexEdges(const std::vector<TilePattern>& p)
{
    for (int x = 0; x < g_maskW; ++x) for (int y = 0; y < g_maskH; ++y) {
        if (!MaskAt(x, y)) continue;
        int l = x > 0 && !MaskAt(x - 1, y), r = x + 1 < g_maskW && !MaskAt(x + 1, y), nw = x > 0 && y > 0 && !MaskAt(x - 1, y - 1), n = y > 0 && !MaskAt(x, y - 1), ne = x + 1 < g_maskW && y > 0 && !MaskAt(x + 1, y - 1), sw = x > 0 && y + 1 < g_maskH && !MaskAt(x - 1, y + 1), so = y + 1 < g_maskH && !MaskAt(x, y + 1), se = x + 1 < g_maskW && y + 1 < g_maskH && !MaskAt(x + 1, y + 1);
        if (nw && ne) n = 1; if (nw && sw) l = 1; if (sw && se) so = 1; if (ne && se) r = 1;
        int c = l + n + so + r;
        if ((so && n) || (r && l) || (l + n + se == 3) || (so + l + ne == 3) || (so + r + nw == 3) || (r + n + sw == 3)) BlitPattern(p[0], x * 6, y * 6);
        else if (c == 2) { if (l && n) BlitPattern(p[5], x * 6, y * 6); else if (so && l) BlitPattern(p[6], x * 6, y * 6); else if (so && r) BlitPattern(p[7], x * 6, y * 6); else BlitPattern(p[8], x * 6, y * 6); }
        else if (c == 1) { if ((se && n) || (r && nw)) BlitPattern(p[8], x * 6, y * 6); else if ((sw && n) || (ne && l)) BlitPattern(p[5], x * 6, y * 6); else if ((nw && so) || (se && l)) BlitPattern(p[6], x * 6, y * 6); else if ((ne && so) || (r && sw)) BlitPattern(p[7], x * 6, y * 6); else if (n) BlitPattern(p[1], x * 6, y * 6); else if (l) BlitPattern(p[2], x * 6, y * 6); else if (so) BlitPattern(p[3], x * 6, y * 6); else BlitPattern(p[4], x * 6, y * 6); }
        else { int d = nw + ne + sw + se; if (d == 2) { if (nw && se) BlitPattern(p[13], x * 6, y * 6); else if (ne && sw) BlitPattern(p[14], x * 6, y * 6); } else if (d == 1) { if (nw) BlitPattern(p[9], x * 6, y * 6); else if (sw) BlitPattern(p[10], x * 6, y * 6); else if (se) BlitPattern(p[11], x * 6, y * 6); else BlitPattern(p[12], x * 6, y * 6); } else BlitPattern(p[15], x * 6, y * 6); }
    }
}
static void RenderCliffTerrain()
{
    static const std::vector<TilePattern> patterns = {
        MakePat(1, { 0xffff }),
        MakePat(6, { 0x9e,0x9f,0x9e,0x9f,0x9e,0x9f,0x8f,0x8f,0x8f,0x8f,0x8f,0x8f,0xaf,0xae,0xaf,0xae,0xaf,0xae,0x12e,0x12f,0x12e,0x12f,0x12e,0x12f,0xbf,0xbe,0xbf,0xbe,0xbf,0xbe,0xa8,0xa8,0xa8,0xa8,0xa8,0xa8 }),
        MakePat(6, { 0xac0,0x8f,0xd2,0xe3,0xd4,0xd5,0xac0,0x8f,0xe2,0xd3,0xe4,0xd5,0xac0,0x8f,0xd2,0xe3,0xd4,0xd5,0xac0,0x8f,0xe2,0xd3,0xe4,0xd5,0xac0,0x8f,0xd2,0xe3,0xd4,0xd5,0xac0,0x8f,0xe2,0xd3,0xe4,0xd5 }),
        MakePat(6, { 0xa9,0xa9,0xa9,0xa9,0xa9,0xa9,0xb9,0xba,0xb9,0xba,0xb9,0xba,0xc9,0xca,0xc9,0xca,0xc9,0xca,0xf9,0xfa,0xf9,0xfa,0xf9,0xfa,0x8f,0x8f,0x8f,0x8f,0x8f,0x8f,0xad1,0xad0,0xad1,0xad0,0xad1,0xad0 }),
        MakePat(6, { 0x110,0x111,0x122,0x113,0x8f,0x82,0x110,0x121,0x112,0x123,0x8f,0x82,0x110,0x111,0x122,0x113,0x8f,0x82,0x110,0x121,0x112,0x123,0x8f,0x82,0x110,0x111,0x122,0x113,0x8f,0x82,0x110,0x121,0x112,0x123,0x8f,0x82 }),
        MakePat(6, { 0xffff,0xffff,0xffff,0xffff,0xffff,0xc6,0xffff,0xffff,0xffff,0xffff,0xf0,0x8f,0xffff,0xffff,0xffff,0xf0,0x8f,0xf1,0xffff,0xffff,0xf0,0x8f,0xf1,0xf2,0xffff,0xf0,0x8f,0xf1,0xf2,0xf3,0xf0,0x8f,0xf1,0xf2,0xf3,0xf4 }),
        MakePat(6, { 0xac1,0x8f,0xac2,0x180,0xb8,0xf7,0xffff,0xac1,0x8f,0xac2,0x191,0xb8,0xffff,0xffff,0xac1,0x8f,0xac2,0x180,0xffff,0xffff,0xffff,0xac1,0x8f,0xac2,0xffff,0xffff,0xffff,0xffff,0xac1,0x8f,0xffff,0xffff,0xffff,0xffff,0xffff,0xac1 }),
        MakePat(6, { 0xfc,0xbb,0xef,0xac3,0x8f,0x92,0xbb,0xfe,0xac3,0x8f,0x92,0xffff,0xef,0xac3,0x8f,0x92,0xffff,0xffff,0xac3,0x8f,0x92,0xffff,0xffff,0xffff,0x8f,0x92,0xffff,0xffff,0xffff,0xffff,0x92,0xffff,0xffff,0xffff,0xffff,0xffff }),
        MakePat(6, { 0xcd,0xffff,0xffff,0xffff,0xffff,0xffff,0x8f,0x105,0xffff,0xffff,0xffff,0xffff,0x104,0x8f,0x105,0xffff,0xffff,0xffff,0x103,0x104,0x8f,0x105,0xffff,0xffff,0x102,0x103,0x104,0x8f,0x105,0xffff,0x101,0x102,0x103,0x104,0x8f,0x105 }),
        MakePat(6, { 0x8f,0xf1,0xf2,0xf3,0xf4,0xf5,0xf1,0xf2,0xf3,0xf4,0xf5,0xffff,0xf2,0xf3,0xf4,0xf5,0xffff,0xffff,0xf3,0xf4,0xf5,0xffff,0xffff,0xffff,0xf4,0xf5,0xffff,0xffff,0xffff,0xffff,0xf5,0xffff,0xffff,0xffff,0xffff,0xffff }),
        MakePat(6, { 0xe7,0xffff,0xffff,0xffff,0xffff,0xffff,0xf7,0xe7,0xffff,0xffff,0xffff,0xffff,0xb8,0xf7,0xe7,0xffff,0xffff,0xffff,0x191,0xb8,0xf7,0xe7,0xffff,0xffff,0xac2,0x180,0xb8,0xf7,0xe7,0xffff,0x8f,0xac2,0x191,0xb8,0xf7,0xe7 }),
        MakePat(6, { 0xffff,0xffff,0xffff,0xffff,0xffff,0xec,0xffff,0xffff,0xffff,0xffff,0xec,0xfc,0xffff,0xffff,0xffff,0xec,0xfc,0xbb,0xffff,0xffff,0xec,0xfc,0xbb,0xfe,0xffff,0xec,0xfc,0xbb,0xef,0xac3,0xec,0xfc,0xbb,0xfe,0xac3,0x8f }),
        MakePat(6, { 0x100,0x101,0x102,0x103,0x104,0x8f,0xffff,0x100,0x101,0x102,0x103,0x104,0xffff,0xffff,0x100,0x101,0x102,0x103,0xffff,0xffff,0xffff,0x100,0x101,0x102,0xffff,0xffff,0xffff,0xffff,0x100,0x101,0xffff,0xffff,0xffff,0xffff,0xffff,0x100 }),
        MakePat(6, { 0x8f,0xf1,0xf2,0xf3,0xf4,0xae0,0xf1,0xf2,0xf3,0xf4,0xae0,0xfc,0xf2,0xf3,0xf4,0xae0,0xfc,0xbb,0xf3,0xf4,0xae0,0xfc,0xbb,0xfe,0xf4,0xae0,0xfc,0xbb,0xef,0xac3,0xae0,0xfc,0xbb,0xfe,0xac3,0x8f }),
        MakePat(6, { 0xae1,0x101,0x102,0x103,0x104,0x8f,0xf7,0xae1,0x101,0x102,0x103,0x104,0xb8,0xf7,0xae1,0x101,0x102,0x103,0x191,0xb8,0xf7,0xae1,0x101,0x102,0xac2,0x180,0xb8,0xf7,0xae1,0x101,0x8f,0xac2,0x191,0xb8,0xf7,0xae1 }),
        MakePat(1, { 0xffff })
    };
    RenderComplexEdges(patterns);
}
static void RenderWaterTerrain()
{
    static const std::vector<TilePattern> patterns = {
        MakePat(1, { 0xffff }),
        MakePat(6, { 0xa9,0xa9,0xa9,0xa9,0xa9,0xa9,0xb9,0xba,0xb9,0xba,0xb9,0xba,0xc9,0xca,0xc9,0xca,0xc9,0xca,0x22e,0x22f,0x22e,0x22f,0x22e,0x22f,0x23e,0x23f,0x23e,0x23f,0x23e,0x23f,0x24e,0x24f,0x24e,0x24f,0x24e,0x24f }),
        MakePat(6, { 0x110,0x111,0x122,0x107,0x108,0x109,0x110,0x121,0x112,0x117,0x118,0x119,0x110,0x111,0x122,0x107,0x108,0x109,0x110,0x121,0x112,0x117,0x118,0x119,0x110,0x111,0x122,0x107,0x108,0x109,0x110,0x121,0x112,0x117,0x118,0x119 }),
        MakePat(6, { 0x10e,0x10f,0x10e,0x10f,0x10e,0x10f,0x11e,0x11f,0x11e,0x11f,0x11e,0x11f,0x98,0x99,0x98,0x99,0x98,0x99,0x12f,0x12e,0x12f,0x12e,0x12f,0x12e,0xbf,0xbe,0xbf,0xbe,0xbf,0xbe,0xa8,0xa8,0xa8,0xa8,0xa8,0xa8 }),
        MakePat(6, { 0x1cd,0x1ce,0x1cf,0x242,0x233,0xd5,0x1dd,0x1de,0x1df,0x232,0x243,0xd5,0x1cd,0x1ce,0x1cf,0x242,0x233,0xd5,0x1dd,0x1de,0x1df,0x232,0x243,0xd5,0x1cd,0x1ce,0x1cf,0x242,0x233,0xd5,0x1dd,0x1de,0x1df,0x232,0x243,0xd5 }),
        MakePat(6, { 0xffff,0xffff,0xffff,0xffff,0xffff,0xec,0xffff,0xffff,0xffff,0xffff,0xec,0xfc,0xffff,0xffff,0xffff,0xec,0xfc,0xbb,0xffff,0xffff,0xec,0xfc,0xbb,0xfe,0xffff,0xec,0xfc,0xbb,0xef,0x126,0xec,0xfc,0xbb,0xfe,0x126,0x127 }),
        MakePat(6, { 0x100,0x101,0x102,0x103,0x131,0x132,0xffff,0x100,0x101,0x102,0x103,0x131,0xffff,0xffff,0x100,0x101,0x102,0x103,0xffff,0xffff,0xffff,0x100,0x101,0x102,0xffff,0xffff,0xffff,0xffff,0x100,0x101,0xffff,0xffff,0xffff,0xffff,0xffff,0x100 }),
        MakePat(6, { 0x3cd,0x3bd,0xf2,0xf3,0xf4,0xf5,0x3bd,0xf2,0xf3,0xf4,0xf5,0xffff,0xf2,0xf3,0xf4,0xf5,0xffff,0xffff,0xf3,0xf4,0xf5,0xffff,0xffff,0xffff,0xf4,0xf5,0xffff,0xffff,0xffff,0xffff,0xf5,0xffff,0xffff,0xffff,0xffff,0xffff }),
        MakePat(6, { 0xe7,0xffff,0xffff,0xffff,0xffff,0xffff,0xf7,0xe7,0xffff,0xffff,0xffff,0xffff,0xb8,0xf7,0xe7,0xffff,0xffff,0xffff,0x191,0xb8,0xf7,0xe7,0xffff,0xffff,0x236,0x180,0xb8,0xf7,0xe7,0xffff,0x235,0x236,0x191,0xb8,0xf7,0xe7 }),
        MakePat(6, { 0xfc,0xbb,0xfe,0x126,0x127,0x128,0xbb,0xfe,0x126,0x127,0x128,0x142,0xfe,0x126,0x127,0x128,0x142,0x142,0x126,0x127,0x128,0x142,0x142,0x142,0x127,0x128,0x142,0x142,0x142,0x142,0x128,0x142,0x142,0x142,0x142,0x142 }),
        MakePat(6, { 0x133,0x142,0x142,0x142,0x142,0x142,0x132,0x133,0x142,0x142,0x142,0x142,0x131,0x132,0x133,0x142,0x142,0x142,0x103,0x131,0x132,0x133,0x142,0x142,0x102,0x103,0x131,0x132,0x133,0x142,0x101,0x102,0x103,0x131,0x132,0x133 }),
        MakePat(6, { 0x142,0x142,0x142,0x142,0x142,0x3cc,0x142,0x142,0x142,0x142,0x3cc,0x3cd,0x142,0x142,0x142,0x3cc,0x3cd,0x3bd,0x142,0x142,0x3cc,0x3cd,0x3bd,0xf2,0x142,0x3cc,0x3cd,0x3bd,0xf2,0xf3,0x3cc,0x3cd,0x3bd,0xf2,0xf3,0xf4 }),
        MakePat(6, { 0x234,0x235,0x236,0x191,0xb8,0xf7,0x142,0x234,0x235,0x236,0x191,0xb8,0x142,0x142,0x234,0x235,0x236,0x191,0x142,0x142,0x142,0x234,0x235,0x236,0x142,0x142,0x142,0x142,0x234,0x235,0x142,0x142,0x142,0x142,0x142,0x234 }),
        MakePat(6, { 0xfc,0xbb,0xfe,0x126,0x127,0x721,0xbb,0xfe,0x126,0x127,0x721,0x3cd,0xfe,0x126,0x127,0x721,0x3cd,0x3bd,0x126,0x127,0x721,0x3cd,0x3bd,0xf2,0x127,0x721,0x3cd,0x3bd,0xf2,0xf3,0x721,0x3cd,0x3bd,0xf2,0xf3,0xf4 }),
        MakePat(6, { 0x720,0x235,0x236,0x191,0xb8,0xf7,0x132,0x720,0x235,0x236,0x191,0xb8,0x131,0x132,0x720,0x235,0x236,0x191,0x103,0x131,0x132,0x720,0x235,0x236,0x102,0x103,0x131,0x132,0x720,0x235,0x101,0x102,0x103,0x131,0x132,0x720 }),
        MakePat(6, { 0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142,0x142 })
    };
    RenderComplexEdges(patterns);
}
static bool IsSandStone(COLORREF c)
{
    return IsColourNear(c, 150, 110, 0) || IsColourNear(c, 113, 82, 0) || IsColourNear(c, 83, 61, 0);
}
static void RenderSandStone(const MapImage& image, int onlyX = -1, int onlyY = -1)
{
    static const std::vector<TilePattern> p = {
        MakePat(1, { 0xffff }),
        MakePat(1, { 0x176 }),
        MakePat(1, { 0x188 }),
        MakePat(1, { 0x169 }),
        MakePat(1, { 0x177 }),
        MakePat(1, { 0x166 }),
        MakePat(1, { 0x167 }),
        MakePat(1, { 0x1a7 }),
        MakePat(1, { 0x178 }),
        MakePat(1, { 0x179 }),
        MakePat(1, { 0x197 }),
        MakePat(1, { 0x186 }),
        MakePat(1, { 0x189 }),
        MakePat(1, { 0x168 }),
        MakePat(1, { 0x187 }),
        MakePat(1, { 0xfe9 }),
        MakePat(1, { 0xff9 }),
        MakePat(1, { 0xf }),
        MakePat(1, { 0x8 }),
        MakePat(1, { 0x12d }),
        MakePat(1, { 0xaa }),
        MakePat(1, { 0xab }),
        MakePat(1, { 0x12d })
    };
    int startX = onlyX < 0 ? 0 : onlyX, endX = onlyX < 0 ? image.width : onlyX + 1;
    int startY = onlyY < 0 ? 0 : onlyY, endY = onlyY < 0 ? image.height : onlyY + 1;
    for (int x = startX; x < endX; ++x) for (int y = startY; y < endY; ++y) {
        COLORREF c = image.At(x, y); if (!IsSandStone(c)) continue;
        auto stamp = [&](const TilePattern& pattern) { BlitPattern(pattern, x - 2, y); };
        auto missing = [&](int px, int py) { return px >= 0 && py >= 0 && px < image.width && py < image.height && !IsSandStone(image.At(px, py)); };
        int l=missing(x-1,y),r=missing(x+1,y),nw=missing(x-1,y-1),n=missing(x,y-1),ne=missing(x+1,y-1),sw=missing(x-1,y+1),so=missing(x,y+1),se=missing(x+1,y+1);
        if (nw&&ne)n=1;if(nw&&sw)l=1;if(sw&&se)so=1;if(ne&&se)r=1;int k=l+n+so+r;
        if ((n&&so)||(l&&r)||(l+n+se==3)||(l+so+ne==3)||(r+so+nw==3)||(r+n+sw==3)) stamp(p[0]);
        else if(k==2){if(l&&n)stamp(p[1]);else if(l&&so)stamp(p[2]);else if(so&&r)stamp(p[3]);else stamp(p[4]);}
        else if(k==1){if((n&&se)||(r&&nw))stamp(p[4]);else if((n&&sw)||(l&&ne))stamp(p[1]);else if((so&&nw)||(l&&se))stamp(p[2]);else if((so&&ne)||(r&&sw))stamp(p[3]);else if(n)stamp(p[(x&1)?6:5]);else if(l)stamp(p[7]);else if(so)stamp(p[(x&1)?9:8]);else stamp(p[10]);}
        else {int d=nw+ne+sw+se;if(d==2){if(nw&&se)stamp(p[15]);else if(ne&&sw)stamp(p[16]);}else if(d==1){if(nw)stamp(p[11]);else if(sw)stamp(p[12]);else if(se)stamp(p[13]);else stamp(p[14]);}else if(IsColourNear(c,150,110,0))stamp(p[19+(x&1)+((y&1)<<1)]);else stamp(IsColourNear(c,113,82,0)?p[18]:p[17]);}
    }
}
static bool IsPuddle(COLORREF c)
{
    return IsColourNear(c, 0, 255, 255) || IsColourNear(c, 0, 179, 179);
}
static void RenderPuddles(const MapImage& image, int onlyX = -1, int onlyY = -1)
{
    static const std::vector<TilePattern> p = {
        MakePat(1, { 0xffff }),
        MakePat(1, { 0x176 }),
        MakePat(1, { 0x188 }),
        MakePat(1, { 0x169 }),
        MakePat(1, { 0x177 }),
        MakePat(1, { 0x1c4 }),
        MakePat(1, { 0x1c5 }),
        MakePat(1, { 0x1d4 }),
        MakePat(1, { 0x1f6 }),
        MakePat(1, { 0x1f7 }),
        MakePat(1, { 0x1bb }),
        MakePat(1, { 0x1c6 }),
        MakePat(1, { 0x196 }),
        MakePat(1, { 0x1b7 }),
        MakePat(1, { 0x1e7 }),
        MakePat(1, { 0xfe9 }),
        MakePat(1, { 0xff9 }),
        MakePat(1, { 0x1f4 }),
        MakePat(1, { 0x18e })
    };
    int startX = onlyX < 0 ? 0 : onlyX, endX = onlyX < 0 ? image.width : onlyX + 1;
    int startY = onlyY < 0 ? 0 : onlyY, endY = onlyY < 0 ? image.height : onlyY + 1;
    for (int x=startX;x<endX;++x) for(int y=startY;y<endY;++y) {
        COLORREF c=image.At(x,y);if(!IsPuddle(c))continue;
        auto stamp=[&](const TilePattern& pattern){BlitPattern(pattern,x-2,y);};
        auto m=[&](int px,int py){return px>=0&&py>=0&&px<image.width&&py<image.height&&!IsPuddle(image.At(px,py));};
        int l=m(x-1,y),r=m(x+1,y),nw=m(x-1,y-1),n=m(x,y-1),ne=m(x+1,y-1),sw=m(x-1,y+1),so=m(x,y+1),se=m(x+1,y+1);if(nw&&ne)n=1;if(nw&&sw)l=1;if(sw&&se)so=1;if(ne&&se)r=1;int k=l+n+so+r;
        if((n&&so)||(l&&r)||(l+n+se==3)||(l+so+ne==3)||(r+so+nw==3)||(r+n+sw==3))stamp(p[0]);
        else if(k==2){if(l&&n)stamp(p[1]);else if(l&&so)stamp(p[2]);else if(so&&r)stamp(p[3]);else stamp(p[4]);}
        else if(k==1){if((n&&se)||(r&&nw))stamp(p[4]);else if((n&&sw)||(l&&ne))stamp(p[1]);else if((so&&nw)||(l&&se))stamp(p[2]);else if((so&&ne)||(r&&sw))stamp(p[3]);else if(n)stamp(p[(x&1)?6:5]);else if(l)stamp(p[7]);else if(so)stamp(p[(x&1)?9:8]);else stamp(p[10]);}
        else{int d=nw+ne+sw+se;if(d==2){if(nw&&se)stamp(p[15]);else if(ne&&sw)stamp(p[16]);}else if(d==1){if(nw)stamp(p[11]);else if(sw)stamp(p[12]);else if(se)stamp(p[13]);else stamp(p[14]);}else stamp(IsColourNear(c,0,255,255)?p[18]:p[17]);}
    }
}

static void RenderSwampsAndBushes(const MapImage& image, int onlyX = -1, int onlyY = -1)
{
    static const TilePattern swamp = MakePat(2, { 0x1a4,0x1a5,0x1b4,0x1b5 });
    static const TilePattern bush = MakePat(2, { 0x2,0x3,0x12,0x13 });
    int startX = onlyX < 0 ? 0 : onlyX, endX = onlyX < 0 ? image.width : onlyX + 1;
    int startY = onlyY < 0 ? 0 : onlyY, endY = onlyY < 0 ? image.height : onlyY + 1;
    for (int x = startX; x < endX; ++x) for (int y = startY; y < endY; ++y) {
        if ((x & 1) || (y & 1) || x + 1 >= image.width || y + 1 >= image.height) continue;
        COLORREF c = image.At(x, y);
        if (IsColourNear(c, 0, 106, 0)) {
            bool okay = true;
            for (int py = 0; py < 2; ++py) for (int px = 0; px < 2; ++px) { COLORREF q = image.At(x + px, y + py); if (!IsColourNear(q, 0, 106, 0) && !IsColourNear(q, 24, 140, 24)) okay = false; }
            if (okay && CanStampOnGrass(swamp, x - 2, y)) BlitPattern(swamp, x - 2, y);
        } else if (IsColourNear(c, 0, 66, 0)) {
            bool okay = true;
            for (int py = 0; py < 2; ++py) for (int px = 0; px < 2; ++px) { COLORREF q = image.At(x + px, y + py); if (!IsColourNear(q, 0, 66, 0) && !IsColourNear(q, 24, 140, 24)) okay = false; }
            if (okay && CanStampOnGrass(bush, x - 2, y)) BlitPattern(bush, x - 2, y);
        }
    }
}

static void RenderFlowers(const MapImage& image, int onlyX = -1, int onlyY = -1)
{
    static const TilePattern flower = MakePat(1, { 0x10 });
    int startX = onlyX < 0 ? 0 : onlyX, endX = onlyX < 0 ? image.width : onlyX + 1;
    int startY = onlyY < 0 ? 0 : onlyY, endY = onlyY < 0 ? image.height : onlyY + 1;
    for (int x = startX; x < endX; ++x) for (int y = startY; y < endY; ++y) if (IsColourNear(image.At(x, y), 255, 0, 0)) BlitPattern(flower, x - 2, y);
}

// The neighbour checks decide edge orientation. We reproduce the original's
// exact decision tree (bVar1..bVar6 = the 6 directional tests) and the
// sequence of BlitPattern calls for each combination.
static void RenderTerrainLayer()
{
    RenderMountainTerrain();
}


// -----------------------------------------------------------------------------
//  Level-name generator
//    col,row -> "x-NN" where x = 'a'+(col%26) (two letters if >26 cols)
//    and NN  = row+1, zero-padded to width of g_levelRows.
// -----------------------------------------------------------------------------
static std::string LevelName(int col, int row)
{
    std::string letters;
    letters += (char)('a' + (col % 26));
    if (g_levelCols > 26) {
        int c2 = (col / 26) % 26;
        letters = (char)('a' + c2) + letters;
    }
    char num[8];
    int n = row + 1;
    if (g_levelRows > 99)
        std::snprintf(num, sizeof(num), "%03d", n);
    else if (g_levelRows > 9)
        std::snprintf(num, sizeof(num), "%02d", n);
    else
        std::snprintf(num, sizeof(num), "%d", n);
    return letters + num;
}

static std::string LevelFileStem(const std::string& prefix, const std::string& levelName)
{
    if (!prefix.empty() && (prefix.back() == '_' || prefix.back() == '-')) return prefix + levelName;
    return prefix + "_" + levelName;
}

static std::string GmapName(const std::string& prefix)
{
    std::string name;
    for (unsigned char c : prefix) {
        if (std::isalpha(c)) name += (char)c;
    }
    return name.empty() ? "world" : name;
}


// -----------------------------------------------------------------------------
//  Build the level grid and neighbour links
//    Creates g_levelCols * g_levelRows levels, names them, and for each level
//    that doesn't already exist on disk adds 4 LINK NPCs pointing at its
//    left/right/up/down neighbours (with playerx/playery warp vars).
// -----------------------------------------------------------------------------
static void BuildLevelGrid(const std::string& outputDir,
                           const std::string& filePrefix, bool createLinks, const std::string& baselineFolder)
{
    g_levels.assign(g_levelCols * g_levelRows, GraalLevel{});

    for (int row = 0; row < g_levelRows; ++row) {
        for (int col = 0; col < g_levelCols; ++col) {
            int idx = row * g_levelCols + col;
            GraalLevel& L = g_levels[idx];
            L.name = LevelName(col, row);

            std::string path = outputDir + "\\" + LevelFileStem(filePrefix, L.name) + ".nw";

            std::string baselinePath = baselineFolder.empty() ? path : JoinPath(baselineFolder, LevelFileStem(filePrefix, L.name) + ".nw");
            if (LoadLevel(L, baselinePath)) { L.existing = true; continue; }

            if (createLinks) {
                // Left neighbour  (col-1, row) -> warp to right edge
                if (col - 1 >= 0) {
                    GraalLink lk;
                    lk.destLevel = LevelFileStem(filePrefix, LevelName(col - 1, row));
                    lk.destX = 0.0;  lk.destY = 0.0;
                    lk.width = 1.0; lk.height = 64.0;
                    lk.destXVar = "61"; lk.destYVar = "playery";
                    L.links.push_back(lk);
                }
                // Right neighbour (col+1, row)
                if (col + 1 <= g_levelCols - 1) {
                    GraalLink lk;
                    lk.destLevel = LevelFileStem(filePrefix, LevelName(col + 1, row));
                    lk.destX = 3.75;  // 0x404f8000 = 3.75 (near right edge)
                    lk.destY = 0.0;
                    lk.width = 1.0; lk.height = 64.0;
                    lk.destXVar = "0"; lk.destYVar = "playery";
                    L.links.push_back(lk);
                }
                // Up neighbour (col, row-1)
                if (row - 1 >= 0) {
                    GraalLink lk;
                    lk.destLevel = LevelFileStem(filePrefix, LevelName(col, row - 1));
                    lk.destX = 0.0;  lk.destY = 0.0;
                    lk.width = 64.0; lk.height = 1.0;
                    lk.destXVar = "playerx"; lk.destYVar = "61";
                    L.links.push_back(lk);
                }
                // Down neighbour (col, row+1)
                if (row + 1 <= g_levelRows - 1) {
                    GraalLink lk;
                    lk.destLevel = LevelFileStem(filePrefix, LevelName(col, row + 1));
                    lk.destX = 0.0;  lk.destY = 3.75;  // 0x404f8000
                    lk.width = 64.0; lk.height = 1.0;
                    lk.destXVar = "playerx"; lk.destYVar = "0";
                    L.links.push_back(lk);
                }
            }
        }
    }
}


// -----------------------------------------------------------------------------
//  .nw level loader / merger
//    Reads a .nw file in any of the 5 header versions and merges its
//    BOARD/LINKS/SIGNS/NPCS/BADDIES into `L`. Used when regenerating over an
//    existing world (the doc's "another round" feature).
// -----------------------------------------------------------------------------
static bool ReadRleBoard(FILE* fp, GraalLevel& level, int bitsPerWord)
{
    unsigned int reservoir = 0;
    int reservoirBits = 0;
    int tileIndex = 0;
    int runLength = 1;
    int previousTile = -1;
    bool alternating = false;
    const unsigned int marker = 1u << (bitsPerWord - 1);
    const unsigned int mask = marker - 1;

    auto readWord = [&]() -> int {
        while (reservoirBits < bitsPerWord) {
            int next = std::fgetc(fp);
            if (next == EOF) return -1;
            reservoir |= (unsigned int)(unsigned char)next << reservoirBits;
            reservoirBits += 8;
        }
        unsigned int word = reservoir & ((1u << bitsPerWord) - 1);
        reservoir >>= bitsPerWord;
        reservoirBits -= bitsPerWord;
        return (int)word;
    };
    auto writeTile = [&](unsigned short tile) -> bool {
        if (tileIndex >= TILES_PER_LEVEL * TILES_PER_LEVEL) return false;
        level.tiles[tileIndex++] = tile;
        return true;
    };

    while (tileIndex < TILES_PER_LEVEL * TILES_PER_LEVEL) {
        int value = readWord();
        if (value < 0) return false;
        if (value & (int)marker) {
            alternating = (value & 0x100) != 0;
            runLength = value & 0xff;
            continue;
        }
        if (runLength == 1) {
            if (!writeTile((unsigned short)value)) return false;
            previousTile = value;
        } else if (!alternating) {
            for (int i = 0; i < runLength; ++i)
                if (!writeTile((unsigned short)value)) return false;
            runLength = 1;
        } else {
            if (previousTile < 0) return false;
            for (int i = 0; i < runLength; ++i) {
                if (!writeTile((unsigned short)previousTile) || !writeTile((unsigned short)value)) return false;
            }
            previousTile = -1;
            alternating = false;
            runLength = 1;
        }
    }
    return true;
}

static void ReadBinaryLinks(FILE* fp, GraalLevel& level)
{
    char line[1024];
    while (std::fgets(line, sizeof(line), fp)) {
        if (line[0] == '\n' || line[0] == '\r') return;
        char destination[256] = {};
        if (std::sscanf(line, "%255s", destination) == 1) {
            GraalLink link;
            link.destLevel = destination;
            level.links.push_back(link);
        }
    }
}

static bool ReadBinaryLine(FILE* fp, std::string& line)
{
    line.clear();
    for (;;) {
        int value = std::fgetc(fp);
        if (value == EOF) return !line.empty();
        if (value == '\n') return true;
        line.push_back((char)(unsigned char)value);
    }
}

static void ReadBinarySigns(FILE* fp, GraalLevel& level)
{
    std::string record;
    while (ReadBinaryLine(fp, record)) {
        if (record.size() == 3 && (unsigned char)record[0] == 0xff &&
            (unsigned char)record[1] == 0xff && (unsigned char)record[2] == 0xff) return;
        if (record.size() < 3) continue;
        GraalSign sign;
        sign.x = (unsigned char)record[0];
        sign.y = (unsigned char)record[1];
        sign.text.assign(record.data() + 3, record.size() - 3);
        level.signs.push_back(sign);
    }
}

static void ReadBinaryBaddies(FILE* fp, GraalLevel& level, bool modern)
{
    std::string record;
    while (ReadBinaryLine(fp, record) && !record.empty()) {
        if (record.size() < (modern ? 4u : 2u)) continue;
        GraalBaddy baddy;
        baddy.x = (double)((unsigned char)record[0] - 0x20);
        baddy.y = (double)((unsigned char)record[1] - 0x20);
        if (modern) {
            baddy.id = (unsigned char)record[2] - 0x20;
            baddy.props = (unsigned char)record[3] - 0x20;
        } else if (record.size() > 2) {
            std::string data(record.data() + 2, record.size() - 2);
            size_t separator = data.find('#');
            baddy.image = data.substr(0, separator);
            if (separator != std::string::npos) baddy.script = data.substr(separator + 1);
        }
        level.baddies.push_back(baddy);
    }
}

static void ReadBinaryNpcs(FILE* fp, GraalLevel& level)
{
    std::string record;
    while (ReadBinaryLine(fp, record)) {
        if (record.size() < 2) continue;
        GraalNpc npc;
        npc.x = (double)((unsigned char)record[0] - 0x20);
        npc.y = (double)((unsigned char)record[1] - 0x20);
        npc.image.assign(record.data() + 2, record.size() - 2);
        level.npcs.push_back(npc);
    }
}

static bool LoadLevel(GraalLevel& L, const std::string& path)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    char hdr[9] = {0 };
    if (std::fread(hdr, 1, 8, fp) != 8) { std::fclose(fp); return false; }
    hdr[8] = 0;

    if (std::strcmp(hdr, "GLEVNW01") == 0) {
        // Text format: lines of "BOARD 0 <row> 64 0 <tile chars>"
        // Each tile is 2 chars indexing into a 4096-entry letter table.
        char line[1024];
        while (std::fgets(line, sizeof(line), fp)) {
            if (std::strncmp(line, "BOARD 0 ", 8) == 0) {
                // parse "<row> 64 0 " then 64 tile-character pairs
                int row = std::atoi(line + 8);
                char* p = line + 8;
                while (*p && *p != ' ') ++p;  // skip row
                while (*p == ' ') ++p;        // skip "64"
                while (*p && *p != ' ') ++p;
                while (*p == ' ') ++p;
                while (*p && *p != ' ') ++p;
                while (*p == ' ') ++p;
                for (int x = 0; x < TILES_PER_LEVEL && p[0] && p[1]; ++x, p += 2) {
                    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    const char* hi = std::strchr(alphabet, p[0]);
                    const char* lo = std::strchr(alphabet, p[1]);
                    if (row >= 0 && row < TILES_PER_LEVEL && hi && lo)
                        TileAt(L, x, row) = (unsigned short)((hi - alphabet) * 64 + (lo - alphabet));
                }
            } else if (std::strncmp(line, "LINK ", 5) == 0) {
                GraalLink lk;
                char dest[256] = {0};
                char xVar[256] = "playerx", yVar[256] = "playery";
                int fields = std::sscanf(line + 5, "%255s %lf %lf %lf %lf %255s %255s", dest, &lk.destX, &lk.destY, &lk.width, &lk.height, xVar, yVar);
                if (fields >= 5) {
                    lk.destLevel = dest;
                    if (lk.destLevel.size() > 3 && lk.destLevel.compare(lk.destLevel.size() - 3, 3, ".nw") == 0) lk.destLevel.resize(lk.destLevel.size() - 3);
                    if (fields >= 6) lk.destXVar = xVar;
                    if (fields >= 7) lk.destYVar = yVar;
                    L.links.push_back(lk);
                }
            }
            // SIGN / NPC / BADDY sections parsed similarly
        }
    } else if (std::strcmp(hdr, "GR-V1.00") == 0 || std::strcmp(hdr, "GR-V1.01") == 0 ||
               std::strcmp(hdr, "GR-V1.02") == 0 || std::strcmp(hdr, "GR-V1.03") == 0) {
        int bitsPerWord = (hdr[6] == '2' || hdr[6] == '3') ? 13 : 12;
        if (!ReadRleBoard(fp, L, bitsPerWord)) {
            std::fclose(fp);
            return false;
        }
        if (hdr[6] == '2' || hdr[6] == '3') {
            ReadBinaryLinks(fp, L);
            ReadBinarySigns(fp, L);
            ReadBinaryBaddies(fp, L, false);
            ReadBinaryBaddies(fp, L, true);
            ReadBinaryNpcs(fp, L);
        } else if (hdr[6] == '0') {
            ReadBinaryBaddies(fp, L, false);
            ReadBinaryNpcs(fp, L);
        } else {
            ReadBinaryBaddies(fp, L, true);
            ReadBinaryNpcs(fp, L);
        }
    }
    std::fclose(fp);
    return true;
}


// -----------------------------------------------------------------------------
//  .nw level writers
// -----------------------------------------------------------------------------

// RLE compressor for the binary board format.
// Packs the 4096-tile board into a 13-bit-packed byte stream. Two run types:
//   - run of identical tiles:      emit (count|0x1000, tile)
//   - run of 2 alternating tiles:  emit (count|0x1100, tileA, tileB)
//   - single tile:                 emit (tile & 0xfff)
// Then the 13-bit values are packed little-endian into the output stream.
static void RleCompressBoard(const GraalLevel& L, std::vector<unsigned char>& out)
{
    const unsigned short* b = L.tiles;
    std::vector<unsigned> words;
    int i = 0;
    while (i < TILES_PER_LEVEL * TILES_PER_LEVEL) {
        unsigned cur = b[i] & 0xfff;
        if (i < 0xffd) {
            unsigned next = b[i + 1] & 0xfff;
            if (cur == next) {
                // identical run
                int count = 2; i += 2;
                while (count < 0x40 && i < 0x1000 && (b[i] & 0xfff) == cur) {
                    ++count; ++i;
                }
                words.push_back(count | 0x1000);
                words.push_back(cur);
                continue;
            }
            // 2-tile alternation?
            if (i + 3 < 0x1000 && cur == (b[i + 2] & 0xfff) &&
                next == (b[i + 3] & 0xfff))
            {
                int count = 2; i += 4;
                while (count < 0x40 && i < 0xfff &&
                       (b[i] & 0xfff) == cur && (b[i + 1] & 0xfff) == next) {
                    ++count; i += 2;
                }
                words.push_back(count | 0x1100);
                words.push_back(cur);
                words.push_back(next);
                continue;
            }
        }
        words.push_back(cur);
        ++i;
    }
    // Pack 13-bit words into bytes (little-endian bitstream).
    unsigned acc = 0; int bits = 0;
    for (unsigned w : words) {
        acc |= (w & 0x1fff) << (bits & 7);
        bits += 13;
        while (bits >= 8) {
            out.push_back((unsigned char)(acc & 0xff));
            acc >>= 8; bits -= 8;
        }
    }
    if (bits > 0) out.push_back((unsigned char)(acc & 0xff));
}

// Binary .nw writer (GR-V1.03).
static bool WriteLevelBinary(const GraalLevel& L, const std::string& path)
{
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;

    std::fwrite("GR-V1.03", 1, 8, fp);

    // Board (RLE-compressed)
    std::vector<unsigned char> board;
    RleCompressBoard(L, board);
    // (original writes the raw packed bytes; no length prefix)
    std::fwrite(board.data(), 1, board.size(), fp);

    // LINKS section: each "<dest> <w>x<h>+<dx>,<dy>\n", terminated by "\n"
    for (const GraalLink& lk : L.links) {
        std::string s = lk.destLevel + " " +
                        std::to_string((int)lk.width) + "x" +
                        std::to_string((int)lk.height) + "+" +
                        std::to_string((int)lk.destX) + "," +
                        std::to_string((int)lk.destY) + "\n";
        std::fwrite(s.data(), 1, s.size(), fp);
    }
    std::fwrite("\n", 1, 1, fp);

    // SIGNS section: "x y\n" header per sign then text lines, term by 0xFF
    for (const GraalSign& sg : L.signs) {
        std::string s = std::to_string(sg.x) + " " + std::to_string(sg.y) + "\n";
        std::fwrite(s.data(), 1, s.size(), fp);
        // (sign text + colour bytes - format-specific)
    }
    char term[4] = { (char)0xff, (char)0xff, (char)0xff, '\n' };
    std::fwrite(term, 1, 4, fp);

    std::fclose(fp);
    return true;
}

// Text .nw writer (GLEVNW01).
//   "BOARD 0 <row> 64 0 " + 64 tile-letter-pairs per row, 64 rows
//   then LINK/SIGN/NPC sections.
static bool WriteLevelText(const GraalLevel& L, const std::string& path)
{
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;

    std::fprintf(fp, "GLEVNW01\n");
    for (int row = 0; row < TILES_PER_LEVEL; ++row) {
        std::fprintf(fp, "BOARD 0 %d 64 0 ", row);
        for (int x = 0; x < TILES_PER_LEVEL; ++x) {
            unsigned short t = TileAt(L, x, row);
            static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::fputc(alphabet[(t >> 6) & 0x3f], fp);
            std::fputc(alphabet[t & 0x3f], fp);
        }
        std::fprintf(fp, "\n");
    }
    for (const GraalLink& lk : L.links) {
        const char* xVar = lk.destXVar.empty() ? "playerx" : lk.destXVar.c_str();
        const char* yVar = lk.destYVar.empty() ? "playery" : lk.destYVar.c_str();
        int x = lk.destX == 0.0 ? 0 : 63;
        int y = lk.destY == 0.0 ? 0 : 63;
        std::fprintf(fp, "LINK %s.nw %d %d %d %d %s %s\n", lk.destLevel.c_str(), x, y, (int)lk.width, (int)lk.height, xVar, yVar);
    }
    for (const GraalSign& sg : L.signs) {
        std::fprintf(fp, "SIGN %d %d %s\n", sg.x, sg.y, sg.text.c_str());
    }
    for (const GraalNpc& np : L.npcs) {
        std::fprintf(fp, "NPC %.0f %.0f %s\n", np.x, np.y, np.image.c_str());
    }
    std::fclose(fp);
    return true;
}

static bool VerifyBinaryRoundTrip()
{
    GraalLevel written;
    for (int y = 0; y < TILES_PER_LEVEL; ++y) {
        for (int x = 0; x < TILES_PER_LEVEL; ++x) {
            TileAt(written, x, y) = 0x484;
        }
    }
    const char* path = "binary-roundtrip-check.nw";
    if (!WriteLevelBinary(written, path)) return false;
    GraalLevel loaded;
    bool ok = LoadLevel(loaded, path) && std::memcmp(written.tiles, loaded.tiles, sizeof(written.tiles)) == 0;
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    return ok;
}


// -----------------------------------------------------------------------------
//  Colour -> terrain-type table  (from levelgendoc.html)
//    The original compares each image pixel's RGB against these constants and
//    sets the corresponding boolean mask layer, then RenderTerrainLayer() /
//    BlitPattern() stamp the tiles.
// -----------------------------------------------------------------------------
struct ColourEntry { BYTE r, g, b; const char* name; };
static const ColourEntry kColourTable[] = {
    {  24, 140,  24, "grass"        },  // default, not drawn
    { 181, 239, 189, "trees"        },
    {   0,   0, 255, "water"        },
    { 128,   0,   0, "mountains"    },
    { 255,   0,   0, "flowers"      },
    { 150, 110,   0, "sand"         },
    { 113,  82,   0, "sand-stones"  },
    {  83,  61,   0, "big-sand"     },
    {   0, 106,   0, "swamp"        },
    {   0,  66,   0, "bushes"       },
    {   0, 255, 255, "puddle"       },
    {   0, 179, 179, "puddle-stone" },
};

static bool IsTerrainSource(COLORREF color, bool redPass)
{
    return IsColourNear(color, 24, 140, 24) || IsColourNear(color, redPass ? 128 : 0, 0, redPass ? 0 : 255) || (redPass && IsColourNear(color, 0, 106, 0));
}

static void BuildTerrainMask(MapImage& image, BYTE r, BYTE g, BYTE b)
{
    bool redPass = r == 128 && g == 0 && b == 0;
    g_mask.assign(g_maskH, std::vector<char>(g_maskW));
    for (int x = 0; x < g_maskW; ++x) for (int y = 0; y < g_maskH; ++y) {
        int x0 = x * 6 + 2 < image.width ? x * 6 + 2 : image.width - 1;
        int x1 = x * 6 + 7 < image.width ? x * 6 + 7 : image.width - 1;
        int y1 = y * 6 + 5 < image.height ? y * 6 + 5 : image.height - 1;
        COLORREF a = image.At(x0, y * 6);
        COLORREF b0 = image.At(x1, y * 6);
        COLORREF c = image.At(x0, y1);
        COLORREF d = image.At(x1, y1);
        bool noTarget = !IsColourNear(a, r, g, b) && !IsColourNear(b0, r, g, b) && !IsColourNear(c, r, g, b) && !IsColourNear(d, r, g, b);
        g_mask[y][x] = !noTarget && IsTerrainSource(a, redPass) && IsTerrainSource(b0, redPass) && IsTerrainSource(c, redPass) && IsTerrainSource(d, redPass);
        if (g_mask[y][x]) {
            COLORREF fill = a;
            if (!IsColourNear(a, r, g, b) && !IsColourNear(b0, r, g, b)) {
                if (IsColourNear(c, r, g, b)) fill = c;
            }
            int writeX0 = redPass ? x0 : x * 6;
            int writeX1 = redPass ? x1 : (x * 6 + 5 < image.width ? x * 6 + 5 : image.width - 1);
            for (int py = y * 6; py <= y1; ++py) for (int px = writeX0; px <= writeX1; ++px) {
                size_t p = ((size_t)py * image.width + px) * 3;
                image.rgb[p] = GetRValue(fill); image.rgb[p + 1] = GetGValue(fill); image.rgb[p + 2] = GetBValue(fill);
            }
        }
    }
}

static void BuildOverlayMask(const MapImage& image, BYTE r, BYTE g, BYTE b)
{
    g_mask.assign(g_maskH, std::vector<char>(g_maskW));
    for (int x = 0; x < g_maskW; ++x) for (int y = 0; y < g_maskH; ++y) {
        int half = (y & 1) ? -1 : 0;
        int bx = x * 6 - 1 + half * 3, by = y * 6;
        int cx = bx + 6, cy = by + 2;
        if (cx < 0 || cx >= image.width || cy < 0 || cy >= image.height || !IsColourNear(image.At(cx, cy), r, g, b)) continue;
        bool foreign = false;
        for (int py = 0; py < 10 && !foreign; ++py) for (int px = 0; px < 8; ++px) {
            int sx = bx + px + 2, sy = by + py;
            if (sx < 0 || sx >= image.width || sy < 0 || sy >= image.height) continue;
            COLORREF c = image.At(sx, sy);
            if (!IsColourNear(c, 24, 140, 24) && !IsColourNear(c, r, g, b)) { foreign = true; break; }
        }
        g_mask[y][x] = !foreign;
    }
}

#ifdef _WIN32
static std::wstring ToWide(const std::string& value)
{
    if (value.empty()) return L"";
    int size = MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, &result[0], size);
    result.resize(size - 1);
    return result;
}

static std::string ToAnsi(const wchar_t* value)
{
    int size = WideCharToMultiByte(CP_ACP, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_ACP, 0, value, -1, &result[0], size, nullptr, nullptr);
    result.resize(size - 1);
    return result;
}

static std::string GetApplicationRoot()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::filesystem::path directory = std::filesystem::path(path).parent_path();
    if (std::filesystem::is_directory(directory / "examples")) return directory.string();
    return directory.parent_path().string();
}

static bool EnsureDirectory(const std::string& path)
{
    char absolute[MAX_PATH] = {};
    DWORD length = GetFullPathNameA(path.c_str(), MAX_PATH, absolute, nullptr);
    if (!length || length >= MAX_PATH) return false;
    int result = SHCreateDirectoryExA(nullptr, absolute, nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}
#else
static bool EnsureDirectory(const std::string& path)
{
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error && std::filesystem::is_directory(path);
}
#endif

static bool LoadMapImage(const std::string& path, MapImage& image, std::string& err)
{
#ifdef _WIN32
    static ULONG_PTR token = 0;
    if (!token) {
        GdiplusStartupInput input;
        if (GdiplusStartup(&token, &input, nullptr) != Ok) {
            err = "Could not start the image decoder.";
            return false;
        }
    }
    Bitmap bitmap(ToWide(path).c_str(), FALSE);
    if (bitmap.GetLastStatus() != Ok) {
        err = "Could not open the map image. PNG and GIF images are supported.";
        return false;
    }
    image.width = (int)bitmap.GetWidth();
    image.height = (int)bitmap.GetHeight();
    if (image.width <= 0 || image.height <= 0) {
        err = "The map image has no pixels.";
        return false;
    }
    image.rgb.resize((size_t)image.width * image.height * 3);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            Color color;
            bitmap.GetPixel(x, y, &color);
            size_t p = ((size_t)y * image.width + x) * 3;
            image.rgb[p] = color.GetRed();
            image.rgb[p + 1] = color.GetGreen();
            image.rgb[p + 2] = color.GetBlue();
        }
    }
    return true;
#else
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) { err = "Could not open the map image."; return false; }
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info) { if (png) png_destroy_read_struct(&png, nullptr, nullptr); std::fclose(file); err = "Could not start the PNG decoder."; return false; }
    if (setjmp(png_jmpbuf(png))) { png_destroy_read_struct(&png, &info, nullptr); std::fclose(file); err = "Could not decode the map PNG."; return false; }
    png_init_io(png, file); png_read_info(png, info);
    png_uint_32 width = 0, height = 0; int depth = 0, colourType = 0;
    png_get_IHDR(png, info, &width, &height, &depth, &colourType, nullptr, nullptr, nullptr);
    if (depth == 16) png_set_strip_16(png);
    if (colourType == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (colourType == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (colourType == PNG_COLOR_TYPE_GRAY || colourType == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_read_update_info(png, info);
    if (!width || !height) { png_destroy_read_struct(&png, &info, nullptr); std::fclose(file); err = "The map image has no pixels."; return false; }
    image.width = (int)width; image.height = (int)height; image.rgb.resize((size_t)width * height * 3);
    size_t rowBytes = png_get_rowbytes(png, info); std::vector<BYTE> row(rowBytes);
    int channels = png_get_channels(png, info);
    for (int y = 0; y < image.height; ++y) { png_read_row(png, row.data(), nullptr); for (int x = 0; x < image.width; ++x) { size_t in = (size_t)x * channels, out = ((size_t)y * image.width + x) * 3; image.rgb[out] = row[in]; image.rgb[out + 1] = row[in + 1]; image.rgb[out + 2] = row[in + 2]; } }
    png_read_end(png, nullptr); png_destroy_read_struct(&png, &info, nullptr); std::fclose(file); return true;
#endif
}

static unsigned short TileForColour(COLORREF color)
{
    BYTE r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
    if (r == 181 && g == 239 && b == 189) return 0x484;
    if (r == 0 && g == 0 && b == 255) return 0x0e1;
    if (r == 128 && g == 0 && b == 0) return 0x06f;
    if (r == 255 && g == 0 && b == 0) return 0x4b7;
    if (r == 150 && g == 110 && b == 0) return 0x491;
    if (r == 113 && g == 82 && b == 0) return 0x497;
    if (r == 83 && g == 61 && b == 0) return 0x4a7;
    if (r == 0 && g == 106 && b == 0) return 0x381;
    if (r == 0 && g == 66 && b == 0) return 0x390;
    if (r == 0 && g == 255 && b == 255) return 0x0c0;
    if (r == 0 && g == 179 && b == 179) return 0x0d0;
    return 0x142;
}

static void PaintMap(const MapImage& image)
{
    MapImage working = image;
    for (GraalLevel& level : g_levels) if (!level.existing) std::fill(std::begin(level.tiles), std::end(level.tiles), 0x7ff);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            COLORREF color = working.At(x, y);
            if ((GetRValue(color) == 128 && GetGValue(color) == 0 && GetBValue(color) == 0) || (GetRValue(color) == 150 && GetGValue(color) == 110 && GetBValue(color) == 0)) continue;
            int level = (x / TILES_PER_LEVEL) + (y / TILES_PER_LEVEL) * g_levelCols;
            TileAt(g_levels[level], x % TILES_PER_LEVEL, y % TILES_PER_LEVEL) = 0x7ff;
        }
    }
    g_maskW = (g_levelCols * TILES_PER_LEVEL + 5) / 6;
    g_maskH = (g_levelRows * TILES_PER_LEVEL + 5) / 6;
    BuildTerrainMask(working, 128, 0, 0);
    RenderCliffTerrain();
    for (int x = 0; x < working.width; ++x) for (int y = 0; y < working.height; ++y) {
        RenderFlowers(working, x, y);
        RenderSandStone(working, x, y);
        RenderPuddles(working, x, y);
        RenderSwampsAndBushes(working, x, y);
    }
    BuildTerrainMask(working, 0, 0, 255);
    RenderWaterTerrain();
    BuildOverlayMask(working, 181, 239, 189);
    RenderMountainTerrain();
    BuildOverlayMask(working, 196, 0, 0);
    RenderSandTerrain();
}


// -----------------------------------------------------------------------------
//  .gmap writer  (original: referenced via LEVELNAMES/LEVELNAMESEND strings)
//    A .gmap is a text file listing all level filenames so Graal loads them
//    as one contiguous world.
// -----------------------------------------------------------------------------
static bool WriteGmap(const std::string& path, const std::string& filePrefix)
{
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::fprintf(fp, "GRMAP001\r\nWIDTH %d\r\nHEIGHT %d\r\nLEVELNAMES\r\n", g_levelCols, g_levelRows);
    for (int row = 0; row < g_levelRows; ++row) {
        for (int col = 0; col < g_levelCols; ++col) {
            std::fprintf(fp, "%s\"%s.nw\"", col == 0 ? "" : ",", LevelFileStem(filePrefix, LevelName(col, row)).c_str());
        }
        std::fprintf(fp, "\r\n");
    }
    std::fprintf(fp, "LEVELNAMESEND\r\n");
    std::fclose(fp);
    return true;
}


// -----------------------------------------------------------------------------
//  Top-level world generation
// -----------------------------------------------------------------------------
struct GenOptions {
    std::string imagePath;    // Edit1
    std::string outputFolder; // Edit2
    std::string filePrefix;   // Edit3 (e.g. "onlinestartlevel")
    std::string baselineFolder;
    bool createLinks = true;
};

static bool GenerateWorld(const GenOptions& opt, std::string& err)
{
    if (opt.imagePath.empty() || opt.outputFolder.empty() || opt.filePrefix.empty()) {
        err = "Select a map image, an output folder, and a filename prefix.";
        return false;
    }
    if (!EnsureDirectory(opt.outputFolder)) {
        err = "Could not create the output folder.";
        return false;
    }
    std::error_code outputError;
    if (!std::filesystem::is_directory(opt.outputFolder, outputError)) {
        err = "The output path is not a folder.";
        return false;
    }
    MapImage image;
    if (!LoadMapImage(opt.imagePath, image, err)) return false;
    if (image.width % TILES_PER_LEVEL || image.height % TILES_PER_LEVEL) {
        err = "The map image width and height must both be multiples of 64 pixels.";
        return false;
    }
    g_levelCols = image.width / TILES_PER_LEVEL;
    g_levelRows = image.height / TILES_PER_LEVEL;
    if (g_levelCols == 0 || g_levelRows == 0) {
        err = "The map image must be at least 64 by 64 pixels.";
        return false;
    }

    BuildLevelGrid(opt.outputFolder, opt.filePrefix, opt.createLinks, opt.baselineFolder);
    PaintMap(image);

    // Write every level as the original text-compatible GLEVNW01 format.
    for (GraalLevel& L : g_levels) {
        std::string path = JoinPath(opt.outputFolder, LevelFileStem(opt.filePrefix, L.name) + ".nw");
        if (!WriteLevelText(L, path)) {
            err = "Could not write " + path;
            return false;
        }
    }

    if (!WriteGmap(JoinPath(opt.outputFolder, GmapName(opt.filePrefix) + ".gmap"), opt.filePrefix)) {
        err = "Could not write the generated .gmap file.";
        return false;
    }

    err.clear();
    return true;
}


#ifdef _WIN32
struct AppWindow {
    HWND hMain = nullptr;
    HWND hEditImage = nullptr,  hBtnBrowseImage = nullptr;
    HWND hEditFolder = nullptr, hBtnBrowseFolder = nullptr;
    HWND hEditPrefix = nullptr;
    HWND hChkLinks = nullptr;
    HWND hBtnGenerate = nullptr, hBtnDesigner = nullptr;
    HFONT hFont = nullptr;
};

static AppWindow g_app;

static void ApplyFont(HWND h) {
    SendMessageW(h, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
}

struct DesignerState {
    HWND window = nullptr, widthEdit = nullptr, heightEdit = nullptr, levelsLabel = nullptr, zoomLabel = nullptr;
    int width = 32, height = 32;
    int zoom = 1;
    int scrollX = 0, scrollY = 0;
    bool panning = false;
    int panStartX = 0, panStartY = 0, panScrollX = 0, panScrollY = 0;
    COLORREF selected = RGB(24, 140, 24);
    int selectedPalette = 0, tooltipPalette = -1, tooltipX = 0, tooltipY = 0;
    std::vector<COLORREF> pixels = std::vector<COLORREF>(32 * 32, RGB(24, 140, 24));
};

static DesignerState g_designer;
static const COLORREF kDesignerColours[] = { RGB(24,140,24), RGB(181,239,189), RGB(0,0,255), RGB(128,0,0), RGB(255,0,0), RGB(150,110,0), RGB(113,82,0), RGB(83,61,0), RGB(0,106,0), RGB(0,66,0), RGB(0,255,255), RGB(0,179,179) };
static const wchar_t* kDesignerColourNames[] = { L"Grass", L"Trees", L"Water", L"Mountains", L"Flowers", L"Sand", L"Sand stones", L"Big sand", L"Swamp", L"Bushes", L"Puddle", L"Puddle stone" };

static int DesignerCellSize()
{
    int largest = g_designer.width > g_designer.height ? g_designer.width : g_designer.height;
    int cell = 400 / largest * g_designer.zoom;
    return cell < 2 ? 2 : cell;
}

static void UpdateDesignerScrollbars(HWND h)
{
    const int gridX = 20, gridY = 125;
    RECT client{}; GetClientRect(h, &client);
    int contentW = g_designer.width * DesignerCellSize(), contentH = g_designer.height * DesignerCellSize();
    int pageW = client.right - gridX, pageH = client.bottom - gridY;
    if (pageW < 1) pageW = 1; if (pageH < 1) pageH = 1;
    int maxX = contentW > pageW ? contentW - pageW : 0, maxY = contentH > pageH ? contentH - pageH : 0;
    if (g_designer.scrollX > maxX) g_designer.scrollX = maxX;
    if (g_designer.scrollY > maxY) g_designer.scrollY = maxY;
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_RANGE|SIF_PAGE|SIF_POS;
    info.nMin = 0; info.nMax = contentW; info.nPage = pageW; info.nPos = g_designer.scrollX; SetScrollInfo(h, SB_HORZ, &info, TRUE);
    info.nMax = contentH; info.nPage = pageH; info.nPos = g_designer.scrollY; SetScrollInfo(h, SB_VERT, &info, TRUE);
}

static void UpdateDesignerInfo()
{
    wchar_t text[64] = {};
    wsprintfW(text, L"Levels: %d x %d", (g_designer.width + 63) / 64, (g_designer.height + 63) / 64);
    SetWindowTextW(g_designer.levelsLabel, text);
    wsprintfW(text, L"Zoom: %d00%%", g_designer.zoom);
    SetWindowTextW(g_designer.zoomLabel, text);
}

static bool SaveDesignerPng(const wchar_t* path)
{
    GdiplusStartupInput input; ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) return false;
    Bitmap bitmap(g_designer.width, g_designer.height, PixelFormat32bppARGB);
    for (int y = 0; y < g_designer.height; ++y) for (int x = 0; x < g_designer.width; ++x) {
        COLORREF c = g_designer.pixels[y * g_designer.width + x];
        bitmap.SetPixel(x, y, Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    }
    UINT count = 0, bytes = 0; GetImageEncodersSize(&count, &bytes);
    std::vector<BYTE> data(bytes); ImageCodecInfo* codecs = (ImageCodecInfo*)data.data();
    GetImageEncoders(count, bytes, codecs); CLSID png{}; bool found = false;
    for (UINT i = 0; i < count; ++i) if (wcscmp(codecs[i].MimeType, L"image/png") == 0) { png = codecs[i].Clsid; found = true; break; }
    bool ok = found && bitmap.Save(path, &png, nullptr) == Ok;
    GdiplusShutdown(token); return ok;
}

static bool LoadDesignerMap(const wchar_t* path)
{
    MapImage image; std::string error;
    if (!LoadMapImage(ToAnsi(path), image, error) || image.width < 8 || image.height < 8 || image.width > 128 || image.height > 128) return false;
    g_designer.width = image.width; g_designer.height = image.height; g_designer.pixels.resize((size_t)image.width * image.height);
    for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) g_designer.pixels[(size_t)y * image.width + x] = image.At(x, y);
    wchar_t value[16] = {}; wsprintfW(value, L"%d", image.width); SetWindowTextW(g_designer.widthEdit, value);
    wsprintfW(value, L"%d", image.height); SetWindowTextW(g_designer.heightEdit, value);
    UpdateDesignerInfo(); UpdateDesignerScrollbars(g_designer.window); InvalidateRect(g_designer.window, nullptr, TRUE); return true;
}

static void ResetDesigner()
{
    wchar_t text[16] = {}; GetWindowTextW(g_designer.widthEdit, text, 16); int width = _wtoi(text);
    GetWindowTextW(g_designer.heightEdit, text, 16); int height = _wtoi(text);
    g_designer.width = width < 8 ? 8 : (width > 128 ? 128 : width);
    g_designer.height = height < 8 ? 8 : (height > 128 ? 128 : height);
    g_designer.pixels.assign(g_designer.width * g_designer.height, RGB(24, 140, 24));
    UpdateDesignerInfo();
    UpdateDesignerScrollbars(g_designer.window);
    InvalidateRect(g_designer.window, nullptr, TRUE);
}

static void PaintDesignerCell(int x, int y)
{
    const int gridX = 20, gridY = 125;
    int cell = DesignerCellSize(), col = (x - gridX + g_designer.scrollX) / cell, row = (y - gridY + g_designer.scrollY) / cell;
    if (col >= 0 && row >= 0 && col < g_designer.width && row < g_designer.height) {
        g_designer.pixels[row * g_designer.width + col] = g_designer.selected;
        InvalidateRect(g_designer.window, nullptr, FALSE);
    }
}

static LRESULT CALLBACK DesignerWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    const int paletteY = 88, gridX = 20, gridY = 125;
    switch (msg) {
    case WM_CREATE: {
        g_designer.window = h;
        CreateWindowW(L"STATIC", L"Map size:", WS_CHILD|WS_VISIBLE, 20, 18, 55, 18, h, nullptr, nullptr, nullptr);
        g_designer.widthEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"32", WS_CHILD|WS_VISIBLE|ES_NUMBER, 78, 16, 42, 21, h, (HMENU)201, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"x", WS_CHILD|WS_VISIBLE, 126, 18, 12, 18, h, nullptr, nullptr, nullptr);
        g_designer.heightEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"32", WS_CHILD|WS_VISIBLE|ES_NUMBER, 140, 16, 42, 21, h, (HMENU)202, nullptr, nullptr);
        HWND reset = CreateWindowW(L"BUTTON", L"New grid", WS_CHILD|WS_VISIBLE, 198, 15, 78, 24, h, (HMENU)203, nullptr, nullptr);
        HWND save = CreateWindowW(L"BUTTON", L"Save map...", WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON, 286, 15, 94, 24, h, (HMENU)204, nullptr, nullptr);
        g_designer.levelsLabel = CreateWindowW(L"STATIC", L"Levels: 1 x 1", WS_CHILD|WS_VISIBLE, 20, 42, 120, 18, h, nullptr, nullptr, nullptr);
        HWND load = CreateWindowW(L"BUTTON", L"Load map...", WS_CHILD|WS_VISIBLE, 140, 40, 88, 24, h, (HMENU)207, nullptr, nullptr);
        HWND zoomOut = CreateWindowW(L"BUTTON", L"Zoom -", WS_CHILD|WS_VISIBLE, 238, 42, 62, 22, h, (HMENU)205, nullptr, nullptr);
        HWND zoomIn = CreateWindowW(L"BUTTON", L"Zoom +", WS_CHILD|WS_VISIBLE, 304, 42, 62, 22, h, (HMENU)206, nullptr, nullptr);
        g_designer.zoomLabel = CreateWindowW(L"STATIC", L"Zoom: 100%", WS_CHILD|WS_VISIBLE, 370, 45, 75, 18, h, nullptr, nullptr, nullptr);
        for (HWND c : {g_designer.widthEdit,g_designer.heightEdit,reset,save,load,g_designer.levelsLabel,zoomOut,zoomIn,g_designer.zoomLabel}) ApplyFont(c);
        UpdateDesignerInfo();
        UpdateDesignerScrollbars(h);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        if (y >= paletteY && y < paletteY + 24 && x >= 20 && x < 20 + (int)(sizeof(kDesignerColours) / sizeof(kDesignerColours[0])) * 30) {
            g_designer.selectedPalette = (x - 20) / 30; g_designer.selected = kDesignerColours[g_designer.selectedPalette]; InvalidateRect(h, nullptr, FALSE); return 0;
        }
        SetCapture(h); PaintDesignerCell(x, y);
        return 0;
    }
    case WM_MOUSEMOVE:
        {
            int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
            int hovered = y >= paletteY && y < paletteY + 24 && x >= 20 && x < 20 + (int)(sizeof(kDesignerColours) / sizeof(kDesignerColours[0])) * 30 ? (x - 20) / 30 : -1;
            if (hovered != g_designer.tooltipPalette || x != g_designer.tooltipX || y != g_designer.tooltipY) { g_designer.tooltipPalette = hovered; g_designer.tooltipX = x; g_designer.tooltipY = y; InvalidateRect(h, nullptr, FALSE); }
        }
        if (g_designer.panning) {
            g_designer.scrollX = g_designer.panScrollX - ((int)(short)LOWORD(lp) - g_designer.panStartX);
            g_designer.scrollY = g_designer.panScrollY - ((int)(short)HIWORD(lp) - g_designer.panStartY);
            UpdateDesignerScrollbars(h); InvalidateRect(h, nullptr, FALSE); return 0;
        }
        if (wp & MK_LBUTTON) PaintDesignerCell((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
        return 0;
    case WM_LBUTTONUP: ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN:
        g_designer.panning = true; g_designer.panStartX = (int)(short)LOWORD(lp); g_designer.panStartY = (int)(short)HIWORD(lp);
        g_designer.panScrollX = g_designer.scrollX; g_designer.panScrollY = g_designer.scrollY; SetCapture(h); return 0;
    case WM_RBUTTONUP: g_designer.panning = false; ReleaseCapture(); return 0;
    case WM_SIZE: UpdateDesignerScrollbars(h); return 0;
    case WM_HSCROLL:
    case WM_VSCROLL: {
        int bar = msg == WM_HSCROLL ? SB_HORZ : SB_VERT, code = LOWORD(wp);
        SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL; GetScrollInfo(h, bar, &info);
        int pos = info.nPos;
        if (code == SB_LINELEFT || code == SB_LINEUP) pos -= DesignerCellSize();
        else if (code == SB_LINERIGHT || code == SB_LINEDOWN) pos += DesignerCellSize();
        else if (code == SB_PAGELEFT || code == SB_PAGEUP) pos -= (int)info.nPage;
        else if (code == SB_PAGERIGHT || code == SB_PAGEDOWN) pos += (int)info.nPage;
        else if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION) pos = info.nTrackPos;
        int limit = info.nMax - (int)info.nPage + 1; if (limit < 0) limit = 0;
        if (pos < 0) pos = 0; if (pos > limit) pos = limit;
        if (bar == SB_HORZ) g_designer.scrollX = pos; else g_designer.scrollY = pos;
        SetScrollPos(h, bar, pos, TRUE); InvalidateRect(h, nullptr, FALSE); return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 203) ResetDesigner();
        if (LOWORD(wp) == 205 && g_designer.zoom > 1) { --g_designer.zoom; UpdateDesignerInfo(); UpdateDesignerScrollbars(h); InvalidateRect(h, nullptr, TRUE); }
        if (LOWORD(wp) == 206 && g_designer.zoom < 3) { ++g_designer.zoom; UpdateDesignerInfo(); UpdateDesignerScrollbars(h); InvalidateRect(h, nullptr, TRUE); }
        if (LOWORD(wp) == 207) {
            wchar_t path[MAX_PATH] = L""; OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = h; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"Map images (*.png;*.gif)\0*.png;*.gif\0"; ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn) && !LoadDesignerMap(path)) MessageBoxW(h, L"Map Designer accepts PNG or GIF images from 8 to 128 pixels wide and high.", L"Map Designer", MB_ICONERROR);
        }
        if (LOWORD(wp) == 204) {
            wchar_t path[MAX_PATH] = L"map.png"; OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = h;
            ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.lpstrFilter = L"PNG image (*.png)\0*.png\0"; ofn.lpstrDefExt = L"png";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                if (SaveDesignerPng(path)) { SetWindowTextW(g_app.hEditImage, path); MessageBoxW(h, L"Map image saved and selected.", L"Map Designer", MB_OK); }
                else MessageBoxW(h, L"Could not save the map image.", L"Map Designer", MB_ICONERROR);
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(h, &ps); SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, 20, 68, L"Palette", 7);
        for (int i = 0; i < (int)(sizeof(kDesignerColours) / sizeof(kDesignerColours[0])); ++i) {
            HBRUSH brush = CreateSolidBrush(kDesignerColours[i]); RECT r{20 + i * 30, paletteY, 44 + i * 30, paletteY + 24}; FillRect(dc, &r, brush); DeleteObject(brush);
            if (i == g_designer.selectedPalette) { InflateRect(&r, 2, 2); FrameRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
        }
        int cell = DesignerCellSize();
        int saved = SaveDC(dc); RECT client{}; GetClientRect(h, &client); IntersectClipRect(dc, gridX, gridY, client.right, client.bottom);
        for (int y = 0; y < g_designer.height; ++y) for (int x = 0; x < g_designer.width; ++x) {
            HBRUSH brush = CreateSolidBrush(g_designer.pixels[y * g_designer.width + x]); RECT r{gridX + x * cell - g_designer.scrollX, gridY + y * cell - g_designer.scrollY, gridX + (x + 1) * cell - g_designer.scrollX, gridY + (y + 1) * cell - g_designer.scrollY}; FillRect(dc, &r, brush); DeleteObject(brush); FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        }
        RestoreDC(dc, saved);
        if (g_designer.tooltipPalette >= 0) {
            const wchar_t* label = kDesignerColourNames[g_designer.tooltipPalette]; SIZE size{}; GetTextExtentPoint32W(dc, label, (int)wcslen(label), &size);
            RECT tooltip{g_designer.tooltipX + 12, g_designer.tooltipY + 12, g_designer.tooltipX + size.cx + 20, g_designer.tooltipY + size.cy + 18}; FillRect(dc, &tooltip, (HBRUSH)GetStockObject(WHITE_BRUSH)); FrameRect(dc, &tooltip, (HBRUSH)GetStockObject(BLACK_BRUSH)); TextOutW(dc, tooltip.left + 4, tooltip.top + 3, label, (int)wcslen(label));
        }
        EndPaint(h, &ps); return 0;
    }
    case WM_DESTROY: g_designer.window = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenMapDesigner(HINSTANCE instance)
{
    if (g_designer.window) { ShowWindow(g_designer.window, SW_RESTORE); SetForegroundWindow(g_designer.window); return; }
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = DesignerWndProc; wc.hInstance = instance; wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR); wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR); wc.lpszClassName = L"LevelGenMapDesigner";
    RegisterClassExW(&wc);
    RECT main{}; GetWindowRect(g_app.hMain, &main);
    HWND drawer = CreateWindowExW(WS_EX_APPWINDOW, L"LevelGenMapDesigner", L"Map Designer", WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_HSCROLL|WS_VSCROLL, main.right + 8, main.top, 430, 560, g_app.hMain, nullptr, instance, nullptr);
    SendMessageW(drawer, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    SendMessageW(drawer, WM_SETICON, ICON_BIG, (LPARAM)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    ShowWindow(g_designer.window, SW_SHOW);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_app.hMain = h;
        std::wstring defaultImage = ToWide(GetApplicationRoot() + "\\examples\\levelgen.png");
        std::wstring defaultOutput = ToWide(GetApplicationRoot() + "\\levels");
        g_app.hFont = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, 0,
                                  L"MS Sans Serif");
        HWND group = CreateWindowExW(0, L"BUTTON", L"Filenames", WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
                                     8, 7, 528, 114, h, nullptr, nullptr, nullptr);
        g_app.hEditImage = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", defaultImage.c_str(), WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                                           145, 31, 304, 23, h, (HMENU)101, nullptr, nullptr);
        g_app.hBtnBrowseImage = CreateWindowExW(0, L"BUTTON", L"Browse",
                                                WS_CHILD|WS_VISIBLE, 451, 30, 77, 25,
                                                h, (HMENU)102, nullptr, nullptr);
        g_app.hEditFolder = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", defaultOutput.c_str(), WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                                            145, 63, 304, 23, h, (HMENU)103, nullptr, nullptr);
        g_app.hBtnBrowseFolder = CreateWindowExW(0, L"BUTTON", L"Browse",
                                                 WS_CHILD|WS_VISIBLE, 451, 62, 77, 25,
                                                 h, (HMENU)104, nullptr, nullptr);
        g_app.hEditPrefix = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"myworld_", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                                            201, 95, 248, 23, h, (HMENU)105, nullptr, nullptr);
        g_app.hChkLinks = CreateWindowExW(0, L"BUTTON", L"Create level links", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,
                                           25, 124, 150, 18, h, (HMENU)107, nullptr, nullptr);
        SendMessageW(g_app.hChkLinks, BM_SETCHECK, BST_CHECKED, 0);
        g_app.hBtnGenerate = CreateWindowExW(0, L"BUTTON", L"&Generate World",
                                             WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON, 394, 130, 135, 27,
                                             h, (HMENU)106, nullptr, nullptr);
        g_app.hBtnDesigner = CreateWindowExW(0, L"BUTTON", L"Map designer...",
                                             WS_CHILD|WS_VISIBLE, 270, 130, 116, 27,
                                             h, (HMENU)108, nullptr, nullptr);
        HWND labelImage = CreateWindowExW(0, L"STATIC", L"Map image:", WS_CHILD|WS_VISIBLE,
                                          25, 35, 110, 15, h, nullptr, nullptr, nullptr);
        HWND labelFolder = CreateWindowExW(0, L"STATIC", L"Output folder:", WS_CHILD|WS_VISIBLE,
                                           25, 67, 110, 15, h, nullptr, nullptr, nullptr);
        HWND labelPrefix = CreateWindowExW(0, L"STATIC", L"Filenames start with:", WS_CHILD|WS_VISIBLE,
                                           25, 99, 165, 15, h, nullptr, nullptr, nullptr);
        for (HWND c : {g_app.hEditImage,g_app.hBtnBrowseImage,g_app.hEditFolder,
                       g_app.hBtnBrowseFolder,g_app.hEditPrefix,g_app.hChkLinks,g_app.hBtnGenerate,g_app.hBtnDesigner,group,
                       labelImage,labelFolder,labelPrefix})
            ApplyFont(c);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 102) {  // Browse image
            char file[MAX_PATH] = "";
            OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = h; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = "Map images (*.png, *.mng, *.gif)|*.png;*.mng;*.gif|All files|*.*";
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
            ofn.lpstrTitle = "Load Map Image";
            if (GetOpenFileNameA(&ofn))
                SetWindowTextA(g_app.hEditImage, file);
        } else if (LOWORD(wp) == 104) {  // Browse folder
            BROWSEINFOA bi{}; char path[MAX_PATH] = "";
            bi.hwndOwner = h; bi.pszDisplayName = path;
            bi.lpszTitle = "Select output folder";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST idl = SHBrowseForFolderA(&bi);
            if (idl) { SHGetPathFromIDListA(idl, path); CoTaskMemFree(idl);
                       SetWindowTextA(g_app.hEditFolder, path); }
        } else if (LOWORD(wp) == 108) {
            OpenMapDesigner((HINSTANCE)GetModuleHandleW(nullptr));
        } else if (LOWORD(wp) == 106) {  // Generate
            char img[MAX_PATH]={}, fol[MAX_PATH]={}, pre[64]={};
            GetWindowTextA(g_app.hEditImage, img, MAX_PATH);
            GetWindowTextA(g_app.hEditFolder, fol, MAX_PATH);
            GetWindowTextA(g_app.hEditPrefix, pre, 64);
            GenOptions opt{ img, fol, pre, "", SendMessageW(g_app.hChkLinks, BM_GETCHECK, 0, 0) == BST_CHECKED };
            std::string err;
            if (GenerateWorld(opt, err))
                MessageBoxA(h, "World generated successfully.", "Graal Level Generator", MB_OK);
            else
                MessageBoxA(h, err.c_str(), "Error", MB_ICONERROR);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR commandLine, int show)
{
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments && argumentCount == 2 && std::wcscmp(arguments[1], L"--verify-binary") == 0) {
        LocalFree(arguments);
        return VerifyBinaryRoundTrip() ? 0 : 1;
    }
    if (arguments && argumentCount > 1 && std::wcscmp(arguments[1], L"--cli") == 0) {
        GenOptions options;
        for (int i = 2; i < argumentCount; ++i) {
            if (std::wcscmp(arguments[i], L"--image") == 0 && i + 1 < argumentCount) options.imagePath = ToAnsi(arguments[++i]);
            else if (std::wcscmp(arguments[i], L"--output") == 0 && i + 1 < argumentCount) options.outputFolder = ToAnsi(arguments[++i]);
            else if (std::wcscmp(arguments[i], L"--prefix") == 0 && i + 1 < argumentCount) options.filePrefix = ToAnsi(arguments[++i]);
            else if (std::wcscmp(arguments[i], L"--baseline") == 0 && i + 1 < argumentCount) options.baselineFolder = ToAnsi(arguments[++i]);
            else if (std::wcscmp(arguments[i], L"--no-links") == 0) options.createLinks = false;
            else { LocalFree(arguments); return 2; }
        }
        LocalFree(arguments);
        std::string error;
        return GenerateWorld(options, error) ? 0 : 1;
    }
    if (arguments) LocalFree(arguments);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.lpszClassName = L"LevelGenWindow";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

    HWND h = CreateWindowExW(WS_EX_APPWINDOW, L"LevelGenWindow", L"Level Generator",
        WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 554, 204, nullptr, nullptr, hInst, nullptr);
    if (!h) {
        wchar_t error[96] = {};
        wsprintfW(error, L"Could not create the main window. Error %lu.", GetLastError());
        MessageBoxW(nullptr, error, L"Level Generator", MB_ICONERROR);
        return 1;
    }
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    ShowWindow(h, SW_RESTORE);
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    UpdateWindow(h);
    MSG m; while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    return 0;
}
#else
int RunLevelGenCli(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--verify-binary") == 0) return VerifyBinaryRoundTrip() ? 0 : 1;
    if (argc < 2 || std::strcmp(argv[1], "--cli") != 0) { std::fprintf(stderr, "Use: LevelGen --cli --image <map.png> --output <folder> --prefix <name> [--baseline <folder>] [--no-links]\n"); return 2; }
    GenOptions options;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--image") == 0 && i + 1 < argc) options.imagePath = argv[++i];
        else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) options.outputFolder = argv[++i];
        else if (std::strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) options.filePrefix = argv[++i];
        else if (std::strcmp(argv[i], "--baseline") == 0 && i + 1 < argc) options.baselineFolder = argv[++i];
        else if (std::strcmp(argv[i], "--no-links") == 0) options.createLinks = false;
        else return 2;
    }
    std::string error;
    if (GenerateWorld(options, error)) return 0;
    std::fprintf(stderr, "%s\n", error.c_str()); return 1;
}
#ifndef LEVELGEN_PORTABLE_GUI
int main(int argc, char** argv)
{
    return RunLevelGenCli(argc, argv);
}
#endif
#endif
