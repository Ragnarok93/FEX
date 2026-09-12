# GameNative FEX 2609 Optimized WCP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and publish two reproducible FEXCore 2609 GameNative test WCPs (O3 baseline and O3+LTO) as a GitHub prerelease from the exact FEX-2609 source commit.

**Architecture:** Keep upstream FEX source semantics untouched. Add small packaging/build helpers plus one dedicated GitHub Actions workflow that checks out the pipeline branch, stages the exact FEX-2609 commit into a separate source worktree, builds ARM64EC and AArch64 DLLs with a pinned llvm-mingw toolchain, validates each WCP, and publishes both candidates together as a prerelease.

**Tech Stack:** Bash, CMake/Ninja, FEX MinGW toolchain file, bylaws/llvm-mingw 20250920, LLVM binutils, jq, tar/xz, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-11-fex-2609-gamenative-optimized-wcp-design.md`

## Global Constraints

- Source commit must be exactly `395b132f346b1a45def246d10c52245edba1ef02` (FEX-2609).
- Build both `arm64ec-w64-mingw32` and `aarch64-w64-mingw32`.
- Use `CMAKE_BUILD_TYPE=Release`, `ENABLE_ASSERTIONS=False`, `BUILD_TESTING=False`, `ENABLE_JEMALLOC_GLIBC_ALLOC=False`, `TUNE_ARCH=generic`, and `TUNE_CPU=generic`.
- Never use `-march=native`, `-mcpu=native`, or runner-specific CPU tuning.
- O3 and O3+LTO candidates must differ only in `ENABLE_LTO` and package identity metadata.
- PGO must not be advertised or shipped without real GameNative workload profiles.
- Publish only after package and binary validation pass.

---

### Task 1: Add deterministic WCP packager and fixture test

**Files:**
- Create: `Scripts/GameNative/package_fexcore_wcp.sh`
- Create: `.github/scripts/test-package-fexcore-wcp.sh`

**Interfaces:**
- Consumes: two existing FEX DLL paths, variant (`o3` or `o3-lto`), output directory, version code.
- Produces: `FEXCore-2609-GameNative-O3.wcp` or `FEXCore-2609-GameNative-O3-LTO.wcp` plus a valid `profile.json` inside the archive.

- [ ] **Step 1: Add a fixture test that fails until the packager exists**

The test creates two fake DLL files, calls the packager, extracts the WCP, and asserts:

```bash
jq -e '.type == "FEXCore"' "$extract/profile.json"
jq -e '.files | length == 2' "$extract/profile.json"
test -f "$extract/system32/libarm64ecfex.dll"
test -f "$extract/system32/libwow64fex.dll"
```

It also checks that the variant maps to the exact expected filename and `versionName`.

- [ ] **Step 2: Implement the packager**

The script must use `set -euo pipefail`, validate all inputs, create a clean temporary staging directory, copy the two DLLs to `system32/`, generate JSON with `jq -n`, and create the WCP with:

```bash
tar -C "$stage" -cJf "$output/$filename" .
```

Variant mapping:

```text
o3     -> FEXCore-2609-GameNative-O3.wcp / 2609-GameNative-O3
o3-lto -> FEXCore-2609-GameNative-O3-LTO.wcp / 2609-GameNative-O3-LTO
```

- [ ] **Step 3: Run the fixture test**

Run:

```bash
bash .github/scripts/test-package-fexcore-wcp.sh
```

Expected: PASS for both variants.

- [ ] **Step 4: Commit**

```bash
git add Scripts/GameNative/package_fexcore_wcp.sh .github/scripts/test-package-fexcore-wcp.sh
git commit -m "build: add deterministic GameNative FEXCore WCP packager"
```

### Task 2: Add reproducible FEX 2609 build helper

**Files:**
- Create: `Scripts/GameNative/build_fexcore_2609_wcp.sh`

**Interfaces:**
- Consumes: staged source directory at the exact FEX-2609 commit, variant (`o3` or `o3-lto`), output directory, version code, `LLVM_MINGW_ROOT`.
- Produces: one verified candidate WCP by building both Windows targets and delegating packaging to `package_fexcore_wcp.sh`.

- [ ] **Step 1: Add argument and source-identity validation**

Require:

```bash
EXPECTED_SOURCE_SHA=395b132f346b1a45def246d10c52245edba1ef02
ACTUAL_SOURCE_SHA=$(git -C "$source_dir" rev-parse HEAD)
test "$ACTUAL_SOURCE_SHA" = "$EXPECTED_SOURCE_SHA"
```

Reject unknown variants.

- [ ] **Step 2: Map the only optimization difference**

```bash
case "$variant" in
  o3)     enable_lto=False ;;
  o3-lto) enable_lto=True ;;
  *) exit 2 ;;
esac
```

Do not add custom `-Ofast`, fast-math, native CPU, exception, or RTTI flags.

- [ ] **Step 3: Build ARM64EC and AArch64 DLLs**

For each target use a clean build directory and these common flags:

```bash
cmake -G Ninja -Wno-dev \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$source_dir/Data/CMake/toolchain_mingw.cmake" \
  -DMINGW_TRIPLE="${triple}-w64-mingw32" \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_LIBDIR=/usr/lib/wine/aarch64-windows \
  -DENABLE_JEMALLOC_GLIBC_ALLOC=False \
  -DENABLE_LTO="$enable_lto" \
  -DENABLE_ASSERTIONS=False \
  -DBUILD_TESTING=False \
  -DTUNE_ARCH=generic \
  -DTUNE_CPU=generic \
  -DOVERRIDE_VERSION=2609 \
  -DOVERRIDE_HASH=395b132f346b1a45def246d10c52245edba1ef02 \
  "$source_dir"
```

Build with `cmake --build ... --parallel 2`.

- [ ] **Step 4: Validate and strip the DLLs**

Require exact outputs:

```text
<arm64ec-build>/Bin/libarm64ecfex.dll
<aarch64-build>/Bin/libwow64fex.dll
```

Run LLVM `file`/`objdump` inspection and then `llvm-strip --strip-all` on copies used for packaging.

- [ ] **Step 5: Package through Task 1 interface**

Call:

```bash
Scripts/GameNative/package_fexcore_wcp.sh \
  "$variant" "$ec_dll" "$wow_dll" "$output_dir" "$version_code"
```

- [ ] **Step 6: Shell syntax validation**

Run:

```bash
bash -n Scripts/GameNative/build_fexcore_2609_wcp.sh
bash -n Scripts/GameNative/package_fexcore_wcp.sh
```

Expected: no output, exit 0.

- [ ] **Step 7: Commit**

```bash
git add Scripts/GameNative/build_fexcore_2609_wcp.sh
git commit -m "build: add reproducible FEX 2609 GameNative builder"
```

### Task 3: Add prerelease workflow and package verification

**Files:**
- Create: `.github/workflows/gamenative-fex-2609-prerelease.yml`

**Interfaces:**
- Consumes: build helpers from Tasks 1-2.
- Produces: two Actions artifacts and one GitHub prerelease containing both WCPs plus `SHA256SUMS`.

- [ ] **Step 1: Configure trigger and permissions**

Use:

```yaml
on:
  workflow_dispatch:
  push:
    branches:
      - gamenative/fex-2609-optimized
    paths:
      - '.github/workflows/gamenative-fex-2609-prerelease.yml'
      - '.github/scripts/test-package-fexcore-wcp.sh'
      - 'Scripts/GameNative/**'

permissions:
  contents: write
```

The branch push path exists so this session can produce the requested prerelease without a separate workflow-dispatch API.

- [ ] **Step 2: Add a two-entry build matrix**

```yaml
matrix:
  variant: [o3, o3-lto]
```

Each matrix job checks out the pipeline branch, runs the fixture test, installs dependencies, installs pinned `bylaws/llvm-mingw` `20250920`, and installs pinned glslang `16.6.0` release tooling.

- [ ] **Step 3: Stage exact FEX-2609 source**

Use the existing repository history rather than building pipeline HEAD:

```bash
git worktree add --detach "$RUNNER_TEMP/fex-2609-source" 395b132f346b1a45def246d10c52245edba1ef02
git -C "$RUNNER_TEMP/fex-2609-source" submodule update --init --recursive
```

Assert `git rev-parse HEAD` equals the expected SHA before building.

- [ ] **Step 4: Build each candidate**

Call Task 2 with a unique output directory and `${{ github.run_number }}` as the version code.

- [ ] **Step 5: Verify each WCP**

For each WCP:

```bash
tar -tJf "$wcp"
mkdir extracted && tar -xJf "$wcp" -C extracted
jq -e '.type == "FEXCore" and (.files | length == 2)' extracted/profile.json
test -f extracted/system32/libarm64ecfex.dll
test -f extracted/system32/libwow64fex.dll
sha256sum "$wcp"
```

Upload the WCP and candidate checksum as an Actions artifact.

- [ ] **Step 6: Create combined prerelease**

The release job downloads both artifacts, verifies exactly two WCPs are present, creates sorted `SHA256SUMS`, and publishes with `softprops/action-gh-release@v2`:

```yaml
prerelease: true
make_latest: false
```

Tag format:

```text
fexcore-2609-gamenative-395b132-pre.<run_number>
```

Release notes must state the source SHA, toolchain tag, candidate optimization difference, and testing-only status.

- [ ] **Step 7: Commit**

```bash
git add .github/workflows/gamenative-fex-2609-prerelease.yml
git commit -m "ci: publish optimized FEX 2609 GameNative prereleases"
```

### Task 4: Verify the live Actions run and prerelease

**Files:**
- No source files unless CI exposes a defect.

**Interfaces:**
- Consumes: workflow run produced by Task 3 push.
- Produces: a successful prerelease with usable test WCPs.

- [ ] **Step 1: Inspect workflow jobs**

Confirm both matrix builds and release job complete successfully.

- [ ] **Step 2: Inspect failures if any**

If a build fails, read the failing job log, make the smallest correction on the same branch, and let the push trigger a new run.

- [ ] **Step 3: Inspect release assets**

Confirm release contains exactly:

```text
FEXCore-2609-GameNative-O3.wcp
FEXCore-2609-GameNative-O3-LTO.wcp
SHA256SUMS
```

- [ ] **Step 4: Record final SHA and release URL in the completion report**

Report the branch, implementation commit, successful workflow run, prerelease tag, source SHA, and asset checksums.
