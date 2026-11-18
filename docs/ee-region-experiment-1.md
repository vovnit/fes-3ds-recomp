# EE region experiment 1 — 9 November 2026

Status: isolated compiler experiment completed; not integrated into the runtime. No console speedup demonstrated.

## Scope

The existing profile identifies function 0x004D59D0 as a frequently executed lobby function. Inspection shows arithmetic interleaved with DMA/hardware accesses. The experiment extracts a short region after its first 13-instruction charge point, spanning the branch into 0x004D5A08 and ending before the next guest-memory load. It preserves the branch delay-slot update, budget charges, two side exits and normal continuation at 0x004D5A24. This is a conservative multi-block register-retention probe, not yet a trace selected from measured edge frequencies.

The original translated unit remains unchanged. The generator reads the owner's local unit and emits private baseline/candidate C++ and a differential harness. Generated game-derived code is not committed. The harness models the region's entire observable state: all 32 128-bit general registers and the signed cycle budget. It does not model the complete CPU; this region accesses no other CPU fields or memory.

## Results

Compiled with the installed devkitpro/devkitarm:20260610 image, GCC -O2, armv6k, arm1136jf-s tuning, hard-float ABI and soft thread pointer, matching the project's target flags.

| Variant | Static ARM instructions in function body | Result |
| --- | ---: | --- |
| Baseline extracted region | 46 | Reference |
| Copy whole 128-bit registers into locals | 95 | Reject: stack spills and aggregate copies |
| Retain low 64 bits; write back only modified registers | 46 | No instruction-count reduction |

Static counts include all branch paths, exclude directives and literal pools, and are not executed instruction counts. The narrowed candidate has 23 memory/stack instructions versus 26 in the baseline, counting loads/stores, multiple transfers, VFP transfers and push/pop equally. This is not a cycle estimate.

Both candidates matched one million generated input cases on the host. The final candidate was also tested with AddressSanitizer and UndefinedBehaviorSanitizer, with no reported error. Inputs include arbitrary upper register halves, noncanonical low 64-bit values, forced zero/nonzero conditions, signed 32-bit overflow boundaries, and budgets from -50 to 50. Exit coverage: 250,000 / 500,000 / 250,000 cases. The comparison checks every register byte, budget and exit address.

Host microbenchmarks of the narrower candidate were approximately 2.5 ns/call, with small inconsistent differences from baseline. They use a compact test CPU rather than the real runtime layout and are not evidence of ARM11 or whole-game speed. Sanitized timings are not performance results.

## Decision

Do not integrate this candidate or install a new console build. This small isolated region does not meet the planned gate for expanding the optimization. The negative result does not disprove register retention across longer paths: GCC already optimizes much of this short arithmetic region, and the isolated function changes the surrounding register-allocation context.

Next experiment should collect actual branch-edge frequencies and select a longer frequently executed region. Use scalar liveness and writeback only at observable boundaries; avoid full Reg128 aggregate locals. Before integration, validate against actual runtime Cpu layout and compare game replays. Only matched hardware trials can establish the proposed whole-scene performance benefit.

## Reproduce locally

```sh
python3 tools/experiments/ee_region.py \
  ../ps2-web-recomp/generated/p3fes/unit_00F5.cpp /private/tmp/ee-region-new
clang++ -O2 -std=c++17 /private/tmp/ee-region-new/experiment.cpp \
  -o /private/tmp/ee-region-new/check
/private/tmp/ee-region-new/check
```

The output directory must be new. The manifest records source-unit and region hashes. The generator checks the expected control flow, register set, write set and absence of memory calls; it is specific to this experiment, not a general-purpose optimizer.

Private outputs from this session are under /private/tmp/fes-ee-region-01 (whole registers), /private/tmp/fes-ee-region-02 (narrowed variant and ARM assembly), and /private/tmp/fes-ee-region-03 (narrowed variant, provenance manifest and sanitized executable). These temporary paths may disappear.
