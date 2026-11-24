// libnds keys -> RSDKv4 input mask (bit = INPUT_* index, see RSDKv4/Input.cpp / RetroEngine.hpp).
// Order: Up Down Left Right | A(Cross) B(Circle) X(Square) Y(Triangle) | L2 R2 L1 R1 | Start Select
#include <nds.h>
#include <stdint.h>

int g_ndsDebugKeys = 0; // set by main.cpp when debug mode is on

uint16_t NdsPollInput(void)
{
    scanKeys();
    uint32_t k = keysHeld();
    uint16_t m = 0;
    if (k & KEY_UP)     m |= 1 << 0;
    if (k & KEY_DOWN)   m |= 1 << 1;
    if (k & KEY_LEFT)   m |= 1 << 2;
    if (k & KEY_RIGHT)  m |= 1 << 3;
    if (k & KEY_B)      m |= 1 << 4; // jump on the DS: B and A (RSDK's A button)
    if (k & KEY_A)      m |= 1 << 4; // (RSDK's B button is the pause / back button in the scripts: nothing maps to it, except Select in debug mode)
    if (k & KEY_Y)      m |= 1 << 6;
    if (k & KEY_X)      m |= 1 << 7;
    if (k & KEY_L)      m |= 1 << 10;
    if (k & KEY_R)      m |= 1 << 11;
    if (k & KEY_START)  m |= 1 << 12;
    if (k & KEY_SELECT) m |= g_ndsDebugKeys ? (1 << 5) : (1 << 13); // debug mode: Select is RSDK's B (the scripts' debug toggle)
    return m;
}
