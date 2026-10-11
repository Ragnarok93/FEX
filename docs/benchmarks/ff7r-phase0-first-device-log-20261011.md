# First FFVII Remake Phase 0 log — FEX 2610 evidence (2026-10-11)

The first user-supplied `wine_debug.log.txt` (938 lines) establishes that FFVII Remake entered Wine's **experimental ARM64EC** path and loaded `C:\\windows\\system32\\libarm64ecfex.dll` in the game processes. It does **not** establish the version/commit of the loaded FEX DLL or a JIT/CPU performance characteristic.

Observed positions (one-based): `0150:err:environ:init_peb starting ...ff7remake_.exe in experimental ARM64EC mode` (342), its `libarm64ecfex.dll` load (352); second game startup (435) and FEX load (437). `libwow64fex.dll` is **not observed** in the capture.

The logs show **no** `FEX-FF7R-PHASE0` startup markers; therefore it is not verified that our opt-in instrumented FEX WCP is installed, selected, or configured to log. Absence may arise from uninstrumented DLLs, disabled `FEX_FF7R_PHASE0`, silent log handling, or missing capture. Do not assert a particular cause.

The active DXVK 3.1.1 fails before game rendering due to wrapper-facing Vulkan `storageBuffer8BitAccess=false` (`ff7remake__dxgi.log`). This is a **DXVK/driver-wrapper initialization blocker**, not established to be caused by FEX.

Other Wine messages: `esync: up and running`, several `fixme:fsync:do_fsync futexes not supported on this platform`, and repeated `fixme:file:NtUnlockFile Unimplemented yet parameter`. They are potential later synchronization/I/O profiling leads, not proof of FPS loss. Shader cache size and JIT tuning cannot fix a Vulkan mandatory-feature rejection.

Before tuning FEX: use a verified instrumented FEX build (or independently confirm its exact SHA/loaded DLL), enable `FEX_FF7R_PHASE0=1` with FEX logging, restore working DXVK 2.7.1, and capture one successful gameplay run with LSFG disabled. Profile genuine game-frame production and CPU JIT time only after renderer acceptance.


## 2026-10-11 resolution: why FEX is not this startup blocker

Comparison against Mesa/Turnip source shows `storageBuffer8BitAccess` depends on
`storage_8bit` in the GPU feature profile. The A650 definition uses A6xx
generation-3 and does not advertise that feature in the inspected Mesa source.
See the DXVK research branch:
https://github.com/Ragnarok93/dxvk-SD865/blob/feature/ff7r-sd865-architecture-20261011/docs/FF7R-SD865-8BIT-ROOT-CAUSE.md

Do not attempt to fix this DXVK 3.1.1 **Vulkan adapter** rejection by tuning
FEX ISA, JIT cache size, threads, TSOs, Wine debug channel settings, or D3D
feature-level overrides. Those are outside the rejected GPU capability path.
The exact driver binary must still be queried to distinguish hardware
implementation limitations from a wrapper-reporting defect. The intended
FFVII Remake performance track remains known-working DXVK 2.7.1 plus
controlled FEX 2610 A/B tests after a successful gameplay launch.
