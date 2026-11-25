// DS 3D engine as a polygon rasteriser for the special stage's path (libnds side, C). The engine's Scene3D (scene3d.cpp) works out
// the screen positions and the colour of every face itself; here each face becomes one flat-coloured quad in an orthographic
// view where one vertex unit is one pixel of the 256x192 window. Faces arrive far to near and each gets a nearer depth, so the
// depth buffer reproduces the painter's order. BG0 shows the result (main.cpp enables it for frames that drew something).
#include <nds.h>

static int s_init = 0, s_active = 0, s_quads = 0;

static void Init(void)
{
    if (s_init)
        return;
    glInit();
    glClearColor(0, 0, 0, 0); // transparent: the other layers show through where nothing is drawn
    glClearPolyID(0);
    glClearDepth(GL_MAX_DEPTH);
    s_init = 1;
}

// Orthographic view, one vertex unit = one pixel of the 256x192 window (flat quads: painter's order by decreasing z)
static void SetupOrtho(void)
{
    glViewport(0, 0, 255, 191);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof32(0, inttof32(256), inttof32(192), 0, inttof32(-32768), inttof32(32768));
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glScalef32(inttof32(4096), inttof32(4096), inttof32(4096)); // vertex coordinates are raw 1.3.12 values: raw 1 = one pixel
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
}

static void FrameBegin(void)
{
    Init();
    SetupOrtho();
    glBegin(GL_QUADS);
    s_active = 1;
    s_quads  = 0;
}

// The geometry engine does the whole special-stage path: vals = the engine's final matrix (rows 0..2 x columns 0..2, then the
// translation row; 256 = 1.0), projX / projY = the engine's projection factors. Vertices are sent raw (16-bit engine units).
// Camera space looks down +z; screen x = 128 + projX * x / z, y = 96 - projY * y / z; the depth buffer orders the faces.
void Nds3dGpuBegin(const int *vals, int projX, int projY)
{
    Init();
    glViewport(0, 0, 255, 191);
    m4x4 p;
    for (int i = 0; i < 16; ++i) p.m[i] = 0;
    p.m[0]  = projX * 32;          // projX / 128 in 20.12
    p.m[5]  = projY * 4096 / 96;   // projY / 96
    p.m[10] = 4097;                // z: near 32, far 196608 (ndc z = A + B / z)
    p.m[14] = -262197;
    p.m[11] = 4096;                // w = z
    glMatrixMode(GL_PROJECTION);
    glLoadMatrix4x4(&p);
    m4x3 mv;
    for (int i = 0; i < 9; ++i) mv.m[i] = vals[i] << 16; // values / 256 * 4096 (raw vertices are 1.3.12) in 20.12
    for (int j = 0; j < 3; ++j) mv.m[9 + j] = vals[9 + j] << 12;
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrix4x3(&mv);
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
    glBegin(GL_QUADS);
    s_active = 1;
    s_quads  = 0;
}

// Switches to the flat orthographic view for what follows (2D faces drawn over the 3D ones)
void Nds3dGpuOrtho(void)
{
    if (s_active)
        glEnd();
    SetupOrtho();
    glBegin(GL_QUADS);
    s_active = 1;
}

// xy: x0 y0 x1 y1 x2 y2 x3 y3 in window pixels, in perimeter order; z: -32767..32767, smaller = nearer. Returns 0 when the
// frame's polygon budget (2048) is used up.
int Nds3dQuad(const int *xy, unsigned rgb15, int z)
{
    if (!s_active)
        FrameBegin();
    if (s_quads >= 1900)
        return 0;
    ++s_quads;
    glColor(rgb15);
    for (int i = 0; i < 4; ++i) glVertex3v16((v16)xy[i * 2], (v16)xy[i * 2 + 1], (v16)z);
    return 1;
}

// After the frame's engine run: closes the polygon list and hands it to the geometry engine (shown after the next vblank).
// Returns 1 when this frame drew 3D (BG0 must show it).
int Nds3dEndFrame(void)
{
    if (!s_active)
        return 0;
    glEnd();
    glFlush(0);
    s_active = 0;
    return 1;
}
int Nds3dQuadCount(void) { return s_quads; }

// The special stage's Scene3D arrays (~94 KB) live in VRAM bank D mapped as plain memory (LCDC, 128 KB at 0x06860000): the
// 1 MB heap is full of sound effects. VRAM takes 16/32-bit accesses only (the caller clears it with word stores).
void *Nds3dWorkRam(unsigned *bytes)
{
    vramSetBankD(VRAM_D_LCD);
    *bytes = 128 * 1024;
    return (void *)0x06860000;
}
void Nds3dWorkRamRelease(void)
{
    vramSetBankD(VRAM_D_LCD); // stays plain memory (nothing else uses bank D)
}
