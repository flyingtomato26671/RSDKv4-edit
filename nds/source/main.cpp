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
void NdsSetBackBuffer(uint16_t *p);
int NdsFrameDrawn(void);
uint32_t NdsRenderTicks(void);
void NdsHwInit(void);
struct NdsHwFrame {
    int hw, nLayers, buf;
    uint16_t vofs[2];
    int uniform[2];
    uint32_t *table[2];
    int palDirty;
    uint16_t pal[256];
    int bright;
};
extern NdsHwFrame g_ndsHw;
extern volatile uint32_t g_ndsSwTiles, g_ndsFillCols;
extern volatile uint32_t g_ndsTkClear, g_ndsTkTiles, g_ndsTkObj;
extern volatile uint32_t g_ndsHwFrames, g_ndsSwFrames, g_ndsHwTiles, g_ndsHwFail;
}

static volatile unsigned g_vbl = 0;
static void vblankCount(void) { g_vbl = g_vbl + 1; }

// Applies the hardware-background state the engine prepared for the frame just drawn (called right after vblank)
static void applyHw(int bitmapBg)
{
    NdsHwFrame &h = g_ndsHw;
    volatile uint16_t *cnt[2]  = { &REG_BG0CNT, &REG_BG1CNT };
    volatile uint16_t *hofs[2] = { &REG_BG0HOFS, &REG_BG1HOFS };
    volatile uint16_t *vofs[2] = { &REG_BG0VOFS, &REG_BG1VOFS };
    for (int i = 0; i < 2; i++) {
        DMA_CR(1 + i) = 0; // stop the previous frame's per-line scroll
        if (h.hw && i < h.nLayers) {
            // farther layer first: layer 0 = priority 3, layer 1 = priority 2; the sprite bitmap (BG3) is 1
            *cnt[i] = BG_PRIORITY(3 - i) | BG_COLOR_256 | BG_64x64 | BG_TILE_BASE(4) | BG_MAP_BASE(h.buf * 8 + i * 4);
            *hofs[i] = h.table[i][0] & 0x1FF;
            *vofs[i] = h.vofs[i];
            if (!h.uniform[i]) {
                DC_FlushRange(h.table[i], 193 * 4);
                DMA_SRC(1 + i)  = (uint32_t)(h.table[i] + 1);
                DMA_DEST(1 + i) = 0x04000010 + i * 4; // HOFS | VOFS << 16
                DMA_CR(1 + i)   = DMA_ENABLE | DMA_START_HBL | DMA_REPEAT | DMA_SRC_INC | DMA_DST_FIX | DMA_32_BIT | 1;
            }
            videoBgEnable(i);
        }
        else
            videoBgDisable(i);
    }
    if (h.palDirty) {
        for (int i = 0; i < 256; i++) BG_PALETTE[i] = h.pal[i];
        h.palDirty = 0;
    }
    setBrightness(1, h.bright);
    bgSetPriority(bitmapBg, 1);
}

static void waitForever(void)
{
    while (1) swiWaitForVBlank();
}

int main(void)
{
    videoSetMode(MODE_5_2D);
    // Two 256x256 16-bit bitmap pages (the window uses the top 192 lines): VRAM A = page 0, VRAM B = page 1.
    // The engine draws straight into the back page; at vblank the pages swap (no copy, no tearing).
    // Main BG VRAM (A+B+C contiguous from 0x06000000): hardware tile maps 0x0000-0x7FFF, tiles 0x10000-0x1FFFF,
    // sprite-layer bitmap pages at 0x20000 and 0x38000 (96 KB each: 256x192 16-bit).
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    vramSetBankB(VRAM_B_MAIN_BG_0x06020000);
    vramSetBankC(VRAM_C_MAIN_BG_0x06040000);
    dmaFillWords(0, (void *)0x06000000, 0x20000); // blank maps and tile 0
    int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 8, 0);
    uint16_t *page[2] = { (uint16_t *)0x06020000, (uint16_t *)0x06038000 };
    int front = 0;
    dmaFillWords(0, page[0], 0x18000);
    dmaFillWords(0, page[1], 0x18000);
    NdsSetBackBuffer(page[1]);
    NdsHwInit();
    videoSetModeSub(MODE_0_2D);
    vramSetBankH(VRAM_H_SUB_BG);
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
            printf("ticks last:%lu max:%lu\ndraw ticks:%lu\n", (unsigned long)lastT, (unsigned long)maxT, (unsigned long)NdsRenderTicks());
            printf("clear:%lu tiles:%lu obj:%lu\n", (unsigned long)g_ndsTkClear, (unsigned long)g_ndsTkTiles, (unsigned long)g_ndsTkObj);
            printf("swTiles:%lu fillCols:%lu\n", (unsigned long)g_ndsSwTiles, (unsigned long)g_ndsFillCols);
            printf("hw fr:%lu sw fr:%lu tiles:%lu fail:%lu\n", (unsigned long)g_ndsHwFrames, (unsigned long)g_ndsSwFrames,
                   (unsigned long)g_ndsHwTiles, (unsigned long)g_ndsHwFail);
            char info[200];
            NdsEngineInfo(info, sizeof(info));
            printf("%s\nlog:\n", info);
            for (int i = 2; i >= 0; i--) printf("%s\n", NdsLogLine(i));
            sumV = 0;
            maxV = 0;
        }
        swiWaitForVBlank();
        if (NdsFrameDrawn()) { // show the page just drawn, draw the next frame into the other one
            front ^= 1;
            bgSetMapBase(bg, front ? 14 : 8); // bitmap base in 16 KB units: page 0 = 0x20000, page 1 = 0x38000
            applyHw(bg);
            NdsSetBackBuffer(page[front ^ 1]);
        }
    }
}
