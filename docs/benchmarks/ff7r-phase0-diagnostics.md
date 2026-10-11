# FF7 Remake Phase 0 — FEX startup diagnostics and source provenance

**Target:** GameNative FFVII Remake Intergrade, FEX 2610 ARM64EC on Snapdragon 865/Adreno 650. Separate branch `feature/ff7r-sd865-architecture-20261011`.

**Code changes:** `Source/Windows/ARM64EC/Module.cpp` and `Source/Windows/WOW64/Module.cpp` each emit a single opt-in startup marker after successful process initialization, conditional on `FEX_FF7R_PHASE0=1`. Markers include translator backend, executable *basename*, process id and whether the runtime detected Wine. No per-block JIT or per-frame logging, no CPU/TSO/exception behavior changes. For the opt-in marker, the process must also allow FEX's logging handler (`FEX_SILENTLOG` must not silence it). FEX's `Source/Windows/Common/Logging.cpp` forwards through Wine `__wine_dbg_output` where available, else to `%LOCALAPPDATA%\\fex-<pid>.log`.

**Upstream/code references:** [FEX-Emu/FEX](https://github.com/FEX-Emu/FEX) FEX 2610 translator startup/configuration patterns; existing `FEXCore::Profiler`, `FEX_CONFIG_OPT(ProfileStats, PROFILESTATS)` and shared-memory stats can be researched in a later profiling phase. Do not incorrectly treat application-specific Wine startup markers as proof of FEX frame cost. Existing [FEX 2610 ARMv8.2 benchmark](ff7r-sd865-fex2610-v82.md) remains a separate, pinned control experiment.

**Packaging safety:** Existing `Scripts/GameNative/build_fexcore_2610_wcp.sh` still defaults to pinned `14c92681f4d62cf84d901460e0358de09c8847a7`, used by existing prereleases. Only when a full commit SHA is supplied explicitly as `FEX_PHASE0_SOURCE_SHA_OVERRIDE=<40-char-sha>` will it accept the modified source. The new `.github/workflows/ff7r-phase0-fex-wcp.yml` stages its exact workflow commit and passes that SHA; it verifies ARM64EC and WoW64 PE DLLs, repacks a uniquely named `FEXCore-2610-FF7R-Phase0-<sha>.wcp` profile, calculates SHA256 and uploads an Action artifact. The Phase-0 workflow does **not** publish a stable release or replace the existing WCP.

## Device test

1. Download an artifact from a successful **FF7R Phase-0 FEX 2610 SD865 diagnostic WCP** GitHub Actions run. This build has not been proven to pass CI or run on the S20+ yet.
2. Save the existing control FEX `.wcp` and container settings. Import the diagnostic profile through GameNative Contents Manager; select only it for the FF7 Remake game/container.
3. In the game's Environment tab, set `FEX_FF7R_PHASE0=1`, and ensure `FEX_SILENTLOG` is not on. For joint DXVK attribution, also use `DXVK_FF7R_PHASE0=1`, `GN_DXVK_DIAGNOSTICS=1`, and `DXVK_LOG_LEVEL=info` with the companion diagnostic DXVK package.
4. One clean launch; verify marker `FEX-FF7R-PHASE0: backend=ARM64EC app=FF7Remake_.exe pid=... wine=1` in Wine debug output or the per-process `fex-<pid>.log` (logging/capture availability is runtime-dependent). A WoW64 marker is for an x86 process, not proof the main game used WoW64.
5. Save logs from exactly one run; classify with the companion [DXVK/FEX evidence parser](https://github.com/Ragnarok93/dxvk-SD865/blob/feature/ff7r-sd865-architecture-20261011/tools/ff7r_phase0_report.py).
6. Turn off Phase-0 markers/verbose logging, revert to prior WCP for all performance benchmarks. No FPS gain is claimed from this instrumentation.

**Exit gate:** build/packaging correctness, on-device startup and executable evidence, no startup/gameplay regression, matching DXVK DXGI evidence or an explicit unverified verdict. FEX hot-path optimizations come only after actual CPU translation profiles.

**Related:** [DXVK Phase-0 instructions](https://github.com/Ragnarok93/dxvk-SD865/blob/feature/ff7r-sd865-architecture-20261011/docs/FF7R-PHASE0-DEVICE-VALIDATION.md).
