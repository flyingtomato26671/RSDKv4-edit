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
void NdsEngineInfo(char *buf, int n);
const char *NdsLogLine(int back);
}

static volatile unsigned g_vbl = 0;
static void vblankCount(void) { g_vbl = g_vbl + 1; }

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

    // Timing two ways: vblanks elapsed during RunFrame (an IRQ counter: 1 = full speed) and raw ARM9 timer ticks.
    irqSet(IRQ_VBLANK, vblankCount);
    irqEnable(IRQ_VBLANK);
    cpuStartTiming(0);
    unsigned frame = 0, sumV = 0, maxV = 0;
    uint32_t lastT = 0, maxT = 0;
    while (1) {
        unsigned v0 = g_vbl;
        uint32_t t0 = cpuGetTiming();
        NdsEngineFrame();
        lastT = cpuGetTiming() - t0;
        unsigned dv = g_vbl - v0;
        sumV += dv;
        if (dv > maxV) maxV = dv;
        if (lastT > maxT) maxT = lastT;
        frame++;
        if ((frame & 15) == 0) {
            consoleClear();
            printf("RSDKv4 DS (step 2)\nready=%d mode=%d pad:%04x\n", NdsEngineReady(), NdsEngineMode(), NdsPollInput());
            printf("frames:%u\n", frame);
            printf("vblanks/frame avg x16:%u max:%u\n", sumV, maxV);
            printf("(16 = full speed, per 16 frames)\n");
            printf("ticks last:%lu max:%lu\n", (unsigned long)lastT, (unsigned long)maxT);
            char info[200];
            NdsEngineInfo(info, sizeof(info));
            printf("%s\nlog:\n", info);
            for (int i = 3; i >= 0; i--) printf("%s\n", NdsLogLine(i));
            sumV = 0;
            maxV = 0;
        }
        swiWaitForVBlank();
    }
}
