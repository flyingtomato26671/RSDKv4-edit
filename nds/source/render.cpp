// DS renderer, step 1: the tile layers. Draws the engine's 320x240 logic screen cropped 1:1 to the 256x192 window
// (logic x 32..287, y 24..215) into a RAM buffer in the DS's 15-bit format; main.cpp copies it to the screen.
// No libnds here (it clashes with the engine's headers): the buffer is exported through NdsFrameBuffer().
#include "RetroEngine.hpp"

#define WIN_W 256
#define WIN_H 192
#define WIN_X0 32 // logic screen -> window crop
#define WIN_Y0 24

byte tilesetGFXData[TILESET_SIZE]; // the stage's 16x16 tiles, 8-bit (PS1 keeps them only in VRAM)
static uint16_t s_fb[WIN_W * WIN_H];
static uint16_t s_dsPal[PALETTE_COUNT][PALETTE_COLOR_COUNT]; // fullPalette32 -> 15-bit with the alpha bit
static int s_clearIndex = 0;

extern "C" const uint16_t *NdsFrameBuffer(void) { return s_fb; }

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
    int tileYPos = yscrollOffset % fullH;
    if (tileYPos < 0)
        tileYPos += fullH;
    for (int wy = 0; wy < WIN_H; ++wy) {
        const int ly = WIN_Y0 + wy; // logic line
        const uint16_t *pal = s_dsPal[gfxLineBuffer[ly] & 7];
        int yy = (tileYPos + ly) % fullH;
        int chunkY = yy >> 7, tileY = (yy & 0x7F) >> 4, tileY16 = yy & 15;
        int sIdx = lineScroll[(tileYPos + ly) % TILELAYER_LINESCROLL_COUNT];
        int chunkX = hParallax.linePos[sIdx];
        if (hParallax.deform[sIdx])
            chunkX += ly < waterDrawPos ? deformationData[ly] : deformationDataW[ly - waterDrawPos];
        chunkX %= fullW;
        if (chunkX < 0)
            chunkX += fullW;

        uint16_t *dst = &s_fb[wy * WIN_W];
        int x = 0;
        while (x < WIN_W) {
            int lx = (chunkX + WIN_X0 + x) % fullW;
            int tx = lx & 15;
            int n  = 16 - tx;
            if (n > WIN_W - x)
                n = WIN_W - x;
            int chunk = layer->tiles[(lx >> 7) + chunkY * TILELAYER_CHUNK_W];
            int ti    = (chunk << 6) + ((lx & 0x7F) >> 4) + 8 * tileY;
            if (tiles128x128.visualPlane[ti] == (byte)aboveMidPoint) {
                int idx  = tiles128x128.tileIndex[ti];
                int dir  = tiles128x128.direction[ti];
                bool fx  = dir == FLIP_X || dir == FLIP_XY;
                bool fy  = dir == FLIP_Y || dir == FLIP_XY;
                const byte *row = &tilesetGFXData[(idx << 8) + (fy ? 15 - tileY16 : tileY16) * 16];
                for (int i = 0; i < n; ++i) {
                    byte p = row[fx ? 15 - (tx + i) : tx + i];
                    if (p)
                        dst[x + i] = pal[p];
                }
            }
            x += n;
        }
    }
}

// ---- the engine's drawing interface (the rest stays a stub in stubs.cpp until its step)
void ClearScreen(byte index) { s_clearIndex = index; }

void DrawStageGFX()
{
    waterDrawPos = waterLevel - yScrollOffset;
    if (waterDrawPos < 0)
        waterDrawPos = 0;
    if (waterDrawPos > SCREEN_YSIZE)
        waterDrawPos = SCREEN_YSIZE;
    BuildPalettes();
    uint16_t bg = s_dsPal[0][s_clearIndex];
    for (int i = 0; i < WIN_W * WIN_H; ++i) s_fb[i] = bg;
    for (int layerID = 0; layerID < 4; ++layerID) {
        if (activeTileLayers[layerID] >= LAYER_COUNT)
            continue;
        if (stageLayouts[activeTileLayers[layerID]].type == LAYER_HSCROLL)
            NdsDrawHLayer(layerID);
    }
}
