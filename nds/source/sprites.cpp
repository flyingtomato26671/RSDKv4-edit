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

// ---- HUD placement: the engine lays the HUD out for 320x240; the DS window shows x 32..287, y 24..215. Sprites drawn by the
// "HUD" object (draw lists 5 and 6) are moved 24 px inward from the nearest corner so score / time / rings / lives stay visible.
volatile uint32_t g_ndsBlitTicks = 0, g_ndsBlitCount = 0, g_ndsBlitPix = 0; // sprite pixel work this frame (diagnostic)
static inline uint32_t BlitTicks()
{
    volatile uint16_t *t = (volatile uint16_t *)0x04000100;
    uint32_t hi, lo;
    do {
        hi = t[2];
        lo = t[0];
    } while (hi != t[2]);
    return (hi << 16) | lo;
}
static bool s_hudShift = false;
volatile char g_ndsHudNames[96]; // names of the objects drawn in lists 5 / 6 this frame (diagnostic)
static bool NameHas(const char *n, const char *sub)
{
    for (; *n; ++n) {
        const char *a = n, *b = sub;
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) { ++a; ++b; }
        if (!*b)
            return true;
    }
    return false;
}
static inline void HudShift(int &x, int &y)
{
    if (s_hudShift) {
        x += x < SCREEN_XSIZE / 2 ? 24 : -24;
        y += y < SCREEN_YSIZE / 2 ? 24 : -24;
    }
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
    HudShift(XPos, YPos);
    if (ink != INK_PLAIN) { // no blending: only the shields flicker; everything else translucent is drawn opaque, subtractive is skipped
        if (ink == INK_SUB_)
            return;
        if (strstr(typeNames[objectEntityList[objectEntityPos].type], "hield") && !NdsFlickerOn(alpha))
            return;
        ink = INK_PLAIN;
    }
    const GFXSurface *surface = &gfxSurface[sheetID];
    const int W = width, H = height, X = XPos, Y = YPos;
    // visible part of the sprite, in sprite-local coordinates
    int x = XPos, y = YPos, w = width, h = height, ox = 0, oy = 0;
    if (ink == INK_PLAIN && width > 0 && height > 0) {
        const byte *sh = &graphicData[surface->dataPosition];
        if (NdsOamSprite(X - WIN_X0, Y - WIN_Y0, W, H, sprX, sprY, flip, sheetID, sh, surface->width))
            return;
        g_ndsOamFallback = g_ndsOamFallback + 1;
    }
    if (!ClipToWindow(x, y, w, h, ox, oy))
        return;
    const bool fx = flip == FLIP_X || flip == FLIP_XY;
    const bool fy = flip == FLIP_Y || flip == FLIP_XY;
    const byte *sheet = &graphicData[surface->dataPosition];
    if (ink == INK_PLAIN) {
        g_ndsSwDraw = true;
        NdsMarkDirty(x - WIN_X0, y - WIN_Y0, x - WIN_X0 + w, y - WIN_Y0 + h);
        const uint32_t tb = BlitTicks();
        BlitPlain(sheet, surface->width, sprX, sprY, W, H, fx, fy, x, y, w, h, ox, oy);
        g_ndsBlitTicks = g_ndsBlitTicks + (BlitTicks() - tb);
        g_ndsBlitCount = g_ndsBlitCount + 1;
        g_ndsBlitPix   = g_ndsBlitPix + w * h;
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
    if (A > 0 && A < 0xFF && XPos <= WIN_X0 && YPos <= WIN_Y0 && XPos + width >= WIN_X0 + WIN_W && YPos + height >= WIN_Y0 + WIN_H) {
        const bool black = R < 8 && G < 8 && B < 8, white = R > 247 && G > 247 && B > 247;
        if (black || white) { // a script's full-screen fade: the master brightness, not flicker (flashing is unsafe)
            const int f = (A * 16 + 127) / 255;
            g_ndsScriptBright = black ? -f : f;
            return;
        }
    }
    HudShift(XPos, YPos);
    if (A <= 0 || !ClipToWindow(XPos, YPos, width, height, sx, sy))
        return;
    if (A < 0xFF) { // flicker instead of blending
        if (!NdsFlickerOn(A))
            return;
        A = 0xFF;
    }
    g_ndsSwDraw = true;
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
    g_ndsSwDraw = true;
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
uint32_t g_ndsTypeUpd[256], g_ndsTypeDraw[256];
volatile uint32_t g_ndsTkLogic = 0;
int g_ndsScriptBright = 0; // set by DrawRectangle for a full-screen black / white fade drawn by a script, read by DrawStageGFX
// per type, computed once per stage: bit0 known, bit1 HUD, bit2 waterfall (string searches per object per frame were a cost)
static uint8_t s_typeFlag[256];
void NdsTypeCacheReset() { memset(s_typeFlag, 0, sizeof(s_typeFlag)); }
static inline int TypeFlags(int type)
{
    uint8_t f = s_typeFlag[type & 0xFF];
    if (!f) {
        f = 1 | (NameHas(typeNames[type], "hud") ? 2 : 0) | (NameHas(typeNames[type], "waterfall") ? 4 : 0)
            | (NameHas(typeNames[type], "specialbg") ? 8 : 0);
        s_typeFlag[type & 0xFF] = f;
    }
    return f;
}

void DrawObjectList(int Layer)
{
    int size = drawListEntries[Layer].listSize;
    if (Layer == 5)
        g_ndsHudNames[0] = 0;
    for (int i = 0; i < size; ++i) {
        objectEntityPos = drawListEntries[Layer].entityRefs[i];
        int type        = objectEntityList[objectEntityPos].type;
        s_hudShift      = false;
        const int tf    = type ? TypeFlags(type) : 0;
        if (type && Layer >= 5) {
            const char *nm = typeNames[type];
            s_hudShift     = (tf & 2) != 0;
            if (!NameHas((const char *)g_ndsHudNames, nm)) { // names list for the console
                int n = 0;
                while (g_ndsHudNames[n]) ++n;
                for (const char *q = nm; *q && n < 90; ++q) g_ndsHudNames[n++] = *q;
                if (n < 94) g_ndsHudNames[n++] = ',';
                g_ndsHudNames[n] = 0;
            }
        }
        if ((tf & 8) && g_ps1FaceCap)
            continue; // the special stage's star field: a full-screen opaque software blit that covers the 3D path (OAM affine later)
        if (type && !(tf & 4)) { // the waterfall overlay costs most of a frame: the animated tiles remain
            if (scriptCode[objectScriptList[type].eventDraw.scriptCodePtr] > 0) {
                const uint32_t t0 = NdsTicksNow();
                ProcessScript(objectScriptList[type].eventDraw.scriptCodePtr, objectScriptList[type].eventDraw.jumpTablePtr, EVENT_DRAW);
                g_ndsTypeDraw[type & 0xFF] += NdsTicksNow() - t0;
            }
        }
    }
    s_hudShift = false;
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
