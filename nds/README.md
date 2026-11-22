# nds/ - Nintendo DS platform layer (work in progress)

Builds the RSDKv4 core with `-DRETRO_NDS` (same console configuration as the PS1 port: pre-converted data, int16 scripts).
All DS code is behind `RETRO_NDS`, so the PS1 builds are unchanged.

Status: 16 core files (Animation, Collision, Debug, Ini, Input, Math, ModAPI, Object, Palette, Reader, RetroEngine,
Scene, Script, Sprite, String, Text) compile for ARM9 (`nds/check_core.sh`, needs gcc-arm-none-eabi).
Still to write: the symbols the core expects from the platform layer (renderer, tile/atlas upload, audio, file access,
menus, save, loading, Scene3D, main), in `nds/`.
