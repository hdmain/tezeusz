# Seerr C++ (tezeusz)

Desktop Jellyseerr/Seerr-style media discovery app in C++ (ImGui + GLFW), with a
built-in download stack (apibay search + libtorrent) — no separate *arr /
qBittorrent required.

## Quick start (Linux / WSL)

```bash
./scripts/linux-deps.sh    # once (needs sudo)
./scripts/build.sh
./scripts/run.sh
```

Or one-shot smoke (build + 8s GUI test):

```bash
./scripts/linux-smoke.sh
```

Assets (fonts, icons, locales) are staged next to `build/seerr` automatically,
so you can run the binary from the build tree without `make install`.

Playback needs VLC (`libvlc5` / `vlc`) — installed by `linux-deps.sh`.

## Windows (MinGW / MSYS2)

```bash
./scripts/windows-deps.sh   # once, inside MINGW64 shell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSEERR_BUNDLED_DEPS=OFF
cmake --build build
```

Or with FetchContent deps (no MSYS2 libtorrent):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSEERR_BUNDLED_DEPS=ON
cmake --build build
```

CI publishes on the `continuous` release:
- **`seerr-windows-setup.exe`** — NSIS installer → `%LOCALAPPDATA%\Programs\Seerr`
  (per-user, writable → in-app auto-update works)
- **Windows portable zip** — exe + MinGW DLLs + bundled libVLC
- **Linux .deb** / portable zip

## Layout

| Path | Role |
|------|------|
| `src/` | Application code |
| `cmake/` | CMake modules (deps, imgui, packaging) |
| `scripts/` | Linux/WSL build & run helpers |
| `packaging/` | Desktop entry, Windows portable packager |
| `locales/` | UI translations (`en`, `pl`) |
| `vendor/` | ImGui, GLFW (Win), fonts, icons |

## CMake options

| Option | Default | Meaning |
|--------|---------|---------|
| `SEERR_BUNDLED_DEPS` | ON (Win) / OFF (Linux) | Fetch Boost + libtorrent instead of system packages |
| `SEERR_COPY_ASSETS_TO_BUILD` | ON | Stage fonts/icons/locales next to the binary |
| `SEERR_VERSION` | `0.1.0` | Package version string |

## License

UI patterns inspired by [Seerr](https://github.com/seerr-team/seerr). Vendor
libraries retain their own licenses (Dear ImGui, libtorrent, etc.).
