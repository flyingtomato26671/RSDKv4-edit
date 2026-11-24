// RSDKv4 for the DS: boots the engine core from the SD card's /rsdkv4 folder and runs frames.
// Main screen: the engine's picture (hardware tile backgrounds BG0-BG2 + a sprite bitmap on BG3, see render.cpp).
// Sub screen: a character picker at start, then a status console (touch the screen to change page).
#include <nds.h>
#include <fat.h>
#include <filesystem.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

extern "C" {
uint16_t NdsPollInput(void);
extern int g_ndsDebugKeys;
void NdsEngineStart(int player, int debug);
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
unsigned NdsHeapFree(void);
struct NdsHwFrame {
    int hw, nLayers, used[3], bg2Front, buf;
    uint16_t vofs[3];
    int uniform[3];
    uint32_t *table[3];
    uint16_t hofs[3];
    int clearIdx, bank0, nSwitch, swLine[4], swBank[4];
    int bright;
};
extern NdsHwFrame g_ndsHw;
extern volatile char g_ndsPassInfo[8];
extern volatile char g_ndsHudNames[96];
extern volatile uint32_t g_ndsSwTiles, g_ndsFillCols, g_ndsBlitTicks, g_ndsBlitCount, g_ndsBlitPix;
extern volatile uint32_t g_ndsSfxLoaded, g_ndsSfxFail, g_ndsSfxPlays, g_ndsSfxBytes;
extern volatile uint32_t g_ndsSfxBadOpen, g_ndsSfxBadFmt, g_ndsSfxBadMem;
extern char g_ndsSfxLastBad[40];
extern char g_ndsMusPath[48];
extern int g_ndsMusRes, g_ndsMusTrack;
extern volatile uint32_t g_ndsTkClear, g_ndsTkTiles, g_ndsTkObj;
extern volatile uint32_t g_ndsHwFrames, g_ndsSwFrames, g_ndsHwTiles, g_ndsHwFail;
}
extern uint16_t fullPalette[8][256]; // the engine's palettes (DS colours)

static volatile unsigned g_vbl = 0;
static void vblankCount(void) { g_vbl = g_vbl + 1; }

// ---- water: the engine's palette bank changes down the screen; the hardware tile backgrounds have one palette, so it is
// switched in the HBlank before the line where the bank changes (VCOUNT interrupt).
static int s_swN = 0, s_swK = 0, s_swLine[4], s_swBank[4], s_clearIdx = 0;
static void palCopy(int bank)
{
    const uint32_t *src = (const uint32_t *)fullPalette[bank];
    volatile uint32_t *dst = (volatile uint32_t *)BG_PALETTE;
    for (int i = 0; i < 128; i++) dst[i] = src[i];
    BG_PALETTE[0] = fullPalette[bank][s_clearIdx]; // entry 0 is the backdrop
}
static inline void setVCount(int line)
{
    REG_DISPSTAT = (REG_DISPSTAT & 0x003F) | ((line & 0xFF) << 8) | ((line & 0x100) ? 0x80 : 0);
}
static void vcountHandler(void)
{
    while (!(REG_DISPSTAT & DISP_IN_HBLANK)) {} // the switch happens in the HBlank of the line before
    palCopy(s_swBank[s_swK]);
    s_swK++;
    if (s_swK < s_swN)
        setVCount(s_swLine[s_swK] - 1);
    else
        irqDisable(IRQ_VCOUNT);
}

// Applies the hardware-background state the engine prepared for the frame just drawn (called right after vblank)
static void applyHw(int bitmapBg)
{
    NdsHwFrame &h = g_ndsHw;
    volatile uint16_t *cnt[2]  = { &REG_BG0CNT, &REG_BG1CNT };
    volatile uint16_t *hofs[2] = { &REG_BG0HOFS, &REG_BG1HOFS };
    volatile uint16_t *vofs[2] = { &REG_BG0VOFS, &REG_BG1VOFS };
    for (int i = 0; i < 2; i++) {
        DMA_CR(1 + i) = 0; // stop the previous frame's per-line scroll
        if (h.hw && h.used[i]) {
            // farther layer first: BG0 = priority 3, BG1 = 2, the extended BG2 = 1; the sprite bitmap (BG3) is 0
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
    if (h.hw && h.used[2]) { // the third pass: extended BG (16-bit map entries), whole-layer scroll through the affine origin
        bgSetMapBase(2, 16 + h.buf * 4);
        REG_BG2X = (int)h.hofs[2] << 8;
        REG_BG2Y = (int)h.vofs[2] << 8;
        videoBgEnable(2);
    }
    else
        videoBgDisable(2);
    if (h.hw) { // palette: bank of the first line now, the others in the HBlanks before their lines
        s_clearIdx = h.clearIdx;
        palCopy(h.bank0);
        s_swN = h.nSwitch;
        s_swK = 0;
        for (int i = 0; i < s_swN; i++) {
            s_swLine[i] = h.swLine[i];
            s_swBank[i] = h.swBank[i];
        }
        if (s_swN > 0) {
            setVCount(s_swLine[0] - 1);
            irqEnable(IRQ_VCOUNT);
        }
        else
            irqDisable(IRQ_VCOUNT);
    }
    setBrightness(1, h.bright);
    bgSetPriority(bitmapBg, h.bg2Front ? 1 : 0); // a front pass on BG2 ties with the bitmap (priority 1) and wins by BG index
}

// ---- the sub screen: fixed 24 x 32 text, rewritten in place (no clear, no scrolling)
static int s_row = 0;
static void row(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_row < 24) {
        printf("\x1b[%d;0H%-32.32s", s_row, buf);
        s_row++;
    }
}
static void rowsEnd()
{
    while (s_row < 24) row("");
    s_row = 0;
}

static void showStatus(int page, unsigned frame, unsigned sumV, unsigned maxV, uint32_t lastT, uint32_t maxT)
{
    s_row = 0;
    if (page == 0) {
        row("RSDKv4 DS 1/3 (touch: page)");
        row("frames:%u mode:%d", frame, NdsEngineMode());
        row("vbl/frame x16:%u max:%u", sumV, maxV);
        row("(16 = 60 fps, 32 = 30 fps)");
        row("ticks:%lu max:%lu", (unsigned long)lastT, (unsigned long)maxT);
        row("draw:%lu", (unsigned long)NdsRenderTicks());
        row(" clear:%lu", (unsigned long)g_ndsTkClear);
        row(" tiles:%lu", (unsigned long)g_ndsTkTiles);
        row(" obj:%lu", (unsigned long)g_ndsTkObj);
        row("  blit:%lu x%lu", (unsigned long)g_ndsBlitTicks, (unsigned long)g_ndsBlitCount);
        row("  px:%lu", (unsigned long)g_ndsBlitPix);
        row("passes:%c%c%c%c swT:%lu", g_ndsPassInfo[0], g_ndsPassInfo[1], g_ndsPassInfo[2], g_ndsPassInfo[3],
            (unsigned long)g_ndsSwTiles);
        row("fillCols:%lu", (unsigned long)g_ndsFillCols);
        row("hw:%lu sw:%lu", (unsigned long)g_ndsHwFrames, (unsigned long)g_ndsSwFrames);
        row("hw tiles:%lu fail:%lu", (unsigned long)g_ndsHwTiles, (unsigned long)g_ndsHwFail);
        row("water switches:%d", g_ndsHw.nSwitch);
        row("heap free: %u KB", NdsHeapFree() / 1024);
    }
    else if (page == 1) {
        row("RSDKv4 DS 2/3: audio");
        row("sfx ok:%lu bad:%lu", (unsigned long)g_ndsSfxLoaded, (unsigned long)g_ndsSfxFail);
        row(" open:%lu fmt:%lu mem:%lu", (unsigned long)g_ndsSfxBadOpen, (unsigned long)g_ndsSfxBadFmt, (unsigned long)g_ndsSfxBadMem);
        row(" ram:%lu KB played:%lu", (unsigned long)(g_ndsSfxBytes / 1024), (unsigned long)g_ndsSfxPlays);
        row(" last bad:");
        row(" %s", g_ndsSfxLastBad);
        row("music track:%d result:%d", g_ndsMusTrack, g_ndsMusRes);
        row("(1 = playing, 0 = no file)");
        row(" %s", g_ndsMusPath);
        row("hud:%s", (const char *)g_ndsHudNames);
    }
    else {
        row("RSDKv4 DS 3/3: engine");
        char info[200];
        NdsEngineInfo(info, sizeof(info));
        char *l = info;
        for (int i = 0; i < 6 && l; i++) {
            char *e = strchr(l, '\n');
            if (e)
                *e = 0;
            row("%s", l);
            l = e ? e + 1 : nullptr;
        }
        row("log:");
        for (int i = 4; i >= 0; i--) row("%s", NdsLogLine(i));
    }
    rowsEnd();
}

static void waitForever(void)
{
    while (1) swiWaitForVBlank();
}

// ---- start screen: character and debug mode
static void pickCharacter(int *player, int *debug)
{
    static const char *names[4] = { "Sonic", "Sonic & Tails", "Tails", "Knuckles" };
    static const int ids[4]     = { 0, 3, 1, 2 }; // stage.playerListPos: 0 Sonic, 1 Tails, 2 Knuckles, 3 Sonic & Tails
    int sel = 0, dbg = 0;
    while (1) {
        s_row = 0;
        row("RSDKv4 DS - Sonic 2");
        row("");
        row("Choose your character:");
        row("");
        for (int i = 0; i < 4; i++) row(" %s %s", i == sel ? ">" : " ", names[i]);
        row("");
        row(" Debug mode: %s  (SELECT)", dbg ? "ON " : "OFF");
        row("");
        row(" UP / DOWN: choose");
        row(" A / START: play");
        rowsEnd();
        swiWaitForVBlank();
        scanKeys();
        uint32_t d = keysDown();
        if ((d & KEY_UP) && sel > 0)
            sel--;
        if ((d & KEY_DOWN) && sel < 3)
            sel++;
        if (d & KEY_SELECT)
            dbg ^= 1;
        if (d & (KEY_A | KEY_START))
            break;
    }
    *player = ids[sel];
    *debug  = dbg;
}

int main(void)
{
    videoSetMode(MODE_5_2D);
    // Main BG VRAM (A+B+C contiguous from 0x06000000): hardware tile maps 0x0000-0x7FFF, tiles 0x10000-0x1FFFF,
    // sprite-layer bitmap pages at 0x20000 and 0x38000 (96 KB each: 256x192 16-bit). The engine draws sprites into the back
    // page; at vblank the pages swap (no copy, no tearing).
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    vramSetBankB(VRAM_B_MAIN_BG_0x06020000);
    vramSetBankC(VRAM_C_MAIN_BG_0x06040000);
    dmaFillWords(0, (void *)0x06000000, 0x20000); // blank maps and tile 0
    int bg2 = bgInit(2, BgType_ExRotation, BgSize_ER_512x512, 16, 4); // third tile pass: map 0x8000 / 0xA000, tiles 0x10000
    bgWrapOn(bg2);
    bgSetPriority(bg2, 1);
    videoBgDisable(2);
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
    irqSet(IRQ_VCOUNT, vcountHandler);
    row("RSDKv4 DS");
    rowsEnd();

    if (!fatInitDefault()) {
        row("FAT init failed: no SD card.");
        rowsEnd();
        waitForever();
    }
    // Data lives in /rsdkv4 (Data/ and Bytecode/ extracted from your Data.rsdk), or in the SD root.
    if (chdir("/rsdkv4") != 0)
        chdir("/");
    FILE *f = fopen("Data/Game/GameConfig.bin", "rb");
    if (!f) {
        row("Data/Game/GameConfig.bin");
        row("not found on the SD card.");
        row("Put the extracted Data/ and");
        row("Bytecode/ in /rsdkv4/");
        rowsEnd();
        waitForever();
    }
    fclose(f);

    int player = 0, debug = 0;
    pickCharacter(&player, &debug);
    NdsEngineStart(player, debug);
    g_ndsDebugKeys = debug;
    row("loading...");
    rowsEnd();
    NdsEngineInit();

    // Timing two ways: vblanks elapsed during RunFrame (an IRQ counter: 1 = full speed) and raw ARM9 timer ticks.
    irqSet(IRQ_VBLANK, vblankCount);
    irqEnable(IRQ_VBLANK);
    cpuStartTiming(0);
    unsigned frame = 0, sumV = 0, maxV = 0;
    uint32_t lastT = 0, maxT = 0;
    int page_ = 0;
    bool touchPrev = false;
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
        const bool touch = keysHeld() & KEY_TOUCH;
        const bool turn  = touch && !touchPrev;
        touchPrev        = touch;
        if (turn)
            page_ = (page_ + 1) % 3;
        swiWaitForVBlank();
        if (NdsFrameDrawn()) { // show the page just drawn, draw the next frame into the other one
            front ^= 1;
            bgSetMapBase(bg, front ? 14 : 8); // bitmap base in 16 KB units: page 0 = 0x20000, page 1 = 0x38000
            applyHw(bg);
            NdsSetBackBuffer(page[front ^ 1]);
        }
        if ((frame & 15) == 0 || turn) { // text is written right after vblank, in place: no flicker
            showStatus(page_, frame, sumV, maxV, lastT, maxT);
            if ((frame & 15) == 0) {
                sumV = 0;
                maxV = 0;
            }
        }
    }
}
