#!/usr/bin/env bash
# Build a portable Windows folder next to seerr.exe (assets + MinGW runtime DLLs).
# Usage: packaging/make-windows-portable.sh <seerr.exe> <out-dir> [version]
set -euo pipefail

EXE="${1:?path to seerr.exe}"
OUT="${2:?output directory}"
VER="${3:-0.1.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MINGW_BIN="${MINGW_PREFIX:-/mingw64}/bin"

if [[ ! -f "$EXE" ]]; then
  echo "missing exe: $EXE" >&2
  exit 1
fi
if [[ ! -d "$MINGW_BIN" ]]; then
  echo "missing MinGW bin: $MINGW_BIN" >&2
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

# Only follow DLLs that actually live in MinGW — never walk Windows api-set / ext-ms trees.
is_skip_name() {
  local base
  base="$(basename "$1" | tr '[:upper:]' '[:lower:]')"
  case "$base" in
    api-ms-win-*.dll|ext-ms-*.dll|ext-ms-onecore-*.dll|ext-ms-windowscore-*.dll) return 0 ;;
    kernel32.dll|user32.dll|gdi32.dll|shell32.dll|ole32.dll|oleaut32.dll|advapi32.dll|\
    winmm.dll|ws2_32.dll|wsock32.dll|iphlpapi.dll|dwmapi.dll|shlwapi.dll|psapi.dll|\
    version.dll|imm32.dll|oleacc.dll|comdlg32.dll|comctl32.dll|setupapi.dll|crypt32.dll|\
    bcrypt.dll|ncrypt.dll|secur32.dll|ntdll.dll|msvcrt.dll|ucrtbase.dll|opengl32.dll|\
    glu32.dll|winhttp.dll|wininet.dll|dnsapi.dll|mswsock.dll|rpcrt4.dll|msimg32.dll|\
    normaliz.dll|wldap32.dll|dbghelp.dll|userenv.dll|bcryptprimitives.dll|kernelbase.dll|\
    sechost.dll|gdi32full.dll|win32u.dll|msvcp_win.dll|cryptbase.dll|cfgmgr32.dll|\
    powrprof.dll|umpdc.dll|profapi.dll|wintrust.dll|imagehlp.dll|dxgi.dll|d3d11.dll|\
    d3d9.dll|d2d1.dll|dwrite.dll|dxcore.dll|hid.dll|devobj.dll|winspool.drv|\
    mpr.dll|wtsapi32.dll|nsi.dll|dhcpcsvc.dll|cryptnet.dll)
      return 0 ;;
  esac
  return 1
}

list_imports() {
  local f="$1"
  # Prefer non-recursive ntldd (direct imports only). Never use ntldd -R — it
  # explodes into thousands of Windows api-set stubs and hangs CI for ages.
  if command -v ntldd >/dev/null 2>&1; then
    ntldd "$f" 2>/dev/null \
      | tr -d '\r' \
      | awk '
          {
            for (i = 1; i <= NF; i++) {
              if ($i ~ /\.dll$/ || $i ~ /\.DLL$/) {
                n = $i
                sub(/^.*[\\\/]/, "", n)
                print n
              }
            }
          }'
    return 0
  fi
  if command -v objdump >/dev/null 2>&1; then
    objdump -p "$f" 2>/dev/null | awk '/DLL Name:/{print $3}'
    return 0
  fi
  return 1
}

declare -A SEEN=()
QUEUE=("$OUT/seerr.exe")
COPIED=0

while ((${#QUEUE[@]})); do
  cur="${QUEUE[0]}"
  QUEUE=("${QUEUE[@]:1}")

  while IFS= read -r dep; do
    [[ -z "$dep" ]] && continue
    base="$(basename "$dep" | tr -d '\r')"
    key="$(echo "$base" | tr '[:upper:]' '[:lower:]')"
    [[ -n "${SEEN[$key]+x}" ]] && continue
    SEEN[$key]=1

    if is_skip_name "$base"; then
      continue
    fi

    src="$MINGW_BIN/$base"
    # Case-insensitive lookup in MinGW bin
    if [[ ! -f "$src" ]]; then
      hit="$(ls -1 "$MINGW_BIN" 2>/dev/null | grep -i "^${base}\$" | head -n1 || true)"
      if [[ -n "$hit" ]]; then
        src="$MINGW_BIN/$hit"
        base="$hit"
      else
        continue
      fi
    fi

    if [[ ! -f "$OUT/$base" ]]; then
      cp -f "$src" "$OUT/$base"
      COPIED=$((COPIED + 1))
      QUEUE+=("$OUT/$base")
    fi
  done < <(list_imports "$cur" || true)
done

# Ensure common MinGW / libtorrent runtime pieces are present even if import scan missed them.
for extra in \
  libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll \
  zlib1.dll libtorrent-rasterbar.dll \
  libcrypto-3-x64.dll libssl-3-x64.dll \
  libcrypto-1_1-x64.dll libssl-1_1-x64.dll \
  libiconv-2.dll libintl-8.dll libssp-0.dll \
  libboost_system-mt.dll
do
  if [[ -f "$MINGW_BIN/$extra" && ! -f "$OUT/$extra" ]]; then
    cp -f "$MINGW_BIN/$extra" "$OUT/"
    COPIED=$((COPIED + 1))
    # One more pass for this DLL's MinGW-only deps
    QUEUE+=("$OUT/$extra")
  fi
done

while ((${#QUEUE[@]})); do
  cur="${QUEUE[0]}"
  QUEUE=("${QUEUE[@]:1}")
  while IFS= read -r dep; do
    [[ -z "$dep" ]] && continue
    base="$(basename "$dep" | tr -d '\r')"
    key="$(echo "$base" | tr '[:upper:]' '[:lower:]')"
    [[ -n "${SEEN[$key]+x}" ]] && continue
    SEEN[$key]=1
    is_skip_name "$base" && continue
    src="$MINGW_BIN/$base"
    if [[ ! -f "$src" ]]; then
      hit="$(ls -1 "$MINGW_BIN" 2>/dev/null | grep -i "^${base}\$" | head -n1 || true)"
      [[ -z "$hit" ]] && continue
      src="$MINGW_BIN/$hit"
      base="$hit"
    fi
    if [[ ! -f "$OUT/$base" ]]; then
      cp -f "$src" "$OUT/$base"
      COPIED=$((COPIED + 1))
      QUEUE+=("$OUT/$base")
    fi
  done < <(list_imports "$cur" || true)
done

cat > "$OUT/README-PORTABLE.txt" <<EOF
Seerr portable (Windows) ${VER}

Extract anywhere and run seerr.exe.
Fonts, icons, locales, images and libVLC are bundled next to the executable.

Config / cache: %APPDATA%\\SeerrCpp
EOF

# Bundle libVLC so playback works without a system VLC install.
VLC_VER="${SEERR_VLC_VERSION:-3.0.21}"
VLC_ZIP_URL="${SEERR_VLC_URL:-https://get.videolan.org/vlc/${VLC_VER}/win64/vlc-${VLC_VER}-win64.zip}"
VLC_CACHE="${ROOT}/build/vlc-cache"
mkdir -p "$VLC_CACHE"
VLC_ZIP="$VLC_CACHE/vlc-${VLC_VER}-win64.zip"

if [[ ! -f "$OUT/libvlc/libvlc.dll" ]]; then
  echo "Fetching VLC ${VLC_VER} for portable libvlc…"
  if [[ ! -f "$VLC_ZIP" ]]; then
    if command -v curl >/dev/null 2>&1; then
      curl -L --retry 3 --fail -o "$VLC_ZIP" "$VLC_ZIP_URL"
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$VLC_ZIP" "$VLC_ZIP_URL"
    else
      echo "warning: no curl/wget — skipping libvlc bundle" >&2
      VLC_ZIP=""
    fi
  fi
  if [[ -n "$VLC_ZIP" && -f "$VLC_ZIP" ]]; then
    VLC_EXTRACT="$VLC_CACHE/extract-${VLC_VER}"
    rm -rf "$VLC_EXTRACT"
    mkdir -p "$VLC_EXTRACT"
    unzip -q -o "$VLC_ZIP" -d "$VLC_EXTRACT"
    # Zip root is usually vlc-3.0.21/
    SRC="$(find "$VLC_EXTRACT" -maxdepth 2 -type f -name libvlc.dll | head -n1 || true)"
    if [[ -n "$SRC" ]]; then
      SRC_DIR="$(dirname "$SRC")"
      mkdir -p "$OUT/libvlc"
      cp -f "$SRC_DIR/libvlc.dll" "$OUT/libvlc/"
      cp -f "$SRC_DIR/libvlccore.dll" "$OUT/libvlc/" 2>/dev/null || true
      if [[ -d "$SRC_DIR/plugins" ]]; then
        cp -a "$SRC_DIR/plugins" "$OUT/libvlc/"
      fi
      # A few runtime DLLs VLC ships beside libvlc
      for d in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
        [[ -f "$SRC_DIR/$d" ]] && cp -f "$SRC_DIR/$d" "$OUT/libvlc/" || true
      done
      echo "Bundled libvlc from $SRC_DIR"
    else
      echo "warning: libvlc.dll not found inside VLC zip" >&2
    fi
  fi
fi

echo "Portable package ready: $OUT (${COPIED} MinGW DLLs)"
ls -la "$OUT" | head -n 60
if [[ -d "$OUT/libvlc" ]]; then
  ls -la "$OUT/libvlc" | head -n 20
fi
