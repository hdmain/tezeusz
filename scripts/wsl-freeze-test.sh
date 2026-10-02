#!/usr/bin/env bash
set -u
ROOT=/mnt/c/Users/makss/Desktop/scoped/projects/tezeusz
BIN=$ROOT/build-wsl/tezeusz
DURATION=${1:-8}

run_one() {
  local label="$1"; shift
  local logfile="/tmp/tezeusz-${label}.log"
  rm -f "$logfile"
  echo ""
  echo "========== ${label} =========="
  env "$@" TEZEUSZ_DISABLE_UPDATE=1 TEZEUSZ_DISABLE_IMGSWARM=1 "$BIN" >"$logfile" 2>&1 &
  local pid=$!
  echo "PID=$pid"
  sleep 0.8
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "DIED immediately"
    cat "$logfile"
    return 1
  fi
  local sum=0 n=0 max=0
  for ((i=1; i<=DURATION; i++)); do
    if ! kill -0 "$pid" 2>/dev/null; then echo "died at $i"; break; fi
    local cpu
    cpu=$(ps -p "$pid" -o pcpu= 2>/dev/null | tr -d ' ')
    cpu=${cpu:-0}
    local cinti=${cpu%%.*}
    cinti=${cinti:-0}
    sum=$((sum+cinti)); n=$((n+1))
    if (( cinti > max )); then max=$cinti; fi
    printf '  t=%02ds  CPU=%s%%\n' "$i" "$cpu"
    sleep 1
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "killed"
  fi
  local avg=0
  if (( n>0 )); then avg=$((sum/n)); fi
  echo "CPU avg~${avg}% max=${max}% samples=${n}"
  echo "--- log ---"
  cat "$logfile"
  echo "--- end ---"
  if (( max>=80 && avg>=50 )); then
    echo "RESULT ${label}: LIKELY FREEZE"
    return 2
  fi
  echo "RESULT ${label}: OK"
  return 0
}

echo "BIN=$BIN DURATION=${DURATION}s"
echo "host DISPLAY=${DISPLAY:-} WAYLAND=${WAYLAND_DISPLAY:-}"
run_one x11 GDK_BACKEND=x11 DISPLAY="${DISPLAY:-:0}" WAYLAND_DISPLAY=
run_one wayland GDK_BACKEND=wayland DISPLAY= WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
echo done
