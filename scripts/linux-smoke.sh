#!/usr/bin/env bash
# One-shot: deps (if needed) → build → smoke-run under current display / xvfb.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

need_deps=0
for pkg in cmake ninja pkg-config; do
  command -v "$pkg" >/dev/null 2>&1 || need_deps=1
done
pkg-config --exists glfw3 2>/dev/null || need_deps=1
pkg-config --exists libtorrent-rasterbar 2>/dev/null || need_deps=1
pkg-config --exists libcurl 2>/dev/null || need_deps=1

if [[ "$need_deps" -eq 1 ]]; then
  echo "==> installing deps"
  bash "$ROOT/scripts/linux-deps.sh"
fi

bash "$ROOT/scripts/build.sh"

echo "==> smoke run (8s)"
export SEERR_DISABLE_IMGSWARM="${SEERR_DISABLE_IMGSWARM:-1}"
set +e
if [[ -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; then
  timeout 8s "$ROOT/build/seerr" 2>"$ROOT/build/seerr-smoke.log"
  rc=$?
else
  timeout 8s xvfb-run -a "$ROOT/build/seerr" 2>"$ROOT/build/seerr-smoke.log"
  rc=$?
fi
set -e

echo "exit=$rc"
sed -n '1,40p' "$ROOT/build/seerr-smoke.log" || true
# 0 = clean quit, 124 = timeout (still running = success for GUI smoke)
if [[ "$rc" -eq 0 || "$rc" -eq 124 ]]; then
  echo "Linux smoke OK"
  exit 0
fi
echo "Linux smoke failed (rc=$rc) — see build/seerr-smoke.log" >&2
exit "$rc"
