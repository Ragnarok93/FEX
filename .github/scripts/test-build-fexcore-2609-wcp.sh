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

if "$builder" "$tmp/source" invalid "$tmp/out" 1 >"$tmp/invalid.out" 2>"$tmp/invalid.err"; then
  echo "expected invalid variant to fail" >&2
  exit 1
fi
grep -q 'Unsupported variant: invalid' "$tmp/invalid.err"

if "$builder" "$tmp/source" o3 "$tmp/out" 1 >"$tmp/sha.out" 2>"$tmp/sha.err"; then
  echo "expected source SHA mismatch to fail" >&2
  exit 1
fi
grep -q 'Source SHA mismatch' "$tmp/sha.err"

echo "FEX 2609 builder guard tests passed"
