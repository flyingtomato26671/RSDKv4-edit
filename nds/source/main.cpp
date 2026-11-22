// Step 1: minimal ARM9 app. Shows the 256x192 window the game will use (1:1, cropped from the 320x240 logic
// screen) on the main screen, a status console on the sub screen, and reports FAT and pad state.
#include <nds.h>
#include <fat.h>
#include <filesystem.h>
#include <stdio.h>
#include <stdint.h>

extern "C" uint16_t NdsPollInput(void);

int main(void)
{
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    uint16_t *fb = (uint16_t *)bgGetGfxPtr(3);

    videoSetModeSub(MODE_0_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 4, 0, false, true);

    bool fat = fatInitDefault();
    bool nfs = nitroFSInit(NULL);

    unsigned frame = 0;
    while (1) {
        uint16_t pad = NdsPollInput();
        // 256x192 test card: border + moving block, so the 1:1 window and the pad are visible
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++) {
                bool edge = x == 0 || y == 0 || x == 255 || y == 191;
                bool blk  = (unsigned)(x - (int)(frame % 200)) < 32u && y > 80 && y < 112;
                fb[y * 256 + x] = edge ? ARGB16(1, 31, 31, 31) : blk ? ARGB16(1, 31, 8, 0) : ARGB16(1, 2, 4, 12);
            }
        consoleClear();
        printf("RSDKv4 DS (step 1)\nfat:%d nitrofs:%d\npad:%04x frame:%u\n", fat, nfs, pad, frame);
        swiWaitForVBlank();
        frame++;
    }
}
