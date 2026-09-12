#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
packager="$repo_root/Scripts/GameNative/package_fexcore_wcp.sh"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
printf 'arm64ec fixture\n' > "$tmp/libarm64ecfex.dll"
printf 'wow64 fixture\n' > "$tmp/libwow64fex.dll"
mkdir -p "$tmp/out"

run_case() {
  local variant="$1"
  local expected_file="$2"
  local expected_version="$3"
  local version_code="$4"
  local extract="$tmp/extract-$variant"

  bash "$packager" "$variant" \
    "$tmp/libarm64ecfex.dll" \
    "$tmp/libwow64fex.dll" \
    "$tmp/out" \
    "$version_code"

  local wcp="$tmp/out/$expected_file"
  test -f "$wcp"
  mkdir -p "$extract"
  tar -xJf "$wcp" -C "$extract"

  jq -e --arg v "$expected_version" --argjson c "$version_code" \
    '.type == "FEXCore" and .versionName == $v and .versionCode == $c and (.files | length == 2)' \
    "$extract/profile.json" >/dev/null
  jq -e '.files[0].source == "system32/libarm64ecfex.dll" and .files[0].target == "${system32}/libarm64ecfex.dll"' "$extract/profile.json" >/dev/null
  jq -e '.files[1].source == "system32/libwow64fex.dll" and .files[1].target == "${system32}/libwow64fex.dll"' "$extract/profile.json" >/dev/null
  test -f "$extract/system32/libarm64ecfex.dll"
  test -f "$extract/system32/libwow64fex.dll"
  cmp -s "$tmp/libarm64ecfex.dll" "$extract/system32/libarm64ecfex.dll"
  cmp -s "$tmp/libwow64fex.dll" "$extract/system32/libwow64fex.dll"
}

run_case o3 FEXCore-2609-GameNative-O3.wcp 2609-GameNative-O3 2609001
run_case o3-lto FEXCore-2609-GameNative-O3-LTO.wcp 2609-GameNative-O3-LTO 2609002

echo "WCP packaging tests passed"
