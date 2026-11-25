# RSDKv4 on the Nintendo DS: what changed, why, and what it bought

Engineering notes for the `nds` branch. They record what had to change to run the PS1 port's engine on the original
Nintendo DS (ARM9 67 MHz, 4 MB RAM, 656 KB VRAM, one 2D + one 3D engine), the reason for each change, and what it did for
speed or for making the thing run at all. Numbers are from the user's console captures on real hardware / emulator unless
marked *not yet measured*.

Conventions:
- A **tick** on the status console is one bus cycle (33.5 MHz): one frame at 60 fps is ~558,000 ticks. `logic` + `draw`
  (+ waiting) must stay under that.
- "Window" = the 256x192 area of the engine's 320x240 logic screen that the DS shows 1:1 (logic x 32..287, y 24..215).
- Everything DS-specific is behind `RETRO_NDS` (which selects the PS1 configuration of the engine: pre-converted data,
  int16 script code), so the PS1 builds are unchanged.

---

## 1. Starting point and goals

The PS1 port (`danielgpinheiro/RSDKv4-ps1`, branch `ps1`) already turned RSDKv4 (Retro Engine v4, Sonic 1/2 2013) into
something that runs on 2 MB RAM: scripts as int16 slots, arrays sized from the data, no runtime asset decoding, natives for
the hot script subs. That is the base. The DS port keeps that engine core and replaces the platform layer (renderer,
audio, input, file access, loading, 3D).

Goals (user): stable 60 fps; the original game's look; use the console's own hardware (tile BGs, OAM, affine, 3D engine,
ARM7 audio) instead of drawing everything with the CPU; users supply their own legal `Data.rsdk`.

---

## 2. Build and data pipeline

| What | Why | Effect |
|---|---|---|
| `RETRO_NDS` maps to the PS1 platform configuration (`RetroEngine.hpp`) | The PS1 core already has the small-RAM data model; reuse it | The 16 core files compile unchanged for ARM9 (`nds/check_core.sh`) |
| Core files included through thin wrappers in `nds/source/core/*.cpp` (no copies) | Keep one copy of the engine | Fixes in the core apply to both ports |
| BlocksDS project in `nds/` (`make`) | libnds + libfat/NitroFS live inside libnds in BlocksDS | `-lnds9` only |
| `tools/build_assets.py --src <Data.rsdk folder> --ds` | Turns the user's extracted data into the DS data tree (steps 1-2 of the PS1 pipeline: tiles/atlases/manifests, `Scripts.ps1`; drops ZoneM like the PS1 build) | Users build their own data; nothing copyrighted is committed |
| Script natives applied **one at a time** (`PS1_PATCH_ONLY=name`), failing ones skipped, with the reason printed | The PS1 patches match exact bytecode (absolute offsets, function ids) of the PS1 author's `Data.rsdk`. The user's data (official Google Play Android build) is newer/different, so some signatures do not match | A mismatch no longer aborts the build; `[1 scripts] natives applied ... skipped ...` shows which ones ran. `--dump fn N` / `--dump sub "Name" N` prints a listing so a skipped native can be adapted to another data build |
| `tools/ds/convert_audio.py` run from `build_assets --ds` on the Data root | Game SFX are `.ogg` and music `.ogg`; the DS has no decoder budget | SFX -> `.wav` (loaded as 8-bit mono); music -> `.dsm` (raw 16-bit mono ring-buffer stream). Needs `ffmpeg` or `soundfile` + `numpy`; a clear error says so |
| Windows toolchain notes | The pipeline compiles `tools/rsdkmanifest` | Needs MSYS2 g++/make on `PATH`, `pip install soundfile numpy`; `rsdkmanifest` accepts Windows output paths |
| Float-free engine code (touch rect checks, matrices, scaled sprites use integer divisions) | The ARM9 has no FPU: every `float`/`double` is a soft-float library call | Removed a class of silent slowdowns; touch is integer; Scene3D inverse uses the PS1 integer path |

Data-version lesson: natives are tied to the exact data build. With the user's data the first run had most natives skipped
(`logic` ~432k ticks); once they overwrote the scripts with the patched ones, `logic` dropped to ~100-165k and 60 fps held in
the tested zone (see section 6).

---

## 3. Memory: what lives where

### 3.1 Main RAM (4 MB)
Almost all of it is static (`.bss`) arrays from the engine, so the **heap is only ~1 MB**. Biggest static symbols
(measured with `arm-none-eabi-nm -S` on the object files):

| Symbol | Size |
|---|---|
| `graphicData` (sprite sheets, 8-bit) | 1 MB |
| `tilesetGFXData` (stage's 16x16 tiles, 8-bit) | 256 KB |
| `gfxPalette16to32` (desktop-only; the DS renderer does not use it; the linker drops it only with `--gc-sections`) | 256 KB |
| `objectEntityList` | ~250 KB |
| `scriptCode` | ~200 KB |
| `collisionMasks` | ~140 KB |
| `tiles128x128`, `s_ps1Layer*Tiles` | ~100 KB / ~130 KB |

The heap is almost entirely the sound effects (~1030 KB for 48 effects at 8-bit mono). **Consequence:** anything big that
is `malloc`'d late fails. This is exactly how the first special-stage attempt failed (`calloc` for the Scene3D arrays
returned null: `3D st:2`, `heap used 1032 KB`). One SFX also fails to load for lack of memory (`sfx bad:1 mem:1`).

### 3.2 VRAM layout (main engine, banks A+B+C = 384 KB at 0x06000000)
| Range | Use |
|---|---|
| 0x0000-0x7FFF | text BG maps: BG0 / BG1, 64x64, two buffers (`buf*0x4000 + layer*0x2000`) |
| 0x8000 / 0xA000 | BG2 extended-rotation map (linear 64x64 16-bit), two buffers |
| 0xC000 / 0xE000 | BG3 extended map (the 4th tile pass), two buffers |
| 0x10000-0x1FFFF | tile data: 1024 8x8 8bpp tiles, shared tile base |
| 0x06020000 / 0x06038000 | BG3 bitmap pages (the software "sprite layer", when needed) |
| Bank E (64 KB, 0x06400000) | OBJ tiles (sprite cache) |
| Bank H | sub-screen text console |
| **Bank D (128 KB)** | mapped as LCDC plain memory at 0x06860000: Scene3D arrays (section 8) |

### 3.3 Fast memories
- **ITCM**: the plain sprite blit and the tile loops (software fallbacks) run from ITCM, no divisions in the inner loops.
- **DTCM**: the script VM state (`scriptEng`, text buffers, stack positions: `NDS_DTCM` in `Script.cpp`) is placed in the
  `.dtcm` section.
- **ARM mode for the core** (`NDS_CORE_ARM`, `core_mode.hpp` uses `#pragma GCC target("arm")`): the engine core is compiled as
  ARM instead of the toolchain's default (Thumb), because the VM is ALU/branch heavy and ARM code executes fewer instructions
  from cached main RAM. Switchable: `-DNDS_CORE_ARM=0`. *Effect not isolated in a single number: it was applied together
  with the DTCM change; keep both switches for A/B.*

---

## 4. Rendering: from "CPU paints everything" to hardware layers

The first renderer (steps 1-2) drew tile layers and sprites with the CPU into a 256x192 16-bit page flipped through VRAM.
It could not hold 60 fps (`draw` was the larger share of the frame). Each step below moved a class of drawing to hardware.

### 4.1 Window crop
The engine renders a 320x240 logic screen. The DS shows a 1:1 256x192 window of it (no scaling, which keeps pixels sharp
and makes the hardware layers line up with the engine's coordinates). The HUD is moved inward by object name so it stays
visible (`TypeFlags` name flag in `sprites.cpp`).

### 4.2 Tile layers on hardware BGs
- Tile layers become hardware backgrounds: BG0 / BG1 as 8bpp text maps, BG2 and BG3 as extended-rotation maps (16-bit
  entries) because the 4th layer does not fit in text BGs. 16x16 engine tiles are split into 8x8 hardware tiles, **lazily
  de-duplicated** (with H/V flips) so a stage fits in 1024 tiles (`tools/ds/tile_stats.py` measures distinct 8x8 tiles per
  stage). `hw tiles:` / `fail:` on the console show use and failures; a failure falls back to the software path.
- Maps are updated incrementally (only new columns/rows) into the back buffer; two buffers are swapped each frame with
  the engine's page flip.
- **Parallax / water line scroll**: per-scanline scroll is done with **HBlank DMA** into BGxHOFS/VOFS (`s_tbl`). *Bug fixed:* the
  DMA stopped after the first frame/pause (Chemical Plant flicker). It is now re-armed every vblank (`vblankCount`).
- **Animated tiles** are re-uploaded to VRAM tile memory; waterfall *objects* are skipped (their tiles animate anyway), which
  removed one of the biggest per-frame costs of the original script.
- **Fades** that the scripts do with full-screen black/white `DrawRectangle` calls use the hardware **master brightness**
  (`g_ndsScriptBright`, `setBrightness`) instead of filling the screen.
- **Water**: the palette is switched in the **HBlank** at the water line (VCOUNT IRQ) so the screen never needs a software
  redraw just because water crosses it. Up to 4 switches per frame.
- **Translucency** is approximated by flicker (shields only); other translucent sprites draw opaque. True blending would
  need OBJ semi-transparency + BLDCNT and was not worth the cost.

### 4.3 The layer/pass model
The engine draws 4 tile layers interleaved with object lists 0-6. The DS assignment is:
- passes *behind* the player: BG0, BG1, BG2 in order (priorities 3, 2, 1);
- passes *in front* of the player (after object lists 3/4): the first on BG3 (tile mode), then BG2, priority 0 (lower BG
  index wins ties);
- game sprites (OAM) priority `front ? 1 : 0`; HUD sprites (lists 5,6) always 0 so they stay above front passes;
- **BG3 is dual-use**: a tile layer, or the 16-bit sprite bitmap. The bitmap is only shown on frames where something was drawn
  with the CPU (software sprite fallback, rectangles, software tile pass...). That is decided from the *previous* frame
  (`s_needBitmapPrev`), so one frame of delay when software drawing starts.
- A pass that cannot go to hardware (vertical scroll, 3D floor/sky layers, non-uniform scroll for BG2/BG3) is drawn in
  software (`passes:` shows `H` hardware, `F` front hardware, `S` software, `-` none). A software 4th pass used to dominate
  `tiles` (swT 1120-3176 ticks, tiles 239k-528k ticks); the BG3 tile layer removed it. *This was untested on hardware at the
  time of writing: expect `passes:` `F`, `swT:0`.*

### 4.4 Remaining software drawing
Rectangles (non-fade), tint rectangles, alpha/additive/subtractive sprite variants, `DrawScaledTintMask` (luminance tint),
sprites that do not fit OAM limits, and software tile passes. `blit` / `px` on page 1 count them.

---

## 5. Sprites on OAM (`nds/source/oam.cpp`)

| Change | Why | Effect |
|---|---|---|
| Plain sprites are queued as OAM entries, not painted | The CPU was painting every pixel | `obj` ticks dropped; the CPU only writes 4 halfwords per sprite |
| Frames are cut into OBJ shapes (8x8..64x64, <= 8 pieces) and uploaded **once** to OBJ VRAM (bank E), cached by (sheet, sprX, sprY, W, H) | Uploading each frame would cost more than blitting | After warm-up a frame costs OAM writes only. Cache flush on tileset load and when > 3/4 full |
| OBJ VRAM 64 B tile boundary (1D mapping, size 64) so OAM tile number = 8x8 8bpp tile index, 1024 tiles | Simple addressing | |
| Flips through OAM flip bits; draw order: first drawn = entry 127 (lower index = on top) | Engine's painter's order | |
| Falls back to the software blit when OAM / VRAM / pieces run out | Never lose a sprite | `g_ndsOamFallback` |
| OAM palette copied with the BG palette (`palCopy`), follows the water HBlank switches | Sprites change colour underwater like the BGs | |

*Bug fixed:* the first OAM version broke every sprite ("UV mapping destroyed") because the upload ignored the frame's
`sprX/sprY`; the sheet pointer passed must be the frame's own top-left (`sheet + sprY*stride + sprX`).

### 5.1 Rotation and scaling on the hardware (affine OAM)
`DrawSpriteScaled`, `DrawSpriteRotated`, `DrawSpriteRotozoom` (and the special stage's textured/3D sprite faces) first try
`NdsOamXform` (`oam.cpp`): the DS affine OBJ hardware does the rotation/scale, the CPU only computes one 2x2 matrix.

- Geometry matches the engine's software functions: a texture point `t` is drawn at `P + F * o`, `o = (sgn*(t.x - pivotX),
  t.y - pivotY)`, `F = rot(angle) * diag(sx, sy) / 512` (512 = 1.0), angle = the engine's inverted 9-bit angle. OAM needs the
  inverse (screen -> texture, 8.8 fixed point): `PA=(c<<8)/sx, PB=-(s<<8)/sx, PC=(s<<8)/sy, PD=(c<<8)/sy`, X mirrored when
  `direction == FLIP_X`.
- Double-size boxes (`attr0` bit 9) so rotated corners are not cut; a piece's box is 2w x 2h around its transformed centre.
  Multi-piece frames share one matrix and each piece's centre is transformed individually.
- Limits and fallbacks (counted on page 2, `xform oam:N sw:N why:N AxB`): `1` frame > 255 px, `2` OBJ VRAM full, `3` > 8
  pieces, `4` OAM full, `5` all 32 matrices used (identical matrices are shared first), `6` zoom beyond the double-size box
  (> ~2x unrotated: the sprite would be clipped, so software draws it). Sprites smaller than 1/128 are dropped.
- Still software: `DrawScaledTintMask` (OAM cannot do a luminance tint) and the cases above.
- *Status: Sonic, Tails and rings were confirmed correct on hardware; the box-overflow check, shared matrices and the counters
  were added afterwards and are not yet measured.*

---

## 6. Game logic performance

Measured problem: `logic` ~430k ticks and `U:` dominated by `Ring` (176k), `LoseRing`, `StageSetup` (62k). Even with perfect
drawing the frame was over budget, so the VM had to get cheaper.

| Change | Why | Result |
|---|---|---|
| Profiler on the console: logic ticks, draw ticks, top 3 object types by update (`U:`) and draw (`D:`), per-type name flags cached (no string search per object) | Needed numbers, not guesses | Showed Ring / LoseRing / StageSetup / PlayerObject / HUD / TailsObject / DebugMode / StarPost / Earthquake as the hot types |
| PS1 script natives applied on the DS (`PS1Ring`, `PS1StageSetup`, Lose Ring, platforms, player physics, ...) | One native replaces dozens of VM instructions per object per frame | With natives matching the data: `logic` ~432k -> ~100-165k ticks, 60 fps stable in the tested zone |
| Natives that do not match the user's data are reported, not forced; dumps allow adapting them | Exact-signature patches | Remaining hot types after patching: TailsObject, HUD, DebugMode, StarPost, Earthquake (candidates for more natives) |
| ARM-mode core + DTCM VM state | Section 3.3 | Lower VM dispatch cost |
| Waterfall draw sub skipped | Animated tiles already animate the water | Removed a frame-sized cost in one zone |
| No float math in input | ARM9 without FPU | |

Open decision: **delta time** (variable timestep) conflicts with the PS1 port's repo rule "no frame skip" (determinism,
byte-exact A/B tests). Not changed; the user has not decided.

---

## 7. Audio

| Change | Why | Effect |
|---|---|---|
| ARM7 plays everything: SFX through `soundPlaySample`; no Maxmod | Native ARM7 audio is the recommended DS approach and keeps the ARM9 free | |
| SFX: `.ogg` -> `.wav` at build time, loaded as **8-bit mono** | No decoder budget; RAM | 48 effects ~1030 KB. The loader maps `.ogg` names to `.wav` |
| Global SFX loaded at init; stage SFX take ids after them | A bug made stage SFX take ids 0.. and replace globals | Correct effects |
| Finished SFX do not `soundKill` a possibly reused channel; only an ended SFX frees a channel | Stutter/cut-offs of the ring sound | |
| Music: `.dsm` file streamed from the SD card into a 16384-sample ring buffer; prefill 8192, refill <= 2048 samples per frame; ARM9 computes the play position from the bus timer | Streaming small pieces in the background | "10/10" music; occasional load hiccups addressed with smaller prefill/refill (*check on hardware*) |
| Diagnostics: SFX ok/bad, open/fmt/mem failures, last bad file, music track/result/path | Find missing/wrong files fast | Showed the earlier mismatch was the user's SD scripts, not a code bug |

---

## 8. The special stage (Sonic 2)

The half-pipe is **3D in the original script**: the script builds ~1,456 vertices and ~754 faces every frame (`Scene3D`),
then `Draw3DScene` sorts and draws them. Faces: `COLORED_3D` / `COLORED_2D` / `FADED` (flat colour, FADED fogged by depth into
`fogColor`), `TEXTURED_C` / `TEXTURED_C_BLEND` / `3DSPRITE` (rings, bombs, players as scaled/rotated sprites), textured 3D/2D
(unused by the half-pipe). The path has no textures, only colours.

### 8.1 Design
Mixed 2D/3D, like many DS games: the DS **3D engine draws the path** (BG0), the **2D hardware draws the sprites** (OAM affine,
section 5.1) and the tile layers behind.

- Matrices (`scene3d_matrix.inc`, extracted from `RSDKv4/Scene3D.cpp`): the upstream integer implementations, including the
  PS1's integer matrix inverse (no doubles).
- Scene3D arrays exist only while a stage whose folder contains "special" is loaded (`PS1Scene3DPrepare`); elsewhere they are
  1-entry dummies with capacity 0 (`g_ps1VtxCap` / `g_ps1FaceCap`), and the script's clamped accesses are counted.
- `PS1Scene3DPrepare` also decides layering: tile layers behind the 3D start at BG1 (`s_hwLayers = 1`), BG0 is the 3D
  output with priority 2, BG1/BG2 background at 3, sprites above.

### 8.2 Findings, in the order we hit them
1. **Special stage started but nothing drew except the "Ready" text.** Two causes: (a) `SpecialBG` (the rotating star field)
   is a 256x256 opaque sprite blitted twice by the CPU (`D:SpecialBG` 899,565 ticks, `blit x2 px 98304`) into the bitmap on BG3, which
   sits above the 3D -> it covered everything and cost most of the frame. It is skipped while a 3D scene is active (replacing it
   with an affine background layer is still open). (b) the 3D arrays were never allocated (next item).
2. **`3D st:2` = allocation failed**, `heap used 1032 KB`: the heap is full of SFX (section 3.1). The ~94 KB of arrays were moved
   to **VRAM bank D mapped as LCDC memory** (128 KB at 0x06860000; filled with word stores because VRAM ignores byte
   writes).
3. **The VRAM trick is slow for the CPU**: with the CPU transforming 1,456 vertices, sorting 754 faces and building quads, the
   measured cost was T:292k, S:317k, D:440k ticks (~1.05M, almost two frames). VRAM is uncached and on a 16-bit bus.
4. **Fix: let the geometry engine do the work** (`Nds3dGpuBegin`, `Draw3DSceneGpu`):
   - modelview = the engine's final matrix (`values << 16` for the 3x3 because raw vertices are 1.3.12 and the matrix is
     20.12; translation `<< 12`), so raw 16-bit engine vertices go straight to `GFX_VERTEX16`;
   - projection matrix built by hand: `x = projX/128`, `y = projY/96` (the 256x192 window is exactly centred on the engine's
     320x240 centre, 160,120), `w = z`, depth mapping near 32 / far 196,608;
   - hardware clipping, perspective, depth buffer (no CPU sort, no CPU projection); flat colours per face, FADED fog colour
     computed on the CPU from the face's average depth as upstream does;
   - the CPU only handles sprite faces (transform vertex `a`, sort the few sprites by depth, draw through OAM) and 2D faces
     (drawn afterwards in an orthographic pass).
   - The original CPU path is kept: **L + R + Y** toggles GPU/CPU at runtime (page 2: `3D path:GPU/CPU`) for A/B comparison.
   - *Status: the GPU path is not yet measured on hardware at the time of writing.* Expect `T` ~ 0, `S` = 0, `D` bound by the
     geometry engine (~754 quads), plus the sprite faces.

### 8.3 Known differences / limits of the GPU path
- Faces at identical depth are ordered by the depth test (first drawn wins) instead of painter's order; coplanar overlays could
  flicker.
- Sprites always draw above the 3D layer (OAM cannot interleave with BG0): a near wall cannot hide a far sprite.
- Faces partly behind the camera are clipped by hardware (upstream skipped the whole face).
- Translucent faces are skipped.
- 16-bit vertex range (same as the PS1's GTE).
- The star-field background is skipped (software blit too big); the DS background would be an affine BG.
- The polygon budget is 2048 per frame (the stage needs ~784 quads); the code stops at 1900.

---

## 9. The status console (sub screen)

Touch the bottom screen to change page.
- **1/3 engine timing**: `ticks` (frame), `logic`, `draw` and its parts (`clear`, `tiles`, `obj`, `blit`, `px`), `passes`, `swT`,
  `hw`/`sw` frame counts, `hw tiles`/`fail`, water switches, top update `U:` and draw `D:` types. `vbl/frame x16`: 16 = 60 fps,
  32 = 30 fps. Note: while the game is paused `ticks` is tiny (the engine is not running).
- **2/3 audio and 3D**: SFX and music diagnostics, `xform oam/sw/why`, `3D path`, `3D tk T/S/D` (transform/sort/draw ticks),
  `3D vtx`, quads, `3D st` (0 never prepared, 1 not a special stage, 2 allocation failed, 3 ready), heap numbers.
- **3/3 engine state**: stage list, log lines.

---

## 10. Lessons (things that cost time)

- Measure first: three "obvious" suspects (tiles, palettes, animated tiles) were not the cost; the profiler's `U:` / `D:` lines and the
  pass letters pointed at the real ones.
- The DS has two incompatible-looking worlds: libnds headers and the engine's headers (e.g. `u8`, `bool` macros) do not mix. All
  libnds calls live in small C files (`nds3d.c`, `audio_ds.c`, `input.c`) with a plain-C API; engine files declare them by hand.
- Scripts' native patches depend on the exact data build; do not force them, dump and adapt.
- `mallinfo().fordblks` counts only free chunks inside memory already taken from the system, not what `sbrk` could still give;
  use `arena` / `uordblks` and the heap limit.
- VRAM used as plain memory is uncached: good for capacity, bad for hot loops; keep hot data small or let hardware read it.
- `git`: a stop hook kept reporting "unpushed commits" after pushes; refreshing `refs/remotes/origin/nds`
  (`git fetch origin +refs/heads/nds:refs/remotes/origin/nds`) fixes the stale remote ref.

---

## 11. Open items

- Special stage: measure the GPU path; fog through the hardware fog table instead of per-face colour; star-field background as
  an affine BG; translucent faces; OAM-affine for the remaining software cases (zoom > 2x via smaller pieces).
- Remaining `U:` hot types with the user's data: TailsObject, HUD, DebugMode, StarPost, Earthquake; adapt skipped natives
  (Stage Setup, Spikes, Monitor, Springboard, InvBlock, player functions) to the Google Play data build.
- Delta time vs the "no frame skip" rule.
- Menus, real pause, save data, Sonic 1.
- After the port is complete: wireless multiplayer, DSi enhancements, lid-closing sleep, per-zone memory check.
