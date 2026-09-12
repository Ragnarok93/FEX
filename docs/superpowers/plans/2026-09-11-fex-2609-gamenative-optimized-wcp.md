# GameNative FEX 2609 Optimized WCP Implementation Plan

> **Status:** Completed. This record reflects the implementation that produced prerelease `fexcore-2609-gamenative-395b132-pre.8`.

**Goal:** Build and publish two reproducible FEXCore 2609 GameNative test WCPs from the exact upstream FEX-2609 source commit.

**Architecture:** Keep FEX runtime/JIT source semantics untouched. Use small Bash build/package helpers plus a dedicated GitHub Actions workflow. Stage the exact FEX-2609 commit, build ARM64EC and AArch64/WoW64 DLLs with a pinned llvm-mingw toolchain, validate each package, and publish both verified candidates together as a prerelease.

**Tech Stack:** Bash, CMake/Ninja, FEX MinGW toolchain file, bylaws/llvm-mingw 20250920, LLVM binutils, jq, tar/xz, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-11-fex-2609-gamenative-optimized-wcp-design.md`

## Global Constraints

- Source commit is exactly `395b132f346b1a45def246d10c52245edba1ef02`.
- Build both `arm64ec-w64-mingw32` and `aarch64-w64-mingw32`.
- Use Release/O3, assertions off, tests off, and LTO off for released candidates.
- Preserve the conservative `armv8-a+crc` baseline.
- Never use `-march=native` or `-mcpu=native`.
- MobileTune may alter scheduling only; it must not require a newer ISA.
- PGO must not be advertised without real GameNative workload profiles.
- Publish only after binary, package, artifact, and checksum verification pass.

---

### Task 1: Deterministic WCP packaging

**Files:**
- `Scripts/GameNative/package_fexcore_wcp.sh`
- `.github/scripts/test-package-fexcore-wcp.sh`

- [x] Add a fixture test that creates fake ARM64EC/WoW64 DLLs and verifies package layout, metadata, mappings, and payload identity.
- [x] Verify RED by requiring `o3-mobile` before the packager implemented it.
- [x] Implement candidate mapping:

```text
o3        -> FEXCore-2609-GameNative-O3.wcp
o3-mobile -> FEXCore-2609-GameNative-O3-MobileTune.wcp
```

- [x] Package exactly `profile.json`, `system32/libarm64ecfex.dll`, and `system32/libwow64fex.dll` as tar.xz `.wcp` archives.
- [x] Re-run contract tests to GREEN.

### Task 2: Reproducible FEX 2609 builder

**Files:**
- `Scripts/GameNative/build_fexcore_2609_wcp.sh`
- `.github/scripts/test-build-fexcore-2609-wcp.sh`

- [x] Reject any source checkout whose `HEAD` is not `395b132f346b1a45def246d10c52245edba1ef02`.
- [x] Build both MinGW targets in clean build directories.
- [x] O3 control uses `TUNE_ARCH=generic`, `TUNE_CPU=generic`, `ENABLE_LTO=False`.
- [x] MobileTune uses `TUNE_ARCH=generic`, `TUNE_CPU=none`, `ENABLE_LTO=False`, and exact Release flags:

```text
-O3 -DNDEBUG -mtune=cortex-a76
```

- [x] Assert the MobileTune CMake cache contains those C and C++ release flags before compiling.
- [x] Strip package copies of both DLLs.
- [x] Validate ARM64EC and AArch64 PE machine types with LLVM tooling.

### Task 3: Evaluate LTO safely

- [x] Build an O3+LTO experiment with `ENABLE_LTO=True`.
- [x] Observe successful configuration/compilation followed by ARM64EC final-link failure from unresolved EC-side libc++/Win32 symbols.
- [x] Confirm this matches the current upstream LLVM ARM64EC LTO limitation (`llvm/llvm-project#168469`, tracking issue `#179532`).
- [x] Reject unsafe linker workarounds.
- [x] Remove LTO from the release matrix and replace it with the link-safe MobileTune experiment.

### Task 4: GitHub Actions prerelease pipeline

**File:**
- `.github/workflows/gamenative-fex-2609-prerelease.yml`

- [x] Pin `bylaws/llvm-mingw` `20250920` and verify archive SHA-256 `8dd8c34fc051a50c2fae86015f35057f8aae93fe1e19b34537ef1269a8b4c772`.
- [x] Remove the inherited glslang setup after confirming FEX 2609 does not consume glslang in this MinGW build path.
- [x] Matrix-build `o3` and `o3-mobile` on Ubuntu 24.04.
- [x] Stage the exact 2609 source using a detached worktree and recursively initialize pinned submodules.
- [x] Run helper contract tests before compiler work.
- [x] Verify each WCP unpacks, has valid FEXCore metadata, contains exactly three files, and has both non-empty DLLs.
- [x] Generate per-candidate SHA-256 files and upload both Actions artifacts.
- [x] Re-download both artifacts in the release job and re-run their SHA-256 checks before publication.
- [x] Generate combined `SHA256SUMS`.
- [x] Publish only after both build jobs are green.

### Task 5: Live verification

Successful workflow run:

```text
run:       34674283999
run number: 8
branch:    gamenative/fex-2609-optimized
head:      4f41a7aedf7cba0f0480e4f1c21ac5fefbc05f06
```

- [x] `Build o3` succeeded.
- [x] `Build o3-mobile` succeeded.
- [x] Both WCP verification steps succeeded.
- [x] Both Actions artifact uploads succeeded.
- [x] Release-set checksum verification succeeded.
- [x] GitHub prerelease publication succeeded.

Published tag:

`fexcore-2609-gamenative-395b132-pre.8`

Published WCP checksums:

```text
e0bafa70aed144620eb8cc9fb81f7e75cdfa7f1679379b4cc2986653a113f0cf  FEXCore-2609-GameNative-O3.wcp
e5a8fca9492066cb3abd7a6283174746844d8fc67953028200551d0c227e88b2  FEXCore-2609-GameNative-O3-MobileTune.wcp
```

### Follow-up: PGO

PGO remains intentionally out of scope for this prerelease. A future PGO phase should collect representative GameNative profiles from real 32-bit and 64-bit games across startup, JIT-heavy transitions, CPU-heavy scenes, and sustained gameplay before producing any `-fprofile-use` candidate.
