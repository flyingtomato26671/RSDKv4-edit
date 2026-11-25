# nds/ - Nintendo DS platform layer (work in progress)

Builds the RSDKv4 core with `-DRETRO_NDS` (same console configuration as the PS1 port: pre-converted data, int16 scripts).
All DS code is behind `RETRO_NDS`, so the PS1 builds are unchanged.

Status: the engine runs on hardware (tiles on BGs, sprites on OAM, audio, special stage in progress); see `PORT_NOTES.md`.
First milestone: 16 core files (Animation, Collision, Debug, Ini, Input, Math, ModAPI, Object, Palette, Reader, RetroEngine,
Scene, Script, Sprite, String, Text) compile for ARM9 (`nds/check_core.sh`, needs gcc-arm-none-eabi).
Still to write: the symbols the core expects from the platform layer (renderer, tile/atlas upload, audio, file access,
menus, save, loading, Scene3D, main), in `nds/`.

## Documentation

`nds/PORT_NOTES.md` explains every change made to run the engine on the DS (memory map, hardware layers, OAM and affine
sprites, script natives, audio, the special stage on the 3D engine), why it was needed, what it did for performance, the
measured numbers, the diagnostics console and the open items.

`nds/PORTING_GUIDE.md` is a general guide for anyone doing a similar port (RSDKv4 or another small 2D engine) to a console with a few MB of RAM:
what must be changed, adapted or rewritten, in what order, and the pitfalls.
