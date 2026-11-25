// DS Scene3D: the engine's 3D scripts (Sonic 2's special stage) on the DS. Upstream's integer pipeline (matrices, vertex
// transform, projection, depth sort) runs on the ARM9; the faces then go to the DS 3D engine as flat-coloured quads (nds3d.c):
// coloured, faded (fogged by depth into a flat colour) and 2D faces. Sprite faces (rings, bombs: TEXTURED_C, 3D sprites) are
// still drawn by the CPU into the sprite bitmap. The 3D models carry no textures (vertex colours only).
#include "RetroEngine.hpp"
#include "ndsdraw.hpp"

extern "C" {
unsigned NdsHeapFree(void);
void NdsHeapInfo(unsigned *totalKB, unsigned *usedKB, unsigned *limitKB);
int Nds3dQuad(const int *xy, unsigned rgb15, int z);
void *Nds3dWorkRam(unsigned *bytes);
}

int vertexCount = 0, faceCount = 0;
Matrix matFinal = Matrix(), matWorld = Matrix(), matView = Matrix(), matTemp = Matrix();
int fogColor = 0, fogStrength = 0, projectionX = 0, projectionY = 0;
int g_ps1VtxCap = 0, g_ps1FaceCap = 0;

// The arrays exist only while the special stage is loaded (PS1Scene3DPrepare); otherwise 1-entry dummies, capacity 0
// (Script.cpp clamps every access and counts it).
static Face s_dummyFace;
static Vertex s_dummyVertex;
Face *faceBuffer     = &s_dummyFace;
Vertex *vertexBuffer = &s_dummyVertex;
DrawListEntry3D *drawList3D = nullptr;
struct VertexT {
    int x, y, z;   // upstream's transformed vertex
    int16_t sx, sy; // its projection in window pixels (valid when z > 0)
};
static VertexT *s_vt           = nullptr;
static DrawListEntry3D *s_tmp  = nullptr;
char g_ndsS3dFolder[20] = "-";
volatile int g_ndsS3dStatus = 0; // 0 never called, 1 folder not a special stage, 2 allocation failed, 3 ready
volatile uint32_t g_ndsS3dHeapUsed = 0, g_ndsS3dHeapArena = 0, g_ndsS3dHeapLimit = 0;
volatile uint32_t g_ndsS3dFreeKB = 0, g_ndsS3dFail = 0; // heap free (KB) at the call, which allocation failed (1..5)
volatile uint32_t g_ndsS3dCalls = 0;
volatile uint32_t g_ndsS3dQuads = 0, g_ndsS3dFaces = 0, g_ndsS3dSkipped = 0;

#define PS1_S2_SMALL
#define PS1_GAME_DUMMY
#include "scene3d_matrix.inc"

void PS1Scene3DPrepare(const char *folder)
{
    vertexBuffer = &s_dummyVertex;
    faceBuffer   = &s_dummyFace;
    s_vt = nullptr;
    drawList3D = s_tmp = nullptr;
    g_ps1VtxCap = g_ps1FaceCap = 0;
    vertexCount = faceCount = 0;
    g_ndsS3dCalls = g_ndsS3dCalls + 1;
    {
        int n = 0;
        for (; folder && folder[n] && n < 19; ++n) g_ndsS3dFolder[n] = folder[n];
        g_ndsS3dFolder[n] = 0;
    }
    bool special = false;
    for (int i = 0; folder && folder[i]; ++i)
        if ((folder[i] | 32) == 's' && (folder[i + 1] | 32) == 'p' && (folder[i + 2] | 32) == 'e' && (folder[i + 3] | 32) == 'c'
            && (folder[i + 4] | 32) == 'i')
            special = true;
    g_ndsS3dStatus = 1;
    if (!special)
        return; // (a list of stages: Data/Game/PS1Scene3D.bin, later; Sonic 1's special stages too)
    g_ndsS3dFreeKB = NdsHeapFree() / 1024;
    {
        unsigned t, u, l;
        NdsHeapInfo(&t, &u, &l);
        g_ndsS3dHeapArena = t, g_ndsS3dHeapUsed = u, g_ndsS3dHeapLimit = l;
    }
    unsigned room = 0;
    uint8_t *ram  = (uint8_t *)Nds3dWorkRam(&room);
    const unsigned szV = VERTEXBUFFER_SIZE * sizeof(Vertex), szF = FACEBUFFER_SIZE * sizeof(Face), szT = VERTEXBUFFER_SIZE * sizeof(VertexT),
                   szD = FACEBUFFER_SIZE * sizeof(DrawListEntry3D);
    g_ndsS3dFail = (szV + szF + szT + 2 * szD > room) ? 1 : 0;
    if (g_ndsS3dFail) {
        g_ndsS3dStatus = 2;
        return;
    }
    for (volatile uint32_t *p = (volatile uint32_t *)ram, *e = (volatile uint32_t *)(ram + szV + szF + szT + 2 * szD); p < e; ++p) *p = 0; // word stores
    Vertex *v = (Vertex *)ram;
    Face *f   = (Face *)(ram + szV);
    VertexT *t = (VertexT *)(ram + szV + szF);
    DrawListEntry3D *d = (DrawListEntry3D *)(ram + szV + szF + szT);
    DrawListEntry3D *m = (DrawListEntry3D *)(ram + szV + szF + szT + szD);
    vertexBuffer = v;
    faceBuffer   = f;
    s_vt         = t;
    drawList3D   = d;
    s_tmp        = m;
    g_ps1VtxCap  = VERTEXBUFFER_SIZE;
    g_ps1FaceCap = FACEBUFFER_SIZE;
    g_ndsS3dStatus = 3;
}

// The ARM9's divider (32 / 32 -> 32 in ~18 cycles; the software division of the library is several times slower)
static inline int HwDiv(int num, int den)
{
    volatile uint32_t *cnt = (volatile uint32_t *)0x04000280;
    *cnt                   = 0;
    *(volatile int32_t *)0x04000290 = num;
    *(volatile int32_t *)0x04000294 = num >> 31;
    *(volatile int32_t *)0x04000298 = den;
    *(volatile int32_t *)0x0400029C = den >> 31;
    while (*cnt & 0x8000) {}
    return *(volatile int32_t *)0x040002A0;
}

void TransformVertices(Matrix *matrix, int startIndex, int endIndex)
{
    if (!g_ps1VtxCap)
        return;
    if (endIndex > g_ps1VtxCap)
        endIndex = g_ps1VtxCap;
    const int m00 = matrix->values[0][0], m10 = matrix->values[1][0], m20 = matrix->values[2][0], m30 = matrix->values[3][0];
    const int m01 = matrix->values[0][1], m11 = matrix->values[1][1], m21 = matrix->values[2][1], m31 = matrix->values[3][1];
    const int m02 = matrix->values[0][2], m12 = matrix->values[1][2], m22 = matrix->values[2][2], m32 = matrix->values[3][2];
    for (Vertex *vert = &vertexBuffer[startIndex], *end = &vertexBuffer[endIndex]; vert < end; ++vert) {
        const int vx = vert->x, vy = vert->y, vz = vert->z;
        vert->x      = (vx * m00 >> 8) + (vy * m10 >> 8) + (vz * m20 >> 8) + m30;
        vert->y      = (vx * m01 >> 8) + (vy * m11 >> 8) + (vz * m21 >> 8) + m31;
        vert->z      = (vx * m02 >> 8) + (vy * m12 >> 8) + (vz * m22 >> 8) + m32;
    }
}

static inline int16_t ClampS(int v) { return (int16_t)(v < -20000 ? -20000 : v > 20000 ? 20000 : v); }

void TransformVertexBuffer()
{
    if (!g_ps1VtxCap)
        return;
    matFinal = matWorld;
    MatrixMultiply(&matFinal, &matView);
    const int m00 = matFinal.values[0][0], m10 = matFinal.values[1][0], m20 = matFinal.values[2][0], m30 = matFinal.values[3][0];
    const int m01 = matFinal.values[0][1], m11 = matFinal.values[1][1], m21 = matFinal.values[2][1], m31 = matFinal.values[3][1];
    const int m02 = matFinal.values[0][2], m12 = matFinal.values[1][2], m22 = matFinal.values[2][2], m32 = matFinal.values[3][2];
    const int n = vertexCount < g_ps1VtxCap ? vertexCount : g_ps1VtxCap;
    const int px = projectionX, py = projectionY;
    for (int v = 0; v < n; ++v) {
        const int vx = vertexBuffer[v].x, vy = vertexBuffer[v].y, vz = vertexBuffer[v].z;
        VertexT &t = s_vt[v];
        t.x = (vx * m00 >> 8) + (vy * m10 >> 8) + (vz * m20 >> 8) + m30;
        t.y = (vx * m01 >> 8) + (vy * m11 >> 8) + (vz * m21 >> 8) + m31;
        t.z = (vx * m02 >> 8) + (vy * m12 >> 8) + (vz * m22 >> 8) + m32;
        if (t.z > 0) {
            t.sx = ClampS(SCREEN_CENTERX + HwDiv(px * t.x, t.z) - WIN_X0);
            t.sy = ClampS(SCREEN_CENTERY - HwDiv(py * t.y, t.z) - WIN_Y0);
        }
    }
}

// Stable merge sort, descending by depth (ties keep their order, as upstream's bubble sort does)
void Sort3DDrawList()
{
    if (!g_ps1FaceCap)
        return;
    const int n = faceCount < g_ps1FaceCap ? faceCount : g_ps1FaceCap;
    for (int i = 0; i < n; ++i) {
        const Face &f = faceBuffer[i];
        drawList3D[i].depth  = (s_vt[f.d].z + s_vt[f.c].z + s_vt[f.b].z + s_vt[f.a].z) >> 2;
        drawList3D[i].faceID = i;
    }
    DrawListEntry3D *src = drawList3D, *dst = s_tmp;
    for (int w = 1; w < n; w <<= 1) {
        for (int lo = 0; lo < n; lo += 2 * w) {
            int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) dst[k++] = (src[j].depth > src[i].depth) ? src[j++] : src[i++];
            while (i < mid) dst[k++] = src[i++];
            while (j < hi) dst[k++] = src[j++];
        }
        DrawListEntry3D *t = src;
        src = dst;
        dst = t;
    }
    if (src != drawList3D)
        for (int i = 0; i < n; ++i) drawList3D[i] = src[i];
}

static inline unsigned Rgb15(int r5, int g5, int b5) { return (unsigned)(r5 | (g5 << 5) | (b5 << 10)); }

// Draws the 4 corners (window pixels, in the face's own order) as one convex quad: the order that keeps the outline convex
static void FlatQuad(const int *x, const int *y, unsigned rgb15, int z)
{
    if ((x[0] < 0 && x[1] < 0 && x[2] < 0 && x[3] < 0) || (x[0] >= WIN_W && x[1] >= WIN_W && x[2] >= WIN_W && x[3] >= WIN_W)
        || (y[0] < 0 && y[1] < 0 && y[2] < 0 && y[3] < 0) || (y[0] >= WIN_H && y[1] >= WIN_H && y[2] >= WIN_H && y[3] >= WIN_H)) {
        g_ndsS3dSkipped = g_ndsS3dSkipped + 1;
        return;
    }
    // the three ways to walk 4 points as a quad: a b d c (upstream's), a b c d, a c b d; take the first that is convex
    static const uint8_t order[3][4] = { { 0, 1, 3, 2 }, { 0, 1, 2, 3 }, { 0, 2, 1, 3 } };
    int best = 0;
    for (int o = 0; o < 3; ++o) {
        int pos = 0, neg = 0;
        for (int k = 0; k < 4; ++k) {
            const int i = order[o][k], j = order[o][(k + 1) & 3], l = order[o][(k + 2) & 3];
            const int cr = (x[j] - x[i]) * (y[l] - y[j]) - (y[j] - y[i]) * (x[l] - x[j]);
            pos += cr > 0;
            neg += cr < 0;
        }
        if (!(pos && neg)) {
            best = o;
            break;
        }
    }
    int xy[8];
    for (int k = 0; k < 4; ++k) {
        xy[k * 2]     = x[order[best][k]];
        xy[k * 2 + 1] = y[order[best][k]];
    }
    if (Nds3dQuad(xy, rgb15, z))
        g_ndsS3dQuads = g_ndsS3dQuads + 1;
}

// A sprite face: the rectangle of the sheet (u0,v0)-(u1,v1) scaled to (x0,y0)-(x1,y1) (logic coordinates), nearest neighbour
static void ScaledRect(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, int sheetID, bool blend)
{
    if (!g_ndsDst || sheetID < 0 || sheetID >= SURFACE_COUNT || x1 == x0 || y1 == y0)
        return;
    if (blend && !NdsFlickerOn(128))
        return;
    const GFXSurface *surface = &gfxSurface[sheetID];
    const byte *sheet = &graphicData[surface->dataPosition];
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; t = u0; u0 = u1; u1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; t = v0; v0 = v1; v1 = t; }
    const int dw = x1 - x0, dh = y1 - y0;
    if (u1 > u0 && v1 > v0 && NdsOamXform(0, x0 - WIN_X0, y0 - WIN_Y0, 0, 0, dw * 512 / (u1 - u0), dh * 512 / (v1 - v0), 0, u1 - u0, v1 - v0, u0, v0,
                                         sheetID, sheet, surface->width))
        return; // the hardware scales it (affine OAM)
    int ya = y0 < WIN_Y0 ? WIN_Y0 : y0, yb = y1 > WIN_Y0 + WIN_H ? WIN_Y0 + WIN_H : y1;
    int xa = x0 < WIN_X0 ? WIN_X0 : x0, xb = x1 > WIN_X0 + WIN_W ? WIN_X0 + WIN_W : x1;
    if (xa >= xb || ya >= yb)
        return;
    g_ndsSwDraw = true;
    NdsMarkDirty(xa - WIN_X0, ya - WIN_Y0, xb - WIN_X0, yb - WIN_Y0);
    for (int y = ya; y < yb; ++y) {
        const int sv = v0 + (v1 - v0) * (2 * (y - y0) + 1) / (2 * dh);
        const byte *row = &sheet[sv * surface->width];
        const uint16_t *pal = fullPalette[gfxLineBuffer[y] & 7];
        uint16_t *d = g_ndsDst + (y - WIN_Y0) * 256 + (xa - WIN_X0);
        for (int x = xa; x < xb; ++x, ++d) {
            const int su = u0 + (u1 - u0) * (2 * (x - x0) + 1) / (2 * dw);
            const byte p = row[su];
            if (p)
                *d = pal[p];
        }
    }
}

void Draw3DScene(int spriteSheetID)
{
    if (!g_ps1FaceCap)
        return;
    const int n = faceCount < g_ps1FaceCap ? faceCount : g_ps1FaceCap;
    int quadZ = 30000;
    for (int i = 0; i < n; ++i) {
        const Face &f = faceBuffer[drawList3D[i].faceID];
        const VertexT &A = s_vt[f.a], &B = s_vt[f.b], &C = s_vt[f.c], &D = s_vt[f.d];
        int x[4], y[4];
        switch (f.flag) {
            case FACE_FLAG_COLORED_3D:
            case FACE_FLAG_FADED: {
                if (!(A.z > 0 && B.z > 0 && C.z > 0 && D.z > 0))
                    break;
                x[0] = A.sx, x[1] = B.sx, x[2] = C.sx, x[3] = D.sx;
                y[0] = A.sy, y[1] = B.sy, y[2] = C.sy, y[3] = D.sy;
                const uint32_t c = f.color;
                int r5 = (c >> 19) & 31, g5 = (c >> 11) & 31, b5 = (c >> 3) & 31;
                if (f.flag == FACE_FLAG_FADED) {
                    int fogStr = (drawList3D[i].depth - 0x8000) >> 8;
                    if (fogStr < 0) fogStr = 0;
                    if (fogStr > fogStrength) fogStr = fogStrength;
                    const int a = 0xFF - fogStr, ia = fogStr;
                    if (a < 1)
                        break;
                    const uint32_t fc = (uint32_t)fogColor;
                    r5 = (((fc >> 19) & 31) * ia >> 8) + (r5 * a >> 8);
                    g5 = (((fc >> 11) & 31) * ia >> 8) + (g5 * a >> 8);
                    b5 = (((fc >> 3) & 31) * ia >> 8) + (b5 * a >> 8);
                }
                else if (((c & 0x7F000000) >> 23) < 128) { // translucent faces: not blended yet (skipped when mostly transparent)
                    break;
                }
                FlatQuad(x, y, Rgb15(r5, g5, b5), quadZ);
                quadZ -= 30;
                break;
            }
            case FACE_FLAG_COLORED_2D: {
                if (!(A.z >= 0 && B.z >= 0 && C.z >= 0 && D.z >= 0))
                    break;
                x[0] = A.x - WIN_X0, x[1] = B.x - WIN_X0, x[2] = C.x - WIN_X0, x[3] = D.x - WIN_X0;
                y[0] = A.y - WIN_Y0, y[1] = B.y - WIN_Y0, y[2] = C.y - WIN_Y0, y[3] = D.y - WIN_Y0;
                for (int k = 0; k < 4; ++k) { x[k] = ClampS(x[k]); y[k] = ClampS(y[k]); }
                const uint32_t c = f.color;
                if (((c & 0x7F000000) >> 23) < 128)
                    break;
                FlatQuad(x, y, Rgb15((c >> 19) & 31, (c >> 11) & 31, (c >> 3) & 31), quadZ);
                quadZ -= 30;
                break;
            }
            case FACE_FLAG_TEXTURED_C:
            case FACE_FLAG_TEXTURED_C_BLEND: {
                if (A.z <= 0)
                    break;
                const Vertex &va = vertexBuffer[f.a], &vb = vertexBuffer[f.b], &vc = vertexBuffer[f.c];
                ScaledRect(SCREEN_CENTERX + HwDiv(projectionX * (A.x - vb.u), A.z), SCREEN_CENTERY - HwDiv(projectionY * (A.y + vb.v), A.z),
                           SCREEN_CENTERX + HwDiv(projectionX * (A.x + vb.u), A.z), SCREEN_CENTERY - HwDiv(projectionY * (A.y - vb.v), A.z),
                           va.u - vc.u, va.v - vc.v, va.u + vc.u, va.v + vc.v, spriteSheetID, f.flag == FACE_FLAG_TEXTURED_C_BLEND);
                break;
            }
            case FACE_FLAG_3DSPRITE: {
                if (A.z <= 0)
                    break;
                const int xpos = SCREEN_CENTERX + HwDiv(projectionX * A.x, A.z), ypos = SCREEN_CENTERY - HwDiv(projectionY * A.y, A.z);
                ObjectScript *scriptInfo = &objectScriptList[vertexBuffer[f.a].u];
                SpriteFrame *frame       = &scriptFrames[scriptInfo->frameListOffset + vertexBuffer[f.b].u];
                switch (vertexBuffer[f.a].v) {
                    case FX_SCALE:
                        DrawSpriteScaled(vertexBuffer[f.b].v, xpos, ypos, -frame->pivotX, -frame->pivotY, vertexBuffer[f.c].u,
                                         vertexBuffer[f.c].u, frame->width, frame->height, frame->sprX, frame->sprY, scriptInfo->spriteSheetID);
                        break;
                    case FX_ROTATE:
                        DrawSpriteRotated(vertexBuffer[f.b].v, xpos, ypos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY,
                                          frame->width, frame->height, vertexBuffer[f.c].v, scriptInfo->spriteSheetID);
                        break;
                    case FX_ROTOZOOM:
                        DrawSpriteRotozoom(vertexBuffer[f.b].v, xpos, ypos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY,
                                           frame->width, frame->height, vertexBuffer[f.c].v, vertexBuffer[f.c].u, scriptInfo->spriteSheetID);
                        break;
                }
                break;
            }
            default: break; // textured 3D / 2D faces: not used by the half-pipe (vertex colours only)
        }
    }
    g_ndsS3dFaces = n;
}

void PS1Draw3DSceneOT(int spriteSheetID)
{
    Sort3DDrawList();
    Draw3DScene(spriteSheetID);
}
