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

// One status line pair for the sub screen: where the engine is.
extern "C" void NdsEngineInfo(char *buf, int n)
{
    snprintf(buf, n, "stageMode:%d list:%d pos:%d\nfolder:%.18s\nframeCtr:%d ents0:%d", stageMode, activeStageList, stageListPosition,
             currentStageFolder, frameCounter, objectEntityList[0].type);
}
