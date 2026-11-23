// DS renderer, step 1: the tile layers. Draws the engine's 320x240 logic screen cropped 1:1 to the 256x192 window
// (logic x 32..287, y 24..215) into a RAM buffer in the DS's 15-bit format; main.cpp copies it to the screen.
// No libnds here (it clashes with the engine's headers): the buffer is exported through NdsFrameBuffer().
#include "RetroEngine.hpp"

#define WIN_W 256
#define WIN_H 192
#define WIN_X0 32 // logic screen -> window crop
#define WIN_Y0 24

byte tilesetGFXData[TILESET_SIZE]; // the stage's 16x16 tiles, 8-bit (PS1 keeps them only in VRAM)
static uint16_t s_spare[WIN_W * WIN_H]; // used until main.cpp hands over the VRAM back page
static uint16_t *s_dst = s_spare;      // where the frame is drawn: the back page of the bitmap background (stride 256)
static uint16_t s_dsPal[PALETTE_COUNT][PALETTE_COLOR_COUNT]; // fullPalette32 -> 15-bit with the alpha bit
static int s_clearIndex = 0;
static bool s_drawn = false;      // a frame was drawn since main last took it (a paused game draws nothing)
volatile uint32_t g_ndsRenderTicks = 0; // ARM9 ticks (33.5 MHz) DrawStageGFX took last

#define ITCM_CODE __attribute__((section(".itcm"), long_call))
#define HOT ITCM_CODE __attribute__((optimize("O3"), target("arm")))

extern "C" void NdsSetBackBuffer(uint16_t *p) { s_dst = p ? p : s_spare; }
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

static inline uint16_t ToDS(const PaletteEntry &c) { return (uint16_t)(0x8000 | (c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10)); }

static void BuildPalettes()
{
    for (int b = 0; b < PALETTE_COUNT; ++b)
        for (int i = 0; i < PALETTE_COLOR_COUNT; ++i) s_dsPal[b][i] = ToDS(fullPalette32[b][i]);
}

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
    return true;
}

// ---- tile layers
#define PIX(i) { byte p = r[i]; if (p) d[i] = pal[p]; }

static HOT void DrawHLayerLines(TileLayer *layer, byte *lineScroll, int *deformationData, int *deformationDataW, int tileYPos,
                                 int fullW, int fullH, bool above)
{
    const ushort *tiles = layer->tiles;
    const ushort *cts   = tiles128x128.ps1Tile; // bits 0-9 tile, 10-11 direction, 12-15 plane
    for (int wy = 0; wy < WIN_H; ++wy) {
        const int ly = WIN_Y0 + wy; // logic line
        const uint16_t *pal = s_dsPal[gfxLineBuffer[ly] & 7];
        int yy = tileYPos + ly;
        while (yy >= fullH) yy -= fullH;
        int ls = tileYPos + ly;
        while (ls >= TILELAYER_LINESCROLL_COUNT) ls -= TILELAYER_LINESCROLL_COUNT;
        const int chunkRow = (yy >> 7) * TILELAYER_CHUNK_W;
        const int tileRow  = 8 * ((yy & 0x7F) >> 4);
        const int tileY16  = yy & 15;
        const int sIdx     = lineScroll[ls];
        int chunkX         = hParallax.linePos[sIdx];
        if (hParallax.deform[sIdx])
            chunkX += ly < waterDrawPos ? deformationData[ly] : deformationDataW[ly - waterDrawPos];
        if ((unsigned)chunkX >= (unsigned)fullW) {
            chunkX %= fullW;
            if (chunkX < 0)
                chunkX += fullW;
        }
        int lx = chunkX + WIN_X0;
        while (lx >= fullW) lx -= fullW;

        uint16_t *dst = s_dst + wy * 256;
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

static void NdsDrawHLayer(int layerID)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;
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

    const int fullW = layerwidth << 7, fullH = layerheight << 7;
    int tileYPos    = yscrollOffset % fullH;
    if (tileYPos < 0)
        tileYPos += fullH;
    DrawHLayerLines(layer, lineScroll, deformationData, deformationDataW, tileYPos, fullW, fullH, aboveMidPoint);
}

// ---- the engine's drawing interface (the rest stays a stub in stubs.cpp until its step)
void ClearScreen(byte index) { s_clearIndex = index; }

static uint32_t s_palSeen = 0xFFFFFFFF;

void DrawStageGFX()
{
    uint32_t t0 = Ticks();
    waterDrawPos = waterLevel - yScrollOffset;
    if (waterDrawPos < 0)
        waterDrawPos = 0;
    if (waterDrawPos > SCREEN_YSIZE)
        waterDrawPos = SCREEN_YSIZE;
    if (g_ps1PalVersion != s_palSeen) { // every palette write bumps it (Palette.hpp)
        BuildPalettes();
        s_palSeen = g_ps1PalVersion;
    }
    uint32_t bg2 = s_dsPal[0][s_clearIndex];
    bg2 |= bg2 << 16;
    for (int y = 0; y < WIN_H; ++y) {
        uint32_t *row = (uint32_t *)(s_dst + y * 256);
        for (int i = 0; i < WIN_W / 2; ++i) row[i] = bg2;
    }
    for (int layerID = 0; layerID < 4; ++layerID) {
        if (activeTileLayers[layerID] >= LAYER_COUNT)
            continue;
        if (stageLayouts[activeTileLayers[layerID]].type == LAYER_HSCROLL)
            NdsDrawHLayer(layerID);
    }
    s_drawn         = true;
    g_ndsRenderTicks = Ticks() - t0;
}
