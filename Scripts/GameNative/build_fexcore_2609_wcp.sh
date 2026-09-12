#!/usr/bin/env bash
set -euo pipefail

EXPECTED_SOURCE_SHA="395b132f346b1a45def246d10c52245edba1ef02"

if [[ $# -ne 4 ]]; then
  echo "Usage: $0 <source-dir> <o3|o3-mobile|o3-mobile-cache-config|o3-mobile-cache-1g> <output-dir> <version-code>" >&2
  exit 2
fi

source_dir="$1"
variant="$2"
output_dir="$3"
version_code="$4"

variant_cmake_args=()
apply_cache_patch=false
disk_cache_default=""
disk_cache_max_mb_default=""
mtune_cpu=""
case "$variant" in
  o3)
    tune_cpu=generic
    ;;
  o3-mobile)
    tune_cpu=none
    mtune_cpu=cortex-a76
    variant_cmake_args+=(
      "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -mtune=$mtune_cpu"
      "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -mtune=$mtune_cpu"
    )
    ;;
  o3-mobile-cache-config)
    tune_cpu=none
    mtune_cpu=cortex-a77
    apply_cache_patch=true
    disk_cache_default=false
    disk_cache_max_mb_default=0
    variant_cmake_args+=(
      "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -mtune=$mtune_cpu"
      "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -mtune=$mtune_cpu"
      '-DFEX_DISKCACHE_DEFAULT=false'
      '-DFEX_DISKCACHE_MAX_SIZE_MB_DEFAULT=0'
    )
    ;;
  o3-mobile-cache-1g)
    tune_cpu=none
    mtune_cpu=cortex-a77
    apply_cache_patch=true
    disk_cache_default=true
    disk_cache_max_mb_default=1024
    variant_cmake_args+=(
      "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -mtune=$mtune_cpu"
      "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -mtune=$mtune_cpu"
      '-DFEX_DISKCACHE_DEFAULT=true'
      '-DFEX_DISKCACHE_MAX_SIZE_MB_DEFAULT=1024'
    )
    ;;
  *)
    echo "Unsupported variant: $variant" >&2
    exit 2
    ;;
esac

[[ -d "$source_dir/.git" || -f "$source_dir/.git" ]] || { echo "Source directory is not a git checkout: $source_dir" >&2; exit 2; }
actual_source_sha="$(git -C "$source_dir" rev-parse HEAD)"
if [[ "$actual_source_sha" != "$EXPECTED_SOURCE_SHA" ]]; then
  echo "Source SHA mismatch: expected $EXPECTED_SOURCE_SHA, got $actual_source_sha" >&2
  exit 2
fi
[[ "$version_code" =~ ^[0-9]+$ ]] || { echo "version-code must be an integer" >&2; exit 2; }

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
packager="$repo_root/Scripts/GameNative/package_fexcore_wcp.sh"
patcher="$repo_root/Scripts/GameNative/apply_fexcore_2609_disk_cache_cap.py"
[[ -f "$packager" ]] || { echo "Packager not found: $packager" >&2; exit 2; }
[[ -f "$patcher" ]] || { echo "Disk-cache patcher not found: $patcher" >&2; exit 2; }

LLVM_MINGW_ROOT="${LLVM_MINGW_ROOT:-/opt/llvm-mingw}"
for tool in clang cmake ninja llvm-strip llvm-readobj; do
  if [[ "$tool" == cmake || "$tool" == ninja ]]; then
    command -v "$tool" >/dev/null || { echo "Required tool missing: $tool" >&2; exit 2; }
  else
    [[ -x "$LLVM_MINGW_ROOT/bin/$tool" ]] || { echo "Required tool missing: $LLVM_MINGW_ROOT/bin/$tool" >&2; exit 2; }
  fi
done
command -v python3 >/dev/null || { echo "Required tool missing: python3" >&2; exit 2; }
command -v c++ >/dev/null || { echo "Required tool missing: c++" >&2; exit 2; }

mkdir -p "$output_dir"
build_root="$(mktemp -d "${RUNNER_TEMP:-/tmp}/fex-2609-${variant}.XXXXXX")"
trap 'rm -rf "$build_root"' EXIT
BUILT_DLL=""

if [[ "$apply_cache_patch" == true ]]; then
  python3 "$patcher" "$source_dir"
  git -C "$source_dir" diff --check
  test -f "$source_dir/FEXCore/include/FEXCore/Core/DiskCacheCapacity.h"

  cat > "$build_root/disk-cache-capacity-test.cpp" <<'EOF'
#include <cstdint>
#include <limits>
#include <FEXCore/Core/DiskCacheCapacity.h>

using FEXCore::DiskCache::CanStoreWithinCapacity;

static_assert(CanStoreWithinCapacity(0, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX));
static_assert(CanStoreWithinCapacity(1024, 256, 256, 256, 256));
static_assert(!CanStoreWithinCapacity(1024, 256, 256, 256, 257));
static_assert(!CanStoreWithinCapacity(1024, 1025, 0, 0, 0));
static_assert(!CanStoreWithinCapacity(UINT64_MAX, UINT64_MAX, 0, 1, 0));
static_assert(!CanStoreWithinCapacity(UINT64_MAX, UINT64_MAX - 1, 0, 1, 1));

int main() { return 0; }
EOF
  c++ -std=c++20 -Wall -Wextra -Werror -I"$source_dir/FEXCore/include" \
    "$build_root/disk-cache-capacity-test.cpp" -o "$build_root/disk-cache-capacity-test"
  "$build_root/disk-cache-capacity-test"
fi

build_arch() {
  local triple="$1"
  local expected_dll="$2"
  local build_dir="$build_root/build-$triple"

  cmake -S "$source_dir" -B "$build_dir" -G Ninja -Wno-dev \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$source_dir/Data/CMake/toolchain_mingw.cmake" \
    -DMINGW_TRIPLE="${triple}-w64-mingw32" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=/usr/lib/wine/aarch64-windows \
    -DENABLE_JEMALLOC_GLIBC_ALLOC=False \
    -DENABLE_LTO=False \
    -DENABLE_ASSERTIONS=False \
    -DENABLE_CCACHE=False \
    -DBUILD_TESTING=False \
    -DTUNE_ARCH=generic \
    -DTUNE_CPU="$tune_cpu" \
    -DOVERRIDE_VERSION=2609 \
    -DOVERRIDE_HASH="$EXPECTED_SOURCE_SHA" \
    "${variant_cmake_args[@]}"

  if [[ -n "$mtune_cpu" ]]; then
    grep -Fq "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -mtune=$mtune_cpu" "$build_dir/CMakeCache.txt"
    grep -Fq "CMAKE_C_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -mtune=$mtune_cpu" "$build_dir/CMakeCache.txt"
  fi

  if [[ "$apply_cache_patch" == true ]]; then
    grep -Fq "FEX_DISKCACHE_DEFAULT:STRING=$disk_cache_default" "$build_dir/CMakeCache.txt"
    grep -Fq "FEX_DISKCACHE_MAX_SIZE_MB_DEFAULT:STRING=$disk_cache_max_mb_default" "$build_dir/CMakeCache.txt"
    python3 - "$build_dir/generated/Config/Config.json" "$disk_cache_default" "$disk_cache_max_mb_default" <<'PY'
import json
import sys

path, expected_enabled, expected_max = sys.argv[1:]
with open(path, encoding="utf-8") as f:
    data = json.load(f)
cpu = data["Options"]["CPU"]
assert cpu["DiskCache"]["Default"] == expected_enabled
assert cpu["DiskCacheMaxSizeMB"]["Default"] == expected_max
assert cpu["DiskCacheMaxSizeMB"]["Type"] == "uint32"
assert cpu["DiskCacheMaxSizeMB"]["AffectsCodeGen"] == "false"
PY
  fi

  cmake --build "$build_dir" --parallel 2

  if [[ "$apply_cache_patch" == true ]]; then
    grep -Fq 'DISKCACHEMAXSIZEMB' "$build_dir/include/FEXCore/Config/ConfigValues.inl"
    grep -Fq 'FEX_DISKCACHEMAXSIZEMB' "$build_dir/generated/FEX.1"
  fi

  local dll="$build_dir/Bin/$expected_dll"
  [[ -s "$dll" ]] || { echo "Expected DLL missing or empty: $dll" >&2; exit 1; }
  BUILT_DLL="$dll"
}

build_arch arm64ec libarm64ecfex.dll
ec_source="$BUILT_DLL"
build_arch aarch64 libwow64fex.dll
wow_source="$BUILT_DLL"

staged="$build_root/staged"
mkdir -p "$staged"
cp "$ec_source" "$staged/libarm64ecfex.dll"
cp "$wow_source" "$staged/libwow64fex.dll"

"$LLVM_MINGW_ROOT/bin/llvm-strip" --strip-all "$staged/libarm64ecfex.dll" "$staged/libwow64fex.dll"

ec_headers="$("$LLVM_MINGW_ROOT/bin/llvm-readobj" --file-headers "$staged/libarm64ecfex.dll")"
wow_headers="$("$LLVM_MINGW_ROOT/bin/llvm-readobj" --file-headers "$staged/libwow64fex.dll")"
grep -q 'ARM64EC' <<<"$ec_headers" || { echo "ARM64EC DLL has unexpected PE machine type" >&2; exit 1; }
grep -q 'ARM64' <<<"$wow_headers" || { echo "WoW64 DLL has unexpected PE machine type" >&2; exit 1; }
if grep -q 'ARM64EC' <<<"$wow_headers"; then
  echo "WoW64 DLL unexpectedly reports ARM64EC machine type" >&2
  exit 1
fi

bash "$packager" "$variant" \
  "$staged/libarm64ecfex.dll" \
  "$staged/libwow64fex.dll" \
  "$output_dir" \
  "$version_code"
