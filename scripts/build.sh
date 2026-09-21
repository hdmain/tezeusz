#!/usr/bin/env bash
# Configure + build Seerr (Linux / WSL / macOS-ish Unix).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${SEERR_BUILD_DIR:-build}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
JOBS="${SEERR_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

EXTRA=()
if [[ "${SEERR_BUNDLED_DEPS:-}" == "1" || "${SEERR_BUNDLED_DEPS:-}" == "ON" ]]; then
  EXTRA+=(-DSEERR_BUNDLED_DEPS=ON)
else
  EXTRA+=(-DSEERR_BUNDLED_DEPS=OFF)
fi

echo "==> cmake (${BUILD_TYPE}) → ${BUILD_DIR}"
cmake -S . -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DSEERR_COPY_ASSETS_TO_BUILD=ON \
  "${EXTRA[@]}" \
  "$@"

echo "==> build (-j${JOBS})"
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" -j "${JOBS}"

BIN="${BUILD_DIR}/seerr"
if [[ -x "$BIN" ]]; then
  echo "==> OK: $BIN"
  ls -lah "$BIN"
else
  echo "error: binary missing: $BIN" >&2
  exit 1
fi
