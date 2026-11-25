// Shared by render.cpp (tile layers, frame loop) and sprites.cpp (sprites, rectangles, object lists).
#pragma once
#include <stdint.h>

#define WIN_W 256 // the DS window: logic x 32..287, y 24..215 of the engine's 320x240 screen, 1:1
#define WIN_H 192
#define WIN_X0 32
#define WIN_Y0 24

extern uint16_t *g_ndsDst; // the page being drawn (stride 256, DS 15-bit colour with bit 15 set), or null before main hands one over

// 5-5-5 channel helpers on DS colours
static inline uint32_t NdsBlend(uint32_t src, uint32_t dst, int alpha) // alpha 0..255 = the source's weight
{
    int ia = 255 - alpha;
    int r  = ((src & 31) * alpha + (dst & 31) * ia) >> 8;
    int g  = (((src >> 5) & 31) * alpha + ((dst >> 5) & 31) * ia) >> 8;
    int b  = (((src >> 10) & 31) * alpha + ((dst >> 10) & 31) * ia) >> 8;
    return 0x8000 | (b << 10) | (g << 5) | r;
}

extern uint32_t g_ndsFlick;                         // frame counter: translucency is faked by flicker (like the Mega Drive shield)
void NdsMarkDirty(int x0, int y0, int x1, int y1);  // window coords, half-open: the page's rows to clear next time
void NdsDirtyAll();                                 // the whole page was touched
// Translucent draws: opaque on some frames only (alpha 0..255 = how often); false = skip this frame
static inline bool NdsFlickerOn(int alpha)
{
    if (alpha >= 192)
        return true;
    if (alpha >= 96)
        return (g_ndsFlick & 1) == 0;
    return g_ndsFlick % 3 == 0;
}

// Hardware sprites (oam.cpp): the engine's sprite draws become OAM entries; false = draw it in software
typedef unsigned char byte_t_;
bool NdsOamSprite(int X, int Y, int W, int H, int sprX, int sprY, int flip, int sheetID, const unsigned char *sheet, int stride);
void NdsOamBegin();
void NdsOamEnd(int priority);
void NdsOamFlush();
extern uint16_t g_ndsOam[128 * 4];
extern volatile uint32_t g_ndsOamFallback;

// Profile (ARM9 ticks, 33.5 MHz): per object type, this frame's update scripts and draw scripts; logic = the whole ProcessObjects
static inline uint32_t NdsTicksNow()
{
    volatile uint16_t *t = (volatile uint16_t *)0x04000100;
    uint32_t hi, lo;
    do {
        hi = t[2];
        lo = t[0];
    } while (hi != t[2]);
    return (hi << 16) | lo;
}
extern uint32_t g_ndsTypeUpd[256], g_ndsTypeDraw[256];
extern volatile uint32_t g_ndsTkLogic;
void NdsTypeCacheReset();
extern int g_ndsScriptBright;
extern bool g_ndsSwDraw; // set by sprite / rectangle draws that go to the bitmap
void NdsOamHud(bool hud); // the sprites queued from here on are the HUD's (in front of the tile passes that are in front of the player)
