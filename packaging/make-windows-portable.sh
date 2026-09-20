#!/usr/bin/env bash
# Build a portable Windows folder next to seerr.exe (assets + MinGW runtime DLLs).
# Usage: packaging/make-windows-portable.sh <seerr.exe> <out-dir> [version]
set -euo pipefail

EXE="${1:?path to seerr.exe}"
OUT="${2:?output directory}"
VER="${3:-0.1.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [[ ! -f "$EXE" ]]; then
  echo "missing exe: $EXE" >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/fonts" "$OUT/icons" "$OUT/locales"

cp -f "$EXE" "$OUT/seerr.exe"
cp -f "$ROOT/vendor/logo_full.png" "$OUT/"
cp -f "$ROOT/vendor/icon.png" "$OUT/"
cp -f "$ROOT/vendor/poster_missing.png" "$OUT/"
cp -f "$ROOT/vendor/fonts/"*.ttf "$OUT/fonts/" 2>/dev/null || true
cp -f "$ROOT/vendor/icons/"*.svg "$OUT/icons/" 2>/dev/null || true
cp -f "$ROOT/locales/"*.json "$OUT/locales/" 2>/dev/null || true

# Collect MinGW / project DLLs recursively (skip system32 / Windows).
MINGW_BIN="${MINGW_PREFIX:-/mingw64}/bin"
declare -A SEEN=()
QUEUE=("$OUT/seerr.exe")

is_system_dll() {
  local base
  base="$(basename "$1" | tr '[:upper:]' '[:lower:]')"
  case "$base" in
    kernel32.dll|user32.dll|gdi32.dll|shell32.dll|ole32.dll|oleaut32.dll|advapi32.dll|\
    winmm.dll|ws2_32.dll|wsock32.dll|iphlpapi.dll|dwmapi.dll|shlwapi.dll|psapi.dll|\
    version.dll|imm32.dll|oleacc.dll|comdlg32.dll|comctl32.dll|setupapi.dll|crypt32.dll|\
    bcrypt.dll|ncrypt.dll|secur32.dll|ntdll.dll|msvcrt.dll|ucrtbase.dll|opengl32.dll|\
    glu32.dll|winhttp.dll|wininet.dll|dnsapi.dll|mswsock.dll|rpcrt4.dll|msimg32.dll|\
    normaliz.dll|wldap32.dll|dbghelp.dll)
      return 0 ;;
  esac
  return 1
}

resolve_dll() {
  local name="$1"
  if [[ -f "$MINGW_BIN/$name" ]]; then
    echo "$MINGW_BIN/$name"
    return 0
  fi
  if command -v where >/dev/null 2>&1; then
    local hit
    hit="$(where "$name" 2>/dev/null | head -n1 || true)"
    if [[ -n "$hit" && -f "$hit" ]]; then
      echo "$hit"
      return 0
    fi
  fi
  return 1
}

deps_of() {
  local f="$1"
  if command -v ntldd >/dev/null 2>&1; then
    ntldd -R "$f" 2>/dev/null | awk '{
      for (i=1;i<=NF;i++) if ($i ~ /\.dll$/ || $i ~ /\.DLL$/) print $i
    }' | while read -r d; do basename "$d"; done
    return 0
  fi
  # Fallback: PE import table via objdump
  if command -v objdump >/dev/null 2>&1; then
    objdump -p "$f" 2>/dev/null | awk '/DLL Name:/{print $3}'
    return 0
  fi
  return 1
}

while ((${#QUEUE[@]})); do
  cur="${QUEUE[0]}"
  QUEUE=("${QUEUE[@]:1}")
  while IFS= read -r dep; do
    [[ -z "$dep" ]] && continue
    base="$(basename "$dep")"
    key="$(echo "$base" | tr '[:upper:]' '[:lower:]')"
    [[ -n "${SEEN[$key]+x}" ]] && continue
    SEEN[$key]=1
    if is_system_dll "$base"; then
      continue
    fi
    if path="$(resolve_dll "$base")"; then
      cp -f "$path" "$OUT/"
      QUEUE+=("$OUT/$base")
    else
      echo "note: unresolved dll $base (may be system-provided)" >&2
    fi
  done < <(deps_of "$cur" || true)
done

# Always try common MinGW runtime + libtorrent deps if present.
for extra in \
  libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll \
  zlib1.dll libtorrent-rasterbar.dll \
  libcrypto-3-x64.dll libssl-3-x64.dll libcrypto-1_1-x64.dll libssl-1_1-x64.dll \
  libiconv-2.dll libintl-8.dll libssp-0.dll
do
  if [[ -f "$MINGW_BIN/$extra" && ! -f "$OUT/$extra" ]]; then
    cp -f "$MINGW_BIN/$extra" "$OUT/"
  fi
done

cat > "$OUT/README-PORTABLE.txt" <<EOF
Seerr portable (Windows) ${VER}

Extract anywhere and run seerr.exe.
Fonts, icons, locales and images are bundled next to the executable.

Config / cache: %APPDATA%\\SeerrCpp
EOF

echo "Portable package ready: $OUT"
ls -la "$OUT" | head -n 80
