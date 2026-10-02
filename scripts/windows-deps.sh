#!/usr/bin/env bash
# Install MinGW64 build + packaging deps (MSYS2 / MINGW64).
# Run inside an MSYS2 MINGW64 shell, or let CI call this after msys2/setup-msys2.
set -euo pipefail

if ! command -v pacman >/dev/null 2>&1; then
  echo "error: pacman not found - open an MSYS2 MINGW64 shell first" >&2
  exit 1
fi

# Keep this list in sync with .github/workflows/build.yml Windows job packages.
PACKAGES=(
  mingw-w64-x86_64-gcc
  mingw-w64-x86_64-cmake
  mingw-w64-x86_64-ninja
  mingw-w64-x86_64-pkgconf
  mingw-w64-x86_64-boost
  mingw-w64-x86_64-libtorrent-rasterbar
  mingw-w64-x86_64-zlib
  mingw-w64-x86_64-curl
  mingw-w64-x86_64-ntldd
  mingw-w64-x86_64-openssl
  mingw-w64-x86_64-nsis
  zip
  unzip
  curl
  git
)

echo "==> pacman -S --needed ${PACKAGES[*]}"
pacman -S --needed --noconfirm "${PACKAGES[@]}"

echo "Windows (MSYS2/MINGW64) deps OK."
echo "Build with:"
echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTEZEUSZ_BUNDLED_DEPS=OFF"
echo "  cmake --build build"
echo "Portable + setup.exe + update pack:"
echo "  bash packaging/make-windows-portable.sh build/tezeusz.exe build/tezeusz-portable"
echo "  bash packaging/make-windows-setup.sh build/tezeusz-portable build/tezeusz-windows-setup.exe"
echo "  bash packaging/make-update-manifest.sh build/tezeusz-portable windows-x64 0.1.0-dev build/update-windows"
