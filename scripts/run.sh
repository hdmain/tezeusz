#!/usr/bin/env bash
# Run a local Linux/WSL build with sane defaults.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${SEERR_BIN:-$ROOT/build/seerr}"

if [[ ! -x "$BIN" ]]; then
  echo "Binary not found: $BIN" >&2
  echo "Run: ./scripts/build.sh" >&2
  exit 1
fi

# Prefer X11 under WSLg / mixed sessions — Wayland + GLFW 3.3 is flaky.
export GDK_BACKEND="${GDK_BACKEND:-x11}"
if [[ -n "${DISPLAY:-}" ]]; then
  unset WAYLAND_DISPLAY || true
fi

if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  echo "No DISPLAY/WAYLAND_DISPLAY — starting under xvfb-run" >&2
  exec env SEERR_DISABLE_IMGSWARM="${SEERR_DISABLE_IMGSWARM:-1}" \
    xvfb-run -a "$BIN" "$@"
fi

# Optional: SEERR_DISABLE_IMGSWARM=1 skips libtorrent DHT at startup.
exec "$BIN" "$@"
