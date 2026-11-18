# New 3DS speed: plan items tried, and bigger ideas

Date: 9 November 2026. This follows the [improvement plan](improvement-implementation-plan.md) and its [research report](improvement-research-2026-11-09.md). Speeds are ms per game frame on the console unless marked otherwise. None of this is committed.

## How changes are checked now

- **Exact checks on the Mac.** `build/native-p3fes` in ps2-web-recomp builds the same generated code for macOS. It is deterministic, and it loads the 3DS `.state` files, so a correct change must reproduce the cycle count exactly. Baselines: cold start to frame 6000 = 29503004426 cycles; `lobby.state` to 41600 = 204484284558; `levelup.state` to 134100 = 659140431162. Each check takes about a minute. It found the one bug in this round, which Azahar showed only as a hang.
- **Pictures** in Azahar: lobby, level-up and a 193-second cold boot, compared shot by shot.
- **Speed** only on the console, with `pmu` counters: lobby (`lob`), battle (`bat`), and both with every picture drawn (`lobf`, `batf`). `tools/p3fes_scenario.py compare` reads these `perf.txt` files as they are.

## Memory is the hard limit

The game's 32 MB of PS2 memory and a 1 MB game stack have to fit in the 126976 KB region after the program. Build lb1 (+2.7 MB of code) still fit. lp1 (+3.3 MB) did not: Azahar logged that it could not map PS2 memory, and the console has not answered since lp1 was installed. Every code-growing idea below has to pay for its bytes. The heap uses 20.8 MB of the 28 MB it reserves (Azahar exit log), so cutting the heap could free about 6 MB for code.

## Plan items

| Item | State | Result |
|---|---|---|
| Scenario harness | used | `compare` works on the existing logs; it still needs to start and collect runs on its own |
| One-path EE registers | built as loop promotion | exact on the Mac and in Azahar; console speed not measured yet |
| Local cycle budget | built, off by default | 0.5–1% faster on the console, but +2.8 MB of code; `EERECOMP_LOCALBUD=1` turns it on |
| MVD movie memory probe | written (`mvdprobe 1`) | not run yet; the console has been down |
| VU output fusion | not started | see below; the profile argues for a different first step |

### Loop promotion (LOOP_EDITS in the patch tool)

The generator marks small loops: a backward branch, no calls, no jumps through registers, no hardware probes, at most 160 instructions. Before such a loop starts, the guest registers it uses move into C++ locals, so GCC can keep them in ARM registers for the whole loop. They are written back at every exit, and around the preempt check when the thread gives up control. A jump from outside into the middle of a loop loads the locals first. That is the bug the exact check found: a loop that began in the delay slot of the branch before it.

- 3613 loops in the program cover 19.5% of guest instructions in the lobby and 20.7% in battle (block counters, Mac build).
- Cycle counts are identical on the Mac, and every Azahar shot is identical.
- Code grows by 0.54 MB (41.04 MB to 41.58 MB).
- Console, same session, medians of 30-frame windows (n2 is the build before loops):

  | Scene | n2 ms / cycles / instr | lo3 | lo5 |
  |---|---|---|---|
  | lob | 37.8 / 861M / 247M | 37.3 / 852M / 244M | 38.5 / 877M / 244M |
  | bat | 21.1 / 451M / 139M | 21.0 / 448M / 137M | 21.3 / 457M / 137M |
  | lobf | 88.1 / 1221M / 260M | 88.5 / 1219M / 257M | 88.0 / 1222M / 257M |
  | batf | 36.4 / 639M / 143M | 36.2 / 637M / 141M | 36.3 / 639M / 141M |

  lo3 cuts 1–1.4% of instructions and up to 1% of cycles: a small win, now the default and on the console. lo5 cuts the same instructions but costs more cycles in the lobby (+1.9%). Its larger code loses more to instruction fetch than the extra coverage saves. Coverage alone is not the goal: code size competes with it.
- Knobs: `EERECOMP_NOLOOPS=1` turns it off; `EERECOMP_LOOPS=a:b` keeps only loops a to b−1, for bisecting; `EERECOMP_LOOPMAX` and `EERECOMP_LOOPOUTER` are for experiments only.

Larger loops, Mac block counts over the same frames:

| Variant | Loops | Lobby | Battle | Build |
|---|---|---|---|---|
| up to 160 instructions, smallest first (default, build lo3) | 3613 | 19.5% | 20.7% | 41.58 MB |
| up to 400, smallest first | 3625 | 20.1% | 21.6% | not built |
| up to 400, outermost first (build lo5) | 3476 | 23.5% | 24.7% | 41.88 MB |
| up to 1000, outermost first | 3465 | 24.1% | 24.7% | not built |

lo5 is also exact: cycle counts match for cold boot, lobby and level-up, and every Azahar shot is identical.

Loops level off at about 24%. The five hottest lobby blocks (8.9% of guest instructions) are not loops at all. For example, L_004D59D0 is a function entered through the dispatch table, with only forward branches. Only whole-function promotion (idea 4 below) can reach code like that.

## Where the console's cycles go

Lobby, game core, 30 frames: 865M cycles.

| Share | What |
|---|---|
| 29% | instructions |
| 29% | instruction fetch stalls (9.1M icache misses; about 85% in generated code) |
| 22% | waiting on data dependencies |
| 12% | load/store unit full |

TLB and data cache misses are negligible. The game core is 100% busy in the lobby. With every picture drawn, the VU core limits instead: compiled VU programs are 57% of it, and one program alone is 27%. All three usable cores are already busy: game on core 0, VU on core 2, graphics and IOP on core 1.

## Bigger ideas, ranked

1. **Skip bounds checks on stack accesses.**
   - sp-based loads and stores are 26–29% of all guest memory accesses (cold boot 263M of 1021M, lobby 89M of 356M, battle 53M of 237M), and none left main RAM in these runs.
   - The offsets are 16-bit and signed, so one check after the function's prologue (and after any other change to sp) covers every sp-based access that follows: is sp between 64 KB and 32 MB − 64 KB? Each access then skips its own compare and branch.
   - Expected gain: about 2 ARM instructions per access and about 1 MB less code, which also helps the icache.
   - Fallback when the check fails: the normal checked path, which needs either a second copy of the function or a flag tested per access. A second copy is too big. Masking the address (`& (32 MB − 1)`) costs one instruction and needs no copy.
   - Risk: low. Effort: small. **Done, the same evening:** masked form (`SP_EDITS`). Exact on the Mac and in Azahar, 2.56 MB smaller (41.58 to 39.03 MB). On the console: instructions down 1.5–2%, cycles 0.7–2.3%, battle 21.0 to 20.6 ms, lockers 30.5 to 30.1 ms.

2. **Order hot code together.**
   - Fetch stalls cost as much as the instructions themselves. Cold marks and a 3% shrink did not reduce them, because GCC for ARM cannot split hot and cold parts of a function.
   - Instead, the block counters can rank functions by heat. A generated linker-script fragment then places the hot `.text.*` sections next to each other (they already exist because of `-ffunction-sections`). The hot loop of the lobby then shares fewer icache sets with cold code.
   - Code size stays the same, so it fits in memory. Effort: small; payoff unknown until measured.

3. **Free memory for code by cutting the heap.** About 6 MB of the reserve is unused. That room would let the local budget (+2.8 MB, 0.5–1%) and wider register promotion in together. Measure the real peak in a long cold run and in movies before cutting.

4. **Promote whole functions, not just loops.** Loop promotion covers about 20% of guest instructions. The next step is function-level register allocation: keep guest registers in locals across a whole function and write them back only at calls, returns, exits and preempts. Last time, liveness across the whole program and changing the return convention saved little. Locals across a function is a different mechanism, and the loop work showed how to keep it exact. It costs code at every exit, so it depends on item 3.

5. **Movies through the hardware decoder (MVD).** Movies run at about 69 ms. The game core is the limit there: about 35 ms of the game's own MPEG code plus IPU decoding. The libctru MVD interface decodes H.264, not MPEG-2. So movies would have to be converted on the owner's own computer, and the game's movie player replaced, with sound, skipping and the return to the scene handled. The probe answers the first question: does the decoder's working memory fit next to the game? If not, stop.

6. **VU programs.** For full-draw scenes the VU core is the limit. The hottest program is mostly integer loads, stores and ALU work, not float math. So the cheap first step is to keep VU integer registers in locals and skip flag updates nobody reads, not fusion. Fusing VU output straight into PICA vertices is the large structural step after that. It needs the harness to compare whole states and pictures first.

### Measured and dropped

- Clang: code 40% bigger than GCC.
- Typed guest memory: no change.
- An out-of-line asm slow path: at best 5% fewer CPU loads, not worth the risk.
- Earlier: cold-code marks, `-Os`, the VU result cache, whole-program liveness.
