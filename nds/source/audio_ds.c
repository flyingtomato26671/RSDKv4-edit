// DS audio, libnds side (C, so the engine's headers never meet libnds'). The ARM7 plays everything: sound effects are
// started with soundPlaySample on the hardware channels; the music is a looping 16-bit mono ring buffer that the ARM9
// refills from the SD card in small pieces (a .dsm file made by tools/ds/convert_audio.py).
//
// libnds has no "is this channel still playing / where is it" query, so the ring's play position is computed from the
// bus timer (33.513982 MHz): one sample = 2 * floor(16777216 / rate) bus ticks, the same formula SOUND_FREQ programs.
#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

void NdsSndInit(void) { soundEnable(); }

// ---- sound effects
int NdsSfxPlay(const void *data, int is16, int bytes, int freq, int vol, int pan, int loop)
{
    DC_FlushRange(data, bytes);
    return soundPlaySample(data, is16 ? SoundFormat_16Bit : SoundFormat_8Bit, bytes, (u16)freq, (u8)vol, (u8)pan, loop != 0, 0);
}
void NdsSfxStop(int id)
{
    if (id >= 0)
        soundKill(id);
}
void NdsSfxPan(int id, int vol, int pan)
{
    if (id >= 0) {
        soundSetVolume(id, (u8)vol);
        soundSetPan(id, (u8)pan);
    }
}
uint32_t NdsTicks(void) { return cpuGetTiming(); }

// ---- music
#define MUS_RATE 22050
#define MUS_RING 16384                              // samples (32 KB)
#define MUS_REFILL 2048                             // refill when this many samples are free
#define MUS_TPS (2 * (16777216 / MUS_RATE))         // bus ticks per sample
static int16_t s_ring[MUS_RING] __attribute__((aligned(32)));
static FILE *s_f = NULL;
static int s_id = -1, s_loop = 0, s_paused = 0, s_vol = 127;
static uint32_t s_total, s_loopStart, s_dataPos;    // samples in the file, loop start (samples), file offset of the data
static uint64_t s_elapsed, s_written, s_endAbs;     // bus ticks played, samples written to the ring, sample where silence begins
static uint32_t s_lastT;
static int s_eof;

// Reads n samples (wrapping to the loop start at the end of the file) into the ring at absolute position s_written.
static void MusFill(int n)
{
    while (n > 0) {
        int pos   = (int)(s_written % MUS_RING);
        int chunk = n;
        if (chunk > MUS_RING - pos)
            chunk = MUS_RING - pos;
        int got = 0;
        if (!s_eof) {
            got = (int)fread(&s_ring[pos], 2, chunk, s_f);
            if (got < chunk) { // end of the file
                if (s_loop) {
                    fseek(s_f, (long)(s_dataPos + s_loopStart * 2), SEEK_SET);
                    int more = (int)fread(&s_ring[pos + got], 2, chunk - got, s_f);
                    got += more > 0 ? more : 0;
                }
                else {
                    s_eof    = 1;
                    s_endAbs = s_written + got;
                }
            }
        }
        if (got < chunk) // past the end of a non-looping track (or a read error): silence
            memset(&s_ring[pos + got], 0, (chunk - got) * 2);
        DC_FlushRange(&s_ring[pos], chunk * 2);
        s_written += chunk;
        n -= chunk;
    }
}

static void MusStartPlayback(void)
{
    s_written = 0;
    s_elapsed = 0;
    s_eof     = 0;
    MusFill(MUS_RING);
    s_id    = soundPlaySample(s_ring, SoundFormat_16Bit, MUS_RING * 2, MUS_RATE, (u8)s_vol, 64, true, 0);
    s_lastT = cpuGetTiming();
}

void NdsMusicStop(void)
{
    if (s_id >= 0)
        soundKill(s_id);
    s_id = -1;
    if (s_f)
        fclose(s_f);
    s_f      = NULL;
    s_paused = 0;
}

// path: a .dsm file; loopPoint in samples of the original rate stored in the header. Returns 1 when playing.
int NdsMusicStart(const char *path, int loop, unsigned loopPoint, int vol)
{
    NdsMusicStop();
    s_f = fopen(path, "rb");
    if (!s_f)
        return 0;
    uint32_t hdr[4];
    if (fread(hdr, 4, 4, s_f) != 4 || hdr[0] != 0x314D5344u /* "DSM1" */ || hdr[1] == 0) {
        fclose(s_f);
        s_f = NULL;
        return 0;
    }
    s_total     = hdr[1];
    s_dataPos   = 16;
    s_loopStart = hdr[2] ? (uint32_t)(((uint64_t)loopPoint * MUS_RATE) / hdr[2]) : 0;
    if (s_loopStart >= s_total)
        s_loopStart = 0;
    s_loop = loop;
    s_vol  = vol;
    MusStartPlayback();
    return s_id >= 0;
}

void NdsMusicVolume(int vol)
{
    s_vol = vol;
    if (s_id >= 0)
        soundSetVolume(s_id, (u8)vol);
}

void NdsMusicPause(int pause)
{
    if (s_id < 0 || pause == s_paused)
        return;
    if (pause) {
        soundPause(s_id);
        s_elapsed += (uint32_t)(cpuGetTiming() - s_lastT); // time played up to here
    }
    else {
        soundResume(s_id);
        s_lastT = cpuGetTiming(); // the pause does not count
    }
    s_paused = pause;
}

// Once per frame: keeps the ring full. Returns 1 while the music plays (0: stopped or a non-looping track ended).
int NdsMusicUpdate(void)
{
    if (s_id < 0 || !s_f)
        return 0;
    if (s_paused)
        return 1;
    uint32_t now = cpuGetTiming();
    s_elapsed += (uint32_t)(now - s_lastT);
    s_lastT = now;
    uint64_t played = s_elapsed / MUS_TPS;
    if (s_eof && played >= s_endAbs) { // the last sample was played
        NdsMusicStop();
        return 0;
    }
    if (!s_eof && played + 256 >= s_written) { // the ring ran dry (a long file read stalled us): restart from here
        soundKill(s_id);
        MusStartPlayback();
        return 1;
    }
    uint64_t freeS = played + MUS_RING - 512 - s_written; // 512 samples of margin: the hardware is a little ahead of our clock
    if (!s_eof && freeS >= MUS_REFILL)
        MusFill((int)(freeS & ~255ull));
    return 1;
}
