# GameNative FEX 2609 Optimized WCP Design

## Goal

Produce reproducible GameNative-focused FEXCore 2609 Windows compatibility packages (`.wcp`) directly from the `FEX-2609` source commit and publish test candidates as GitHub prereleases from the FEX fork.

The source baseline is the exact upstream FEX-2609 commit:

`395b132f346b1a45def246d10c52245edba1ef02`

No post-2609 source changes are included in the binaries unless explicitly documented in a later experimental branch.

## Scope

This work adds a dedicated GitHub Actions pipeline and packaging metadata. It does not modify FEXCore runtime semantics, JIT behavior, host feature detection, or GameNative itself.

The first prerelease contains two build candidates from identical source and toolchain inputs:

1. `FEXCore-2609-GameNative-O3.wcp`
   - Release build (`-O3 -DNDEBUG` through CMake Release mode)
   - assertions off
   - tests off
   - generic AArch64 tuning
   - LTO off
   - intended as the controlled GameNative baseline

2. `FEXCore-2609-GameNative-O3-LTO.wcp`
   - same inputs as the O3 candidate
   - CMake interprocedural optimization enabled through `ENABLE_LTO=True`
   - intended as the primary optimized candidate

PGO is intentionally not fabricated in CI. A real PGO build requires representative GameNative workload profiles collected on target devices. The workflow may be extended later to consume validated profile data, but this prerelease must not label an untrained binary as PGO-optimized.

## Toolchain and build environment

The workflow runs on a GitHub-hosted Ubuntu x86_64 runner and installs a pinned `bylaws/llvm-mingw` release rather than depending on a mutable self-hosted runner environment.

Pinned toolchain:

- `bylaws/llvm-mingw` tag `20250920`
- UCRT Ubuntu x86_64 archive

Both required MinGW targets are built:

- `arm64ec-w64-mingw32` -> `libarm64ecfex.dll`
- `aarch64-w64-mingw32` -> `libwow64fex.dll`

Common CMake policy:

- `CMAKE_BUILD_TYPE=Release`
- `ENABLE_ASSERTIONS=False`
- `BUILD_TESTING=False`
- `ENABLE_JEMALLOC_GLIBC_ALLOC=False`
- `TUNE_ARCH=generic`
- `TUNE_CPU=generic`
- `OVERRIDE_VERSION=2609-GameNative`

The workflow must never use `-march=native`, `-mcpu=native`, or runner-specific CPU tuning.

## WCP layout

Each package contains:

```text
profile.json
system32/
  libarm64ecfex.dll
  libwow64fex.dll
```

The archive format is `tar.xz` with the `.wcp` extension, matching the component format GameNative-compatible WCP packages already use.

`profile.json` declares type `FEXCore` and installs both DLLs into `${system32}`. Candidate identity is encoded in the version/description so test devices can distinguish O3 from O3+LTO packages.

## Verification

Before a candidate is published, CI must verify:

- the checked-out commit exactly matches the pinned FEX-2609 commit
- both expected DLLs exist for each candidate
- each DLL is a PE/COFF ARM64-family binary according to LLVM tooling
- neither package accidentally contains build directories or unrelated artifacts
- `profile.json` is valid JSON and references files that exist in the package
- the `.wcp` can be unpacked successfully
- SHA-256 hashes are generated for every `.wcp`

The release job runs only after both candidate builds and package verification succeed.

## Release policy

The workflow is manually dispatchable and may also run automatically on changes to its own branch for validation. Publishing is explicit and creates a prerelease rather than a stable release.

Release naming:

- tag: `fexcore-2609-gamenative-<short-sha>-pre`
- title: `FEXCore 2609 GameNative optimized test builds`

Release assets:

- `FEXCore-2609-GameNative-O3.wcp`
- `FEXCore-2609-GameNative-O3-LTO.wcp`
- `SHA256SUMS`

Release notes identify the exact FEX commit, pinned toolchain, optimization difference between candidates, and warn that these are testing builds.

## Testing intent

The two packages are deliberately identical except for LTO. This makes on-device A/B results attributable to LTO rather than a bundle of unrelated compiler changes. Testing should compare game startup, shader/JIT-heavy transitions, CPU-heavy scenes, frametime consistency, compatibility, and crash behavior before any additional tuning is introduced.
