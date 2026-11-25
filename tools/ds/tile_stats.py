#!/usr/bin/env python3
"""How many distinct 8x8 tiles does each stage need? (DS hardware backgrounds hold 1024 per tile set.)

Per stage: the 16x16 tiles the stage's chunks use, and the distinct 8x8 tiles they make, counting an 8x8 tile and its
horizontal / vertical flips as one (the DS flips tiles in hardware). Needs Pillow:  pip install pillow
Usage: tile_stats.py ROOT   (ROOT contains Data/Stages/<Zone>/16x16Tiles.gif + 128x128Tiles.bin, e.g. build/iso)
"""
import os, sys
from PIL import Image

root = sys.argv[1]
stages = os.path.join(root, 'Data', 'Stages')
print('%-10s %8s %8s %8s %8s' % ('stage', '16x16', '8x8', 'unique', 'uniq+flip'))
for st in sorted(os.listdir(stages)):
    gif, chunks = os.path.join(stages, st, '16x16Tiles.gif'), os.path.join(stages, st, '128x128Tiles.bin')
    if not (os.path.exists(gif) and os.path.exists(chunks)):
        continue
    im = Image.open(gif)
    w, h = im.size
    px = im.tobytes()  # palette indices, w*h
    transparent = px[0]
    px = bytes(0 if v == transparent else v for v in px)
    used = set()
    d = open(chunks, 'rb').read()
    for i in range(0x200 * 64):
        e0, e1 = d[i * 3], d[i * 3 + 1]
        e0 &= 0x3F
        used.add(((e0 & 3) << 8) | e1)
    def tile8(t, qx, qy):
        rows = []
        for y in range(8):
            o = (t * 16 + qy * 8 + y) * w + qx * 8  # the sheet is 16 wide: tile t = rows t*16..t*16+15
            rows.append(px[o:o + 8])
        return rows
    plain, canon = set(), set()
    for t in used:
        if (t * 16 + 15) * w + 15 >= len(px):
            continue
        for qy in range(2):
            for qx in range(2):
                r = tile8(t, qx, qy)
                variants = []
                for fx in (0, 1):
                    for fy in (0, 1):
                        rr = [bytes(reversed(x)) if fx else x for x in (r[::-1] if fy else r)]
                        variants.append(b''.join(rr))
                plain.add(variants[0])
                canon.add(min(variants))
    print('%-10s %8d %8d %8d %8d' % (st, len(used), len(used) * 4, len(plain), len(canon)))
