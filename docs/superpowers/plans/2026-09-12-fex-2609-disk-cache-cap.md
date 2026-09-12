# FEX 2609 bounded disk-cache prerelease plan

## Status

Completed and published from GitHub Actions run `34717331076` (run number 13).

Prerelease tag: `fexcore-2609-gamenative-cache-395b132-pre.13`

## Goal

Produce two GameNative FEXCore 2609 prerelease WCPs based on the existing MobileTune optimization profile, adding an internal writable JIT disk-cache capacity limit:

- `CacheConfig`: disk cache remains off by default, capacity defaults to `0` (unlimited) until explicitly configured, and both settings remain runtime-configurable.
- `Cache1GB`: disk cache is on by default with a 1024 MiB capacity, while runtime configuration can still disable it or change the capacity.

The existing pre.8 O3/MobileTune artifacts remain untouched controls.

## Design

A deterministic build-time patch is applied to an exact detached checkout of upstream FEX-2609 commit `395b132f346b1a45def246d10c52245edba1ef02`. The pipeline aborts if that source SHA does not match.

The patch adds a generated FEX configuration option `DiskCacheMaxSizeMB` (`uint32`, `0` means unlimited) and makes the existing `DiskCache` default build-configurable. Both remain ordinary FEX configuration options, so JSON/environment layers can override their compiled defaults:

- `FEX_DISKCACHE`
- `FEX_DISKCACHEMAXSIZEMB`

The limit is enforced against the active writable FOZ cache database plus its writable index. Read-only/precompiled cache databases are excluded from the budget. At store time, while holding both FOZ file locks, FEX calculates the current writable database and index sizes plus the exact pending cache and index record sizes. If admitting the next entry would exceed the configured capacity, the write is skipped without affecting execution or existing cache reads.

The capacity helper uses subtract-as-you-go accounting rather than summing sizes, avoiding uint64 overflow in the admission check. Invalid/overflowing pending-size calculations are rejected safely.

No live eviction or compaction is implemented in this 2609 patch. The upstream index has a `last_access_time` field but does not maintain it, and the append-only FOZ/index format makes safe compaction substantially more invasive.

### Capacity scope

The cap is a hard admission limit for the **active writable cache bucket** (`RWCacheDB.foz` + `RWCacheDB_idx.foz`). FEX can create separate historical bucket directories when its cache key changes (for example because of host features, guest mode, or code-generation-affecting configuration). This patch does not scan, purge, compact, or globally budget those old bucket directories. Therefore `Cache1GB` is not a global 1 GiB ceiling across all historical FEX cache buckets in a prefix.

A future global-budget implementation should handle historical-bucket cleanup separately rather than adding repeated directory scans to the JIT store hot path.

## Compatibility and tuning

Both candidates retain the established MobileTune profile:

- Release / O3 / NDEBUG
- `-mtune=cortex-a76`
- `TUNE_ARCH=generic`
- `TUNE_CPU=none`
- LTO disabled because ARM64EC ThinLTO is currently broken in LLVM
- assertions, tests and ccache disabled in production DLL builds
- no PGO claim because no representative GameNative workload profile is baked into these binaries

No newer ARM ISA extensions are enabled by the scheduling tune, retaining broad compatibility including Snapdragon 865-class devices.

Toolchain is pinned to `bylaws/llvm-mingw` tag `20250920` with archive SHA-256 `8dd8c34fc051a50c2fae86015f35057f8aae93fe1e19b34537ef1269a8b4c772`. Actions confirmed clang 21.1.0.

## Provenance

1. Exact upstream FEX-2609 source: `395b132f346b1a45def246d10c52245edba1ef02`.
2. The branch carries a deterministic patcher at `Scripts/GameNative/apply_fexcore_2609_disk_cache_cap.py`; runtime source is patched only after the exact upstream SHA guard succeeds.
3. Production pipeline/release commit: `98cf820836ca5f15b3fd1552c9985a24604edf8f`.
4. Release notes identify the exact upstream source and pipeline commit.

## TDD and verification

Observed RED -> GREEN sequence:

1. RED commit `006f1e9b260fbfccbc80669d6bca0935684da274` extended the helper contracts before implementation.
2. Actions run `34717099503` failed as intended in `Run helper contract tests` because `o3-mobile-cache-config` was not yet recognized.
3. GREEN implementation added the deterministic patcher, new variants, capacity helper, generated config verification, packaging changes, and bounded-cache workflow.
4. Production run `34717331076` completed successfully for both matrix candidates and the publish job.
5. The pipeline verified the pinned toolchain checksum, exact upstream source SHA, helper contracts, shell/Python syntax, generated configuration defaults, generated environment-option documentation, overflow-safe capacity static assertions, ARM64EC and AArch64 compilation, PE machine types, WCP layout, WCP checksums, and release asset set.

## Published candidates

- `FEXCore-2609-GameNative-O3-MobileTune-CacheConfig.wcp`
  - cache default: off
  - capacity default: `0` MiB (unlimited until configured)
  - SHA-256: `8e4463da323d8936873447f74ef444e1617ba37865e86c204fa8fa09d28f2998`

- `FEXCore-2609-GameNative-O3-MobileTune-Cache1GB.wcp`
  - cache default: on
  - capacity default: `1024` MiB
  - SHA-256: `f1ed9c930a217ae637304737e873daf467bf65332d0c8526d5311c35b8a0f234`

The release also contains `SHA256SUMS`; GitHub release metadata independently reports matching SHA-256 digests for both WCP assets.
