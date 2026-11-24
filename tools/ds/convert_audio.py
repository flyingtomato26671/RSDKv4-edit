#!/usr/bin/env python3
"""Music for the Nintendo DS port: every Data/Music/*.ogg -> a .dsm file next to it (nds/source/audio_ds.c streams it).

.dsm = 16-byte header (b'DSM1', samples at 22050 Hz, the original sample rate, 0) + signed 16-bit little-endian mono PCM at
22050 Hz. The original rate is kept because the engine's loop points are in samples of the ogg: the DS scales them.

Decoder: ffmpeg (preferred; also gives the original rate through ffprobe) or, without it, the python packages
soundfile + numpy (pip install soundfile numpy; a plain linear resampler).

Usage: convert_audio.py DATA/Music [--delete-ogg]
"""
import os, shutil, struct, subprocess, sys

RATE = 22050


def via_ffmpeg(path):
    ff, fp = shutil.which('ffmpeg'), shutil.which('ffprobe')
    orig = 44100
    if fp:
        r = subprocess.run([fp, '-v', 'error', '-select_streams', 'a:0', '-show_entries', 'stream=sample_rate', '-of',
                            'default=nw=1:nk=1', path], capture_output=True, text=True)
        try:
            orig = int(r.stdout.strip())
        except ValueError:
            pass
    r = subprocess.run([ff, '-v', 'error', '-i', path, '-f', 's16le', '-ac', '1', '-ar', str(RATE), '-'], capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.decode(errors='replace'))
    return r.stdout, orig


def via_soundfile(path):
    import numpy as np, soundfile as sf
    data, rate = sf.read(path, dtype='float32', always_2d=True)
    mono = data.mean(axis=1)
    n = int(len(mono) * RATE / rate)
    out = np.interp(np.linspace(0, len(mono) - 1, n), np.arange(len(mono)), mono)
    return (np.clip(out, -1, 1) * 32767).astype('<i2').tobytes(), rate


def convert(path):
    pcm, orig = via_ffmpeg(path) if shutil.which('ffmpeg') else via_soundfile(path)
    out = os.path.splitext(path)[0] + '.dsm'
    with open(out, 'wb') as f:
        f.write(b'DSM1' + struct.pack('<III', len(pcm) // 2, orig, 0))
        f.write(pcm)
    return out, len(pcm) // 2, orig


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root, delete = sys.argv[1], '--delete-ogg' in sys.argv
    n = 0
    for dp, _, files in os.walk(root):
        for fn in sorted(files):
            if fn.lower().endswith('.ogg'):
                p = os.path.join(dp, fn)
                out, samples, orig = convert(p)
                print('  %s -> %s (%.1f s, original %d Hz)' % (fn, os.path.basename(out), samples / RATE, orig))
                if orig != 44100:
                    print('  note: %s is %d Hz; loop points are scaled by the stored rate' % (fn, orig))
                if delete:
                    os.remove(p)
                n += 1
    print('[music] %d tracks converted' % n)


if __name__ == '__main__':
    main()
