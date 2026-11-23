// DS platform stubs: every symbol the engine core expects from a platform layer (PS1: ps1/*, upstream:
// Drawing / Renderer / Audio / Userdata / Scene3D), defined empty so the ARM9 ROM links. Each group is replaced by a
// real implementation in its own file (nds/source/render.cpp, audio.cpp, ...) and removed from here.
#include "RetroEngine.hpp"

// ---- screen / drawing state (real: render.cpp)
int SCREEN_XSIZE = 320, SCREEN_CENTERX = 160;
GFXSurface gfxSurface[SURFACE_COUNT];
int gfxDataPosition = 0;
DrawListEntry drawListEntries[DRAWLAYER_COUNT];
volatile uint32_t g_ps1AtlasLoads = 0, g_ps1SurfaceGen = 0, g_ps1TileInputGen = 0;
int InitRenderDevice() { return 1; }
void ResetRenderStates() {}
void FlipScreen() {}
void RenderScene() {}
void DrawObjectList(int) {}
void DrawObjectAnimation(void *, void *, int, int) {}
void DrawDebugOverlays() {}
void DrawTextMenu(void *, int, int) {}
void DrawRectangle(int, int, int, int, int, int, int, int) {}
void DrawTintRectangle(int, int, int, int) {}
void DrawSprite(int, int, int, int, int, int, int) {}
void DrawBlendedSprite(int, int, int, int, int, int, int) {}
void DrawSpriteFlipped(int, int, int, int, int, int, int, int) {}
void DrawAlphaBlendedSprite(int, int, int, int, int, int, int, int) {}
void DrawAdditiveBlendedSprite(int, int, int, int, int, int, int, int) {}
void DrawSubtractiveBlendedSprite(int, int, int, int, int, int, int, int) {}
void DrawSpriteRotated(int, int, int, int, int, int, int, int, int, int, int) {}
void DrawSpriteScaled(int, int, int, int, int, int, int, int, int, int, int, int) {}
void DrawSpriteRotozoom(int, int, int, int, int, int, int, int, int, int, int, int) {}
void DrawScaledTintMask(int, int, int, int, int, int, int, int, int, int, int, int) {}
void GenerateBlendLookupTable() {}
void FadeScreen_Create(void *) {}
void FadeScreen_Main(void *) {}
bool PS1AtlasLoad(const char *, int) { return true; }
void PS1BuildTileVisuals() {}
void PS1Copy16x16Tile(int, int) {}
void PS1LoadBGStrips(const char *, int) {}
void PS1PaletteLinesChanged() {}
void PS1TileChanged(int) {}

// ---- audio (real: audio.cpp)
int masterVolume = 100, bgmVolume = 100, sfxVolume = 100;
bool musicEnabled = true;
StreamInfo streamInfo[STREAMFILE_COUNT];
int musicStatus = 0, musicPosition = 0, currentStreamIndex = 0, trackID = 0;
int globalSFXCount = 0, stageSFXCount = 0;
ChannelInfo sfxChannels[CHANNEL_COUNT];
SFXInfo sfxList[SFX_COUNT];
char sfxNames[SFX_COUNT][0x20];
int InitAudioPlayback() { return 1; }
void LoadSfx(char *, byte) {}
void PlaySfx(int, bool) {}
void SetSfxAttributes(int, int, sbyte) {}
void SetSfxName(const char *, int) {}
bool PlayMusic(int, int) { return false; }
void SetMusicTrack(const char *, byte, bool, uint) {}
void SwapMusicTrack(const char *, byte, uint, uint) {}
void PS1MusicPause(bool) {}
void PS1MusicStop() {}
void PS1SfxChannelStop(int) {}
void PS1SfxLoadStageBank() {}
void PS1SfxReleaseStage() {}

// ---- files / timing / loading (real: files.cpp)
void PS1FsSetLoadFolder(const char *) {}
void PS1LoadingBegin() {}
void PS1LoadingEnd() {}
uint32_t PS1Hblanks() { return 0; }
uint32_t PS1HblanksSince(uint32_t) { return 0; }

// ---- menus / pause (real: menu.cpp)
void PS1MenuOpen(int) {}
void PS1MenuFrame() {}
void PS1MenuResetGame() {}
// Minimal pause: the engine enters ENGINE_INITPAUSE on a script's Start; Start again resumes (the real menu comes later).
void PS1PauseOpen() { Engine.gameMode = ENGINE_PS1PAUSE; }
void PS1PauseFrame()
{
    if (inputDevice[INPUT_START].press)
        Engine.gameMode = ENGINE_EXITPAUSE;
}

// ---- save data (real: save.cpp, FAT on the SD card)
int saveRAM[SAVEDATA_SIZE];
volatile uint32_t g_ps1SaveRAMClamped = 0;
void InitUserdata() {}
bool ReadSaveRAMData() { return false; }
bool WriteSaveRAMData() { return false; }

// ---- Scene3D (special stages; real: scene3d.cpp, later)
Face *faceBuffer = nullptr;
Vertex *vertexBuffer = nullptr;
int faceCount = 0, vertexCount = 0, g_ps1VtxCap = 0, g_ps1FaceCap = 0;
int fogColor = 0, fogStrength = 0, projectionX = 0, projectionY = 0;
Matrix matWorld, matView, matTemp;
void PS1Scene3DPrepare(const char *) {}
void PS1Draw3DSceneOT(int) {}
void TransformVertexBuffer() {}
void TransformVertices(Matrix *, int, int) {}
void SetIdentityMatrix(Matrix *) {}
void MatrixMultiply(Matrix *, Matrix *) {}
void MatrixInverse(Matrix *) {}
void MatrixRotateX(Matrix *, int) {}
void MatrixRotateY(Matrix *, int) {}
void MatrixRotateZ(Matrix *, int) {}
void MatrixRotateXYZ(Matrix *, short, short, short) {}
void MatrixScaleXYZ(Matrix *, int, int, int) {}
void MatrixTranslateXYZ(Matrix *, int, int, int) {}

// ---- online / achievements / globals the PS1 build has no use for
int achievementCount = 0;
Achievement achievements[ACHIEVEMENT_COUNT];
int globalVariablesCount = 0;
int globalVariables[GLOBALVAR_COUNT];
char globalVariableNames[GLOBALVAR_COUNT][0x20];
void *nativeFunction[NATIIVEFUNCTION_COUNT];
int nativeFunctionCount = 0;
bool forceUseScripts = false, skipStartMenu = false;
void Connect2PVS(int *, int *) {}
void Disconnect2PVS() {}
void SendEntity(int *, int *) {}
void SendValue(int *, int *) {}
void ReceiveEntity(int *, int *) {}
void ReceiveValue(int *, int *) {}
void NotifyCallback(int *, int *, int *, int *) {}
void SetAchievement(int *, int *) {}
int SetLeaderboard(int *, int *) { return 0; }
void ShowPromoPopup(int *, const char *) {}
void TransmitGlobal(int *, const char *) {}
