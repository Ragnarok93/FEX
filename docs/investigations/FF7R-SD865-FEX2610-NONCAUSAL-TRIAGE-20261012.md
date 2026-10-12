# FFVII Remake on SD865: FEXCore 2610 observation and boundaries

Date: 2026-10-12 UTC. Evidence routing: this file contains **only FEX-specific observations and missing-proof boundaries**. GameNative configuration work is in `Ragnarok93/GameNative`; D3D11/DXVK allocator traces are in `Ragnarok93/dxvk-SD865`; Turnip/KGSL investigation belongs in the user's Turnip fork once identified.

## Observed execution configuration

The user's GameNative settings screenshot showed:
- **FEXCore version:** `2610-GameNative-FF7R-Phase0-71b9...` (UI truncates full build identifier, so SHA/version suffix is **not verified**).
- **64-bit Emulator:** FEXCore. **32-bit Emulator:** FEXCore.
- **FEXCore preset:** `FF7R-Balanced`.
- **Box64 Version:** `0.3.7`, preset `Compatibility`, displayed in the UI but **Box64 was not selected** for 64-bit execution.
- Env vars in screenshot: `FEX_DISKCACHE=1`, `FEX_DISKCACHEFILEMAPPING=1`, `FEX_DISKCACHEPRUNESTALEENTRIES=1`, `FEX_FF7R_PHASE0=1`. Numeric `FEX_DISKCACHEMEMORYSIZE` and `FEX_DISKCACHEMAXFILESIZE` values are clipped in the original screenshot and must not be inferred.

## What crash evidence does and does not show

FFVII Remake's current capture establishes **Turnip returning `VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)` to DXVK's real `vkAllocateMemory` calls**, even after DXVK frees unused chunks and retries. The submitted logs consistently show device presentation and command processing preceding GPU allocator failure, including DXVK `DXVK-SD865-ALLOC stage=driver_retry` telemetry.

- This is **not presently evidence of an FEX JIT translation failure**, guest instruction exception, register mapping bug or FEX disk-cache failure. None of those has been isolated in the submitted captures.
- DXVK sometimes allocated 3.9–4.0 GiB at the failure, while Vulkan reported a larger heap/budget. This allocation volume is DXVK's tracking, **not** a measured FEX process Resident Set Size. Do not convert it into FEX RAM overhead claims.
- The older Android GameNative crash report (October 6) documents a JNI missing-symbol `UnsatisfiedLinkError`, **not** an FEX exception and not a stack for the current GPU OOM.
- FEX cache environment variables may affect CPU memory/time and cache storage independently, but there is **no controlled A/B** showing they caused the Turnip memory failure. Do not alter the successful FEX2610 preset to "fix" a known `vkAllocateMemory` error.

## FEX-specific next evidence, only if needed

If separate evidence of Android-wide memory pressure arises, collect **non-ADB** FEX/log-export diagnostic memory measurements (guest mappings vs host RSS, JIT code cache bytes, FEX disk cache actual allocation) at the same FF7R scene and compare with the 64/32-MiB DXVK pool and normal DXVK runs. Add instrumentation to this repo only if it resolves a FEX-specific unknown, never as a blind response to Turnip OOM.

Cross-links:
- [DXVK allocator experiment on S20+](https://github.com/Ragnarok93/dxvk-SD865/blob/feature/ff7r-sd865-architecture-20261011/docs/FF7R-SD865-POOL-EXPERIMENT-DEVICE-RESULT-20261012.md)
- [GameNative env/VRAM configuration analysis](https://github.com/Ragnarok93/GameNative/blob/research/ff7r-vram-config-20261012/docs/investigations/FF7R-SD865-VRAM-CONFIG-PROPAGATION-20261012.md)

**Status:** Context-only, no FEX patch or claim that FEX is defective.

## Turnip repository routing confirmed (2026-10-12)

User identified the designated Turnip fork as [`Ragnarok93/Banners-Turnip`](https://github.com/Ragnarok93/Banners-Turnip). The driver-specific investigation is now committed at [`docs/investigations/FF7R-SD865-KGSL-VULKAN-OOM-20261012.md`](https://github.com/Ragnarok93/Banners-Turnip/blob/research/ff7r-a650-kgsl-oom-20261012/docs/investigations/FF7R-SD865-KGSL-VULKAN-OOM-20261012.md), commit `a1f89412d024`.

**Provenance qualification:** Banners-Turnip's Standard A6xx driver is built from pinned/upstream Mesa, whereas the user's currently tested driver is Mr Purple T30 / PurpleVK; source/binary equivalence has **not** been confirmed. Turnip build, KGSL ioctl failures, driver errno and GPU virtual-memory questions belong to the linked driver report, not this subsystem's patch queue.
