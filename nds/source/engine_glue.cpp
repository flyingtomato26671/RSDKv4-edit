// Engine side of the DS app. Kept apart from main.cpp so libnds headers and RetroEngine.hpp never meet in one file.
#include "RetroEngine.hpp"

extern "C" void NdsEngineInit(void)
{
    memset(&scriptEng, 0, sizeof(scriptEng));
    // "No start stage requested" is 0xFF (Userdata.cpp's ini defaults; PS1 reaches it through InitUserdata): the
    // fields start at -1, which Engine.Init would read as a stage request (list -1 / stage -1 / player -1).
    Engine.startList = Engine.startStage = Engine.startPlayer = Engine.startSave = 0xFF;
    Engine.startList_Game = Engine.startStage_Game = 0xFF;
    Engine.Init(); // GameConfig, global scripts, first stage (relative paths: the app has chdir'd to the data folder)
}
extern "C" int NdsEngineReady(void) { return Engine.initialised ? 1 : 0; }
extern "C" int NdsEngineMode(void) { return Engine.gameMode; }
extern "C" void NdsEngineFrame(void) { Engine.RunFrame(); }

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
