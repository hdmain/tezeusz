# Seerr C++ (tezeusz)

Desktop Jellyseerr/Seerr-style media discovery app in C++ (ImGui + GLFW), with a built-in download stack (apibay search + libtorrent) — no separate *arr / qBittorrent required.

## Build

### Windows (MinGW)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSEERR_BUNDLED_DEPS=ON -Dencryption=OFF
cmake --build build
```

Or with MSYS2 packages (`mingw-w64-x86_64-libtorrent-rasterbar`, `mingw-w64-x86_64-boost`):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSEERR_BUNDLED_DEPS=OFF
cmake --build build
```

### Linux

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  libglfw3-dev libgl1-mesa-dev libcurl4-openssl-dev libboost-dev libtorrent-rasterbar-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSEERR_BUNDLED_DEPS=OFF
cmake --build build
```

## License

UI patterns inspired by [Seerr](https://github.com/seerr-team/seerr). Vendor libraries retain their own licenses (Dear ImGui, libtorrent, etc.).
