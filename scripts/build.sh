#!/usr/bin/env bash
# Configure + build Tezeusz (Linux / WSL / macOS-ish Unix).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${TEZEUSZ_BUILD_DIR:-build}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
JOBS="${TEZEUSZ_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

EXTRA=()
if [[ "${TEZEUSZ_BUNDLED_DEPS:-}" == "1" || "${TEZEUSZ_BUNDLED_DEPS:-}" == "ON" ]]; then
  EXTRA+=(-DTEZEUSZ_BUNDLED_DEPS=ON)
else
  EXTRA+=(-DTEZEUSZ_BUNDLED_DEPS=OFF)
fi

echo "==> cmake (${BUILD_TYPE}) → ${BUILD_DIR}"
cmake -S . -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DTEZEUSZ_COPY_ASSETS_TO_BUILD=ON \
  "${EXTRA[@]}" \
  "$@"

echo "==> build (-j${JOBS})"
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" -j "${JOBS}"

BIN="${BUILD_DIR}/tezeusz"
if [[ -x "$BIN" ]]; then
  echo "==> OK: $BIN"
  ls -lah "$BIN"
else
  echo "error: binary missing: $BIN" >&2
  exit 1
fi
