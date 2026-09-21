#!/usr/bin/env bash
# Install build + runtime dependencies (Debian/Ubuntu/Pop!_OS/WSL).
set -euo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
  SUDO=sudo
else
  SUDO=
fi

export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update
$SUDO apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  libglfw3-dev libgl1-mesa-dev \
  libcurl4-openssl-dev \
  libboost-dev \
  libtorrent-rasterbar-dev \
  zlib1g-dev \
  libvlc5 vlc \
  xvfb

echo "Linux deps OK."
