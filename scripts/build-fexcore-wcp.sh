#!/usr/bin/env bash
set -euo pipefail

# Build FEXCore DLLs for Winlator Bionic (arm64ec + aarch64 WOW64).
#
# Usage:
#   scripts/build-fexcore.sh <fex-src-dir> <output-root>
#
# Produces:
#   <output-root>/stage-ec/   arm64ec install tree
#   <output-root>/stage-wo/   aarch64 install tree

FEX_SRC="${1:?FEX source directory required}"
OUTPUT_ROOT="${2:?Output root directory required}"

if [[ ! -d "$FEX_SRC" ]]; then
  echo "error: FEX source directory not found: $FEX_SRC" >&2
  exit 1
fi

# Resolve to absolute paths so nested `cd` into build dirs stays correct.
FEX_SRC="$(cd "$FEX_SRC" && pwd)"
mkdir -p "$OUTPUT_ROOT"
OUTPUT_ROOT="$(cd "$OUTPUT_ROOT" && pwd)"

if ! command -v ninja >/dev/null 2>&1; then
  echo "error: ninja is required" >&2
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "error: cmake is required" >&2
  exit 1
fi

build_arch() {
  local triple="$1"
  local dest="$2"
  local build_dir="${OUTPUT_ROOT}/build-${triple}"

  rm -rf "$build_dir"
  mkdir -p "$build_dir"
  cd "$build_dir"

  local ccache_args=()
  if command -v ccache >/dev/null 2>&1; then
    ccache_args=(
      -DCMAKE_C_COMPILER_LAUNCHER=ccache
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    )
  fi

  cmake -G Ninja -Wno-dev \
    "${ccache_args[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${FEX_SRC}/Data/CMake/toolchain_mingw.cmake" \
    -DMINGW_TRIPLE="${triple}-w64-mingw32" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=/usr/lib/wine/aarch64-windows \
    -DENABLE_JEMALLOC_GLIBC_ALLOC=False \
    -DENABLE_LTO=False \
    -DENABLE_ASSERTIONS=False \
    -DBUILD_TESTING=False \
    "$FEX_SRC"

  ninja
  DESTDIR="$dest" ninja install
}

build_arch "arm64ec" "${OUTPUT_ROOT}/stage-ec"
build_arch "aarch64" "${OUTPUT_ROOT}/stage-wo"

echo "FEXCore build complete:"
echo "  arm64ec -> ${OUTPUT_ROOT}/stage-ec"
echo "  aarch64 -> ${OUTPUT_ROOT}/stage-wo"
