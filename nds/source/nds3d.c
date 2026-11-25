// DS 3D engine as a polygon rasteriser for the special stage's path (libnds side, C). The engine's Scene3D (scene3d.cpp) works out
// the screen positions and the colour of every face itself; here each face becomes one flat-coloured quad in an orthographic
// view where one vertex unit is one pixel of the 256x192 window. Faces arrive far to near and each gets a nearer depth, so the
// depth buffer reproduces the painter's order. BG0 shows the result (main.cpp enables it for frames that drew something).
#include <nds.h>

static int s_init = 0, s_active = 0, s_quads = 0;

static void FrameBegin(void)
{
    if (!s_init) {
        glInit();
        glClearColor(0, 0, 0, 0); // transparent: the other layers show through where nothing is drawn
        glClearPolyID(0);
        glClearDepth(GL_MAX_DEPTH);
        s_init = 1;
    }
    glViewport(0, 0, 255, 191);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof32(0, inttof32(256), inttof32(192), 0, inttof32(-32768), inttof32(32768));
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glScalef32(inttof32(4096), inttof32(4096), inttof32(4096)); // vertex coordinates are raw 1.3.12 values: raw 1 = one pixel
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
    glBegin(GL_QUADS);
    s_active = 1;
    s_quads  = 0;
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
    glColor((rgb_t)rgb15);
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
