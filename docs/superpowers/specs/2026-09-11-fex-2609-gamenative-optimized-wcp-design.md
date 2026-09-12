# GameNative FEX 2609 Optimized WCP Design

## Goal

Produce reproducible GameNative-focused FEXCore 2609 Windows compatibility packages (`.wcp`) from the exact upstream FEX-2609 source commit and publish controlled test candidates as GitHub prereleases from this fork.

Pinned source:

`395b132f346b1a45def246d10c52245edba1ef02`

No post-2609 FEX runtime/JIT source changes are included in these binaries.

## Final candidate set

The prerelease contains two candidates built from identical FEX source and the same pinned compiler toolchain.

### O3 control

`FEXCore-2609-GameNative-O3.wcp`

- CMake `Release` / O3
- assertions off
- tests off
- LTO off
- `TUNE_ARCH=generic`
- `TUNE_CPU=generic`
- conservative ARM64 ISA baseline
- intended as the control build

### O3 MobileTune

`FEXCore-2609-GameNative-O3-MobileTune.wcp`

- same source and common release configuration as the control
- LTO off
- `TUNE_ARCH=generic`
- `TUNE_CPU=none`
- `CMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -mtune=cortex-a76`
- `CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -mtune=cortex-a76`
- retains the generic `armv8-a+crc` ISA baseline; the added `-mtune` changes scheduling rather than enabling additional ISA extensions
- intended as the primary GameNative mobile scheduling experiment

## Why LTO is not shipped

An O3+LTO candidate was implemented and exercised in CI. FEX configured and compiled successfully, but ARM64EC ThinLTO failed during the final `libFEXCore.dll` link with unresolved EC-side libc++/Win32 symbols. This matches the current upstream LLVM ARM64EC LTO limitation tracked in `llvm/llvm-project#168469` and ARM64EC tracking issue `#179532`.

The pipeline therefore does not use linker workarounds or claim an LTO optimization that cannot be produced reliably with the pinned toolchain.

PGO is also intentionally not claimed. A valid PGO build requires representative GameNative workload profiles collected from real gaming runs; that is a separate future experiment.

## Toolchain and build environment

The workflow runs on GitHub-hosted Ubuntu 24.04 x86_64 and installs a checksum-verified compiler rather than depending on mutable runner state.

Pinned toolchain:

- `bylaws/llvm-mingw` tag `20250920`
- clang 21.1.0
- archive SHA-256 `8dd8c34fc051a50c2fae86015f35057f8aae93fe1e19b34537ef1269a8b4c772`

Both required targets are built for each candidate:

- `arm64ec-w64-mingw32` -> `libarm64ecfex.dll`
- `aarch64-w64-mingw32` -> `libwow64fex.dll`

The workflow never uses `-march=native` or `-mcpu=native`.

## WCP layout

Each package contains only:

```text
profile.json
system32/
  libarm64ecfex.dll
  libwow64fex.dll
```

The archive is tar.xz with a `.wcp` extension. `profile.json` declares type `FEXCore`, provides a distinct candidate version name, and maps both DLLs into `${system32}`.

## Verification gates

Before publication, CI verifies:

- the source checkout is exactly `395b132f346b1a45def246d10c52245edba1ef02`
- the pinned llvm-mingw archive checksum
- packaging contract tests and builder guard tests
- mobile-tune CMake cache contains the exact `-mtune=cortex-a76` release flags
- both ARM64EC and AArch64/WoW64 builds complete
- expected DLLs exist and are non-empty
- PE machine types match the intended targets
- WCPs unpack successfully
- `profile.json` is valid and contains exactly the two expected mappings
- each WCP contains exactly three files
- per-candidate SHA-256 checks pass after artifact transfer
- the release contains exactly both WCP candidates plus combined `SHA256SUMS`

## Release policy

The workflow publishes prereleases only after both matrix candidates pass all build and package verification gates.

Assets:

- `FEXCore-2609-GameNative-O3.wcp`
- `FEXCore-2609-GameNative-O3-MobileTune.wcp`
- `SHA256SUMS`

The initial verified release is tagged `fexcore-2609-gamenative-395b132-pre.8`.

## Testing intent

Use the O3 package as the control and MobileTune as the experiment. Compare startup, JIT-heavy transitions, CPU-heavy scenes, average FPS, 1%/0.1% lows, frametime variance, thermal behavior, compatibility, and crashes. The MobileTune candidate should only replace the control if real on-device measurements show a repeatable benefit without regressions.
