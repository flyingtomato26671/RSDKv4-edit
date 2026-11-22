cd "$(dirname "$0")/.."; O=${TMPDIR:-/tmp}/ndsobj; rm -rf $O; mkdir -p $O
mkdir -p build/gen; [ -f build/gen/trig_tables.inc ] || { cc -O2 -o build/gen/gen_trig tools/trig/gen_trig.c -lm && build/gen/gen_trig > build/gen/trig_tables.inc; }
for f in Animation Collision Debug Ini Input Math ModAPI Object Palette Reader RetroEngine Scene Script Sprite String Text; do
arm-none-eabi-g++ -std=gnu++20 -march=armv5te -mtune=arm946e-s -Os -DRETRO_NDS -DRETRO_USE_MOD_LOADER=0 -DRETRO_USE_NETWORKING=0 -DARM9 -IRSDKv4 -IRSDKv4/NativeObjects -Ibuild/gen -w -c RSDKv4/$f.cpp -o $O/$f.o 2>&1 | grep -m1 error | sed "s/^/$f: /"; done
ls $O | wc -l
