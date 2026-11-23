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
