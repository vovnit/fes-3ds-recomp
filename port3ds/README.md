# New 3DS port (experimental)

The goal is to run the recompiled game on a real New 3DS, locally, with no streaming. This folder holds the 3DS host and the build files. Nothing here comes from the game: the build is made on your computer from your own disc, and its output must stay private (see [CONTRIBUTING.md](../CONTRIBUTING.md)).

**State: the scenes checked from the title through the first battle render correctly in Azahar, apart from one small flicker noted under [Status](#status). On a real New 3DS the lobby runs at about 13 game frames per second, the first battle at about 25 to 30, and movies at about 13; full speed is 60. Outside movies it shows one picture in three or four. Not playable yet.** See [Status](#status).

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

How it runs: the game and the I/O processor run on one core. The vector programs, the GIF and all drawing run on the New 3DS's second application core (core 2, or core 1 if that cannot be had; `vuthread 0` in `args.txt` keeps everything on one core). Between them is a 7 MB queue, big enough for a whole picture to wait while the one before it is drawn. The game thread follows the VIF's registers itself, so it never waits for a result.

When the game had to wait on that queue, later pictures are not drawn, up to three in a row. Their register changes and absolute uploads still happen, and only vector programs that have run before are left out, so the next picture finds the same state. Uploads to the double buffer are left out too, because only those left-out programs would read them, and the game thread filters them before they reach the queue. That rests on that data being used once: lobby pictures still match with it on, but the first battle has only been checked by a few shots, and other scenes not at all. A program not seen before still runs in a skipped picture, with drawing turned off. Many scenes swap between two frame buffers, and a fixed skip would always draw into the same one, leaving the one on show frozen, so while skipping, the buffer just drawn is shown instead. Some scenes also blend the last picture into the next, so an odd run of skipped pictures alternates one fewer and one more: that averages the same and draws into each buffer in turn. Without it the stale buffer showed through as noise. `draw N` in `args.txt` sets a fixed rate, and `draw 1` draws everything.

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
1. **Test on real hardware.** It runs on a New 3DS from the Homebrew Menu. A cold start ran for 37 minutes (34,700 game frames) with no crash report. Per game frame the console gives: first battle 34 to 40 ms, its level-up screens 41 to 57 ms, lobby 76 ms with skipping (65 ms at one picture in four, 268 ms with every picture drawn), movies about 79 ms, which beats Azahar's clock. With every picture drawn, the lobby's pixels stay within rounding of Azahar's. Installing and restarting over the wireless link work there, a restart takes about 12 seconds. The lobby matches Azahar with every picture drawn: the first shot after loading is black in both, because the game is fading in. One lobby shot taken with skipping on was also black, cause unknown; none of 11 shots in a live fight were.
2. **More speed.** The vector programs are real work: moving registers into local variables gained 1%. The game core is the limit when pictures are skipped, and its time is spread thinly. A sampling profile of the game thread in the lobby gave: game code 36%, I/O processor 18%, VIF parsing 18%, DMA 10%, queue posts 8%, blocked on the queue 6%. The I/O processor is interpreted at about 60 ARM cycles per instruction, about 9 ms per vblank, so a predecoded interpreter is the clearest saving left. Build with `PROF=1` for the detailed timers, which cost speed. Decoding video on the idle drawing core would need the game's whole picture loop replaced, because the game asks the IPU for one piece at a time.
3. **More of the game.** Everything up to the end of the first battle has been looked at. Tartarus, the shops, the in-game menus and other battles have not, and will need new vector programs recorded, which memory has little room for.
4. **Sound output** through the 3DS DSP.

## Testing
`route.txt` holds the button presses from a cold boot to the first battle, one `frame:button[:frames]` per line, for `ps2run --press`. It is timed for the Mac build: a 3DS cold boot drifts from it by hundreds of frames, so to reach a later scene on the 3DS, make a saved state on the Mac and load it.

Saved states from the Mac build load here, which skips the long opening: make one with `ps2run --save-state FRAME:file`, copy it to `/p3fes/`, and add `load file` to `args.txt`. `record sdmc:/p3fes/vu1` saves any vector programs that are not compiled yet: copy them to `games/p3fes/vu1` and run `vurecomp` again. `soft 1` draws with the software renderer, which is the reference when the GPU picture looks wrong.

- The test build logs to `/p3fes/log.txt`, so a run can be checked without looking at the emulator window. An uncaught C++ exception is logged there with its thread, then stops the program. In Azahar that shows up as CPU use falling to almost nothing.
- Always test from a cold boot as well as from a saved state. The out-of-memory stop at frame 1802 never appeared in the lobby, and a movie with no picture, because menu textures had split up the free memory, looked fine from a state.
- In the `[pica] frame` lines each number comes before its label: `9 reused 1 missing` means one texture could not be made, so its shape was left out. Any `missing` above 0 is a fault.
- `tests/test_3ds_natives.py` only checks that the list and the code agree. To compare a native routine with the game's own code, build a Mac copy that calls both and compares their output: the C paths in `native3ds.cpp` are written to run there too. The two small ARM instruction paths can only be checked in the 3DS build.
- `hide A-B` in `args.txt` leaves out batches of each picture, and `dump N` numbers them, which finds the shape behind a wrong pixel. Textures move around GS memory every frame here, so an address does not identify one for long.
- Before calling a difference from the Mac build a fault, compare over a few seconds. The Velvet Room's bright light is an animation with a 150-frame cycle, and the two builds were simply caught at different moments.
- With the default picture skipping, `shots N` only catches a picture that was drawn. Use `draw 1` for comparisons, but also run scenes with skipping on: the frozen second buffer only showed up that way, and it looked just like a loading hang. `debug 4` skips the drawing but still runs the vector programs.
- A run that ends without a crash proves little. Look at the shots along the way: a frozen picture and a slow hang both pass as a clean run.
- Timing a whole-program run with `stats 1` is fair, but keep the counters away from anything called per shape: in Azahar one `svcGetSystemTick` costs about 450 ticks, which once hid inside the draw figures.
- The first shot or two of a run can come out black. That is the capture, not the picture: the same frame is fine later in a run.
- Azahar ignores a setting whose `key\default=true` line is still there, so test scripts must change both lines.
- A normal quit signal did not close Azahar here (probably its `confirmClose` prompt), so make sure test scripts leave no copies running: several copies share one log and SD folder.
