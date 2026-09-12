# FEX 2609 bounded disk-cache prerelease plan

## Goal

Produce two GameNative FEXCore 2609 prerelease WCPs based on the existing MobileTune optimization profile, adding an internal writable JIT disk-cache capacity limit:

- `CacheConfig`: disk cache remains off by default, capacity defaults to 1024 MiB when enabled, and both settings remain runtime-configurable.
- `Cache1GB`: disk cache is on by default with a 1024 MiB capacity, while runtime configuration can still disable it or change the capacity.

Preserve the existing pre.8 O3/MobileTune artifacts as untouched controls.

## Design

Add a generated FEX configuration option `DiskCacheMaxSizeMB` (`int32`, `0` means unlimited) and make the existing `DiskCache` default build-configurable. Both are normal FEX configuration options, so JSON/environment layers can override their compiled defaults.

Limit only the writable per-application cache database. Read-only/precompiled cache databases are excluded from the budget. At store time, while holding both FOZ file locks, calculate total current writable bytes (`cache .foz + index _idx.foz`) plus the exact pending cache and index record sizes. If admitting the next entry would exceed the configured capacity, skip that cache write without affecting execution or existing cache reads.

Do not implement live eviction/compaction in this 2609 patch. The upstream index has a `last_access_time` field but does not maintain it, and the append-only FOZ/index format makes safe compaction substantially more invasive.

## Compatibility and tuning

Keep the established MobileTune profile for both candidates:

- Release / O3 / NDEBUG
- `-mtune=cortex-a76`
- `TUNE_ARCH=generic`
- `TUNE_CPU=none`
- LTO disabled because ARM64EC ThinLTO is currently broken in LLVM
- assertions, tests and ccache disabled in production DLL builds

No newer ARM ISA extensions are enabled by the tuning change, retaining broad compatibility including Snapdragon 865-class devices.

## Provenance

1. Create and verify the cache-cap runtime patch on top of upstream FEX-2609 commit `395b132f346b1a45def246d10c52245edba1ef02`.
2. Record the runtime-patch commit SHA.
3. Pin the prerelease builder to that exact runtime commit, so later workflow/package/docs commits cannot silently change FEXCore runtime source.
4. Release notes must identify both the upstream 2609 base and the runtime-patch commit.

## TDD and verification

1. RED: extend helper/workflow contract tests first so they require the new cache variants/configuration/capacity guard; push and capture a failing Actions run before implementation.
2. GREEN: implement a small overflow-safe capacity helper and compile-time static assertions covering unlimited, exact-fit, over-limit, already-over-limit and uint64 overflow-edge cases.
3. Integrate the helper into `IndexedDB::StoreCacheBlob`, with exact pending FOZ record accounting and safe lock release when admission is denied.
4. Verify generated config defaults and environment names (`FEX_DISKCACHE`, `FEX_DISKCACHEMAXSIZEMB`) in Actions.
5. Build ARM64EC and AArch64 DLLs for both candidates with pinned llvm-mingw, validate PE machine types, WCP layout and checksums.
6. Publish exactly two WCP assets plus `SHA256SUMS` as a prerelease and verify the release asset set and hashes.

## Candidate names

- `FEXCore-2609-GameNative-O3-MobileTune-CacheConfig.wcp`
- `FEXCore-2609-GameNative-O3-MobileTune-Cache1GB.wcp`
