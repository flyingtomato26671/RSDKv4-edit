# Porting a Retro Engine v4 game (or a similar 2D engine) to a small console: a practical guide

Written from the Nintendo DS port of RSDKv4 (Sonic 1/2), which started from the PS1 port of the same engine. It is about
*what you will have to change, adapt or rewrite, and in what order*. The details of what we did are in `PORT_NOTES.md`.
Most of it applies to any console with a few MB of RAM, no FPU, and dedicated 2D/3D hardware (GBA, DS, PS1, Saturn,
N64-class); items specific to the DS are marked.

---

## 0. The shape of the work

An RSDK-style engine has four layers. Ports differ in how much of each they keep:

| Layer | Keep | Adapt | Rewrite |
|---|---|---|---|
| Game logic (script VM, object lists, collision, animation, scene/stage loading, palettes) | usually the code | data layout, array sizes, hot subs as natives | nothing, at first |
| Data (stage files, sprite sheets, bytecode, audio) | the *content* | convert offline into the console's native formats | the loaders (read pre-converted data, not decode at runtime) |
| Platform layer (file I/O, input, timing, memory, save) | the interfaces | | the implementations |
| Renderer and audio | the *engine's calls* (`DrawSprite`, `DrawRectangle`, `PlaySfx`...) | | the implementations, using the console's hardware |

The best decision we made was to **keep the engine core and port the platform layer**, so engine fixes stay shared and the
behaviour (physics, scripts, object order) stays identical to the original. Rewriting the core is how ports end up
"almost like" the game.

---

## 1. Order of work (what to do first)

1. **Get the core to compile** for the target CPU with every platform symbol stubbed out. Do not try to run yet. A thin wrapper
   file per core `.cpp` (that just `#include`s the original) avoids copying code.
2. **Boot it with a text console** that prints the engine's state (stage list, last log lines, frame time). You will be
   debugging blind otherwise; the first runs on hardware fail silently.
3. **Tile layers first** (the biggest visible thing, and easy to verify), with a *simple* CPU renderer into a framebuffer.
4. **Sprites** with a simple CPU blit.
5. **Input and timing** (fixed step at the display rate).
6. **Audio**.
7. Only now **measure** and replace the CPU renderer with hardware features, one class of drawing at a time.
8. **Script natives** for the hot object types (needs the profiler from step 7).
9. Special/odd modes (3D special stages, bonus stages), menus, saves, pause.

Resist doing hardware tricks in step 3-4. A simple renderer gives you a reference to compare every later optimisation
against (same picture, fewer ticks).

---

## 2. Things that must be changed (the engine assumes a PC)

### 2.1 Memory model
- **Static array sizes.** Desktop RSDKv4 sizes arrays for the worst case (entity lists, script code, tile tables, 2 MB+ of
  sprite sheets). On a 4 MB (DS) or 2 MB (PS1) machine, the static arrays *are* the memory budget. Print the biggest `.bss`
  symbols early (`arm-none-eabi-nm -S --size-sort -C *.o`) and size every array from the real data of the games you support
  (the PS1 port measured the real maxima per stage and sized arrays from them).
- **The heap is what is left.** After the static data, we had ~1 MB for everything dynamic, and sound effects ate all of it.
  Decide up front what is static, what is on the heap, and what lives in video memory.
- **No runtime decoding.** GIF/OGG decoding at runtime is too slow and needs RAM for the decoder state. Convert everything
  offline: tiles to the console's tile format, sheets to 8-bit with the engine's palette indices, audio to raw PCM/ADPCM.
  Make the converter decode its own output back and compare (golden rule from the PS1 port): this found format bugs early.
- **Script code as small integers.** Storing script bytecode as 16-bit slots plus a side table for rare big values halves its
  RAM. (Access through accessors; direct pointer writes break.)
- **`mallinfo()` lies about headroom**: `fordblks` is free space inside memory already requested from the system. Look at
  `arena`/`uordblks`/the heap limit, and check allocation failures explicitly (log them to the console).

### 2.2 Numbers
- **No FPU**: every `float`/`double` becomes a soft-float library call (tens to hundreds of cycles). Hunt them down: touch
  coordinates, matrix inverses, scaled-sprite setup, `1.0 / det`, trig tables. Replace with fixed point and integer
  divisions (use the hardware divider if the CPU has one: the DS ARM9 does).
- **Divisions in inner loops** (tile/sprite scalers) are the other trap; precompute reciprocals outside the loop.
- Check **int width**: the engine assumes 32-bit `int` and often relies on wrap-around; keep fixed-width types.

### 2.3 Files and data
- Replace the desktop file API (`fopen` on `Data.rsdk`) with whatever the console has (SD card via FAT, a disc file index,
  cart ROM). Cache small files that are reopened often; avoid seeks on slow media.
- Users must supply their own legal game data. Provide a **build script** that extracts and converts it. Never commit game
  data. Make the script tolerate data-version differences (see 4.3).

### 2.4 Timing and input
- Run the logic at the display rate with a fixed step. Our decision was **no frame skipping** (determinism, reproducible tests);
  if you want delta time you are changing the physics, because RSDK physics is tuned per frame.
- Map the buttons; remove desktop-only input (mouse, analog stick). Touch screens need integer-only hit tests.
- Pause/menu: the engine's own menu objects are native C++ in v4; on a small console rebuilding menus in the platform layer
  is simpler than running them.

---

## 3. Things you will have to rewrite (platform specific)

### 3.1 The renderer
Everything the engine draws goes through a small interface (`DrawSprite`, `DrawSpriteFlipped`, `DrawBlendedSprite`,
`DrawRectangle`, `DrawScaledSprite/Rotated/Rotozoom`, `DrawTileLayer*`, ...). Implement it in layers of ambition:

1. CPU into a framebuffer (reference).
2. Move **tile layers** to hardware backgrounds. Prerequisites: a tile format conversion with de-duplication of 8x8 hardware
   tiles (and flips), a per-line scroll mechanism for parallax/water (HBlank DMA/IRQ), and a fallback path for layers the
   hardware cannot do (vertical scroll, rotating floors).
3. Move **sprites** to hardware sprites (OAM / sprite lists): cut frames into the hardware's shapes, upload once, cache, draw
   by writing a few words. Keep a software fallback for what does not fit.
4. Move **rotation/scaling** to the affine hardware where available (DS affine OAM, GBA, PS1 GPU quads).
5. Move **fades** to a hardware brightness/blend register instead of filling the screen.

What we learned about each:
- The engine's draw *order* (4 tile layers interleaved with 7 object lists) must be preserved. We mapped "behind the player"
  to some hardware backgrounds and "in front of the player" to others with different priorities; where hardware cannot
  separate them, fall back to software for that pass.
- **Palette tricks**: RSDK games switch palette banks per scanline for water. Do it in an HBlank interrupt rather than
  redrawing in software.
- Translucency is expensive and rarely worth exact emulation; flickering (screen-door) the few translucent things kept the
  look and the speed.
- Report fallbacks as counters. The first thing you will want after any change is "how many sprites went to software and why".

### 3.2 3D modes (Sonic 2's special stage; other RSDK games have them too)
The engine's `Scene3D` rebuilds a mesh in script every frame and draws it with a CPU rasteriser.
- **Reuse the engine's matrix and projection maths; do not rewrite them** (keep the integer versions, drop `double`).
- If the console has a 3D engine, use it for the polygons and the 2D hardware for the sprites (billboards drawn as sprites).
  Verify the matrix conventions (the RSDK matrix is row-major, with vertex*matrix and a +z camera) once with a test mesh before
  trusting the picture.
- **Where the data lives matters more than the algorithm.** Our first version put the mesh arrays in uncached video memory
  because the heap was full; the CPU transform/sort/draw then took two frames. Moving the *work* to the geometry engine fixed it,
  not micro-optimising loops. Prefer hardware transform + z-buffer over CPU sorting when you can.
- Painter's-order content (overlapping coplanar polygons) behaves differently with a z-buffer; keep the CPU path available
  behind a runtime switch for A/B comparison.
- Large opaque full-screen software blits (a rotating starfield) will wreck the frame budget and can cover the 3D layer. Draw
  those with a hardware background or skip them until you can.

### 3.3 Audio
- Keep the engine's `PlaySfx`/`PlayMusic` interface; reimplement it on the console's sound hardware.
- Convert SFX offline to the cheapest format that sounds acceptable (8-bit mono). Budget their RAM first: they are usually the
  biggest dynamic allocation.
- Stream music in small chunks from storage into a ring buffer; derive the play position from a hardware timer, refill a
  bounded amount per frame, and start with a small prefill so loading does not stall the frame.
- Make the audio processor (if separate, like the DS ARM7) do the mixing; leave the main CPU for the game.
- Log which SFX failed to load and why. Wrong effects are usually wrong file names or channel ids (we had stage effects take
  the global effects' ids).

---

## 4. Performance: how to find what to change

1. **Build the profiler before optimising.** Ours: frame ticks split into `logic` and `draw`, draw split into clear / tiles /
   objects / software blits, plus the top 3 object types by update ticks and by draw ticks, plus counters for every fallback.
   Show it on a debug screen on the console.
2. **Convert ticks to a frame budget** (bus ticks per frame at your refresh rate) so you know how much you are over.
3. Typical order of hot spots: script VM in per-object `update` (Rings, StageSetup, Player, HUD) -> software tile passes ->
   software sprite draws -> special modes.
4. **Script natives**: replace the hottest script subs with C++ that does the same thing statement by statement. The PS1 port
   did this with a bytecode patcher that recognises exact subs and rewrites them into one native opcode. It is a big win
   (our `logic` dropped ~3x on the tested zone) but:
   - it depends on the **exact bytecode of one data build**: absolute offsets and function ids are in the signature. Newer game
     builds (e.g. the store version) will not match; apply patches one by one, report mismatches with a reason, and provide
     a dump tool so a signature can be re-derived;
   - verify each native with A/B tests (same inputs, compare object state/frames against the unpatched VM).
5. **Compile the VM as ARM and put its state in the fast memory** (DTCM/scratchpad) where the target has them.
6. **Skip work the hardware already does**: a waterfall draw sub that repaints animated tiles is pure cost once tiles animate on
   hardware.
7. Re-measure after every change; keep a table of before/after numbers. Some "obvious" suspects (palette changes, animated tiles)
   were not the bottleneck.

---

## 5. Pitfalls we hit (so you do not)

- **Header collisions**: the console SDK's headers (`bool`, `u8`, `BIT()`) and the engine's collide. Put SDK calls in small C
  files with a plain C API, and declare them by hand from C++.
- **Hardware limits are exact, not "about"**: 1024 8x8 tiles for our layout, 128 OAM entries, 32 affine matrices, double-size
  boxes only hold ~2x zoom, 2048 polygons per frame, 4 palette switches per frame. Count them, show the counters, and design the
  fallback before you hit the wall.
- **Byte writes to video memory** are dropped on some consoles (DS VRAM takes 16/32-bit only): clear and copy with word stores.
- **Uncached memory regions** (video memory used as RAM) make hot loops several times slower; the cost shows up as "mysterious"
  tick counts.
- **Per-line DMA/IRQ state must be re-armed every frame** or it works once and then flickers after pauses or fades.
- **Double buffering of maps** (tile maps, OAM) is needed or you will see tearing; apply updates right after vblank.
- **One-frame delays**: when a decision for frame N+1 is made from frame N (for example "is the software layer needed"), the
  first frame after a change is wrong. Accept it or make it explicit.
- **Heap exhaustion is silent** unless you check every allocation result. Log it to the debug screen.
- **Do not trust a diagnosis from a screenshot alone**: add a counter or a line of text to the debug console for each suspect.
  "Nothing draws" turned out to be three different problems (a covering background, a failed allocation, uncached memory) found
  only by adding counters.
- **Git hygiene on tooling**: keep game data out of the repo, make the data build reproducible, commit each step so you can
  bisect a regression to one hardware change.

---

## 6. Checklist for a new port

- [ ] Core compiles for the target CPU with stubs; no float/double left in hot paths.
- [ ] Memory budget table (static / heap / video / fast memory) written down and re-checked after each feature.
- [ ] Data converter produces the console's formats and verifies them by decoding back.
- [ ] Debug console with frame budget, profiler, fallback counters and allocation failures.
- [ ] Reference CPU renderer working before any hardware renderer.
- [ ] Hardware tile layers + per-line scroll + water palette switching, with a software fallback per pass.
- [ ] Hardware sprites with cache, flips, priorities, affine rotation/scale, and counters for fallbacks.
- [ ] Audio on the sound hardware: offline-converted SFX, streamed music, failure logging.
- [ ] Script natives for the measured hot objects, with A/B verification and tolerance for data-build differences.
- [ ] Special modes (3D) designed around where the data lives and what the hardware transforms for free.
- [ ] Notes kept as you go (`PORT_NOTES.md`): what changed, why, and the numbers.
