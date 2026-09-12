#!/usr/bin/env bash
set -euo pipefail

EXPECTED_SOURCE_SHA="395b132f346b1a45def246d10c52245edba1ef02"

if [[ $# -ne 4 ]]; then
  echo "Usage: $0 <source-dir> <o3|o3-lto> <output-dir> <version-code>" >&2
  exit 2
fi

source_dir="$1"
variant="$2"
output_dir="$3"
version_code="$4"

case "$variant" in
  o3) enable_lto=False ;;
  o3-lto) enable_lto=True ;;
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
[[ -x "$packager" ]] || { echo "Packager is not executable: $packager" >&2; exit 2; }

LLVM_MINGW_ROOT="${LLVM_MINGW_ROOT:-/opt/llvm-mingw}"
for tool in clang cmake ninja llvm-strip llvm-readobj; do
  if [[ "$tool" == cmake || "$tool" == ninja ]]; then
    command -v "$tool" >/dev/null || { echo "Required tool missing: $tool" >&2; exit 2; }
  else
    [[ -x "$LLVM_MINGW_ROOT/bin/$tool" ]] || { echo "Required tool missing: $LLVM_MINGW_ROOT/bin/$tool" >&2; exit 2; }
  fi
done

mkdir -p "$output_dir"
build_root="$(mktemp -d "${RUNNER_TEMP:-/tmp}/fex-2609-${variant}.XXXXXX")"
trap 'rm -rf "$build_root"' EXIT

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
    -DENABLE_LTO="$enable_lto" \
    -DENABLE_ASSERTIONS=False \
    -DENABLE_CCACHE=False \
    -DBUILD_TESTING=False \
    -DTUNE_ARCH=generic \
    -DTUNE_CPU=generic \
    -DOVERRIDE_VERSION=2609 \
    -DOVERRIDE_HASH="$EXPECTED_SOURCE_SHA"

  cmake --build "$build_dir" --parallel 2

  local dll="$build_dir/Bin/$expected_dll"
  [[ -s "$dll" ]] || { echo "Expected DLL missing or empty: $dll" >&2; exit 1; }
  printf '%s\n' "$dll"
}

ec_source="$(build_arch arm64ec libarm64ecfex.dll | tail -n 1)"
wow_source="$(build_arch aarch64 libwow64fex.dll | tail -n 1)"

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

"$packager" "$variant" \
  "$staged/libarm64ecfex.dll" \
  "$staged/libwow64fex.dll" \
  "$output_dir" \
  "$version_code"
