// Engine side of the DS app. Kept apart from main.cpp so libnds headers and RetroEngine.hpp never meet in one file.
#include "RetroEngine.hpp"

int g_ndsStartPlayer = 0; // 0 Sonic, 1 Tails, 2 Knuckles, 3 Sonic & Tails (stage.playerListPos)
extern "C" void NdsEngineStart(int player, int debug)
{
    g_ndsStartPlayer = player;
    debugMode        = debug != 0;
}

extern "C" void NdsEngineInit(void)
{
    memset(&scriptEng, 0, sizeof(scriptEng));
    // "No start stage requested" is 0xFF (Userdata.cpp's ini defaults; PS1 reaches it through InitUserdata): the
    // fields start at -1, which Engine.Init would read as a stage request (list -1 / stage -1 / player -1).
    Engine.startList = Engine.startStage = Engine.startPlayer = Engine.startSave = 0xFF;
    Engine.startList_Game = Engine.startStage_Game = 0xFF;
    Engine.startSave = 0; // a new game: Emerald Hill 1 with the character picked on the start screen (no title screen)
    Engine.Init(); // GameConfig, global scripts, first stage (relative paths: the app has chdir'd to the data folder)
}
extern "C" int NdsEngineReady(void) { return Engine.initialised ? 1 : 0; }
extern "C" int NdsEngineMode(void) { return Engine.gameMode; }
extern "C" void NdsAudioFrame(void); // audio.cpp
extern "C" void NdsEngineFrame(void)
{
    Engine.RunFrame();
    NdsAudioFrame(); // ended sound effects, music volume, music ring refill
}

// Status lines for the sub screen: where the engine is.
extern "C" void NdsEngineInfo(char *buf, int n)
{
    int k = snprintf(buf, n, "mode:%d list:%d pos:%d fr:%d\nfolder:[%.14s]\ncnt:%d,%d,%d,%d gv:%d\n", stageMode, activeStageList,
                     stageListPosition, frameCounter, currentStageFolder, stageListCount[0], stageListCount[1], stageListCount[2],
                     stageListCount[3], globalVariablesCount);
    snprintf(buf + k, n - k, "s00:[%.10s][%.4s] pl:%d", stageList[0][0].folder, stageList[0][0].id, playerListPos);
}

// Last engine log lines (PrintLog), newest last.
#define NDS_LOG_LINES 5
#define NDS_LOG_W 96
static char s_log[NDS_LOG_LINES][NDS_LOG_W];
static int s_logN = 0;
extern "C" void NdsLogPush(const char *line)
{
    char *d = s_log[s_logN % NDS_LOG_LINES];
    int i = 0;
    for (; line[i] && line[i] != '\n' && i < NDS_LOG_W - 1; i++) d[i] = line[i];
    d[i] = 0;
    s_logN++;
}
extern "C" const char *NdsLogLine(int back) // 0 = newest
{
    if (back >= NDS_LOG_LINES || back >= s_logN) return "";
    return s_log[(s_logN - 1 - back) % NDS_LOG_LINES];
}

#include "ndsdraw.hpp"
// The three types with most ticks in `arr` (last frame): "name:ticks" lines into buf
static void Top3(const uint32_t *arr, char *buf, int n)
{
    int best[3] = { -1, -1, -1 };
    for (int t = 1; t < 256; ++t) {
        if (!arr[t])
            continue;
        for (int k = 0; k < 3; ++k) {
            if (best[k] < 0 || arr[t] > arr[best[k]]) {
                for (int j = 2; j > k; --j) best[j] = best[j - 1];
                best[k] = t;
                break;
            }
        }
    }
    int o = 0;
    for (int k = 0; k < 3 && best[k] >= 0 && o < n - 1; ++k) o += snprintf(buf + o, n - o, "%.11s:%lu ", typeNames[best[k]], (unsigned long)arr[best[k]]);
    if (o == 0 && n > 0) buf[0] = 0;
}
extern "C" uint32_t NdsTkLogic(void) { return g_ndsTkLogic; }
extern "C" void NdsTopUpd(char *buf, int n) { Top3(g_ndsTypeUpd, buf, n); }
extern "C" void NdsTopDraw(char *buf, int n) { Top3(g_ndsTypeDraw, buf, n); }
