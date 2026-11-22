// libnds keys -> RSDKv4 input mask (bit = INPUT_* index, see RSDKv4/Input.cpp / RetroEngine.hpp).
// Order: Up Down Left Right | A(Cross) B(Circle) X(Square) Y(Triangle) | L2 R2 L1 R1 | Start Select
#include <nds.h>
#include <stdint.h>

uint16_t NdsPollInput(void)
{
    scanKeys();
    uint32_t k = keysHeld();
    uint16_t m = 0;
    if (k & KEY_UP)     m |= 1 << 0;
    if (k & KEY_DOWN)   m |= 1 << 1;
    if (k & KEY_LEFT)   m |= 1 << 2;
    if (k & KEY_RIGHT)  m |= 1 << 3;
    if (k & KEY_B)      m |= 1 << 4; // jump on the DS: B (bottom button), like the Mega Drive layout
    if (k & KEY_A)      m |= 1 << 5;
    if (k & KEY_Y)      m |= 1 << 6;
    if (k & KEY_X)      m |= 1 << 7;
    if (k & KEY_L)      m |= 1 << 10;
    if (k & KEY_R)      m |= 1 << 11;
    if (k & KEY_START)  m |= 1 << 12;
    if (k & KEY_SELECT) m |= 1 << 13;
    return m;
}
