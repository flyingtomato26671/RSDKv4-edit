# RSDKv4 for the Nintendo DS (BlocksDS). Build from this folder:  make   (needs $BLOCKSDS)
# Step 1: a minimal ARM9 app (video, input, 1:1 crop test). The engine core is added as thin wrappers in
# nds/source/core/ (each #includes ../../../RSDKv4/X.cpp, no copies) once the platform symbols exist.
BLOCKSDS     ?= /opt/wonderful/thirdparty/blocksds/core
BLOCKSDSEXT  ?= /opt/wonderful/thirdparty/blocksds/external

NAME          := rsdkv4-nds
GAME_TITLE    := RSDKv4 DS
GAME_SUBTITLE := Sonic 2 port (WIP)
GAME_AUTHOR   := RSDKv4-DS port

SOURCES      := source
INCLUDEDIRS  := ../RSDKv4 ../RSDKv4/NativeObjects gen
DEFINES      := -DRETRO_NDS -DRETRO_USE_MOD_LOADER=0 -DRETRO_USE_NETWORKING=0
LIBS         := -lfilesystem -lfat -lnds9
LIBDIRS      := $(BLOCKSDS)/libs/libnds $(BLOCKSDS)/libs/libfat

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
