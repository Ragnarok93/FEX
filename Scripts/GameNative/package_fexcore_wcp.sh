#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 ]]; then
  echo "Usage: $0 <o3|o3-mobile|o3-mobile-cache-config|o3-mobile-cache-1g> <arm64ec-dll> <wow64-dll> <output-dir> <version-code>" >&2
  exit 2
fi

variant="$1"
ec_dll="$2"
wow_dll="$3"
output_dir="$4"
version_code="$5"

case "$variant" in
  o3)
    filename="FEXCore-2609-GameNative-O3.wcp"
    version_name="2609-GameNative-O3"
    description="FEXCore 2609 GameNative O3 control build"
    ;;
  o3-mobile)
    filename="FEXCore-2609-GameNative-O3-MobileTune.wcp"
    version_name="2609-GameNative-O3-MobileTune"
    description="FEXCore 2609 GameNative O3 mobile scheduling test build"
    ;;
  o3-mobile-cache-config)
    filename="FEXCore-2609-GameNative-O3-MobileTune-CacheConfig.wcp"
    version_name="2609-GameNative-O3-MobileTune-CacheConfig"
    description="FEXCore 2609 GameNative mobile tune with configurable bounded disk cache"
    ;;
  o3-mobile-cache-1g)
    filename="FEXCore-2609-GameNative-O3-MobileTune-Cache1GB.wcp"
    version_name="2609-GameNative-O3-MobileTune-Cache1GB"
    description="FEXCore 2609 GameNative mobile tune with disk cache enabled and 1 GiB default limit"
    ;;
  *)
    echo "Unsupported variant: $variant" >&2
    exit 2
    ;;
esac

[[ -f "$ec_dll" ]] || { echo "Missing ARM64EC DLL: $ec_dll" >&2; exit 2; }
[[ -f "$wow_dll" ]] || { echo "Missing WoW64 DLL: $wow_dll" >&2; exit 2; }
[[ "$version_code" =~ ^[0-9]+$ ]] || { echo "version-code must be an integer" >&2; exit 2; }

mkdir -p "$output_dir"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/system32"
cp "$ec_dll" "$stage/system32/libarm64ecfex.dll"
cp "$wow_dll" "$stage/system32/libwow64fex.dll"

jq -n \
  --arg versionName "$version_name" \
  --argjson versionCode "$version_code" \
  --arg description "$description" \
  '{
    type: "FEXCore",
    versionName: $versionName,
    versionCode: $versionCode,
    description: $description,
    author: "Ragnarok93 / GameNative",
    files: [
      {source: "system32/libarm64ecfex.dll", target: "${system32}/libarm64ecfex.dll"},
      {source: "system32/libwow64fex.dll", target: "${system32}/libwow64fex.dll"}
    ]
  }' > "$stage/profile.json"

output="$output_dir/$filename"
rm -f "$output"
tar -C "$stage" -cJf "$output" .
printf '%s\n' "$output"
