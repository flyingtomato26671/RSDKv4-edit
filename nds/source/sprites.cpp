// DS renderer: sprites, rectangles and the object draw lists, into the page render.cpp is drawing (g_ndsDst).
// Ported from upstream's software renderer (RSDKv4/Drawing.cpp): same clipping, same palette per line (gfxLineBuffer),
// colour index 0 transparent; the pixels are DS 15-bit colours (the palettes already hold them, Palette.hpp).
#include "RetroEngine.hpp"
#include "ndsdraw.hpp"

// ---- clip a logic-screen rectangle to the window; on return (x, y) are window coordinates
static inline bool ClipToWindow(int &x, int &y, int &w, int &h, int &sx, int &sy)
{
    if (x < WIN_X0) {
        sx += WIN_X0 - x;
        w -= WIN_X0 - x;
        x = WIN_X0;
    }
    if (x + w > WIN_X0 + WIN_W)
        w = WIN_X0 + WIN_W - x;
    if (y < WIN_Y0) {
        sy += WIN_Y0 - y;
        h -= WIN_Y0 - y;
        y = WIN_Y0;
    }
    if (y + h > WIN_Y0 + WIN_H)
        h = WIN_Y0 + WIN_H - y;
    return w > 0 && h > 0;
}

#define ITCM_CODE __attribute__((section(".itcm"), long_call))
#define HOT ITCM_CODE __attribute__((optimize("O3"), target("arm")))

// Plain (opaque-where-not-transparent) blit of the visible part: no per-pixel ink switch, 4 transparent pixels skipped per word
static HOT void BlitPlain(const byte *sheet, int stride, int sprX, int sprY, int W, int H, bool fx, bool fy, int x, int y, int w, int h,
                          int ox, int oy)
{
    for (int r = 0; r < h; ++r) {
        const int ly            = y + r;
        const uint16_t *pal     = fullPalette[gfxLineBuffer[ly] & 7];
        const int sy            = sprY + (fy ? H - 1 - (oy + r) : oy + r);
        uint16_t *d             = g_ndsDst + (ly - WIN_Y0) * 256 + (x - WIN_X0);
        if (!fx) {
            const byte *s = &sheet[sy * stride + sprX + ox];
            int n         = w;
            while (n > 0 && ((uintptr_t)s & 3)) {
                byte p = *s++;
                if (p)
                    *d = pal[p];
                ++d;
                --n;
            }
            while (n >= 4) {
                const uint32_t q = *(const uint32_t *)s;
                if (q) {
                    byte p;
                    if ((p = q & 0xFF)) d[0] = pal[p];
                    if ((p = (q >> 8) & 0xFF)) d[1] = pal[p];
                    if ((p = (q >> 16) & 0xFF)) d[2] = pal[p];
                    if ((p = q >> 24)) d[3] = pal[p];
                }
                s += 4;
                d += 4;
                n -= 4;
            }
            while (n > 0) {
                byte p = *s++;
                if (p)
                    *d = pal[p];
                ++d;
                --n;
            }
        }
        else {
            const byte *s = &sheet[sy * stride + sprX + (W - 1 - ox)];
            for (int c = 0; c < w; ++c) {
                byte p = *s--;
                if (p)
                    d[c] = pal[p];
            }
        }
    }
}

enum { INK_PLAIN, INK_50, INK_ALPHA_, INK_ADD_, INK_SUB_ };

// One routine for every sprite draw: flip = FLIP_* of the sheet region, ink picks how a pixel meets the one below.
static void SpriteBlit(int XPos, int YPos, int width, int height, int sprX, int sprY, int flip, int sheetID, int ink, int alpha)
{
    if (!g_ndsDst || sheetID < 0 || sheetID >= SURFACE_COUNT)
        return;
    if (ink != INK_PLAIN) { // no blending: flicker opaque on some frames; subtractive effects are skipped
        if (ink == INK_SUB_ || !NdsFlickerOn(alpha))
            return;
        ink = INK_PLAIN;
    }
    const GFXSurface *surface = &gfxSurface[sheetID];
    const int W = width, H = height, X = XPos, Y = YPos;
    // visible part of the sprite, in sprite-local coordinates
    int x = XPos, y = YPos, w = width, h = height, ox = 0, oy = 0;
    if (!ClipToWindow(x, y, w, h, ox, oy))
        return;
    const bool fx = flip == FLIP_X || flip == FLIP_XY;
    const bool fy = flip == FLIP_Y || flip == FLIP_XY;
    const byte *sheet = &graphicData[surface->dataPosition];
    if (ink == INK_PLAIN) {
        NdsMarkDirty(x - WIN_X0, y - WIN_Y0, x - WIN_X0 + w, y - WIN_Y0 + h);
        BlitPlain(sheet, surface->width, sprX, sprY, W, H, fx, fy, x, y, w, h, ox, oy);
        return;
    }
    for (int r = 0; r < h; ++r) {
        const int ly = y + r; // logic line
        const uint16_t *pal = fullPalette[gfxLineBuffer[ly] & 7];
        const int sy = sprY + (fy ? H - 1 - (oy + r) : oy + r);
        const byte *src = &sheet[sy * surface->width + sprX];
        uint16_t *dst   = g_ndsDst + (ly - WIN_Y0) * 256 + (x - WIN_X0);
        for (int c = 0; c < w; ++c) {
            const byte p = src[fx ? W - 1 - (ox + c) : ox + c];
            if (!p)
                continue;
            const uint32_t col = pal[p];
            switch (ink) {
                case INK_PLAIN: dst[c] = (uint16_t)col; break;
                case INK_50: dst[c] = (uint16_t)NdsBlend(col, dst[c], 128); break;
                case INK_ALPHA_: dst[c] = (uint16_t)NdsBlend(col, dst[c], alpha); break;
                case INK_ADD_: {
                    uint32_t d = dst[c];
                    int r5 = (d & 31) + ((col & 31) * alpha >> 8), g5 = ((d >> 5) & 31) + (((col >> 5) & 31) * alpha >> 8),
                        b5 = ((d >> 10) & 31) + (((col >> 10) & 31) * alpha >> 8);
                    dst[c] = (uint16_t)(0x8000 | ((b5 > 31 ? 31 : b5) << 10) | ((g5 > 31 ? 31 : g5) << 5) | (r5 > 31 ? 31 : r5));
                    break;
                }
                case INK_SUB_: {
                    uint32_t d = dst[c];
                    int r5 = (d & 31) - ((col & 31) * alpha >> 8), g5 = ((d >> 5) & 31) - (((col >> 5) & 31) * alpha >> 8),
                        b5 = ((d >> 10) & 31) - (((col >> 10) & 31) * alpha >> 8);
                    dst[c] = (uint16_t)(0x8000 | ((b5 < 0 ? 0 : b5) << 10) | ((g5 < 0 ? 0 : g5) << 5) | (r5 < 0 ? 0 : r5));
                    break;
                }
            }
        }
    }
    (void)X;
    (void)Y;
}

void DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
    SpriteBlit(XPos, YPos, width, height, sprX, sprY, FLIP_NONE, sheetID, INK_PLAIN, 255);
}
void DrawSpriteFlipped(int XPos, int YPos, int width, int height, int sprX, int sprY, int direction, int sheetID)
{
    SpriteBlit(XPos, YPos, width, height, sprX, sprY, direction, sheetID, INK_PLAIN, 255);
}
void DrawBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
    SpriteBlit(XPos, YPos, width, height, sprX, sprY, FLIP_NONE, sheetID, INK_50, 128);
}
void DrawAlphaBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    if (alpha > 0xFF)
        alpha = 0xFF;
    SpriteBlit(XPos, YPos, width, height, sprX, sprY, FLIP_NONE, sheetID, alpha == 0xFF ? INK_PLAIN : INK_ALPHA_, alpha);
}
void DrawAdditiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    if (alpha > 0xFF)
        alpha = 0xFF;
    SpriteBlit(XPos, YPos, width, height, sprX, sprY, FLIP_NONE, sheetID, INK_ADD_, alpha);
}
void DrawSubtractiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    if (alpha > 0xFF)
        alpha = 0xFF;
    SpriteBlit(XPos, YPos, width, height, sprX, sprY, FLIP_NONE, sheetID, INK_SUB_, alpha);
}

// ---- rectangles
void DrawRectangle(int XPos, int YPos, int width, int height, int R, int G, int B, int A)
{
    if (!g_ndsDst)
        return;
    if (A > 0xFF)
        A = 0xFF;
    int sx = 0, sy = 0;
    if (A <= 0 || !ClipToWindow(XPos, YPos, width, height, sx, sy))
        return;
    if (A < 0xFF) { // flicker instead of blending
        if (!NdsFlickerOn(A))
            return;
        A = 0xFF;
    }
    NdsMarkDirty(XPos - WIN_X0, YPos - WIN_Y0, XPos - WIN_X0 + width, YPos - WIN_Y0 + height);
    const uint32_t clr = PACK_RGB888(R, G, B);
    for (int r = 0; r < height; ++r) {
        uint16_t *dst = g_ndsDst + (YPos + r - WIN_Y0) * 256 + (XPos - WIN_X0);
        if (A == 0xFF)
            for (int c = 0; c < width; ++c) dst[c] = (uint16_t)clr;
        else
            for (int c = 0; c < width; ++c) dst[c] = (uint16_t)NdsBlend(clr, dst[c], A);
    }
}
void DrawTintRectangle(int XPos, int YPos, int width, int height)
{
    if (!g_ndsDst)
        return;
    int sx = 0, sy = 0;
    if (!ClipToWindow(XPos, YPos, width, height, sx, sy))
        return;
    NdsMarkDirty(XPos - WIN_X0, YPos - WIN_Y0, XPos - WIN_X0 + width, YPos - WIN_Y0 + height);
    for (int r = 0; r < height; ++r) {
        uint16_t *dst = g_ndsDst + (YPos + r - WIN_Y0) * 256 + (XPos - WIN_X0);
        for (int c = 0; c < width; ++c) { // upstream's tint table: the average of the channels + 6, as grey
            uint32_t d = dst[c];
            int v      = ((d & 31) + ((d >> 5) & 31) + ((d >> 10) & 31)) / 3 + 6;
            if (v > 31)
                v = 31;
            dst[c] = (uint16_t)(0x8000 | (v << 10) | (v << 5) | v);
        }
    }
}

// ---- object draw lists (Script.cpp's draw events call the sprite functions above)
void DrawObjectList(int Layer)
{
    int size = drawListEntries[Layer].listSize;
    for (int i = 0; i < size; ++i) {
        objectEntityPos = drawListEntries[Layer].entityRefs[i];
        int type        = objectEntityList[objectEntityPos].type;
        if (type) {
            if (scriptCode[objectScriptList[type].eventDraw.scriptCodePtr] > 0)
                ProcessScript(objectScriptList[type].eventDraw.scriptCodePtr, objectScriptList[type].eventDraw.jumpTablePtr, EVENT_DRAW);
        }
    }
}

// Animated sprite of an object (upstream's, rotation styles included)
void DrawObjectAnimation(void *objScr, void *ent, int XPos, int YPos)
{
    ObjectScript *objectScript = (ObjectScript *)objScr;
    Entity *entity             = (Entity *)ent;
    SpriteAnimation *sprAnim   = &animationList[objectScript->animFile->aniListOffset + entity->animation];
    SpriteFrame *frame         = &animFrames[sprAnim->frameListOffset + entity->frame];
    int rotation               = 0;

    switch (sprAnim->rotationStyle) {
        case ROTSTYLE_NONE:
            switch (entity->direction) {
                case FLIP_NONE:
                    DrawSpriteFlipped(frame->pivotX + XPos, frame->pivotY + YPos, frame->width, frame->height, frame->sprX, frame->sprY, FLIP_NONE,
                                      frame->sheetID);
                    break;

                case FLIP_X:
                    DrawSpriteFlipped(XPos - frame->width - frame->pivotX, frame->pivotY + YPos, frame->width, frame->height, frame->sprX,
                                      frame->sprY, FLIP_X, frame->sheetID);
                    break;
                case FLIP_Y:

                    DrawSpriteFlipped(frame->pivotX + XPos, YPos - frame->height - frame->pivotY, frame->width, frame->height, frame->sprX,
                                      frame->sprY, FLIP_Y, frame->sheetID);
                    break;

                case FLIP_XY:
                    DrawSpriteFlipped(XPos - frame->width - frame->pivotX, YPos - frame->height - frame->pivotY, frame->width, frame->height,
                                      frame->sprX, frame->sprY, FLIP_XY, frame->sheetID);
                    break;

                default: break;
            }
            break;

        case ROTSTYLE_FULL:
            DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width, frame->height,
                              entity->rotation, frame->sheetID);
            break;

        case ROTSTYLE_45DEG:
            if (entity->rotation >= 0x100)
                DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width,
                                  frame->height, 0x200 - ((0x214 - entity->rotation) >> 6 << 6), frame->sheetID);
            else
                DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width,
                                  frame->height, (entity->rotation + 20) >> 6 << 6, frame->sheetID);
            break;

        case ROTSTYLE_STATICFRAMES: {
            if (entity->rotation >= 0x100)
                rotation = 8 - ((532 - entity->rotation) >> 6);
            else
                rotation = (entity->rotation + 20) >> 6;
            int frameID = entity->frame;
            switch (rotation) {
                case 0: // 0 deg
                case 8: // 360 deg
                    rotation = 0x00;
                    break;

                case 1: // 45 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0;
                    else
                        rotation = 0x80;
                    break;

                case 2: // 90 deg
                    rotation = 0x80;
                    break;

                case 3: // 135 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0x80;
                    else
                        rotation = 0x100;
                    break;

                case 4: // 180 deg
                    rotation = 0x100;
                    break;

                case 5: // 225 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0x100;
                    else
                        rotation = 384;
                    break;

                case 6: // 270 deg
                    rotation = 384;
                    break;

                case 7: // 315 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 384;
                    else
                        rotation = 0;
                    break;

                default: break;
            }

            frame = &animFrames[sprAnim->frameListOffset + frameID];
            DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width, frame->height,
                              rotation, frame->sheetID);
            // DrawSpriteRotozoom(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width,
            // frame->height,
            //                  rotation, entity->scale, frame->sheetID);
            break;
        }

        default: break;
    }
}
