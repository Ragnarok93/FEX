# FFVII Remake Intergrade / SD865 — FEX architectural plan (documentation only)

**Status:** task-specific planning branch (2026-10-11). No FEX runtime or JIT code was generated, modified or optimized here. No on-device speedup claim.

**Repository restriction:** `AGENTS.md` says: **“AI must not be used to generate code for contributions to this project.”** This file is documentation. FEX source contributions must be independently authored according to that restriction.

**Full cross-repo architecture:** https://github.com/Ragnarok93/GameNative/blob/feature/ff7r-sd865-architecture-20261011/docs/ff7r-sd865/ARCHITECTURE.md

**Base:** `feature/fex2610-sd865-armv82-benchmark-20261010` at `0ae689940a6f`. Reuse existing `docs/benchmarks/ff7r-sd865-fex2610-v82.md` rather than replacing benchmark infrastructure.

## 1. The controlled experiment already ready

- Pinned upstream source: FEX 2610 SHA `14c92681f4d62cf84d901460e0358de09c8847a7`.
- Control package: `o3-snapdragon865`, Cortex-A77 scheduled, compiler baseline ARMv8-A+CRC.
- Candidate package: `o3-snapdragon865-v82`, `-mtune=cortex-a77` and ARMv8.2-A+CRC compiler target applied to ARM64EC and WoW64 native C/C++ objects. **The original JIT is unchanged.**
- Existing scripts: `Scripts/GameNative/build_fexcore_2610_wcp.sh`, `package_fexcore_2610_wcp.sh`, `benchmark_fex2610_frametimes.py`. Existing unit/compile-command attestation and WCP validation must pass before device testing.

No build source SHA, compiler flags, Wine ABI or runtime behavior is changed in this document.

## 2. Game-specific performance envelope

Snapdragon 865 Adreno 650; GameNative Bionic Proton 10 ARM64EC; Mr. Purple T30; 720p output with effective dynamic scaling measured, not guessed; genuine target 30 FPS; VSync and LSFG off; FF7 engine and GPU driver fixed per A/B. The game may launch either D3D11/DXVK or D3D12/VKD3D, but FEX sits in the **CPU translation path in both**. Keep API and graphics translator constant for an FEX comparison.

Existing identical-run controls: `FEX_DISKCACHE=1`, `FEX_DISKCACHEMEMORYSIZE=33554432`; preserve TSO, multiblock, x87, L1/L2 cache and thread presets. Do not assume FEX runtime options from newer source releases exist in this pinned 2610 variant.

## 3. Profiling and safe optimization opportunities

| Step | Instrument | Hypothesis only; no result established |
| --- | --- | --- |
| A | Game/Render thread CPU time, GPU time, true present timing | Are the 15 FPS dips CPU translation-limited? |
| B | JIT translation vs execution, block/code-cache lookup and eviction, invalidations | Is repeated translation stealing frame budget? |
| C | Thread wakeups, syscalls, Wine thunk/exception overhead, lock wait | Is host/guest scheduling or synchronization dominant? |
| D | x86 atomics/TSO barriers and hot basic blocks | Can a *correctness-preserving* host/JIT optimization help? |
| E | cold vs warmed FEX disk cache, compiler workers | Are startup hitches mostly code-generation and cache misses? |
| F | CPU frequency/thermal and performance-core time | Is slow execution due to host downclock or poor scheduling? |

Avoid making per-game unsafe memory-order changes, disabling exception checks, hard CPU pinning, unbounded JIT caches, changing host feature detection without evidence, or turning on SVE/nonexistent ISA extensions. Preserve x86 guest semantics, Wine ARM64EC/WoW64 ABI, XInput, cutscenes and saves.

## 4. Measurement and decision

Three or more warmed 2–5 minute identical-scene runs per WCP in alternated order with reproducible save. Capture real `frame_time_ms` CSV or monotonically increasing `present_time_ns` from genuine frame/display observations; LSFG generated/queued timestamps are invalid for FEX comparisons. Compare with:

```bash
python3 Scripts/GameNative/benchmark_fex2610_frametimes.py --baseline control-warm.csv --candidate v82-warm.csv --warmup-seconds 15 --json-out ff7r-v82-comparison.json
```

Record average genuine FPS, 1% low, p95/p99, >50ms hitches, crash/visual/audio/input behavior, GPU timing and thermals. A faster GPU-bound run does not prove FEX improved. Advance only on repeatable CPU-limited improvements above run-to-run variation with no correctness, RAM or temperature regression.

## 5. Checklist

- [x] Create exclusive docs branch from existing validated experiment branch.
- [ ] Verify active API/Proton/FEX path in GameNative.
- [ ] Run FEX 2610 control and v82 candidate WCPs with unchanged graphics config and cache.
- [ ] Collect unbiased actual frame durations and compile provenance.
- [ ] Inspect JIT/translation/lock profiles only if CPU-bound.
- [ ] Document any proposed code change for independent non-AI implementation under repository policy.
- [ ] Test code and correctness on device before releasing separately named WCP; keep rollback.

No performance-changing implementation is authorized by this planning file.
