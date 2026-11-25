// DS hardware sprites (OAM) for the engine's sprite draws: the CPU no longer paints sprite pixels, it queues OAM entries.
//
// A sprite is a rectangle of the 8-bit sheet (sheet, x, y, w, h). The first time one is drawn it is cut into the DS's OBJ
// shapes (8x8 .. 64x64, padded with transparent pixels) and uploaded once into OBJ VRAM (bank D, 1D tile mapping); the
// upload is cached, so a frame normally costs only the OAM entries. Flips use the OAM flip bits. 128 entries: a sprite that
// does not fit (OAM full, VRAM cache full, more than 8 pieces) falls back to the software blit in sprites.cpp.
// Draw order: the first sprite drawn of a frame gets entry 127 and later ones lower numbers (lower number = on top).
// No libnds here; main.cpp copies g_ndsOam to OAM at vblank.
#include "RetroEngine.hpp"
#include "ndsdraw.hpp"

#define OBJ_VRAM 0x06400000u
#define OBJ_TILES 1024 // 8x8 8bpp tiles in VRAM E (64 KB); the tile boundary is 64 bytes, so an OAM tile number = this index

uint16_t g_ndsOam[128 * 4]; // attr0, attr1, attr2, (affine parameter, unused)
volatile uint32_t g_ndsOamCount = 0, g_ndsOamFallback = 0, g_ndsOamUploads = 0, g_ndsOamTilesUsed = 0, g_ndsOamFlushes = 0;

struct Shape {
    uint8_t w, h, shape, size; // pixels, OAM attr0 shape bits, attr1 size bits
};
// largest first (the greedy cover takes the first that fits)
static const Shape kShapes[] = {
    { 64, 64, 0, 3 }, { 64, 32, 1, 3 }, { 32, 64, 2, 3 }, { 32, 32, 0, 2 }, { 32, 16, 1, 2 }, { 16, 32, 2, 2 },
    { 16, 16, 0, 1 }, { 32, 8, 1, 1 },  { 8, 32, 2, 1 },  { 16, 8, 1, 0 },  { 8, 16, 2, 0 },  { 8, 8, 0, 0 },
};
#define NSHAPES ((int)(sizeof(kShapes) / sizeof(kShapes[0])))

struct Piece {
    int16_t ox, oy; // position inside the frame, pixels
    uint8_t shape, size, w, h;
    uint16_t tile;  // OAM tile number (32-byte units)
};
#define MAX_PIECES 8
struct Frame {
    uint64_t key; // 0 = empty
    uint8_t n;    // pieces (0 = too big: software)
    Piece p[MAX_PIECES];
};
#define CACHE_SIZE 512
static Frame s_cache[CACHE_SIZE];
static int s_cacheUsed = 0;
static int s_tileNext = 0; // next free 64-byte tile in OBJ VRAM
static int s_count = 0;    // OAM entries queued this frame
static uint16_t s_tile[128];
static uint8_t s_hudFlag[128];
static bool s_hud = false;
void NdsOamHud(bool hud) { s_hud = hud; }

static inline volatile uint32_t *ObjTile(int t) { return (volatile uint32_t *)(OBJ_VRAM + t * 64); }

void NdsOamFlush() // new stage / cache full: every upload is forgotten
{
    for (int i = 0; i < CACHE_SIZE; ++i) s_cache[i].key = 0;
    s_cacheUsed = 0;
    s_tileNext  = 0;
    g_ndsOamFlushes = g_ndsOamFlushes + 1;
}

void NdsOamBegin()
{
    if (s_cacheUsed > CACHE_SIZE * 3 / 4 || s_tileNext > OBJ_TILES - 256) // flush between frames only: queued entries point into the cache
        NdsOamFlush();
    for (int i = 0; i < 128; ++i) g_ndsOam[i * 4] = 0x0200; // hidden
    s_count = 0;
    s_hud   = false;
}

// Covers a wt x ht tile rectangle (in 8 px units) with shapes; returns the piece count or 0 when more than MAX_PIECES are needed
static int Cover(Piece *out, int n, int x0, int y0, int w, int h)
{
    if (w <= 0 || h <= 0)
        return n;
    int best = -1;
    for (int i = 0; i < NSHAPES; ++i) {
        if (kShapes[i].w / 8 <= w && kShapes[i].h / 8 <= h) {
            best = i;
            break;
        }
    }
    if (best < 0 || n >= MAX_PIECES)
        return MAX_PIECES + 1;
    const Shape &s = kShapes[best];
    out[n].ox = (int16_t)(x0 * 8);
    out[n].oy = (int16_t)(y0 * 8);
    out[n].shape = s.shape;
    out[n].size  = s.size;
    out[n].w     = s.w;
    out[n].h     = s.h;
    ++n;
    n = Cover(out, n, x0 + s.w / 8, y0, w - s.w / 8, s.h / 8); // the strip to the right
    return Cover(out, n, x0, y0 + s.h / 8, w, h - s.h / 8);     // everything below
}

// Builds and uploads a frame; returns its cache entry (n == 0: too big for the OAM path)
static Frame *Upload(Frame *f, uint64_t key, int W, int H, const byte *sheet, int stride)
{
    f->key = key;
    f->n   = 0;
    const int wt = (W + 7) >> 3, ht = (H + 7) >> 3;
    Piece tmp[MAX_PIECES + 1];
    // one shape covering the whole frame when it wastes little; else the greedy cover
    int cnt = 0;
    int bestArea = 1 << 30, bestShape = -1;
    for (int i = 0; i < NSHAPES; ++i) {
        if (kShapes[i].w >= wt * 8 && kShapes[i].h >= ht * 8 && kShapes[i].w * kShapes[i].h < bestArea) {
            bestArea  = kShapes[i].w * kShapes[i].h;
            bestShape = i;
        }
    }
    if (bestShape >= 0 && bestArea <= wt * ht * 64 * 2) {
        const Shape &s = kShapes[bestShape];
        tmp[0] = { 0, 0, s.shape, s.size, s.w, s.h, 0 };
        cnt    = 1;
    }
    else {
        cnt = Cover(tmp, 0, 0, 0, wt, ht);
    }
    if (cnt == 0 || cnt > MAX_PIECES)
        return f; // n = 0: software
    int tiles = 0;
    for (int i = 0; i < cnt; ++i) tiles += (tmp[i].w >> 3) * (tmp[i].h >> 3);
    if (s_tileNext + tiles > OBJ_TILES)
        return nullptr; // VRAM full this frame: software for this one (the cache is flushed at the next frame)
    for (int i = 0; i < cnt; ++i) {
        Piece &p = tmp[i];
        p.tile   = (uint16_t)s_tileNext;
        const int tw = p.w >> 3, th = p.h >> 3;
        for (int ty = 0; ty < th; ++ty) {
            for (int tx = 0; tx < tw; ++tx) {
                volatile uint32_t *dst = ObjTile(s_tileNext + ty * tw + tx);
                const int px0 = p.ox + tx * 8, py0 = p.oy + ty * 8; // pixel of the tile's corner inside the frame
                for (int r = 0; r < 8; ++r) {
                    uint32_t lo = 0, hi = 0;
                    const int y = py0 + r;
                    if (y < H) {
                        const byte *src = &sheet[y * stride];
                        for (int c = 0; c < 8; ++c) {
                            const int x = px0 + c;
                            const uint32_t v = x < W ? src[x] : 0;
                            if (c < 4)
                                lo |= v << (c * 8);
                            else
                                hi |= v << ((c - 4) * 8);
                        }
                    }
                    dst[r * 2]     = lo;
                    dst[r * 2 + 1] = hi;
                }
            }
        }
        s_tileNext += tw * th;
    }
    f->n = (uint8_t)cnt;
    for (int i = 0; i < cnt; ++i) f->p[i] = tmp[i];
    g_ndsOamUploads   = g_ndsOamUploads + 1;
    g_ndsOamTilesUsed = s_tileNext;
    return f;
}

static Frame *Lookup(int sheetID, int sprX, int sprY, int W, int H, const byte *sheet, int stride)
{
    const uint64_t key = 1ull | ((uint64_t)sheetID << 1) | ((uint64_t)sprX << 6) | ((uint64_t)sprY << 18) | ((uint64_t)W << 30)
                         | ((uint64_t)H << 42);
    uint32_t h = (uint32_t)(key ^ (key >> 21) ^ (key >> 37)) * 2654435761u;
    for (int probe = 0; probe < CACHE_SIZE; ++probe) {
        Frame *f = &s_cache[(h >> 8) % CACHE_SIZE];
        if (f->key == key)
            return f;
        if (f->key == 0) {
            ++s_cacheUsed;
            return Upload(f, key, W, H, sheet, stride);
        }
        h += 0x9E3779B1u;
    }
    return nullptr;
}

// Queues a sprite (window coordinates of its top-left corner). Returns false when it must be drawn in software instead.
bool NdsOamSprite(int X, int Y, int W, int H, int sprX, int sprY, int flip, int sheetID, const byte *sheet, int stride)
{
    if (W <= 0 || H <= 0)
        return true;
    if (X >= WIN_W || Y >= WIN_H || X + W <= 0 || Y + H <= 0)
        return true; // entirely off the window
    if (W > 255 || H > 255 || sprX > 4095 || sprY > 4095)
        return false;
    Frame *f = Lookup(sheetID, sprX, sprY, W, H, sheet + sprY * stride + sprX, stride); // the frame's own top-left corner
    if (!f || f->n == 0)
        return false;
    const bool fx = flip == FLIP_X || flip == FLIP_XY, fy = flip == FLIP_Y || flip == FLIP_XY;
    // visible pieces first: the whole sprite is queued or none of it
    int need = 0;
    int16_t px[MAX_PIECES], py[MAX_PIECES];
    for (int i = 0; i < f->n; ++i) {
        const Piece &p = f->p[i];
        px[i]          = (int16_t)(fx ? X + W - p.ox - p.w : X + p.ox); // the mirror image of the frame, padding included
        py[i]          = (int16_t)(fy ? Y + H - p.oy - p.h : Y + p.oy);
        if (px[i] < WIN_W && py[i] < WIN_H && px[i] + p.w > 0 && py[i] + p.h > 0)
            ++need;
        else
            px[i] = -32768;
    }
    if (s_count + need > 128)
        return false;
    for (int i = 0; i < f->n; ++i) {
        if (px[i] == -32768)
            continue;
        const Piece &p   = f->p[i];
        const int idx    = 127 - s_count; // later draws on top
        const int yy     = py[i] & 0xFF;
        const int xx     = px[i] & 0x1FF;
        g_ndsOam[idx * 4]     = (uint16_t)(yy | 0x2000 | (p.shape << 14)); // 256 colours
        g_ndsOam[idx * 4 + 1] = (uint16_t)(xx | (fx ? 0x1000 : 0) | (fy ? 0x2000 : 0) | (p.size << 14));
        s_tile[idx]           = p.tile;
        s_hudFlag[idx]        = s_hud;
        ++s_count;
    }
    return true;
}

// End of the frame: tile numbers and priority: the game's sprites get `priority` (1 when a tile pass is in front of them, so it
// covers them), the HUD's 0 (above that pass)
void NdsOamEnd(int priority)
{
    for (int i = 128 - s_count; i < 128; ++i) g_ndsOam[i * 4 + 2] = (uint16_t)(s_tile[i] | ((s_hudFlag[i] ? 0 : priority) << 10));
    g_ndsOamCount = s_count;
}
