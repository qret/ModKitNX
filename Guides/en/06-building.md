# Building and translating

[← Guide](README.md) · [На русском](../ru/06-building.md)

## Building

For developers. You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) (Linux, macOS or WSL) with these
packages, plus CMake 3.16 or newer, make and git:

```sh
sudo dkp-pacman -S --needed switch-dev switch-cmake switch-pkg-config switch-zlib switch-curl switch-glfw switch-mesa switch-libdrm_nouveau switch-glm
```

Then, in the repository root:

```sh
cmake -B build -DPLATFORM_SWITCH=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The result is `build/ModKitNX.nro`. CMake downloads the third-party sources (zstd 1.5.7, LZ4 1.10.0,
nlohmann/json 3.11.3, the xfangfang fork of Borealis at a fixed commit) into the build folder and checks each archive
against a pinned SHA-256 ([`cmake/ModkitDeps.cmake`](../../cmake/ModkitDeps.cmake)). To build offline, pass `-DMODKIT_DEPS_DIR=<folder>` with
`zstd/`, `lz4/`, `nlohmann/nlohmann/json.hpp` and `borealis/` inside.

With Docker instead of a local devkitPro:

```sh
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64:20260219 sh -c 'dkp-pacman -Sy --needed --noconfirm switch-dev switch-cmake switch-pkg-config switch-zlib switch-curl switch-glfw switch-mesa switch-libdrm_nouveau switch-glm && cmake -B build -DPLATFORM_SWITCH=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build -j'
```

Core tests on a PC: `cmake -S tests/host -B build-host && cmake --build build-host -j && ctest --test-dir build-host`.

The source of the game module is in `nxredirect/` (GPL-2.0). The modules in `romfs/modules/` can be rebuilt and
compared byte for byte with the shipped ones (Linux or WSL, devkitA64, make, rsync):
`bash nxredirect/build-romfs-modules.sh <output folder> --check romfs/modules`.

## Interface languages

English, Русский, Українська. Translations live in `resources/i18n/<language>/`. To add your own language,
see [`TRANSLATING.md`](../../TRANSLATING.md).
