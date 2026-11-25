// DS audio, engine side (no libnds here: audio_ds.c has it). Replaces the stubs: the engine's LoadSfx / PlaySfx / music API on
// the DS hardware channels (played by the ARM7). SFX are the game's .wav files read at stage load and turned into mono
// 8 / 16-bit samples in RAM; music is a .dsm file per track (tools/ds/convert_audio.py) streamed from the SD card.
#include "RetroEngine.hpp"
#include <malloc.h>

extern "C" {
void NdsSndInit(void);
int NdsSfxPlay(const void *data, int is16, int bytes, int freq, int vol, int pan, int loop);
void NdsSfxStop(int id);
void NdsSfxPan(int id, int vol, int pan);
uint32_t NdsTicks(void);
int NdsMusicStart(const char *path, int loop, unsigned loopPoint, int vol);
int NdsMusicLastResult(void);
void NdsMusicStop(void);
void NdsMusicVolume(int vol);
void NdsMusicPause(int pause);
int NdsMusicUpdate(void);
}

bool audioEnabled = true;
int musicStartPos = 0, musicRatio = 0;
TrackInfo musicTracks[TRACK_COUNT];

struct NdsSfx {
    void *pcm;    // mono, signed 8 or 16 bit
    int bytes;
    int rate;
    bool is16;
};
static NdsSfx s_sfx[SFX_COUNT];
static int s_hw[CHANNEL_COUNT];       // libnds sound id of each RSDK channel (-1: none)
static uint32_t s_endT[CHANNEL_COUNT]; // bus tick when a non-looping sample ends
static int s_musicTrack = -1;
char g_ndsMusPath[48];
int g_ndsMusRes = 0, g_ndsMusTrack = -1; // last music request: 1 playing, 0 file missing, -1 bad header
volatile uint32_t g_ndsSfxLoaded = 0, g_ndsSfxFail = 0, g_ndsSfxBytes = 0, g_ndsSfxPlays = 0;

// The global SFX names and paths from GameConfig.bin (ids 0..globalSFXCount-1; the stage's follow). Upstream's InitAudioPlayback
// calls it; without it the stage's SFX would take ids 0.. and every script would play the wrong sound.
void LoadGlobalSfx()
{
    FileInfo info;
    FileInfo infoStore;
    char strBuffer[0x100];
    byte fileBuffer = 0;
    int fileBuffer2 = 0;

    globalSFXCount = 0;

    if (LoadFile("Data/Game/GameConfig.bin", &info)) {
        infoStore = info;

        FileRead(&fileBuffer, 1);
        FileRead(strBuffer, fileBuffer);

        FileRead(&fileBuffer, 1);
        FileRead(strBuffer, fileBuffer);

        byte buf[3];
        for (int c = 0; c < 0x60; ++c) FileRead(buf, 3);

        // Read Obect Names
        byte objectCount = 0;
        FileRead(&objectCount, 1);
        for (byte o = 0; o < objectCount; ++o) {
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
        }

        // Read Script Paths
        for (byte s = 0; s < objectCount; ++s) {
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
        }

        byte varCount = 0;
        FileRead(&varCount, 1);
        for (byte v = 0; v < varCount; ++v) {
            // Read Variable Name
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);

            // Read Variable Value
            FileRead(&fileBuffer2, 4);
        }

        // Read SFX
        FileRead(&fileBuffer, 1);
        globalSFXCount = fileBuffer;
        for (byte s = 0; s < globalSFXCount; ++s) { // SFX Names
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;

            SetSfxName(strBuffer, s);
        }
        for (byte s = 0; s < globalSFXCount; ++s) { // SFX Paths
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;

            GetFileInfo(&infoStore);
            CloseFile();
            LoadSfx(strBuffer, s);
            SetFileInfo(&infoStore);
        }

        CloseFile();
    }

    for (int i = 0; i < CHANNEL_COUNT; ++i) sfxChannels[i].sfxID = -1;
}

int InitAudioPlayback()
{
    NdsSndInit();
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        s_hw[c]               = -1;
        sfxChannels[c].sfxID  = -1;
    }
    audioEnabled = true;
    LoadGlobalSfx();
    return 1;
}

// ---- sound effects
static void FreeSfx(int id)
{
    if (s_sfx[id].pcm)
        free(s_sfx[id].pcm);
    g_ndsSfxBytes = g_ndsSfxBytes - s_sfx[id].bytes;
    s_sfx[id].pcm = nullptr;
    s_sfx[id].bytes = 0;
    sfxList[id].loaded = false;
}

volatile uint32_t g_ndsSfxBadOpen = 0, g_ndsSfxBadFmt = 0, g_ndsSfxBadMem = 0;
char g_ndsSfxLastBad[40];

// Reads a .wav into a mono 8-bit signed sample (stereo averaged, rates of 32 kHz and up halved): the DS's RAM is 4 MB, the
// game's 16-bit 44 kHz effects would not fit with everything else.
static bool ReadWav(const char *path, int id)
{
    FileInfo info;
    if (!LoadFile(path, &info)) {
        g_ndsSfxBadOpen = g_ndsSfxBadOpen + 1;
        return false;
    }
    byte h[12];
    FileRead(h, 12);
    if (h[0] != 'R' || h[1] != 'I' || h[2] != 'F' || h[3] != 'F') {
        CloseFile();
        g_ndsSfxBadFmt = g_ndsSfxBadFmt + 1;
        return false;
    }
    int pos = 12, fmtTag = 0, channels = 1, rate = 22050, bits = 16, dataLen = 0, dataPos = 0;
    for (int guard = 0; guard < 16; ++guard) {
        byte ck[8];
        SetFilePosition(pos);
        FileRead(ck, 8);
        int size = ck[4] | (ck[5] << 8) | (ck[6] << 16) | (ck[7] << 24);
        if (ck[0] == 'f' && ck[1] == 'm' && ck[2] == 't') {
            byte f[16];
            FileRead(f, 16);
            fmtTag   = f[0] | (f[1] << 8);
            channels = f[2] | (f[3] << 8);
            rate     = f[4] | (f[5] << 8) | (f[6] << 16) | (f[7] << 24);
            bits     = f[14] | (f[15] << 8);
        }
        else if (ck[0] == 'd' && ck[1] == 'a' && ck[2] == 't' && ck[3] == 'a') {
            dataLen = size;
            dataPos = pos + 8;
            break;
        }
        pos += 8 + size + (size & 1);
    }
    if ((fmtTag != 1 && fmtTag != 0xFFFE) || !dataLen || channels < 1 || channels > 2 || (bits != 8 && bits != 16)) {
        CloseFile();
        g_ndsSfxBadFmt = g_ndsSfxBadFmt + 1;
        StrCopy(g_ndsSfxLastBad, path + (StrLength(path) > 38 ? StrLength(path) - 38 : 0));
        return false;
    }
    const int bytesPer = bits / 8, frameBytes = bytesPer * channels, frames = dataLen / frameBytes;
    const int ds       = rate >= 32000 ? 2 : 1; // input frames per output sample
    const int outN     = frames / ds;
    int8_t *out        = (int8_t *)malloc(outN + 4);
    if (!out) {
        CloseFile();
        g_ndsSfxBadMem = g_ndsSfxBadMem + 1;
        return false;
    }
    SetFilePosition(dataPos);
    byte buf[512];
    int inFrames = 0, o = 0, acc = 0, accN = 0;
    const int perRead = 512 / frameBytes;
    while (inFrames < frames && o < outN) {
        int n = frames - inFrames;
        if (n > perRead)
            n = perRead;
        FileRead(buf, n * frameBytes);
        for (int i = 0; i < n && o < outN; ++i) {
            int v; // 16-bit scale
            if (bits == 8) {
                v = (buf[i * channels] - 128) << 8;
                if (channels == 2)
                    v = (v + ((buf[i * 2 + 1] - 128) << 8)) / 2;
            }
            else {
                v = (int16_t)(buf[i * frameBytes] | (buf[i * frameBytes + 1] << 8));
                if (channels == 2)
                    v = (v + (int16_t)(buf[i * 4 + 2] | (buf[i * 4 + 3] << 8))) / 2;
            }
            acc += v;
            if (++accN == ds) {
                out[o++] = (int8_t)(acc / ds >> 8);
                acc      = 0;
                accN     = 0;
            }
        }
        inFrames += n;
    }
    CloseFile();
    s_sfx[id].pcm   = out;
    s_sfx[id].bytes = o;
    s_sfx[id].rate  = rate / ds;
    s_sfx[id].is16  = false;
    g_ndsSfxBytes   = g_ndsSfxBytes + o;
    return true;
}

void LoadSfx(char *filePath, byte sfxID)
{
    if (sfxID >= SFX_COUNT)
        return;
    if (s_sfx[sfxID].pcm)
        FreeSfx(sfxID);
    StrCopy(sfxList[sfxID].name, filePath);
    char path[0x80];
    StrCopy(path, "Data/SoundFX/");
    StrAdd(path, filePath);
    int pl = StrLength(path); // the global effects are .ogg in the game's data: tools/ds/convert_audio.py writes a .wav next to each
    if (pl > 4 && path[pl - 4] == '.' && (path[pl - 3] | 32) == 'o' && (path[pl - 2] | 32) == 'g' && (path[pl - 1] | 32) == 'g') {
        path[pl - 3] = 'w';
        path[pl - 2] = 'a';
        path[pl - 1] = 'v';
    }
    if (ReadWav(path, sfxID)) {
        sfxList[sfxID].loaded = true;
        g_ndsSfxLoaded        = g_ndsSfxLoaded + 1;
    }
    else {
        sfxList[sfxID].loaded = false;
        g_ndsSfxFail          = g_ndsSfxFail + 1;
    }
}

void PS1SfxLoadStageBank() {}
void PS1SfxReleaseStage()
{
    for (int c = 0; c < CHANNEL_COUNT; ++c) PS1SfxChannelStop(c);
    for (int i = globalSFXCount; i < SFX_COUNT; ++i)
        if (s_sfx[i].pcm)
            FreeSfx(i);
}

static int SfxVolume() { return 127 * sfxVolume / MAX_VOLUME; }
static int SfxPan(int pan) { return 64 + pan * 63 / 100; } // RSDK pan -100..100 -> 0..127

void PS1SfxChannelStop(int c)
{
    if ((uint)c >= CHANNEL_COUNT)
        return;
    if (s_hw[c] >= 0)
        NdsSfxStop(s_hw[c]);
    s_hw[c] = -1;
}

void PlaySfx(int sfx, bool loop)
{
    if ((uint)sfx >= SFX_COUNT || !sfxList[sfx].loaded || !s_sfx[sfx].pcm)
        return;
    int ch = -1;
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        if (sfxChannels[c].sfxID == sfx || sfxChannels[c].sfxID == -1) {
            ch = c;
            break;
        }
    }
    if (ch < 0)
        return;
    PS1SfxChannelStop(ch); // the same sample restarts
    ChannelInfo *info  = &sfxChannels[ch];
    info->sfxID        = sfx;
    info->samplePtr    = nullptr;
    info->sampleLength = s_sfx[sfx].bytes;
    info->loopSFX      = loop;
    info->pan          = 0;
    NdsSfx &s          = s_sfx[sfx];
    int id             = NdsSfxPlay(s.pcm, s.is16, s.bytes, s.rate, SfxVolume(), 64, loop);
    s_hw[ch]           = id;
    g_ndsSfxPlays      = g_ndsSfxPlays + 1;
    if (id < 0) { // no free hardware channel
        info->sfxID = -1;
        return;
    }
    const int samples = s.bytes / (s.is16 ? 2 : 1);
    s_endT[ch]        = NdsTicks() + (uint32_t)(((uint64_t)samples * 33513982u) / (uint32_t)(s.rate ? s.rate : 22050)) + 1000000u; // + 30 ms
}

void SetSfxAttributes(int sfx, int loopCount, sbyte pan)
{
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        if (sfxChannels[c].sfxID == sfx) {
            sfxChannels[c].loopSFX = loopCount == -1 ? sfxChannels[c].loopSFX : loopCount;
            sfxChannels[c].pan     = pan;
            NdsSfxPan(s_hw[c], SfxVolume(), SfxPan(pan));
            return;
        }
    }
}

void SetSfxName(const char *sfxName, int sfxID)
{
    if ((uint)sfxID >= SFX_COUNT)
        return;
    int a = 0, b = 0;
    while (sfxName[a]) {
        if (sfxName[a] != ' ')
            sfxNames[sfxID][b++] = sfxName[a];
        ++a;
    }
    sfxNames[sfxID][b] = 0;
}

// ---- music
static void TrackPath(const char *file, char *out)
{
    StrCopy(out, file); // "Data/Music/X.ogg" -> "Data/Music/X.dsm"
    int n = StrLength(out);
    while (n > 0 && out[n - 1] != '.' && out[n - 1] != '/') --n;
    if (n > 0 && out[n - 1] == '.')
        out[n - 1] = 0;
    StrAdd(out, ".dsm");
}

void SetMusicTrack(const char *filePath, byte trackID, bool loop, uint loopPoint)
{
    if (trackID >= TRACK_COUNT)
        return;
    TrackInfo *track = &musicTracks[trackID];
    StrCopy(track->fileName, "Data/Music/");
    StrAdd(track->fileName, filePath);
    track->trackLoop = loop;
    track->loopPoint = loopPoint;
}

bool PlayMusic(int track, int startPos)
{
    if (!audioEnabled || (uint)track >= TRACK_COUNT || !musicTracks[track].fileName[0]) {
        NdsMusicStop();
        musicStatus  = MUSIC_STOPPED;
        s_musicTrack = -1;
        return false;
    }
    char path[0x60];
    TrackPath(musicTracks[track].fileName, path);
    musicStartPos = startPos;
    int vol       = 127 * bgmVolume / MAX_VOLUME * masterVolume / MAX_VOLUME;
    {
        int n = 0;
        for (; path[n] && n < 47; ++n) g_ndsMusPath[n] = path[n];
        g_ndsMusPath[n] = 0;
        g_ndsMusTrack   = track;
    }
    bool started = NdsMusicStart(path, musicTracks[track].trackLoop, musicTracks[track].loopPoint, vol);
    g_ndsMusRes  = NdsMusicLastResult();
    if (!started) {
        musicStatus  = MUSIC_STOPPED;
        s_musicTrack = -1;
        return false;
    }
    musicStatus  = MUSIC_PLAYING;
    s_musicTrack = track;
    return true;
}

void SwapMusicTrack(const char *filePath, byte trackID, uint loopPoint, uint ratio)
{
    if (StrLength(filePath) <= 0) {
        StopMusic(true);
        return;
    }
    if (trackID >= TRACK_COUNT)
        return;
    TrackInfo *track = &musicTracks[trackID];
    StrCopy(track->fileName, "Data/Music/");
    StrAdd(track->fileName, filePath);
    track->trackLoop = true;
    track->loopPoint = loopPoint;
    musicRatio       = ratio;
    PlayMusic(trackID, 1);
}

void PS1MusicPause(bool pause) { NdsMusicPause(pause); }
void PS1MusicStop() { NdsMusicStop(); }

// Once per frame (main.cpp): frees channels whose sample ended, applies the music volume, keeps the music ring full.
extern "C" void NdsAudioFrame(void)
{
    if (!audioEnabled)
        return;
    const uint32_t now = NdsTicks();
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        const int id = sfxChannels[c].sfxID;
        if (id < 0)
            continue;
        if (!sfxChannels[c].loopSFX && (int32_t)(now - s_endT[c]) >= 0) {
            // The sample ended by itself on the ARM7: just free the engine's channel. A soundKill here could cut the tail of a
            // sample whose end we estimated a little early, or kill another sound that got the same hardware channel meanwhile.
            s_hw[c]              = -1;
            sfxChannels[c].sfxID = -1;
        }
    }
    if (musicStatus == MUSIC_PLAYING) {
        NdsMusicVolume(127 * bgmVolume / MAX_VOLUME * masterVolume / MAX_VOLUME);
        if (!NdsMusicUpdate())
            musicStatus = MUSIC_STOPPED;
    }
    else if (musicStatus == MUSIC_PAUSED)
        NdsMusicUpdate();
}

// Heap bytes still free (diagnostic for the console)
extern "C" unsigned NdsHeapFree(void)
{
    struct mallinfo mi = mallinfo();
    return (unsigned)mi.fordblks;
}
