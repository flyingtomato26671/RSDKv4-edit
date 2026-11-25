// Build mode of the engine core (nds/source/core/*.cpp). BlocksDS compiles ARM9 code as Thumb by default; the script VM and the
// collision / object code are hot, and ARM code (32-bit instructions, no Thumb's extra moves) is usually faster on the ARM946E-S
// with its instruction cache. -DNDS_CORE_ARM=0 in nds/Makefile (DEFINES) goes back to Thumb for an A/B comparison of `logic:`.
#pragma once
#ifndef NDS_CORE_ARM
#define NDS_CORE_ARM 1
#endif
#if NDS_CORE_ARM
#pragma GCC target("arm")
#endif
