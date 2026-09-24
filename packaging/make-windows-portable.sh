#!/usr/bin/env bash
# Build a fully self-contained Windows portable folder (exe + MinGW DLLs + libVLC).
# Usage: packaging/make-windows-portable.sh <seerr.exe> <out-dir> [version]
set -euo pipefail

EXE="${1:?path to seerr.exe}"
OUT="${2:?output directory}"
VER="${3:-0.1.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MINGW_BIN="${MINGW_PREFIX:-/mingw64}/bin"
MINGW_ROOT="$(cd "$MINGW_BIN/.." && pwd)"

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

# ---------------------------------------------------------------------------
# MinGW DLL collector — only follow DLLs that live under the MinGW prefix.
# Never walk Windows api-set / ext-ms trees (ntldd -R hangs forever on those).
# ---------------------------------------------------------------------------
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
    mpr.dll|wtsapi32.dll|nsi.dll|dhcpcsvc.dll|cryptnet.dll|authz.dll|netapi32.dll|\
    samcli.dll|samlib.dll|wldp.dll|sspictli.dll|sspicli.dll|cryptsp.dll|rsaenh.dll|\
    gpapi.dll|dpapi.dll|webio.dll|fwpuclnt.dll|rasadhlp.dll)
      return 0 ;;
  esac
  return 1
}

list_imports() {
  local f="$1"
  if command -v objdump >/dev/null 2>&1; then
    objdump -p "$f" 2>/dev/null | awk '/DLL Name:/{print $3}'
    return 0
  fi
  if command -v ntldd >/dev/null 2>&1; then
    ntldd "$f" 2>/dev/null \
      | tr -d '\r' \
      | awk '{
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
  return 1
}

find_mingw_dll() {
  local base="$1"
  local src="$MINGW_BIN/$base"
  if [[ -f "$src" ]]; then
    echo "$src"
    return 0
  fi
  local hit
  hit="$(ls -1 "$MINGW_BIN" 2>/dev/null | grep -i "^${base}\$" | head -n1 || true)"
  if [[ -n "$hit" && -f "$MINGW_BIN/$hit" ]]; then
    echo "$MINGW_BIN/$hit"
    return 0
  fi
  return 1
}

declare -A SEEN=()
QUEUE=("$OUT/seerr.exe")
COPIED=0

copy_mingw_deps_of() {
  local cur="$1"
  local dep base key src
  while IFS= read -r dep; do
    [[ -z "$dep" ]] && continue
    base="$(basename "$dep" | tr -d '\r')"
    key="$(echo "$base" | tr '[:upper:]' '[:lower:]')"
    [[ -n "${SEEN[$key]+x}" ]] && continue
    SEEN[$key]=1
    if is_skip_name "$base"; then
      continue
    fi
    if src="$(find_mingw_dll "$base")"; then
      base="$(basename "$src")"
      if [[ ! -f "$OUT/$base" ]]; then
        cp -f "$src" "$OUT/$base"
        COPIED=$((COPIED + 1))
        QUEUE+=("$OUT/$base")
        echo "  + $base"
      fi
    else
      echo "  ! missing MinGW dll: $base" >&2
    fi
  done < <(list_imports "$cur" || true)
}

echo "Collecting MinGW runtime DLLs…"
while ((${#QUEUE[@]})); do
  cur="${QUEUE[0]}"
  QUEUE=("${QUEUE[@]:1}")
  copy_mingw_deps_of "$cur"
done

# Hard extras that scanners sometimes miss (OpenSSL / boost / gettext).
for extra in \
  libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll libssp-0.dll \
  zlib1.dll libzstd.dll liblzma-5.dll \
  libtorrent-rasterbar.dll \
  libcrypto-3-x64.dll libssl-3-x64.dll \
  libcrypto-1_1-x64.dll libssl-1_1-x64.dll \
  libiconv-2.dll libintl-8.dll \
  libboost_system-mt.dll libboost_atomic-mt.dll libboost_chrono-mt.dll \
  libboost_random-mt.dll libboost_date_time-mt.dll \
  libcurl-4.dll libbrotlidec.dll libbrotlicommon.dll libnghttp2-14.dll \
  libidn2-0.dll libpsl-5.dll libssh2-1.dll
do
  if src="$(find_mingw_dll "$extra" 2>/dev/null)"; then
    base="$(basename "$src")"
    key="$(echo "$base" | tr '[:upper:]' '[:lower:]')"
    if [[ -z "${SEEN[$key]+x}" || ! -f "$OUT/$base" ]]; then
      SEEN[$key]=1
      cp -f "$src" "$OUT/$base"
      COPIED=$((COPIED + 1))
      echo "  + $base (extra)"
      copy_mingw_deps_of "$OUT/$base"
      while ((${#QUEUE[@]})); do
        cur="${QUEUE[0]}"
        QUEUE=("${QUEUE[@]:1}")
        copy_mingw_deps_of "$cur"
      done
    fi
  fi
done

# OpenSSL 3 providers (rarely needed at startup, but cheap to ship).
if [[ -d "$MINGW_ROOT/lib/ossl-modules" ]]; then
  mkdir -p "$OUT/ossl-modules"
  cp -f "$MINGW_ROOT/lib/ossl-modules/"*.dll "$OUT/ossl-modules/" 2>/dev/null || true
  echo "  + ossl-modules/"
fi

# ---------------------------------------------------------------------------
# Bundle full libVLC (DLL + plugins) so playback works without system VLC.
# ---------------------------------------------------------------------------
VLC_VER="${SEERR_VLC_VERSION:-3.0.21}"
VLC_ZIP_URL="${SEERR_VLC_URL:-https://get.videolan.org/vlc/${VLC_VER}/win64/vlc-${VLC_VER}-win64.zip}"
VLC_CACHE="${ROOT}/build/vlc-cache"
mkdir -p "$VLC_CACHE"
VLC_ZIP="$VLC_CACHE/vlc-${VLC_VER}-win64.zip"

echo "Bundling libVLC ${VLC_VER}…"
if [[ ! -f "$VLC_ZIP" ]]; then
  if command -v curl >/dev/null 2>&1; then
    curl -L --retry 5 --retry-delay 2 --fail -o "$VLC_ZIP" "$VLC_ZIP_URL"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$VLC_ZIP" "$VLC_ZIP_URL"
  else
    echo "error: no curl/wget — cannot fetch VLC" >&2
    exit 1
  fi
fi

VLC_EXTRACT="$VLC_CACHE/extract-${VLC_VER}"
rm -rf "$VLC_EXTRACT"
mkdir -p "$VLC_EXTRACT"

extract_vlc() {
  # Prefer PowerShell on Windows — MSYS bsdtar often chokes on VLC locale/*.mo entries.
  if command -v powershell.exe >/dev/null 2>&1; then
    local zip_win extract_win
    if command -v cygpath >/dev/null 2>&1; then
      zip_win="$(cygpath -w "$VLC_ZIP")"
      extract_win="$(cygpath -w "$VLC_EXTRACT")"
    else
      zip_win="$VLC_ZIP"
      extract_win="$VLC_EXTRACT"
    fi
    powershell.exe -NoProfile -Command \
      "Expand-Archive -LiteralPath '$zip_win' -DestinationPath '$extract_win' -Force"
    return $?
  fi
  if command -v unzip >/dev/null 2>&1; then
    unzip -q -o "$VLC_ZIP" -d "$VLC_EXTRACT"
    return $?
  fi
  if command -v bsdtar >/dev/null 2>&1; then
    # Ignore non-fatal locale extraction errors if libvlc.dll appears.
    bsdtar -xf "$VLC_ZIP" -C "$VLC_EXTRACT" || true
    return 0
  fi
  echo "error: need powershell Expand-Archive, unzip, or bsdtar" >&2
  return 1
}

extract_vlc
SRC="$(find "$VLC_EXTRACT" -maxdepth 3 -type f -name libvlc.dll | head -n1 || true)"
if [[ -z "$SRC" ]]; then
  echo "error: libvlc.dll not found inside VLC zip" >&2
  exit 1
fi
SRC_DIR="$(dirname "$SRC")"
mkdir -p "$OUT/libvlc"

# Core libs + every other DLL VLC ships in its root (codec helpers, etc.).
cp -f "$SRC_DIR/libvlc.dll" "$OUT/libvlc/"
cp -f "$SRC_DIR/libvlccore.dll" "$OUT/libvlc/"
shopt -s nullglob
for d in "$SRC_DIR"/*.dll; do
  base="$(basename "$d")"
  [[ "$base" == "libvlc.dll" || "$base" == "libvlccore.dll" ]] && continue
  cp -f "$d" "$OUT/libvlc/"
done
shopt -u nullglob

if [[ -d "$SRC_DIR/plugins" ]]; then
  cp -a "$SRC_DIR/plugins" "$OUT/libvlc/"
fi

# MinGW runtimes next to libvlc too (plugins sometimes load relative to libvlc).
for d in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
  [[ -f "$OUT/$d" ]] && cp -f "$OUT/$d" "$OUT/libvlc/" || true
done

echo "Bundled libvlc from $SRC_DIR"

# ---------------------------------------------------------------------------
# Verify: every non-system import of seerr.exe must exist beside it.
# ---------------------------------------------------------------------------
echo "Verifying seerr.exe imports…"
UNRESOLVED=0
while IFS= read -r dep; do
  [[ -z "$dep" ]] && continue
  base="$(basename "$dep" | tr -d '\r')"
  is_skip_name "$base" && continue
  if [[ ! -f "$OUT/$base" ]]; then
    hit="$(ls -1 "$OUT" 2>/dev/null | grep -i "^${base}\$" | head -n1 || true)"
    if [[ -z "$hit" ]]; then
      echo "  FAIL unresolved: $base" >&2
      UNRESOLVED=$((UNRESOLVED + 1))
    fi
  fi
done < <(list_imports "$OUT/seerr.exe" || true)

if [[ ! -f "$OUT/libvlc/libvlc.dll" || ! -f "$OUT/libvlc/libvlccore.dll" ]]; then
  echo "  FAIL missing libvlc core" >&2
  UNRESOLVED=$((UNRESOLVED + 1))
fi
if [[ ! -d "$OUT/libvlc/plugins" ]]; then
  echo "  FAIL missing libvlc plugins" >&2
  UNRESOLVED=$((UNRESOLVED + 1))
fi

cat > "$OUT/README-PORTABLE.txt" <<EOF
Seerr portable (Windows) ${VER}

1. Unzip the whole folder (keep DLLs next to seerr.exe).
2. Run seerr.exe
3. Do not move seerr.exe alone — MinGW DLLs + libvlc\\ must stay beside it.

Included:
- MinGW runtime + libtorrent + OpenSSL DLLs
- libVLC ${VLC_VER} (libvlc\\ + plugins) for playback

Config / cache: %APPDATA%\\SeerrCpp

If the window never appears, update your GPU OpenGL driver,
or run check-deps.bat to verify the package is complete.
EOF

cat > "$OUT/check-deps.bat" <<'EOF'
@echo off
setlocal
cd /d "%~dp0"
echo Checking DLLs next to seerr.exe ...
set FAIL=0
for %%D in (
  libgcc_s_seh-1.dll
  libstdc++-6.dll
  libwinpthread-1.dll
  zlib1.dll
  libcrypto-3-x64.dll
  libssl-3-x64.dll
  libtorrent-rasterbar.dll
  libvlc\libvlc.dll
  libvlc\libvlccore.dll
) do (
  if not exist "%%D" (
    echo MISSING %%D
    set FAIL=1
  ) else (
    echo OK      %%D
  )
)
if "%FAIL%"=="1" (
  echo.
  echo Portable package is incomplete. Re-download the zip.
  pause
  exit /b 1
)
echo.
echo All listed files present. Launching seerr.exe ...
start "" "%~dp0seerr.exe"
EOF

DLL_COUNT="$(ls -1 "$OUT"/*.dll 2>/dev/null | wc -l | tr -d ' ')"
PLUGIN_COUNT=0
if [[ -d "$OUT/libvlc/plugins" ]]; then
  PLUGIN_COUNT="$(find "$OUT/libvlc/plugins" -type f -name '*.dll' 2>/dev/null | wc -l | tr -d ' ')"
fi

echo "Portable package ready: $OUT"
echo "  MinGW DLLs copied: $COPIED (total dlls beside exe: $DLL_COUNT)"
echo "  VLC plugin DLLs:   $PLUGIN_COUNT"
ls -la "$OUT" | head -n 80

if ((UNRESOLVED > 0)); then
  echo "error: $UNRESOLVED unresolved dependencies — portable is incomplete" >&2
  exit 1
fi
if ((DLL_COUNT < 6)); then
  echo "error: suspiciously few DLLs beside exe ($DLL_COUNT)" >&2
  exit 1
fi
