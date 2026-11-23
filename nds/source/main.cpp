// Step 2: boots the engine core (with the stubs in stubs.cpp) from the SD card's /rsdkv4 folder and runs frames.
// Main screen: the 256x192 1:1 window test card (the renderer replaces it). Sub screen: status console.
#include <nds.h>
#include <fat.h>
#include <filesystem.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

extern "C" {
uint16_t NdsPollInput(void);
void NdsEngineInit(void);
int NdsEngineReady(void);
int NdsEngineMode(void);
void NdsEngineFrame(void);
}

static void waitForever(void)
{
    while (1) swiWaitForVBlank();
}

int main(void)
{
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    uint16_t *fb = (uint16_t *)bgGetGfxPtr(3);
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++)
            fb[y * 256 + x] = (x == 0 || y == 0 || x == 255 || y == 191) ? ARGB16(1, 31, 31, 31) : ARGB16(1, 2, 4, 12);

    videoSetModeSub(MODE_0_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 4, 0, false, true);
    printf("RSDKv4 DS (step 2)\n");

    if (!fatInitDefault()) {
        printf("FAT init failed: no SD card.\n");
        waitForever();
    }
    // Data lives in /rsdkv4 (Data/ and Bytecode/ extracted from your Data.rsdk), or in the SD root.
    if (chdir("/rsdkv4") != 0)
        chdir("/");
    FILE *f = fopen("Data/Game/GameConfig.bin", "rb");
    if (!f) {
        printf("Data/Game/GameConfig.bin\nnot found on the SD card.\nPut the extracted Data/ and\nBytecode/ in /rsdkv4/\n");
        waitForever();
    }
    fclose(f);

    printf("data found, Engine.Init...\n");
    NdsEngineInit();
    printf("init done: ready=%d mode=%d\n", NdsEngineReady(), NdsEngineMode());

    // Timing: ARM9 ticks at the 33.51 MHz bus clock (cpuStartTiming); a DS frame is 559,000 ticks (59.83 Hz).
    cpuStartTiming(0);
    unsigned frame = 0, vbl = 0;
    uint32_t sum = 0, mx = 0, last = 0;
    while (1) {
        uint32_t t0 = cpuGetTiming();
        NdsEngineFrame();
        last = cpuGetTiming() - t0;
        sum += last;
        if (last > mx) mx = last;
        frame++;
        if ((frame & 15) == 0) {
            consoleClear();
            printf("RSDKv4 DS (step 2)\nready=%d mode=%d pad:%04x\n", NdsEngineReady(), NdsEngineMode(), NdsPollInput());
            printf("frames:%u\n", frame);
            printf("RunFrame ms avg:%lu last:%lu max:%lu\n", (unsigned long)(sum / 16 / 33514), (unsigned long)(last / 33514), (unsigned long)(mx / 33514));
            printf("(16.7 ms = full speed)\n");
            sum = 0;
            mx  = 0;
        }
        swiWaitForVBlank();
        vbl++;
    }
}
