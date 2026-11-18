# Improvement implementation plan

Started 9 November 2026. Rationale and alternative ideas: [research report](improvement-research-2026-11-09.md).

## Sequence and acceptance gates

1. **Scenario baseline — in progress.** Prepare immutable run folders with exact settings and hashes of the executable and local checkpoint. Compare identical frame windows. Next: validate settings comprehensively, verify checkpoint availability on the console, automate run completion and collection, and record presentation IDs. Acceptance: repeat one scene three times and distinguish run variation from a real change.
2. **One-path EE optimization — pending.** Record frequent transitions and generate one guarded path across basic blocks. Preserve guest state, delay slots, hardware access and cycle budgets. Acceptance: matching state at all exits and repeatable end-to-end improvement on hardware at identical draw settings.
3. **Native movie feasibility — pending.** Probe MVD initialization and working memory with the game's memory footprint. Stop if usable memory cannot fit. Only then implement playback, sound, skip and scene completion; do not leave guest decoding running underneath.
4. **PC texture deadlines — pending.** Establish cold/warm menu baselines, add palette-aware identity, then transition-based preparation and bounded uploads. Acceptance: lower hitches without wrong textures in the known boss-room cases.
5. **Widescreen scene logic — pending.** Locate the July 7 offscreen predicate and separate it from rendering bounds. Acceptance: scene completion at true 16:9 with original event timing.
6. **VU output fusion — pending.** Implement one specialized output adapter with fallback after the regression harness supports full-state and image comparisons.
7. **Mod profiles and session notebook — pending.** Add explainable conflict previews and save/profile identity before player notes and bookmarks.

Each optimization needs interleaved control/candidate/control trials. Instruction counts and emulator speed alone do not establish hardware improvement. Preserve the user's existing work and keep game-derived artifacts private.

## First implemented slice

`tools/p3fes_scenario.py` provides offline preparation and matched-window comparison using the existing 3DS `args.txt` and `perf.txt` formats. It does not yet launch the console or prove deterministic execution.

Prepare a local settings file with explicit `frames` and `draw` values. For checkpoint runs include `load lobby.state` and supply the corresponding local file. Frame limits and input schedules retain the existing runtime's semantics; choose the endpoint after the checkpoint's frame counter.

```sh
python3 tools/p3fes_scenario.py prepare --name lobby-full \
  --settings /private/tmp/lobby-args.txt \
  --binary ../ps2-web-recomp/build/3ds/p3fes.3dsx \
  --state /private/tmp/lobby.state --output /private/tmp/lobby-baseline
```

The output directory must not exist. It contains `args.txt` and a JSON manifest with settings and SHA-256 identities. Copy settings using the existing SD/link workflow and collect logs using `p3fes_3ds_link.py fetch`. Ensure the matching checkpoint is already on the SD card: the current runtime can silently boot from the beginning if it is missing. This is a required manual check until the runner verifies it.

```sh
python3 tools/p3fes_scenario.py compare /private/tmp/control/perf.txt /private/tmp/candidate/perf.txt
```

The comparison reports matching windows, excluded windows, medians and paired percentage changes (negative is faster). It rejects duplicate frames, malformed timing rows and nonpositive timings. Compare only identical scenes/settings and intentionally selected stable windows; it does not establish that two logs came from the same workload. These are 30-frame averages, not p99 individual frame times or actual picture cadence. Manifest compatibility checks and automatic warm-up exclusion remain pending.

## Experiment log

9 November: [EE region experiment 1](ee-region-experiment-1.md) completed as an isolated probe. One million differential cases passed, including sanitizer validation. Whole-register locals increased ARM code from 46 to 95 instructions; narrowed locals restored 46 instructions, with no demonstrated hardware benefit. Candidate not promoted. Next compiler step is actual edge-frequency capture and a longer scalar region, followed by real-runtime validation.

9 November, later: [3DS speed research](3ds-speed-research-2026-11-09.md).
- Loops with no calls now keep guest registers in locals in the real runtime (LOOP_EDITS in the 3DS patch tool). They cover 19.5–24.7% of guest instructions, depending on the size limit.
- Cycle-exact on a native Mac build and picture-identical in Azahar. Console timing is pending.
- A local cycle budget is 0.5–1% faster on hardware but costs 2.8 MB of code, so it stays opt-in.
- The MVD memory probe (`mvdprobe 1`) is built but not run.
