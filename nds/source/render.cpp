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
volatile uint32_t g_ndsRenderTicks = 0; // ARM9 ticks (33.5 MHz) DrawStageGFX took last

#define ITCM_CODE __attribute__((section(".itcm"), long_call))
#define HOT ITCM_CODE __attribute__((optimize("O3"), target("arm")))

extern "C" void NdsSetBackBuffer(uint16_t *p) { g_ndsDst = p; }
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

static HOT void DrawHLayerLines(const LayerCtx &c)
{
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
        int x = 0;
        while (x < WIN_W) {
            const int tx = lx & 15;
            int n        = 16 - tx;
            if (n > WIN_W - x)
                n = WIN_W - x;
            const int ti = (tiles[(lx >> 7) + chunkRow] << 6) + ((lx & 0x7F) >> 4) + tileRow;
            const unsigned v = cts[ti];
            if (((v >> 12) & 0xF) == (unsigned)above) {
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
    int nLayers;          // 0..2
    int buf;              // map buffer to show
    uint16_t vofs[2];
    int uniform[2];       // every line has the same x scroll
    uint32_t *table[2];   // 193 entries: x scroll | y scroll << 16, per logic window line
    int palDirty;
    uint16_t pal[256];
    int bright;           // -16..16 (master brightness, fades to black / white)
};
extern "C" NdsHwFrame g_ndsHw;
NdsHwFrame g_ndsHw;

static uint32_t s_tbl[2][2][193];
static uint16_t s_q[TILE_COUNT][4]; // hardware map entries of each 16x16 tile's quadrants (0xFFFF = not built yet)
static uint16_t s_hash[2048];       // 8x8 block hash -> hardware tile + 1
static int s_next = 1, s_dynNext = HW_STATIC_MAX;
static bool s_hwReady = false, s_hwFail = false, s_dyn[TILE_COUNT];
static int s_mapBuf = 0;
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
    s_palVer  = 0xFFFFFFFF;
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
    return (volatile uint16_t *)(HW_MAP_BASE + buf * 0x4000 + layer * 0x2000)
           + ((row >> 5) * 2 + (col >> 5)) * 1024 + (row & 31) * 32 + (col & 31);
}

// Fills the map columns c16a..c16b (16 px units, unbounded) of the 16x16 tile row r16
static void HwFillCols(const LayerCtx &c, int buf, int layer, int r16, int c16a, int c16b)
{
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

static void HwLayer(int layer, const LayerCtx &c, int buf)
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
    g_ndsHw.uniform[layer] = uniform;
    g_ndsHw.vofs[layer]    = (uint16_t)vofs;
    g_ndsHw.table[layer]   = tbl;

    const int r16a = y0 >> 4, r16b = (y0 + WIN_H - 1) >> 4;
    for (int r16 = r16a; r16 <= r16b; ++r16) {
        // the window lines that fall in this 16-row band
        int wa = r16 * 16 - y0, wb = wa + 15;
        if (wa < 0) wa = 0;
        if (wb > WIN_H - 1) wb = WIN_H - 1;
        int lo = 1, hi = 0; // filled column range (empty at first)
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
    }
}

static bool HwFrameOk()
{
    if (!s_hwReady || s_hwFail)
        return false;
    for (int y = 0; y < WIN_H; ++y)
        if (gfxLineBuffer[WIN_Y0 + y] & 7)
            return false; // per-line palette banks (water): software frame
    return true;
}

// ---- the engine's drawing interface (the rest stays a stub in stubs.cpp until its step)
void ClearScreen(byte index) { s_clearIndex = index; }

static bool s_hwMode = false, s_hwOpen = false;
static int s_hwLayers = 0, s_hwBuf = 0;

static void ObjList(int i)
{
    if (drawListEntries[i].listSize > 0)
        s_hwOpen = false;
    DrawObjectList(i);
}

static void DrawTileLayer(int k)
{
    if (activeTileLayers[k] >= LAYER_COUNT)
        return;
    if (stageLayouts[activeTileLayers[k]].type != LAYER_HSCROLL)
        return; // (vertical scroll, 3D floor and 3D sky layers: later steps)
    LayerCtx c;
    if (!PrepareLayer(k, c))
        return;
    if (s_hwMode && s_hwOpen && s_hwLayers < 2)
        HwLayer(s_hwLayers++, c, s_hwBuf);
    else {
        s_hwOpen = false;
        DrawHLayerLines(c);
    }
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
    s_hwLayers = 0;
    s_hwBuf    = s_mapBuf;
    const uint32_t bgc = fullPalette[0][s_clearIndex];
    uint32_t fill      = s_hwMode ? 0 : (bgc | (bgc << 16)); // hardware frame: the bitmap is a transparent sprite layer
    for (int y = 0; y < WIN_H; ++y) {
        uint32_t *row = (uint32_t *)(g_ndsDst + y * 256);
        for (int i = 0; i < WIN_W / 2; ++i) row[i] = fill;
    }

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

    g_ndsHw.bright = 0;
    if (fadeMode > 0) {
        const bool black = fadeR < 8 && fadeG < 8 && fadeB < 8, white = fadeR > 247 && fadeG > 247 && fadeB > 247;
        if (s_hwMode && (black || white)) { // master brightness covers tiles and sprites alike
            int f          = (fadeA * 16 + 127) / 255;
            g_ndsHw.bright = black ? -f : f;
        }
        else
            DrawRectangle(0, 0, SCREEN_XSIZE, SCREEN_YSIZE, fadeR, fadeG, fadeB, fadeA);
    }

    g_ndsHw.hw      = s_hwMode;
    g_ndsHw.nLayers = s_hwLayers;
    g_ndsHw.buf     = s_hwBuf;
    if (s_hwMode) {
        s_mapBuf ^= 1;
        g_ndsHwFrames = g_ndsHwFrames + 1;
        if (s_palVer != g_ps1PalVersion || s_palClear != s_clearIndex) {
            s_palVer   = g_ps1PalVersion;
            s_palClear = s_clearIndex;
            for (int i = 0; i < 256; ++i) g_ndsHw.pal[i] = fullPalette[0][i];
            g_ndsHw.pal[0] = (uint16_t)bgc;
            g_ndsHw.palDirty = 1;
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
