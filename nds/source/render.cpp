// DS renderer, step 1: the tile layers. Draws the engine's 320x240 logic screen cropped 1:1 to the 256x192 window
// (logic x 32..287, y 24..215) into a RAM buffer in the DS's 15-bit format; main.cpp copies it to the screen.
// No libnds here (it clashes with the engine's headers): the buffer is exported through NdsFrameBuffer().
#include "RetroEngine.hpp"

#include "ndsdraw.hpp"

byte tilesetGFXData[TILESET_SIZE]; // the stage's 16x16 tiles, 8-bit (PS1 keeps them only in VRAM)
uint16_t *g_ndsDst = nullptr;
byte graphicData[GFXDATA_SIZE]; // the stage's sprite sheets (8-bit)
static int s_clearIndex = 0;
static bool s_drawn = false;      // a frame was drawn since main last took it (a paused game draws nothing)
extern volatile uint32_t g_ndsBlitTicks, g_ndsBlitCount, g_ndsBlitPix; // sprites.cpp
volatile uint32_t g_ndsRenderTicks = 0;
volatile uint32_t g_ndsTkClear = 0, g_ndsTkTiles = 0, g_ndsTkObj = 0; // split of the draw: bitmap clear / tile passes / object lists // ARM9 ticks (33.5 MHz) DrawStageGFX took last

#define ITCM_CODE __attribute__((section(".itcm"), long_call))
#define HOT ITCM_CODE __attribute__((optimize("O3"), target("arm")))

uint32_t g_ndsFlick = 0;
static int s_page = 0;                  // which of the two bitmap pages g_ndsDst is
static bool s_allDirty[2] = { false, false };
static uint16_t s_dirty[2][WIN_H][2];   // per page and row: touched x range [x0, x1) in pixels (x0 >= x1: clean)
static void ResetDirty(int pg)
{
    for (int y = 0; y < WIN_H; ++y) {
        s_dirty[pg][y][0] = 0xFFFF;
        s_dirty[pg][y][1] = 0;
    }
    s_allDirty[pg] = false;
}
void NdsDirtyAll() { s_allDirty[s_page] = true; }
void NdsMarkDirty(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > WIN_W) x1 = WIN_W;
    if (y1 > WIN_H) y1 = WIN_H;
    for (int y = y0; y < y1; ++y) {
        uint16_t *d = s_dirty[s_page][y];
        if (x0 < d[0]) d[0] = (uint16_t)x0;
        if (x1 > d[1]) d[1] = (uint16_t)x1;
    }
}
extern "C" void NdsSetBackBuffer(uint16_t *p)
{
    static bool init = false;
    if (!init) {
        init = true;
        ResetDirty(0);
        ResetDirty(1);
    }
    g_ndsDst = p;
    s_page   = (uintptr_t)p == 0x06020000 ? 0 : 1;
}
extern "C" int NdsFrameDrawn(void)
{
    int d   = s_drawn;
    s_drawn = false;
    return d;
}
extern "C" uint32_t NdsRenderTicks(void) { return g_ndsRenderTicks; }

// cpuStartTiming's pair of cascaded timers (TIMER0 data = low half, TIMER1 data = high half)
static inline uint32_t Ticks()
{
    volatile uint16_t *t = (volatile uint16_t *)0x04000100;
    uint32_t hi, lo;
    do {
        hi = t[2];
        lo = t[0];
    } while (hi != t[2]);
    return (hi << 16) | lo;
}

static void HwReset();

// ---- the stage's 16x16 tiles (upstream LoadStageGIFFile, which the PS1 build leaves out): GIF -> tilesetGFXData
bool PS1TileSetLoad(const char *folder, uint8_t *pal768)
{
    char path[0x100];
    snprintf(path, sizeof(path), "Data/Stages/%s/16x16Tiles.gif", folder);
    FileInfo info;
    if (!LoadFile(path, &info))
        return false;
    byte b = 0;
    SetFilePosition(6); // GIF89a
    FileRead(&b, 1);
    int width = b;
    FileRead(&b, 1);
    width |= b << 8;
    FileRead(&b, 1);
    int height = b;
    FileRead(&b, 1);
    height |= b << 8;
    FileRead(&b, 1); // palette size, background colour, aspect (unused)
    FileRead(&b, 1);
    FileRead(&b, 1);
    byte clr[3];
    for (int c = 0; c < 0x80; ++c) FileRead(clr, 3);
    for (int c = 0x80; c < 0x100; ++c) {
        FileRead(clr, 3);
        pal768[c * 3]     = clr[0];
        pal768[c * 3 + 1] = clr[1];
        pal768[c * 3 + 2] = clr[2];
    }
    FileRead(&b, 1);
    while (b != ',') FileRead(&b, 1); // image descriptor
    ushort skip = 0;
    FileRead(&skip, 2);
    FileRead(&skip, 2);
    FileRead(&skip, 2);
    FileRead(&skip, 2);
    FileRead(&b, 1);
    bool interlaced = (b & 0x40) >> 6;
    if ((unsigned int)b >> 7 == 1) {
        int c = 128;
        do {
            ++c;
            FileRead(&b, 3);
        } while (c != 256);
    }
    ReadGifPictureData(width, height, interlaced, tilesetGFXData, 0);
    byte transparent = tilesetGFXData[0];
    for (int i = 0; i < TILESET_SIZE; ++i)
        if (tilesetGFXData[i] == transparent)
            tilesetGFXData[i] = 0;
    CloseFile();
    NdsOamFlush();
    NdsTypeCacheReset();
    HwReset(); // new tileset: the hardware tile pool starts over
    return true;
}

// ---- tile layers
#define PIX(i) { byte p = r[i]; if (p) d[i] = pal[p]; }

// One tile layer pass, prepared (scroll state advanced) and ready to draw in software or to feed the hardware BGs.
struct LayerCtx {
    TileLayer *layer;
    byte *lineScroll;
    int *def, *defW;
    int tileYPos, fullW, fullH;
    bool above;
};

// Unbounded-by-the-window x of logic line ly's left edge (0..fullW-1) for this pass
static inline int LineX(const LayerCtx &c, int ly)
{
    int ls = c.tileYPos + ly;
    while (ls >= TILELAYER_LINESCROLL_COUNT) ls -= TILELAYER_LINESCROLL_COUNT;
    const int sIdx = c.lineScroll[ls];
    int chunkX     = hParallax.linePos[sIdx];
    if (hParallax.deform[sIdx])
        chunkX += ly < waterDrawPos ? c.def[ly] : c.defW[ly - waterDrawPos];
    if ((unsigned)chunkX >= (unsigned)c.fullW) {
        chunkX %= c.fullW;
        if (chunkX < 0)
            chunkX += c.fullW;
    }
    int lx = chunkX + WIN_X0;
    while (lx >= c.fullW) lx -= c.fullW;
    return lx;
}

// Which visual planes each 128x128 chunk contains (bit 0: plane 0, bit 1: plane 1): a pass skips chunks without its plane
static byte s_chunkPlanes[0x200];
static uint32_t s_chunkVer = 1, s_chunkVerDone = 0;
volatile uint32_t g_ndsSwTiles = 0, g_ndsFillCols = 0; // tiles the software passes drew / map columns the hardware passes wrote, last frame
static void UpdateChunkPlanes()
{
    if (s_chunkVerDone == s_chunkVer)
        return;
    s_chunkVerDone = s_chunkVer;
    for (int ch = 0; ch < 0x200; ++ch) {
        byte f = 0;
        const ushort *t = &tiles128x128.ps1Tile[ch * 64];
        for (int i = 0; i < 64; ++i) {
            const unsigned pl = (t[i] >> 12) & 0xF;
            if (pl < 2)
                f |= 1 << pl;
        }
        s_chunkPlanes[ch] = f;
    }
}

static HOT void DrawHLayerLines(const LayerCtx &c)
{
    UpdateChunkPlanes();
    const byte planeBit = c.above ? 2 : 1;
    const ushort *tiles = c.layer->tiles;
    const ushort *cts   = tiles128x128.ps1Tile; // bits 0-9 tile, 10-11 direction, 12-15 plane
    const int fullW = c.fullW, fullH = c.fullH;
    const bool above = c.above;
    for (int wy = 0; wy < WIN_H; ++wy) {
        const int ly = WIN_Y0 + wy; // logic line
        const uint16_t *pal = fullPalette[gfxLineBuffer[ly] & 7];
        int yy = c.tileYPos + ly;
        while (yy >= fullH) yy -= fullH;
        const int chunkRow = (yy >> 7) * TILELAYER_CHUNK_W;
        const int tileRow  = 8 * ((yy & 0x7F) >> 4);
        const int tileY16  = yy & 15;
        int lx             = LineX(c, ly);

        uint16_t *dst = g_ndsDst + wy * 256;
        int x = 0, dmin = WIN_W, dmax = 0;
        while (x < WIN_W) {
            const int tx = lx & 15;
            int n        = 16 - tx;
            if (n > WIN_W - x)
                n = WIN_W - x;
            const int chk = tiles[(lx >> 7) + chunkRow];
            if (!(s_chunkPlanes[chk] & planeBit)) { // nothing of this pass in the chunk: skip to its edge
                n = 128 - (lx & 0x7F);
                if (n > WIN_W - x)
                    n = WIN_W - x;
                x += n;
                lx += n;
                if (lx >= fullW)
                    lx -= fullW;
                continue;
            }
            const int ti = (chk << 6) + ((lx & 0x7F) >> 4) + tileRow;
            const unsigned v = cts[ti];
            if (((v >> 12) & 0xF) == (unsigned)above) {
                g_ndsSwTiles = g_ndsSwTiles + 1;
                if (x < dmin) dmin = x;
                if (x + n > dmax) dmax = x + n;
                const int dir = (v >> 10) & 3;
                const byte *r = &tilesetGFXData[((v & 0x3FF) << 8) + ((dir & 2) ? 15 - tileY16 : tileY16) * 16];
                uint16_t *d   = dst + x;
                if (!(dir & 1)) {
                    r += tx;
                    if (n == 16) {
                        PIX(0) PIX(1) PIX(2) PIX(3) PIX(4) PIX(5) PIX(6) PIX(7)
                        PIX(8) PIX(9) PIX(10) PIX(11) PIX(12) PIX(13) PIX(14) PIX(15)
                    }
                    else {
                        for (int i = 0; i < n; ++i) PIX(i)
                    }
                }
                else {
                    for (int i = 0; i < n; ++i) {
                        byte p = r[15 - (tx + i)];
                        if (p)
                            d[i] = pal[p];
                    }
                }
            }
            x += n;
            lx += n;
            if (lx >= fullW)
                lx -= fullW;
        }
        if (dmax > dmin)
            NdsMarkDirty(dmin, wy, dmax, wy + 1);
    }
}

// Advances the layer's scroll state (as upstream's DrawHLayer does) and returns the pass's parameters; false = nothing to draw
static bool PrepareLayer(int layerID, LayerCtx &c)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return false;
    int layerwidth     = layer->xsize;
    int layerheight    = layer->ysize;
    bool aboveMidPoint = layerID >= tLayerMidPoint;

    byte *lineScroll;
    int *deformationData, *deformationDataW;
    int yscrollOffset = 0;
    if (activeTileLayers[layerID]) { // BG layer
        int yScroll    = yScrollOffset * layer->parallaxFactor >> 8;
        int fullheight = layerheight << 7;
        layer->scrollPos += layer->scrollSpeed;
        if (layer->scrollPos > fullheight << 16)
            layer->scrollPos -= fullheight << 16;
        yscrollOffset    = (yScroll + (layer->scrollPos >> 16)) % fullheight;
        layerheight      = fullheight >> 7;
        lineScroll       = layer->lineScroll;
        deformationData  = &bgDeformationData2[(byte)(yscrollOffset + layer->deformationOffset)];
        deformationDataW = &bgDeformationData3[(byte)(yscrollOffset + waterDrawPos + layer->deformationOffsetW)];
    }
    else { // FG layer
        lastXSize     = layer->xsize;
        yscrollOffset = yScrollOffset;
        lineScroll    = layer->lineScroll;
        for (int i = 0; i < PARALLAX_COUNT; ++i) hParallax.linePos[i] = xScrollOffset;
        deformationData  = &bgDeformationData0[(byte)(yscrollOffset + layer->deformationOffset)];
        deformationDataW = &bgDeformationData1[(byte)(yscrollOffset + waterDrawPos + layer->deformationOffsetW)];
    }

    if (layer->type == LAYER_HSCROLL) {
        if (lastXSize != layerwidth) {
            int fullLayerwidth = layerwidth << 7;
            for (int i = 0; i < hParallax.entryCount; ++i) {
                hParallax.linePos[i] = xScrollOffset * hParallax.parallaxFactor[i] >> 8;
                if (hParallax.scrollPos[i] > fullLayerwidth << 16)
                    hParallax.scrollPos[i] -= fullLayerwidth << 16;
                if (hParallax.scrollPos[i] < 0)
                    hParallax.scrollPos[i] += fullLayerwidth << 16;
                hParallax.linePos[i] += hParallax.scrollPos[i] >> 16;
                hParallax.linePos[i] %= fullLayerwidth;
            }
        }
        int w = -1;
        if (activeTileLayers[layerID])
            w = layerwidth;
        lastXSize = w;
    }

    c.layer      = layer;
    c.lineScroll = lineScroll;
    c.def        = deformationData;
    c.defW       = deformationDataW;
    c.fullW      = layerwidth << 7;
    c.fullH      = layerheight << 7;
    c.above      = aboveMidPoint;
    c.tileYPos   = yscrollOffset % c.fullH;
    if (c.tileYPos < 0)
        c.tileYPos += c.fullH;
    return true;
}

// ================= hardware backgrounds (BG0 / BG1, 8bpp text, 64x64 maps) =================
// VRAM (main BG banks A-C, contiguous): maps 0x0000-0x7FFF (2 layers x 2 buffers x 8 KB), tiles 0x10000-0x1FFFF
// (1024 8x8 tiles: 0 = blank, 1..863 deduplicated statics, 864..1023 animated 16x16 tiles), sprite pages from 0x20000.
// Tiles are deduplicated lazily by 8x8 block, with flips; every 16x16 tile maps to 4 hardware entries.
#define HW_MAP_BASE   0x06000000u
#define HW_TILE_BASE  0x06010000u
#define HW_STATIC_MAX 864
#define HW_TILE_MAX   1024

struct NdsHwFrame {
    int hw;               // 1: BG0/BG1 carry tile layers this frame (else the bitmap holds everything)
    int nLayers;          // (unused)
    int used[4];          // BG0, BG1 (text) and BG2, BG3 (extended) carry a tile pass this frame (BG3: else the sprite bitmap)
    int front;            // bit i: BGi is a pass in front of the game's sprites (else behind them)
    int buf;              // map buffer to show
    uint16_t vofs[4];
    int uniform[4];       // every line has the same x scroll (BG2 / BG3, the extended BGs, need it)
    uint32_t *table[4];   // 193 entries: x scroll | y scroll << 16, per logic window line
    uint16_t hofs[4];     // x scroll of line 0
    int clearIdx;         // backdrop colour index (palette entry 0 of every bank)
    int bank0;            // palette bank of window line 0
    int nSwitch;          // palette bank changes down the screen (water): at most 4
    int swLine[4];        // window line where the bank changes
    int swBank[4];
    int bright;           // -16..16 (master brightness, fades to black / white)
};
extern "C" NdsHwFrame g_ndsHw;
NdsHwFrame g_ndsHw;

static uint32_t s_tbl[2][4][193];
static uint16_t s_q[TILE_COUNT][4]; // hardware map entries of each 16x16 tile's quadrants (0xFFFF = not built yet)
static uint16_t s_hash[2048];       // 8x8 block hash -> hardware tile + 1
static int s_next = 1, s_dynNext = HW_STATIC_MAX;
static bool s_hwReady = false, s_hwFail = false, s_dyn[TILE_COUNT];
static int s_mapBuf = 0;
static uint32_t s_mapVer = 1; // bumped whenever map contents could change without the scroll (tileset, tile edits)
struct MapRec { int r16; int lo, hi; uintptr_t sig; uint32_t ver; };
static MapRec s_rec[2][4][32]; // per map buffer, layer and 16-px row: the columns already written
static uint s_palVer = 0xFFFFFFFF;
static int s_palClear = -1;
volatile uint32_t g_ndsHwTiles = 0, g_ndsHwFrames = 0, g_ndsSwFrames = 0, g_ndsHwFail = 0;

static inline volatile uint32_t *HwTile(int idx) { return (volatile uint32_t *)(HW_TILE_BASE + idx * 64); }

static void HwReset()
{
    for (int t = 0; t < TILE_COUNT; ++t) {
        s_q[t][0] = s_q[t][1] = s_q[t][2] = s_q[t][3] = 0xFFFF;
        s_dyn[t] = false;
    }
    for (int i = 0; i < 2048; ++i) s_hash[i] = 0;
    s_next    = 1;
    s_dynNext = HW_STATIC_MAX;
    s_hwFail  = false;
    s_mapVer++;
    s_chunkVer++;
    s_palVer  = 0xFFFFFFFF;
    // nothing of the previous stage may stay in the maps or in the sprite bitmap pages (a stage that started with tiles of the
    // previous one on screen was fixed by dying, which does not reset the pool)
    for (volatile uint32_t *mp = (volatile uint32_t *)HW_MAP_BASE, *me = mp + 0x10000 / 4; mp < me; ++mp) *mp = 0;
    s_allDirty[0] = s_allDirty[1] = true;
    volatile uint32_t *z = HwTile(0);
    for (int i = 0; i < 16; ++i) z[i] = 0;
}

static inline uint32_t HashWords(const uint32_t *w)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; ++i) h = (h ^ w[i]) * 16777619u;
    return (h ^ (h >> 15)) & 2047;
}

static void ExtractBlock(int tile, int qx, int qy, byte *out)
{
    const byte *s = &tilesetGFXData[tile * 256 + qy * 8 * 16 + qx * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) out[y * 8 + x] = s[y * 16 + x];
}

static uint16_t HwBlockEntry(const byte *b0) // dedupes an 8x8 block into the static pool
{
    alignas(4) static byte v[4][64];
    bool any = false;
    for (int i = 0; i < 64; ++i) {
        v[0][i] = b0[i];
        any |= b0[i] != 0;
    }
    if (!any)
        return 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            byte p            = v[0][y * 8 + x];
            v[1][y * 8 + 7 - x]       = p;
            v[2][(7 - y) * 8 + x]     = p;
            v[3][(7 - y) * 8 + 7 - x] = p;
        }
    for (int f = 0; f < 4; ++f) {
        const uint32_t *w = (const uint32_t *)v[f];
        for (uint32_t h = HashWords(w); s_hash[h]; h = (h + 1) & 2047) {
            const int idx             = s_hash[h] - 1;
            volatile const uint32_t *t = HwTile(idx);
            int i                      = 0;
            while (i < 16 && t[i] == w[i]) ++i;
            if (i == 16)
                return (uint16_t)(idx | ((f & 1) << 10) | ((f & 2) << 10));
        }
    }
    if (s_next >= HW_STATIC_MAX) {
        s_hwFail    = true;
        g_ndsHwFail = g_ndsHwFail + 1;
        return 0;
    }
    const int idx     = s_next++;
    const uint32_t *w = (const uint32_t *)v[0];
    volatile uint32_t *t = HwTile(idx);
    for (int i = 0; i < 16; ++i) t[i] = w[i];
    uint32_t h = HashWords(w);
    while (s_hash[h]) h = (h + 1) & 2047;
    s_hash[h] = idx + 1;
    g_ndsHwTiles = s_next;
    return (uint16_t)idx;
}

static void HwEnsureTile(int t)
{
    alignas(4) byte b[64];
    for (int q = 0; q < 4; ++q) {
        ExtractBlock(t, q & 1, q >> 1, b);
        s_q[t][q] = HwBlockEntry(b);
    }
}

// An animated tile (Copy16x16Tile): it gets 4 private hardware tiles, rewritten on every copy
static void HwDynTile(int dest)
{
    if (!s_dyn[dest]) {
        if (s_dynNext + 4 > HW_TILE_MAX) {
            s_hwFail    = true;
            g_ndsHwFail = g_ndsHwFail + 1;
            return;
        }
        s_dyn[dest] = true;
        s_mapVer++; // maps written before this tile became animated hold its static copy
        for (int q = 0; q < 4; ++q) s_q[dest][q] = (uint16_t)(s_dynNext + q);
        s_dynNext += 4;
    }
    alignas(4) byte b[64];
    for (int q = 0; q < 4; ++q) {
        ExtractBlock(dest, q & 1, q >> 1, b);
        const uint32_t *w    = (const uint32_t *)b;
        volatile uint32_t *t = HwTile(s_q[dest][q]);
        for (int i = 0; i < 16; ++i) t[i] = w[i];
    }
}

void PS1TileChanged(int) { s_mapVer++; s_chunkVer++; } // a chunk's tiles were edited by a script

void PS1Copy16x16Tile(int dest, int src)
{
    byte *d = &tilesetGFXData[dest * TILE_DATASIZE];
    byte *s = &tilesetGFXData[src * TILE_DATASIZE];
    for (int i = 0; i < TILE_DATASIZE; ++i) d[i] = s[i];
    if (s_hwReady && !s_hwFail)
        HwDynTile(dest);
}

static inline volatile uint16_t *HwMapEntry(int buf, int layer, int col, int row)
{
    if (layer >= 2) // the extended BGs' maps are linear: 64 x 64 entries, BG2 at 0x8000 / 0xA000, BG3 at 0xC000 / 0xE000
        return (volatile uint16_t *)(HW_MAP_BASE + 0x8000 + (layer - 2) * 0x4000 + buf * 0x2000) + row * 64 + col;
    return (volatile uint16_t *)(HW_MAP_BASE + buf * 0x4000 + layer * 0x2000)
           + ((row >> 5) * 2 + (col >> 5)) * 1024 + (row & 31) * 32 + (col & 31);
}

// Fills the map columns c16a..c16b (16 px units, unbounded) of the 16x16 tile row r16
static void HwFillCols(const LayerCtx &c, int buf, int layer, int r16, int c16a, int c16b)
{
    g_ndsFillCols = g_ndsFillCols + (c16b - c16a + 1);
    const int cols16 = c.fullW >> 4, rows16 = c.fullH >> 4;
    const int wr     = r16 % rows16;
    const ushort *tiles = c.layer->tiles;
    const ushort *cts   = tiles128x128.ps1Tile;
    const int chunkRow  = (wr >> 3) * TILELAYER_CHUNK_W;
    const int tileRow   = 8 * (wr & 7);
    const int mr0 = (r16 * 2) & 63, mr1 = (r16 * 2 + 1) & 63;
    for (int c16 = c16a; c16 <= c16b; ++c16) {
        const int wc = c16 % cols16;
        const unsigned v = cts[(tiles[(wc >> 3) + chunkRow] << 6) + (wc & 7) + tileRow];
        uint16_t e0 = 0, e1 = 0, e2 = 0, e3 = 0;
        if (((v >> 12) & 0xF) == (unsigned)c.above) {
            const int t = v & 0x3FF;
            if (s_q[t][0] == 0xFFFF)
                HwEnsureTile(t);
            e0 = s_q[t][0];
            e1 = s_q[t][1];
            e2 = s_q[t][2];
            e3 = s_q[t][3];
            const int dir = (v >> 10) & 3;
            if (dir & 1) {
                uint16_t x = e0; e0 = e1 ^ 0x400; e1 = x ^ 0x400;
                x = e2; e2 = e3 ^ 0x400; e3 = x ^ 0x400;
            }
            if (dir & 2) {
                uint16_t x = e0; e0 = e2 ^ 0x800; e2 = x ^ 0x800;
                x = e1; e1 = e3 ^ 0x800; e3 = x ^ 0x800;
            }
        }
        const int mc0 = (c16 * 2) & 63, mc1 = (c16 * 2 + 1) & 63;
        *HwMapEntry(buf, layer, mc0, mr0) = e0;
        *HwMapEntry(buf, layer, mc1, mr0) = e1;
        *HwMapEntry(buf, layer, mc0, mr1) = e2;
        *HwMapEntry(buf, layer, mc1, mr1) = e3;
    }
}

static bool HwLayer(int layer, const LayerCtx &c, int buf)
{
    const int y0 = c.tileYPos + WIN_Y0;
    const uint32_t vofs = (uint32_t)(y0 & 511);
    uint32_t *tbl = s_tbl[buf][layer];
    int lx[WIN_H];
    bool uniform = true;
    for (int wy = 0; wy < WIN_H; ++wy) {
        lx[wy] = LineX(c, WIN_Y0 + wy);
        if (lx[wy] != lx[0])
            uniform = false;
        tbl[wy] = (uint32_t)(lx[wy] & 511) | (vofs << 16);
    }
    tbl[WIN_H] = tbl[0];
    if (layer >= 2 && !uniform)
        return false; // the extended BGs scroll as a whole: this pass stays in software
    g_ndsHw.uniform[layer] = uniform;
    g_ndsHw.hofs[layer]    = (uint16_t)(lx[0] & 511);
    g_ndsHw.vofs[layer]    = (uint16_t)vofs;
    g_ndsHw.table[layer]   = tbl;

    const int r16a = y0 >> 4, r16b = (y0 + WIN_H - 1) >> 4;
    for (int r16 = r16a; r16 <= r16b; ++r16) {
        // the window lines that fall in this 16-row band
        int wa = r16 * 16 - y0, wb = wa + 15;
        if (wa < 0) wa = 0;
        if (wb > WIN_H - 1) wb = WIN_H - 1;
        MapRec &R = s_rec[buf][layer][r16 & 31];
        const uintptr_t sig = (uintptr_t)c.layer * 2 + (c.above ? 1 : 0);
        if (R.r16 != r16 || R.sig != sig || R.ver != s_mapVer) {
            R.r16 = r16;
            R.sig = sig;
            R.ver = s_mapVer;
            R.lo  = 1;
            R.hi  = 0;
        }
        int lo = R.lo, hi = R.hi; // columns this map row already holds (from earlier frames)
        int lastx = -1;
        for (int wy = wa; wy <= wb; ++wy) {
            if (lx[wy] == lastx)
                continue;
            lastx = lx[wy];
            const int a = lastx >> 4, b = (lastx + WIN_W - 1) >> 4;
            if (lo > hi) {
                HwFillCols(c, buf, layer, r16, a, b);
                lo = a;
                hi = b;
            }
            else if (a <= hi + 1 && b >= lo - 1 && (b > hi ? b : hi) - (a < lo ? a : lo) < 32) {
                if (a < lo) HwFillCols(c, buf, layer, r16, a, lo - 1);
                if (b > hi) HwFillCols(c, buf, layer, r16, hi + 1, b);
                if (a < lo) lo = a;
                if (b > hi) hi = b;
            }
            else { // a separate region (parallax bands in one row): glitch-prone, see docs
                HwFillCols(c, buf, layer, r16, a, b);
                lo = a;
                hi = b;
            }
        }
        R.lo = lo;
        R.hi = hi;
    }
    return true;
}

static bool HwFrameOk() { return s_hwReady && !s_hwFail; }

// ---- the engine's drawing interface (the rest stays a stub in stubs.cpp until its step)
void ClearScreen(byte index) { s_clearIndex = index; }

// 8 words per store-multiple burst (VRAM likes bursts); n is a multiple of 8
static HOT void ClearWords(uint32_t *p, uint32_t v, int n)
{
    asm volatile("mov r4, %2\n mov r5, %2\n mov r6, %2\n mov r7, %2\n mov r8, %2\n mov r9, %2\n mov r10, %2\n mov r11, %2\n"
                 "1: stmia %0!, {r4-r11}\n subs %1, %1, #8\n bgt 1b\n"
                 : "+r"(p), "+r"(n)
                 : "r"(v)
                 : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "memory", "cc");
}

static bool s_hwMode = false, s_hwOpen = false, s_obj34 = false;
static int s_hwLayers = 0, s_hwBuf = 0;
volatile char g_ndsPassInfo[8] = { '-', '-', '-', '-', 0, 0, 0, 0 }; // per tile pass: H hardware, S software, - none

static uint32_t s_tkTiles, s_tkObj;

static void ObjList(int i)
{
    if (i == 3 || i == 4)
        s_obj34 = true; // from here on a tile pass is in front of the player: it stays in software
    NdsOamHud(i >= 5);
    uint32_t t = Ticks();
    DrawObjectList(i);
    s_tkObj += Ticks() - t;
}

// Whether every window line of the pass has the same x scroll (what BG2 / BG3 need)
static bool LayerUniform(const LayerCtx &c)
{
    const int x0 = LineX(c, WIN_Y0);
    for (int wy = 1; wy < WIN_H; ++wy)
        if (LineX(c, WIN_Y0 + wy) != x0)
            return false;
    return true;
}

static bool s_bg3Tile = false;    // this frame BG3 may carry a tile pass (else it shows the sprite bitmap)
static bool s_needBitmapPrev = false, s_needBitmapNext = false;
bool g_ndsSwDraw = false;         // a draw went to the bitmap this frame (sprite fallback, rectangle, rotated sprite)

static void DrawTileLayer(int k)
{
    g_ndsPassInfo[k] = '-';
    if (activeTileLayers[k] >= LAYER_COUNT)
        return;
    if (stageLayouts[activeTileLayers[k]].type != LAYER_HSCROLL)
        return; // (vertical scroll, 3D floor and 3D sky layers: later steps)
    LayerCtx c;
    if (!PrepareLayer(k, c))
        return;
    uint32_t t = Ticks();
    int bg = -1;
    if (s_hwMode && s_hwOpen && !s_obj34 && s_hwLayers < 3 && HwLayer(s_hwLayers, c, s_hwBuf)) {
        g_ndsHw.used[s_hwLayers] = 1; // a pass behind the sprites: BG0, BG1, BG2 in order
        s_hwLayers++;
        g_ndsPassInfo[k] = 'H';
    }
    else if (s_hwMode && s_obj34) {
        // passes in front of the player: the first goes on BG3 (when it is not the sprite bitmap this frame), the second on BG2
        // (the lower BG index wins at equal priority: the later pass is on top)
        if (s_bg3Tile && !g_ndsHw.used[3] && HwLayer(3, c, s_hwBuf))
            bg = 3;
        else if (!g_ndsHw.used[2] && HwLayer(2, c, s_hwBuf))
            bg = 2;
        if (bg >= 0) {
            g_ndsHw.used[bg] = 1;
            g_ndsHw.front |= 1 << bg;
            g_ndsPassInfo[k] = 'F';
        }
    }
    if (g_ndsPassInfo[k] == '-') {
        s_hwOpen = false;
        g_ndsPassInfo[k] = 'S';
        if (s_hwMode && (s_bg3Tile || !LayerUniform(c)))
            s_needBitmapNext = true; // drawn into a bitmap that is not shown (lost this frame) or a pass BG2 / BG3 cannot scroll
        DrawHLayerLines(c);
    }
    s_tkTiles += Ticks() - t;
}

// Upstream's DrawStageGFX order: the objects' draw layers interleaved with the tile layers.
void DrawStageGFX()
{
    if (!g_ndsDst)
        return;
    uint32_t t0  = Ticks();
    waterDrawPos = waterLevel - yScrollOffset;
    if (waterDrawPos < 0)
        waterDrawPos = 0;
    if (waterDrawPos > SCREEN_YSIZE)
        waterDrawPos = SCREEN_YSIZE;

    s_hwMode   = HwFrameOk();
    s_hwOpen   = s_hwMode;
    s_hwLayers = g_ps1FaceCap ? 1 : 0; // a 3D scene: BG0 is the geometry engine's, the tile layers behind it start at BG1
    s_obj34    = false;
    g_ndsHw.used[0] = g_ndsHw.used[1] = g_ndsHw.used[2] = g_ndsHw.used[3] = 0;
    g_ndsHw.front = 0;
    s_bg3Tile        = s_hwMode && !s_needBitmapPrev; // BG3 shows the sprite bitmap only while something draws into it
    s_needBitmapNext = false;
    g_ndsSwDraw      = false;
    s_hwBuf    = s_mapBuf;
    const uint32_t bgc = fullPalette[0][s_clearIndex];
    uint32_t fill      = s_hwMode ? 0 : (bgc | (bgc << 16)); // hardware frame: the bitmap is a transparent sprite layer
    s_tkTiles = s_tkObj = 0;
    g_ndsSwTiles = g_ndsFillCols = 0;
    g_ndsBlitTicks = g_ndsBlitCount = g_ndsBlitPix = 0;
    if (!s_hwMode || s_allDirty[s_page]) {
        ClearWords((uint32_t *)g_ndsDst, fill, WIN_H * 256 / 2);
        ResetDirty(s_page);
        s_allDirty[s_page] = !s_hwMode; // a software frame fills the whole page: the next hardware frame clears it all
    }
    else { // only what the frame drawn into this page two frames ago touched
        for (int y = 0; y < WIN_H; ++y) {
            int a = s_dirty[s_page][y][0] & ~1, b = (s_dirty[s_page][y][1] + 1) & ~1; // whole words
            if (a < b) {
                uint32_t *p = (uint32_t *)(g_ndsDst + y * 256 + a);
                for (int n = (b - a) >> 1; n > 0; --n) *p++ = 0;
                s_dirty[s_page][y][0] = 0xFFFF;
                s_dirty[s_page][y][1] = 0;
            }
        }
    }
    g_ndsFlick++;
    NdsOamBegin();
    g_ndsScriptBright = 0;
    memset(g_ndsTypeDraw, 0, sizeof(g_ndsTypeDraw));
    g_ndsTkClear = Ticks() - t0;

    if (tLayerMidPoint < 3) {
        ObjList(0);
        DrawTileLayer(0);
        ObjList(1);
        DrawTileLayer(1);
        ObjList(2);
        ObjList(3);
        ObjList(4);
        DrawTileLayer(2);
    }
    else if (tLayerMidPoint < 6) {
        ObjList(0);
        DrawTileLayer(0);
        ObjList(1);
        DrawTileLayer(1);
        ObjList(2);
        DrawTileLayer(2);
        ObjList(3);
        ObjList(4);
    }
    if (tLayerMidPoint < 6) {
        DrawTileLayer(3);
        ObjList(5);
        ObjList(6);
    }

    g_ndsTkTiles = s_tkTiles;
    g_ndsTkObj   = s_tkObj;
    g_ndsHw.bright = s_hwMode ? g_ndsScriptBright : 0;
    if (fadeMode > 0) {
        const bool black = fadeR < 8 && fadeG < 8 && fadeB < 8, white = fadeR > 247 && fadeG > 247 && fadeB > 247;
        if (s_hwMode && (black || white)) { // master brightness covers tiles and sprites alike
            int f          = (fadeA * 16 + 127) / 255;
            g_ndsHw.bright = black ? -f : f;
        }
        else
            DrawRectangle(0, 0, SCREEN_XSIZE, SCREEN_YSIZE, fadeR, fadeG, fadeB, fadeA);
    }

    if (g_ndsSwDraw)
        s_needBitmapNext = true;
    s_needBitmapPrev = s_needBitmapNext;
    NdsOamEnd(g_ndsHw.front ? 1 : 0);
    g_ndsHw.hw      = s_hwMode;
    g_ndsHw.nLayers = s_hwLayers;
    g_ndsHw.buf     = s_hwBuf;
    if (s_hwMode) {
        s_mapBuf ^= 1;
        g_ndsHwFrames = g_ndsHwFrames + 1;
        g_ndsHw.clearIdx = s_clearIndex;
        g_ndsHw.bank0    = gfxLineBuffer[WIN_Y0] & 7;
        g_ndsHw.nSwitch  = 0;
        for (int wy = 1, cur = g_ndsHw.bank0, last = -9; wy < WIN_H && g_ndsHw.nSwitch < 4; ++wy) {
            const int bk = gfxLineBuffer[WIN_Y0 + wy] & 7;
            if (bk != cur && wy - last >= 2) { // the water line: the palette is switched in the HBlank before it
                g_ndsHw.swLine[g_ndsHw.nSwitch] = wy;
                g_ndsHw.swBank[g_ndsHw.nSwitch] = bk;
                g_ndsHw.nSwitch++;
                cur  = bk;
                last = wy;
            }
        }
    }
    else
        g_ndsSwFrames = g_ndsSwFrames + 1;

    s_drawn          = true;
    g_ndsRenderTicks = Ticks() - t0;
}

// main.cpp calls this once the BG banks are mapped (VRAM is not writable before)
extern "C" void NdsHwInit()
{
    s_hwReady = true;
    HwReset();
}
