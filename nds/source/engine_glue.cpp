// Engine side of the DS app. Kept apart from main.cpp so libnds headers and RetroEngine.hpp never meet in one file.
#include "RetroEngine.hpp"

extern "C" void NdsEngineInit(void)
{
    memset(&scriptEng, 0, sizeof(scriptEng));
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
