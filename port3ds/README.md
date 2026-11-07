# New 3DS port (experimental)

The goal is to run the recompiled game on a real New 3DS, locally, with no streaming. This folder holds the 3DS host and the build files. Nothing here comes from the game: the build is made on your computer from your own disc, and its output must stay private (see [CONTRIBUTING.md](../CONTRIBUTING.md)).

**State: the scenes checked from the title through the first battle render correctly in Azahar, apart from one small flicker noted under [Status](#status). On a real New 3DS the level-up screens run at full speed, 60 game frames per second, the first battle at about 45, the lobby at about 22, and movies at about 14. Outside movies it shows about one picture in five in the lobby and the battle. Not playable yet.** See [Status](#status).

## What it needs
- A Mac or Linux PC with Docker (or OrbStack). The build uses the official `devkitpro/devkitarm` image.
- The patched `ps2-web-recomp` folder next to this repository, with the game translated into `generated/p3fes` (see [docs/install.md](../docs/install.md)), and `tools/ps2run_3ds_patch.py` applied.
- A New 3DS, New 3DS XL or New 2DS XL. An original 3DS or 2DS will not work: 64 MB of application memory is not enough.

## Build
```bash
python3 ../fes-pc-recomp-main/tools/ps2run_3ds_patch.py
```
Run that once, in the `ps2-web-recomp` folder. Then, from this repository:
```bash
port3ds/build.sh
```
Before the first build, compile the vector unit programs you recorded (see [Testing](#testing)), or set `VU=0` to leave them in the interpreter:
```bash
build/tools/vurecomp games/p3fes/vu1 generated/p3fes-vu3ds --scalar
```
Translate the game once more with the list of functions the 3DS runs natively. This changes only the 3DS build, because each call is inside `#ifdef __3DS__`:
```bash
build/tools/eerecomp games/p3fes/disc/SLUS_216.21 --entries games/p3fes/entries.txt --natives ../fes-pc-recomp-main/port3ds/natives.txt --out generated/p3fes
```
Run both in the `ps2-web-recomp` folder.

The result is `ps2-web-recomp/build/3ds/p3fes.3dsx`.

## Put it on an SD card
```bash
python3 tools/p3fes_3ds_sd.py "path/to/disc.iso" ../ps2-web-recomp -o /Volumes/YOUR_CARD
```
This needs about 4.7 GB free. It copies the programs and your disc, splits the disc into parts because FAT32 stops at 4 GB, and checks the parts against the original. If it stops, run it again: finished parts are kept.

| File | What it is |
| --- | --- |
| `/3ds/p3fes.3dsx` | The game. Start it from the Homebrew Launcher. |
| `/3ds/p3fes-probe.3dsx` | A small test that shows the memory and speed this console really gives. |
| `/p3fes/SLUS_216.21` | The boot program from your disc. |
| `/p3fes/disc.00`, `disc.01`, ... | Your disc in parts. A single `/p3fes/disc.iso` also works where the card allows it. |
| `/p3fes/args.txt` | Optional test settings, one per line. See the top of `main3ds.cpp`. |
| `/p3fes/log.txt` | Everything the runtime prints. |
| `/p3fes/perf.txt` | One line of timing every 30 game frames. |

If the game crashes on a console with Luma3DS, the crash report is in `/luma/dumps/arm11`.

## Wireless test link
While the game runs it listens on port 5577 of the console, and `tools/p3fes_3ds_link.py` works from any computer on the same network: `status`, `fetch <folder>` (logs, crash reports, screenshots), `press a 4`, `shots 30`, `draw 1`, `quit`, `run`, `settings "load lobby.state;stats 1"` and `install`. The address it tries first is in the tool; change it with `--host`.

**The link has no password, on purpose: anyone on your network can replace the game while it runs.** It can write only `/3ds/p3fes.3dsx` and `/p3fes/args.txt`. A new program must start like a 3DSX and match its checksum, and the old one stays as `/3ds/p3fes.3dsx.bak`. It refuses to read the disc, the boot program, saved states and Luma's backups. `link 0` in `args.txt` turns it off. Don't run it on a network you don't trust.

The first build with this link has to get there another way. Without moving the card, press Y in the Homebrew Menu on the console, then run `3dslink` from the devkitARM image:
```bash
docker run --rm -v "$PWD/build/3ds:/b" devkitpro/devkitarm:20260610 /opt/devkitpro/tools/bin/3dslink -a 192.168.1.133 /b/p3fes.3dsx
```
Run it in the `ps2-web-recomp` folder. It replaces `/3ds/p3fes.3dsx` and starts it.

## Limits found so far
| Limit | Number | What it means |
| --- | --- | --- |
| Translated game code and vector programs for the ARM11 | 43.4 MB | Fits. Measured with `-O2`, which ran faster on the console than the smaller `-Os` (91 against 102 ms per game frame). Each new vector program adds to this. |
| Code region of a 3DS program | about 63 MB | The 32 MB of PS2 RAM cannot sit in the program's data, so it lives at a fixed heap address right after the 28 MB heap (`0x09C00000`, which fits an ARM immediate, so a guest address takes one `add`). If the heap size changes, this address must change with it. A real console refused the PS2 RAM at `0x0B000000` and `0x0C000000` with `0xE0E01BFD`, and Azahar allows both. |
| Application memory | 124 MB | 48.4 MiB program + 32 MiB PS2 RAM + 28 MiB heap + 13 MiB linear heap = 121.4 MiB, confirmed on the console, so about 2.6 MiB is left. Going over stops the game at start-up, so more vector programs than that will need the heap cut. The heap has about 6 MB spare in the lobby: a queue doubling in size stopped the game once, so the queues between the threads now have fixed sizes. The linear heap holds textures, and the cache keeps it nearly full. After the menus its free space is in pieces, so a big texture needs older ones thrown out until a large enough piece appears. |
| Azahar CPU speed | 268 MHz clock | Azahar does not implement the New 3DS speed switch, so its timing does not match a real console. |

## Status
Checked in Azahar 2126.2 (New 3DS mode):
- The game boots from the split disc parts on two threads and runs from a cold start through the logos, title, menus and well into the opening movie (about 15,500 game frames) with no crash or hang, and the movie shows. That run stopped at its time limit, still in the movie.
- Saved states from the Mac build load. These were compared side by side with the Mac build's picture, with no crash and no missing texture: name entry, the dorm lounge, hallways and stairs, the player's room, shoe lockers, school lobby, assembly hall (54,000 triangles), school gate, dialogue with portraits, the Dark Hour scenes, the Velvet Room, the rooftop battle with its menus, and the level-up screens.
- The rooftop battle was also played live on this build from the end of the awakening movie, with button presses: the fight, the command menu, Results, level-up, and the talk with Yukari afterwards. That was about 5,000 game frames on the final build. Only shots every 30 frames were looked at.
- One open fault: a small black square appeared on the text box in 1 of 11 3DS shots of the level-up screens, with skipping on, and in none of 151 Mac shots. The cause is not known.
- Sound output is off.

How it runs: the game runs on one core and the I/O processor on core 1, which the system shares (`iopthread 0` in `args.txt` puts it back on the game's core, in exact lockstep). The vector programs, the GIF and all drawing run on the New 3DS's second application core (core 2, or core 1 if that cannot be had; `vuthread 0` in `args.txt` keeps everything on one core). Between them is a 7 MB queue, big enough for a whole picture to wait while the one before it is drawn. The game thread follows the VIF's registers itself, so it never waits for a result.

For full drawing on a New 3DS, put `draw 1` and `splitgs 1` on separate lines in `args.txt` to run the renderer on core 0 at a lower priority than the game. Use `splitgs 2` to share core 1 with the I/O processor instead. Either mode lets core 2 run the next vector program while the renderer consumes the previous one's graphics packets. It adds a 512 KiB queue and a 512 KiB worker stack. The usual renderer is used with picture skipping, software rendering, or when the VU cannot run on core 2. To re-enable picture skipping from split mode, change the settings and restart.

When the game had to wait on that queue, later pictures are not drawn, up to five or six in a row. Their register changes and absolute uploads still happen, and only vector programs that have run before are left out, so the next picture finds the same state. Uploads to the double buffer are left out too, because only those left-out programs would read them, and the game thread filters them before they reach the queue. That rests on that data being used once: lobby pictures still match with it on, but the first battle has only been checked by a few shots, and other scenes not at all. A program not seen before still runs in a skipped picture, with drawing turned off. Many scenes swap between two frame buffers, and a fixed skip would always draw into the same one, leaving the one on show frozen, so while skipping, the buffer just drawn is shown instead. Some scenes also blend the last picture into the next, so an odd run of skipped pictures alternates one fewer and one more: that averages the same and draws into each buffer in turn. Without it the stale buffer showed through as noise. `draw N` in `args.txt` sets a fixed rate, and `draw 1` draws everything.

Times per game frame on Azahar's emulated clock (the probe's timing fits 268 MHz). Azahar charges nothing for GPU work, so these are CPU costs only:
| Screen | Start of speed work | Now |
| --- | --- | --- |
| Boot, nothing drawn | about 130 ms | not measured again |
| 2D screen, 40 to 90 shapes | about 17 ms | about 10 ms |
| Menu, about 590 shapes | not measured | about 30 ms |
| Opening movie | about 700 ms | about 117 ms |
| 3D lobby, about 13,700 triangles, every picture drawn | about 670 ms | 292 ms |
| 3D lobby, one picture in four drawn | not available | about 142 ms |
| 3D lobby on one core, every picture drawn | about 670 ms | about 420 ms |
| First battle, about 14,000 triangles, one picture in four drawn | not measured | about 70 ms |
| Assembly hall, about 54,000 triangles, every picture drawn | not measured | about 430 ms |

Per picture (two game frames) in the lobby with everything drawn, the drawing core is busy for all of the 584 ms: vector programs about 350, GIF and shape collection 85, VIF about 110, uploads 40. The game code itself only runs for about 2 million of the 9.8 million EE cycles in a picture; the rest is idle and costs nothing.

With one picture in four drawn, the game core is now the limit: about 284 ms per picture, of which the I/O processor is about 45 and DMA about 50 (mostly copying the vector data into the queue and walking it). The "game" figure in the log subtracts the drawing core's time as well, so it reads low when the cores overlap. Translated code alone comes to an estimated 16 million ARM instructions per picture, or about 60 ms. The rest of that time is spread thinly, with no single large cause left.

How a movie picture is made: the IPU decodes one macroblock at a time, and the game's own code does motion compensation and adds the two together. About 780 macroblocks make one picture. The IPU decoder takes about 50 ms of the 234 ms per picture. Twenty routines from the game's video code and C library run natively (`native3ds.cpp`, listed in `natives.txt`). Nineteen of them were compared with the game's code on every call for about 2,000 movie frames, with no difference. The other is `memcpy`.

A real New 3DS runs at 804 MHz but does not finish one instruction every cycle, so Azahar's times do not convert to the console's. The hardware floats rely on the FPU's start-up mode (flush denormals, round toward zero), which has not been read back from a console, and pictures from the console match Azahar's with it.

Accuracy given up for speed, on the 3DS build only: EE and VU0 floats use the FPU, so the last bit of a result can differ from the bit-exact code, EE floats and the common VU0 float operations no longer clamp infinities and invalid values on the way in or out (rounding toward zero already stops overflow reaching infinity), and vector programs skip the PS2's overflow and denormal handling. The IPU uses an integer IDCT and leaves out the mismatch toggle: in testing 0.8% of samples differed, by at most 1, and a movie frame stayed within 3 levels out of 255 of the exact decoder's picture. Vector programs not compiled yet still run in the interpreter: a few calls per picture in the Dark Hour scenes, about 15 in battle.

How the renderer works: `pica.cpp` replaces `raster::draw`. Shapes collect in a batch until a GS setting changes. Each PS2 frame buffer gets a render target of its true size, up to 512x256, and the shapes drawn with a texture that was rendered earlier use that target. The PICA has no palettes, so textures are decoded into a cache, keyed by a hash of the bytes uploaded for them, because this game moves the same textures around GS memory every frame.

Three things the GS does need care on this GPU:
- **Alpha test with a fail action.** Nearly all models here keep colour but not depth for pixels that fail the alpha test. The GPU cannot split a pixel, so such a batch is drawn twice, once per kind of pixel. Before this, the clear parts of models wrote depth and hid what came later: that was the black patch near the bench and the hard edges round the battle's pools. This adds GPU work that Azahar does not charge for.
- **Fog.** The GS mixes towards a colour by a value that comes with each vertex. That value reaches the third texture stage as a coordinate into a small ramp texture.
- **Blending by destination alpha** works because alpha is doubled on the way in, so 0x80 is stored as 1.0.

Not done in the renderer: the destination alpha test, dithering, mipmaps, region wrap modes, render-to-texture at an offset, and copies from the frame buffer.

## What is left
1. **Test on real hardware.** It runs on a New 3DS from the Homebrew Menu. A cold start ran for 37 minutes (34,700 game frames) with no crash report. Per game frame the console gives: level-up screens 13 ms of work, held to 16.7, first battle 22 ms, lockers 32 ms, lobby 45 ms with automatic skipping (one picture in five, one every 0.4 s), 36 ms at one in twelve, movies about 71 ms, which beats Azahar's clock. With every picture drawn, the lobby's pixels stay within rounding of Azahar's. Installing and restarting over the wireless link work there, a restart takes about 12 seconds. The lobby matches Azahar with every picture drawn: the first shot after loading is black in both, because the game is fading in. One lobby shot taken with skipping on was also black, cause unknown; none of 11 shots in a live fight were.
2. **More speed.** The vector programs are real work: moving registers into local variables gained 1%. With pictures skipped the game core is the limit, and the console's own counters now show where its time goes. Add `pmu A B` to `args.txt` and each `perf.txt` line gets that core's cycles, two hardware events (0 instruction cache misses, 1 and 2 stalls on instructions and data, 8 instructions, 11 and 13 data read and write misses, 18 load and store queue full) and the time the game waited for the I/O processor and the time that processor worked. Splitting the counts by activity for 30 frames of the lobby at `draw 12` showed the sampling profile used before had been far off: transfers took 43% of the core, not 18%.

| Activity, millions per 30 frames | Cycles before | Cycles now | Instructions now |
| --- | --- | --- | --- |
| Game code and everything else | 546 | 552 | 171 |
| VIF channel | 334 | 218 | 78 |
| Texture channel | 129 | 38 | 8 |
| Other DMA | 39 | 40 | 7 |
| Events and kernel | 24 | 24 | 7 |

   Three things were wrong with the transfers. This game names most of its data in one DMA tag and sends it with the next, and a command split that way fell back to the path that copied everything, twice. Everything kept was copied again through a buffer. And each transfer posted its own message to the queue. Now the double-buffer data of a skipped picture is dropped even when split, what is kept is written in place into one long message, and the long data of an unpack, and textures of 4 KB or more, go as an address for the drawing core to read from PS2 memory. The game can change that memory first: in the lobby 40% of the addressed unpack data had changed by then (none of the textures), because bone matrices live in a buffer the game rewrites. That reaches only a skipped picture, and only as newer values in VU memory, which the next drawn picture sets again. Commands are always copied, so both sides read the same stream. Copying the commands costs about 25 million cycles of the saving. An earlier version sent whole runs of commands as addresses, and those changed too, so leave it this way.

   On the console, ms per game frame in the lobby went from 50 to 42 at `draw 12` and from 64 to 45 with automatic skipping, and the level-up screen from 29 to 20. What is left of the VIF channel is about 5,600 transfers per picture at about 900 instructions each, spread evenly over parsing, DMA tags and the queue. Memory barriers are not it: the store buffer drains only 0.2 million times, and building without thread-safe statics changed nothing. Waiting for the I/O processor is about 180 ms of each 30 frames, and polling instead of sleeping does not help, because it really is behind. It runs the music sequencer and the ADX mixer in an interpreter, flat across many routines (the largest, a sample doubling loop at 0x4A5B8, is about 9%), at about 600,000 instructions per vblank and 19 ms of core 1, so hand ports will not fix it. That also puts a floor of about 19 ms under every scene, and the level-up screen is now at it. A predecoded interpreter is the clearest saving left. Build with `PROF=1` for the detailed timers, which cost speed. Decoding video on the idle drawing core would need the game's whole picture loop replaced, because the game asks the IPU for one piece at a time.
   The I/O processor was next, and it was the limit on every light scene: about 600,000 instructions per vblank at about 25 cycles each in the interpreter. It runs the sound driver, and only about 7,500 instructions of that run in any one scene. `tools/p3fes_iopgen.py` turns the code that ran into C++, one function per block, each jumping straight to the next. The work per 30 frames on the level-up screen fell from about 450 ms to 170, and the time the game spends waiting for it from about 240 ms to 60.

   | ms per game frame on the console | Before | With translated code |
   | --- | --- | --- |
   | Level-up screen, nothing held back | 20 | 13 |
   | First battle | 29 to 32 | 22 |
   | Lobby, `draw 12` | 42 | 36 |
   | Lobby, automatic skipping | 45 | 45 |
   | Movie | 72 to 75 | 71 |

   Since the level-up screen now beats a PS2, the game holds itself to 59.94 game frames per second. `uncapped 1` in `args.txt` takes that off, for measuring. The lobby's game core is now busy all the time, so what limits it is back to item 3, and movies are still limited by decoding.

   Uncapped runs also record `sampled_frames`, `peak_frame_us` and `frames_over_16ms` for each timing window. These measure the intervals between game vblanks, including stalls, so a 30-frame average cannot hide missed deadlines. Use `draw 1` for a full-draw replay and `stats 1` in a separate run to check the renderer's picture cadence.

   The movie decoder now reads only the three bytes needed for a coefficient lookup and skips parts of the inverse transform when the higher frequency rows are empty. On the New 3DS, the same opening movie state with `draw 1;uncapped 1;pmu 8 1` went from 76.8 to 74.8 ms per game frame, taking the median across 14 matched 30-frame windows ending at frames 9090 through 9480. The decoded output matched byte for byte across 249,383 macroblocks in a separate headless replay.

   Doing the motion prediction and output together for each row reduced the same replay to 74.2 ms per game frame. Reading contiguous input for color conversion in batches reduced it to 74.0 ms. Motion prediction matched the previous implementation in 2,048 ARM11 test cases, and the color conversion output matched across 275,080 macroblocks in a headless replay.

   The 3DS movie decoder now looks up short coefficient codes with their sign in one table and clips coefficients with ARM's saturation instruction. The same 2,571 captured macroblocks took 2.3% fewer instructions in the ARM11 replay, including its setup and checksum work. The saved opening movie replay produced the same decoded samples across 249,383 macroblocks. The updated console build took 73.4 ms per game frame over the same 14 windows, compared with 74.0 ms on the previous build.

   With the same timing settings, the saved first battle ran at a median of 69.4 ms per game frame with every picture drawn. Recording and compiling one missing VU program reduced it to 64.1 ms across the same nine 30-frame windows ending at frames 132000 through 132240. The interpreter went from ten calls per picture to zero in this replay, and the captures at frames 132120 and 132240 matched byte for byte. The recording and compiled program are private build inputs.

   Indexed draws reuse shared vertices in strips, fans and quads. They reduced the same battle replay to 62.2 ms per game frame. In the captures at 132120 and 132240 the largest difference was 3 out of 255 in any colour channel. In the full-draw lobby replay with stats enabled, the median fell from 130.5 to 126.0 ms per game frame over 106 matched pictures ending at frames 40920 through 41130. The lobby stored about 41,000 vertices per picture instead of 82,500, with zero missing textures in both runs. The index buffer adds 352 KiB to linear memory use.

   With the split graphics worker, the full-draw battle went from 62.6 to 42.5 ms per game frame across nine matched 30-frame windows on the same test binary. The two nonblank battle captures at 132120 and 132240 matched the indexed renderer byte for byte. The full-draw lobby went from 126.0 to 94.5 ms per game frame over 105 matched pictures ending at frames 40920 through 41128, with stats on and zero missing textures in either run.

   The installed build later measured 48.7 ms in the same split battle windows and 95.0 ms in the same split lobby pictures. The battle captures matched the prototype byte for byte after a live request to change the draw rate. The lobby again reported zero missing textures.

   On a single build with the faster VIF parser, the same battle took 60.0 ms with serial graphics, 46.8 ms with graphics on core 0, and 40.1 ms on core 1. The slowest game frame on core 1 took 82.5 ms, and 135 of 270 exceeded 16 ms. The two nonblank core 1 captures matched the previous split renderer byte for byte. The lobby took 92.0 ms on core 0 and 93.0 ms on core 1 over the same 105 pictures, with zero missing textures.

   The renderer now indexes cached textures by their upload hash and keeps the original order for collisions. The cache test matched all 80,640 lookups, including 10,644 evictions, and the ARM11 cache benchmark used 56% fewer instructions. Vertex conversion also does fewer arithmetic operations and avoids computing unused texture coordinates. Its ARM11 benchmark used 2.5% fewer instructions, and all 179,001 expanded vertices matched. These are component measurements. Console frame timing for both changes is pending.

   The earlier lobby timings taken from `stats 1` divide each renderer picture interval by two and exclude the time the renderer spends writing those stats. For end-to-end timing, leave verbose stats off and use `perf.txt`. Across eight matched 30-frame windows, the lobby measured 105.5 ms per game frame with the updated texture and vertex paths. An experimental build that omitted triangles with repeated clip positions took 103.9 ms and used about half as many vertices, but changed pixels in both nonblank screenshots. The main build keeps those triangles.

   The renderer also receives the adjusted texture clamp with the draw and applies it at batch setup, instead of rewriting the GS context for every primitive. The ARM11 test submitted the same 163,950 primitives and selected the same clamp values. With the change, the lobby measured 105.3 ms per game frame, close to the previous 105.5 ms.

   A 60-frame trace on the console found that opening a drawn picture waited a median of 47.6 ms in the lobby and 33.3 ms in the battle for the previous GPU queue. The vblank markers also spent about 349 ms and 284 ms respectively between the game thread posting them and the VU worker reaching them. Those intervals include queued work from earlier frames, so they measure backlog rather than how long a single frame spends on the VU.

   Reducing the offscreen target limit from 512×256 to 256×128 or 128×64 changed the full-draw lobby median from 105.2 ms to 104.4 or 103.9 ms per game frame across seven matched windows. The battle changed from 40.0 to 39.6 ms at 256×128. Both scenes still missed 16 ms on 105 of 210 frames at each tested size. The standard build keeps the original target size.

   Loop peeling is enabled only for compiled vector programs. In an ARM11 replay of 357 captured calls to six hot lobby programs, it used 5.4% fewer instructions and produced identical VU memory and register digests. It adds about 152 KiB of code to the full build. Console frame timing for this compiler change is pending.

   All of the above together, measured on the console on 2026-11-07 (ms per game frame): lobby 37 with automatic skipping (was 45) and 36 at `draw 12`, first battle 21, level-up 13 uncapped, movie 72. With every picture drawn the battle took 58 without the split worker and 39 with `splitgs 2`, and the lobby 95 with `splitgs 2`. Azahar's pictures of a cold boot, the lobby and level-up matched the build before these changes exactly.

   With every picture drawn, the core running the vector programs is never idle, and the game waits on it about 130 ms per lobby picture. Timing each compiled program there showed they take 57% of that core; the rest is the VIF and GIF work around them. One program, `vu1_E4D910A60DC18989`, takes 27% of the program time in the lobby and the battle alike, about 57 µs a call. Its inner loop scales colours with `MINI` and `MUL` on four values a turn, so it is the place to start on the vector code.

   The translated code comes from your own disc, so like the rest of `generated/` it stays private. To make it, build with `IOPRECORD=1`, play each scene for a few minutes with `iopthread 0`, and keep `iopseen.bin` and `iopram.bin` from `sdmc:/p3fes` for each. Then run this and build as usual, leaving `IOPRECORD` off:

   ```bash
   python3 tools/p3fes_iopgen.py ../ps2-web-recomp/generated/p3fes-iop/blocks.cpp seen_one.bin ram_one.bin seen_two.bin ram_two.bin
   ```

   Anything that was not recorded stays in the interpreter, which costs speed and nothing else. Blocks are checked against what is in memory after every module load and state load, so a different disc or load order also falls back to the interpreter. Each block was run next to the interpreter on the Mac with made-up registers, 275,000 runs with no difference, and the pictures from a cold boot, the lobby and the level-up screen match the build before it exactly.
3. **Cheaper game code.** Game code is now about two thirds of the game core: 552 million cycles per 30 frames in the lobby for 171 million instructions, and 222 million of those cycles are waits for instruction fetches, at about 25 cycles per cache miss. The game is in 324 units of about 120 KB, so the linker cannot move hot functions together. To find where the game's code goes, add `g_bb[k] += n` next to every `c.budget -= n` in a throwaway copy of `generated/p3fes` and dump the table at exit: that gives exact counts per block with no sampling, and it runs in Azahar. In the lobby the game runs about 870,000 instructions per vblank in about 7,300 of the 224,000 translated blocks. The top of that list was the bone animation, which is now in `natives.txt`: `pose_blend` (one call per bone, about 440 calls per vblank), `pose_matrix`, `matrix_product`, the matrix loop `loop_bone_matrices` and the copy loop `loop_copy_pairs`. Together they cut the lobby at `draw 12` in Azahar from 2043 to 1930 ms for 30 frames, 5.5%, but the console barely moved: 51 to 50 ms at `draw 12`, and no change at `draw 4` (55), automatic skipping (64) or `draw 1` (150). Azahar counts about one tick per ARM instruction and has no caches, so its savings do not carry over, and a saving in game code does not show that game code is what limits the console. Measure on the console before spending more time here. What remains is spread thinly: 0x004D59D0 5%, 0x0053A410 4%, 0x004D6B90 3%, 0x004A0E00 3%. Keeping registers in ARM registers does not pay: forwarding them within a block saved 0.7% of the code, because blocks are about five instructions long. Telling GCC that memory and registers do not overlap, with typed guest memory, register unions without character types and slow paths in assembly, cut loads by 15 to 20% in the code but made the program 2% larger and took 0.4% longer in Azahar. Marking blocks that never ran as cold barely changes the hot units, 0 to 11% fewer hot cache lines, because ARM GCC cannot move them out of the function, so it was dropped. Azahar's debug port is no use as a profiler: it reports each thread's context from its last switch, so most samples fall on a few addresses.
4. **More of the game.** Everything up to the end of the first battle has been looked at. Tartarus, the shops, the in-game menus and other battles have not, and will need new vector programs recorded, which memory has little room for.
5. **Sound output** through the 3DS DSP.

The queue now uses 32-bit cursors and wraps with a comparison, keeping the same 7 MiB capacity. VIF resolves the thread and register bank once per transfer, and contiguous V4-32 uploads use a bulk copy. On the New 3DS, loading the lobby state and using `draw 4` gave a median of 57.9 ms per game frame, down from 60.5 ms. The comparison uses nine 30-frame timing windows ending at frames 40890 through 41130. This is still above the 16.7 ms budget at a draw rate of one picture in four.

When a VIF command is the only word pending and the next transfer contains its complete payload, the parser now reads the payload directly. It also skips runs of padding words together. An ARM11 replay of 6,000 captured lobby transfers and register writes used 5.9% fewer instructions and produced the same checksum. The saved lobby replay finished with identical guest RAM, and console timing for the parser change is pending.

With drawing suppressed after warmup, the lobby still took 53.3 ms per game frame over five 30-frame windows. Keeping the IOP program counters in registers and translating a sampled set of IOP blocks both tied their matched `draw 4` controls. These tests put the next priority on reducing the work done by the game core, as well as speeding up full drawing.

Compiled VU1 loads and stores now use the alignment of the VU data RAM. The private test build shrank by 119 KiB. The full-draw lobby took 162.8 ms per game frame versus 162.3 ms on the same control, and the rendered capture at frame 41040 matched byte for byte. The timing comparison excludes windows that contain a screenshot write.

The renderer reuses transformed vertices between consecutive triangles in a strip or fan and between the two triangles of a quad. The same full-draw replay dropped from 162.8 to 160.2 ms per game frame with these two changes, and the rendered frame 41040 still matched byte for byte.

The lobby's limits, measured on the console with timers. The core with the vector programs needs about 300 ms for a picture with everything drawn (150 ms per game frame), and the game core alone about 51 ms per game frame. Moving the I/O processor to core 1 alone changed nothing at one picture in four, because the game core then spent the time it gained waiting for the queue. It only pays together with more skipping, so the automatic mode now skips sooner and further: the lobby went from 72 to 55 ms per game frame, at the cost of a picture every 0.55 s instead of every 0.44 s. Cheaper vector programs (no flags that nothing can read, fixed-point conversions and min/max in the FPU, `__restrict` on the registers and VU memory) then took a drawn picture from 358 to 300 ms, and the automatic mode spends that on pictures: one in three at 64 ms per game frame. The I/O processor takes about 17 ms per vblank on core 1.

With the I/O processor on its own core, a run depends on real time and no longer repeats exactly. In Azahar the five lobby pictures still matched byte for byte, but use `iopthread 0` for picture comparisons.

## Testing
`route.txt` holds the button presses from a cold boot to the first battle, one `frame:button[:frames]` per line, for `ps2run --press`. It is timed for the Mac build: a 3DS cold boot drifts from it by hundreds of frames, so to reach a later scene on the 3DS, make a saved state on the Mac and load it.

Saved states from the Mac build load here, which skips the long opening: make one with `ps2run --save-state FRAME:file`, copy it to `/p3fes/`, and add `load file` to `args.txt`. `record sdmc:/p3fes/vu1` saves any vector programs that are not compiled yet: copy them to `games/p3fes/vu1` and run `vurecomp` again. `soft 1` draws with the software renderer, which is the reference when the GPU picture looks wrong.

- The test build logs to `/p3fes/log.txt`, so a run can be checked without looking at the emulator window. An uncaught C++ exception is logged there with its thread, then stops the program. In Azahar that shows up as CPU use falling to almost nothing.
- Always test from a cold boot as well as from a saved state. The out-of-memory stop at frame 1802 never appeared in the lobby, and a movie with no picture, because menu textures had split up the free memory, looked fine from a state.
- In the `[pica] frame` lines each number comes before its label: `9 reused 1 missing` means one texture could not be made, so its shape was left out. Any `missing` above 0 is a fault.
- `tests/test_3ds_natives.py` only checks that the list and the code agree. To compare a native routine with the game's own code, build a Mac copy that calls both and compares their output: the C paths in `native3ds.cpp` are written to run there too. The two small ARM instruction paths can only be checked in the 3DS build. To do the same with floating point, cut the routine's text out of `generated/p3fes`, compile it with `-D__3DS__ -ffp-contract=off` next to `native3ds.cpp`, set the ARM64 FPCR to flush denormals, default NaN and round to zero, and compare memory, registers and the budget on random input, overlapping pointers included. The animation and matrix routines had no difference in about 17 million cases.
- `hide A-B` in `args.txt` leaves out batches of each picture, and `dump N` numbers them, which finds the shape behind a wrong pixel. Textures move around GS memory every frame here, so an address does not identify one for long.
- Before calling a difference from the Mac build a fault, compare over a few seconds. The Velvet Room's bright light is an animation with a 150-frame cycle, and the two builds were simply caught at different moments.
- For a comparison with skipping on, use `shots 2;draw 3`, not `draw 4`. An even `draw` skips an odd number of pictures, so only one of the game's two frame buffers is drawn, and in the lobby every shot is black. Check the pictures have content before believing a match.
- With the default picture skipping, `shots N` only catches a picture that was drawn. Use `draw 1` for comparisons, but also run scenes with skipping on: the frozen second buffer only showed up that way, and it looked just like a loading hang. `debug 4` skips the drawing but still runs the vector programs.
- A run that ends without a crash proves little. Look at the shots along the way: a frozen picture and a slow hang both pass as a clean run.
- Timing a whole-program run with `stats 1` is fair, but keep the counters away from anything called per shape: in Azahar one `svcGetSystemTick` costs about 450 ticks, which once hid inside the draw figures.
- The first shot or two of a run can come out black. That is the capture, not the picture: the same frame is fine later in a run.
- Azahar ignores a setting whose `key\default=true` line is still there, so test scripts must change both lines.
- A normal quit signal did not close Azahar here (probably its `confirmClose` prompt), so make sure test scripts leave no copies running: several copies share one log and SD folder.
