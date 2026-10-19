#!/usr/bin/env bash
set -euo pipefail

# Package FEXCore build outputs into a Winlator Bionic .wcp archive.
#
# Usage:
#   scripts/package-wcp.sh <stage-ec> <stage-wo> <version-name> [output-dir]
#
# Example:
#   scripts/package-wcp.sh stage-ec stage-wo 2607-abc1234 dist

STAGE_EC="${1:?arm64ec stage directory required}"
STAGE_WO="${2:?aarch64 stage directory required}"
VERSION_NAME="${3:?version name required}"
OUTPUT_DIR="${4:-.}"

if [[ ! -d "$STAGE_EC" || ! -d "$STAGE_WO" ]]; then
  echo "error: stage directories not found" >&2
  exit 1
fi

STRIP_BIN="${LLVM_STRIP:-}"
if [[ -z "$STRIP_BIN" ]]; then
  if command -v llvm-strip >/dev/null 2>&1; then
    STRIP_BIN="llvm-strip"
  elif [[ -x /opt/llvm-mingw/bin/llvm-strip ]]; then
    STRIP_BIN="/opt/llvm-mingw/bin/llvm-strip"
  fi
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

PACKAGE_DIR="${WORK_DIR}/final_package"
mkdir -p "${PACKAGE_DIR}/system32"

DLL_EC="$(find "$STAGE_EC" -type f -name '*.dll' | head -n 1)"
DLL_WO="$(find "$STAGE_WO" -type f -name '*.dll' | head -n 1)"

if [[ -z "$DLL_EC" || -z "$DLL_WO" ]]; then
  echo "error: expected DLL outputs in stage directories" >&2
  echo "  stage-ec: $STAGE_EC" >&2
  echo "  stage-wo: $STAGE_WO" >&2
  exit 1
fi

cp "$DLL_EC" "${PACKAGE_DIR}/system32/libarm64ecfex.dll"
cp "$DLL_WO" "${PACKAGE_DIR}/system32/libwow64fex.dll"

if [[ -n "$STRIP_BIN" ]]; then
  "$STRIP_BIN" --strip-all "${PACKAGE_DIR}/system32/libarm64ecfex.dll"
  "$STRIP_BIN" --strip-all "${PACKAGE_DIR}/system32/libwow64fex.dll"
else
  echo "warning: llvm-strip not found; shipping unstripped binaries" >&2
fi

FULL_VERSION_NAME="FEXCore-${VERSION_NAME}"

cat > "${PACKAGE_DIR}/profile.json" <<EOF
{
  "type": "FEXCore",
  "versionName": "${FULL_VERSION_NAME}",
  "versionCode": 0,
  "description": "FEXCore ${VERSION_NAME} for Winlator Bionic",
  "files": [
    {
      "source": "system32/libarm64ecfex.dll",
      "target": "\${system32}/libarm64ecfex.dll"
    },
    {
      "source": "system32/libwow64fex.dll",
      "target": "\${system32}/libwow64fex.dll"
    }
  ]
}
EOF

mkdir -p "$OUTPUT_DIR"
# Resolve to an absolute path so the tar below (run after cd into the temp
# package dir) writes to the intended output location.
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
WCP_PATH="${OUTPUT_DIR}/${FULL_VERSION_NAME}.wcp"

(
  cd "$PACKAGE_DIR"
  tar -cJf "$WCP_PATH" profile.json system32
)

echo "Created ${WCP_PATH}"
