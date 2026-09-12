#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
builder="$repo_root/Scripts/GameNative/build_fexcore_2609_wcp.sh"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/source" "$tmp/out"
git -C "$tmp/source" init -q
git -C "$tmp/source" config user.email test@example.invalid
git -C "$tmp/source" config user.name test
printf 'fixture\n' > "$tmp/source/README"
git -C "$tmp/source" add README
git -C "$tmp/source" commit -qm fixture

if bash "$builder" "$tmp/source" invalid "$tmp/out" 1 >"$tmp/invalid.out" 2>"$tmp/invalid.err"; then
  echo "expected invalid variant to fail" >&2
  exit 1
fi
grep -q 'Unsupported variant: invalid' "$tmp/invalid.err"

for variant in o3 o3-mobile o3-mobile-cache-config o3-mobile-cache-1g; do
  if bash "$builder" "$tmp/source" "$variant" "$tmp/out" 1 >"$tmp/${variant}.out" 2>"$tmp/${variant}.err"; then
    echo "expected source SHA mismatch for $variant" >&2
    exit 1
  fi
  grep -q 'Source SHA mismatch' "$tmp/${variant}.err"
done

# Preserve the previous A76 candidate as a control, but require the bounded-cache
# candidates to carry the newer A77 scheduling-only tune.
o3_mobile_block="$(sed -n '/^  o3-mobile)$/,/^    ;;/p' "$builder")"
cache_config_block="$(sed -n '/^  o3-mobile-cache-config)$/,/^    ;;/p' "$builder")"
cache_1g_block="$(sed -n '/^  o3-mobile-cache-1g)$/,/^    ;;/p' "$builder")"
grep -Fq -- '-mtune=cortex-a76' <<<"$o3_mobile_block"
grep -Fq -- '-mtune=cortex-a77' <<<"$cache_config_block"
grep -Fq -- '-mtune=cortex-a77' <<<"$cache_1g_block"

echo "FEX 2609 builder guard tests passed"
