# Further improvements: research and experiments

Research date: 9 November 2026. Scope: the current PC/browser project and the experimental New 3DS port. This is a research proposal, not a benchmark report or an implementation. Local evidence includes the working tree, including changes not yet committed. No console or game was run for this research.

The strongest opportunities are to remove entire layers of repeated work, make graphics enhancements independent of game logic, and turn existing debugging tools into repeatable scenarios. More compiler flags, more frame skipping, or more launcher decoration are unlikely to change the project's trajectory.

## What the existing evidence rules out

The detailed [3DS notes](../port3ds/README.md) are more current than their opening summary. The combined hardware measurements dated 7 November report approximately 37 ms per game frame in the lobby with automatic skipping, 21 ms in battle, 13 ms on level-up screens uncapped, and 72 ms in movies. With every picture drawn and `splitgs 2`, the reported lobby/battle times are 95/39 ms. These configurations are different workloads and must not be compared as if they displayed the same number of pictures.

Already tried or present: IOP translation, native ADX routines, several native MPEG routines, tiled movie-texture conversion, VU register locals, register-width narrowing, packed GIF parsing, indexed draws, a split graphics worker, hardware counters, frame-deadline counts, and cold-code recording. The notes also document disappointing hardware results from cold-code marking, a VU memoization cache, smaller render targets, earlier GPU submission, and a macroblock worker. Repeating these under a new name would not be a fresh proposal.

The PC project already has autosaves, a dialogue history with voice replay, mod priority, HD textures, translations, subtitles, and walkthrough checklists. The proposals below extend those capabilities rather than count them again.

## 1. Compile frequently executed paths across basic-block boundaries

**Priority: first 3DS CPU experiment. Effort: high; begin with one path.**

The notes attribute substantial instruction-fetch stalls to translated game code and describe roughly six ARM instructions per guest instruction. Forwarding registers within short blocks saved little. Whole-function register retention is already identified as a possible direction; the fresh experiment is a narrower, guarded path through several blocks and small calls.

Record the most frequent block-to-block transitions in a fixed lobby replay. Select one path with substantial repeated guest-register loads/stores. Generate an additional ahead-of-time C++ function that keeps only its useful guest values in locals. Every exit must reconstruct the correct guest state and PC; hardware accesses, scheduler boundaries, unknown calls, and potentially aliasing memory accesses require explicit synchronization. Preserve cycle accounting and branch-delay semantics. Unexpected execution falls back to the existing translator. Do not assume every routine follows the guest calling convention.

This borrows the trace-selection and guarded-exit concept from [Dynamo](https://www.complang.tuwien.ac.at/andi/bala.pdf), adapted here to offline generation. That paper is evidence for the technique, not evidence of a speedup on ARM11 or this game.

**Go/no-go:** compare guest state and memory writes at every exit, then run interleaved control/candidate/control hardware trials. Require lower end-to-end time and instruction-fetch stalls at the same drawing setting. Track added code size: a duplicated fast path can worsen the very cache problem it is intended to solve. An engineering target of at least 5% whole-scene improvement would justify expansion; it is not a predicted result.

## 2. Replace the complete movie playback path on New 3DS

**Priority: first movie feasibility probe. Effort: medium probe, high integration.**

Convert movies locally from the owner's disc into a conservative H.264 format, then play them through the New 3DS MVD service. The [libctru API](https://raw.githubusercontent.com/devkitPro/libctru/master/libctru/include/3ds/services/mvd.h) exposes H.264 decoding, RGB565 output and working-buffer calculation. It does not expose MPEG-2 input in this interface.

The new part is replacing the game's movie decode loop and its completion contract. The existing PC proposal in [HD cutscenes](hd-cutscenes.md) leaves the original movie decoding underneath a host video. That would retain the 3DS bottleneck. A replacement must provide movie completion, skip, audio timing, subtitles and return-to-scene behavior without running the expensive original decoder underneath.

**First gate is memory:** libctru's default buffer is `0x9006C8`, approximately 9 MiB, versus the project's reported 2.6 MiB free application memory. Query the required size for the actual encoding and test service initialization with the game resident. Investigate reclaiming texture caches and temporary graphics allocations during playback; cold-code builds may provide some additional memory, although they did not improve speed. Neither option guarantees enough usable memory.

**Go/no-go:** one opening movie must decode at its native cadence with bounded memory, stable audio sync, successful skipping and a correct return to gameplay. Begin with a standalone decoder probe under a representative memory budget. If memory cannot fit, stop before modifying game flow. Sound output on 3DS is currently off, so audio integration remains real work rather than an existing capability to assume.

## 3. Fuse a known VU output path with vertex submission

**Priority: second 3DS structural experiment. Effort: high; correctness risk high.**

The compiled VU programs still produce packets that the GIF path decodes before the PICA renderer assembles vertices. Recent optimization made that parser cheaper; a further step is to avoid producing and decoding a known packet layout at all.

Choose one frequent, simple VU output pattern. Generate a specialized adapter that produces renderer vertices directly while preserving observable VU registers, memory, GS state, ordering, clipping and numerical quantization. Unknown programs, unexpected packet layouts and state changes use the original route. Initially keep the normal output available for shadow comparison; only eliminate writes after proving nothing else reads them.

This is an architectural inference from the current `xgkick`/GIF hooks and [PICA vertex path](../port3ds/pica.cpp), not a demonstrated optimization. It differs from the already-tested VU result cache: it removes representation conversions rather than trying to predict repeated inputs.

**Go/no-go:** compare full guest state and vertex/packet-equivalent digests, plus live screenshots, on lobby and battle replays. Measure full-draw cadence and both worker workloads. A faster GIF microbenchmark without faster presentation is insufficient.

## 4. Give gameplay and widescreen rendering separate camera rules

**Priority: high-value PC correctness project. Effort: medium/high.**

The [0.57 changelog](changelog.md) records a scene that waited forever for Akihiko to leave the widened screen; the workaround narrows the aspect to 1.70. This suggests a deeper dependency between visibility tests and scene progression.

Keep original visibility semantics for event progression while applying a wider camera to rendering. Start by locating the exact offscreen predicate used in the July 7 scene. Preserve its original bounds rather than narrowing every scene globally. Rendering culling may need separate treatment so newly visible scenery is actually submitted.

Use the original camera for unsupported cutscenes until their logic is understood. A generic vertex shader stretch cannot solve logic that executes before rendering. [Zelda64Recomp's documentation](https://github.com/Zelda64Recomp/Zelda64Recomp#readme) also acknowledges cutscene-edge quirks under wide aspect ratios; this is useful precedent, not an implementation that can simply be copied into a PS2 pipeline.

**Go/no-go:** the July 7 exit completes with true 16:9, other offscreen-triggered actions preserve their timing, and normal fields render without missing edge geometry. Broader ultrawide support comes later.

## 5. Build texture preparation around frame deadlines

**Priority: strongest near-term PC smoothness opportunity. Effort: medium.**

[HD texture documentation](hd-textures.md) permits waiting up to 150 ms in a frame for a replacement. A 60 Hz frame has about 16.7 ms. Existing worker threads and the 24-entry hot list help, but a global popularity list does not know which screen the player will open next.

Learn small asset sets for specific screens and transitions: shop, fusion, battle menu, compendium. Prepare their replacements when the preceding transition starts. Cache decoded or GPU-ready assets locally, keyed by source content and conversion version. Impose an explicit per-frame upload budget. When a deadline cannot be met, use the original texture until a safe transition instead of blocking the frame.

Palette-aware identity is an already-known gap, so it is a prerequisite rather than the fresh idea. Match pixel indices together with effective palette, dimensions, format and relevant state; track writes that invalidate either texture or palette. [PCSX2's replacement implementation](https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp) includes palette hashes and texture metadata in its keys. Study its identity model before extending the current upload-only hash.

**Go/no-go:** compare cold and warm runs through the same menu sequence, recording p95/p99 presentation intervals, largest hitch, missed HD substitutions and memory usage. Include the Hierophant lamps and Lovers floor as correctness cases. Success requires less stutter without restoring wrong-texture flashes.

## 6. Turn existing replays into a reusable scenario runner

**Priority: enabling work for all performance changes. Effort: medium.**

Component replays, saved states, screenshots, hardware counters and wireless control already exist. Combine them into named scenarios with a local checkpoint, frame-indexed inputs, exact runtime/mod configuration, capture points and expected assertions. Start with lobby, first battle, movie, fusion menu and the July 7 exit.

Add frame IDs across game execution, queue submission, VU/GIF work and actual presentation. The existing notes show hundreds of milliseconds of queue backlog: game vblank throughput alone can hide a picture that is stale or rarely updated. Measure displayed-picture age, unique pictures per second and input-to-visible-response alongside game-frame time.

Use compact buffered events and export after a run. [Perfetto](https://perfetto.dev/docs/instrumentation/track-events) provides duration, counter and flow-event concepts suitable for showing causality across workers. A full tracing SDK need not run on the handheld; the PC can convert a small custom capture. Existing Tracy work may serve the same purpose.

Determinism must be tested, not assumed from a fixed input list: asynchronous I/O and thread scheduling may require recording events or constraining replay mode. Exact state checks are suitable within one configuration; cross-platform graphics comparisons may require documented tolerances.

**Go/no-go:** a single command repeats a scenario and identifies its first divergence. Public reports contain hashes, versions, measurements and synthetic test inputs; game-derived checkpoints, dumps, images and generated code remain private under the project's existing rules.

## 7. Explain mod combinations and make them reproducible

**Priority: medium; useful as the mod ecosystem grows. Effort: medium.**

Extend existing priority merging with a dry-run explanation: which mod owns each changed file or instruction, which mod loses an overlap, and which dependent generated assets must change. Save a small profile manifest containing mod versions/hashes, order, settings and runtime build identity.

Attach that manifest to autosave metadata. On resume, compare it with the current installation and explain mismatches before loading a potentially incompatible state. Offer a cloned profile for experimentation and an automatic binary search across enabled mods to isolate a reproducible failure. Never mutate the user's main save during diagnosis.

**First experiment:** two synthetic mods deliberately change one file and one code location. The preview must explain the exact winner; switching profiles must rebuild affected artifacts and preserve separate saves. This is more useful than merely adding another mod list or switch.

## 8. A local, spoiler-safe “where was I?” notebook

**Priority: distinct player-facing feature. Effort: small manual prototype, medium automatic version.**

Build on the existing dialogue log and checklist: a Resume panel with the last location/date when reliably detected, recent speakers, the player's pinned goal, and manually recorded fusion plans. Allow a dialogue line to be bookmarked without exposing future guide entries. Do not infer that a quest completed or a clue was understood merely because text appeared.

Start with manual notes, log bookmarks and profile association; later add verified game-state signals. The 3DS bottom screen could show pinned notes once performance and input handling permit it. This is a product proposal, not a feature borrowed from the external sources above.

## Recommended order

1. Establish a repeatable scenario and presentation-age baseline using the existing tools.
2. Run the one-path EE translation experiment and the standalone MVD memory probe as separate investigations.
3. For PC polish, pursue texture preparation with deadlines and the July 7 visibility predicate.
4. Attempt VU-to-renderer fusion only after the scenario harness can check its state and pixels.
5. Add reproducible mod profiles and the session notebook as focused player-facing improvements.

Do not promise 60 fps on New 3DS from these ideas. The current full-draw measurements imply a substantial gap, and individual gains may move the bottleneck to another worker. Accept changes on matched real-hardware runs with the same rendering policy, not just lower instruction counts or emulator timing.
