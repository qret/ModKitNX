# Сборка и переводы

[← Руководство](README.md) · [In English](../en/06-building.md)

## Сборка

Для разработчиков. Нужен [devkitPro](https://devkitpro.org/wiki/Getting_Started) (Linux, macOS или WSL) с пакетами
ниже, а также CMake 3.16 или новее, make и git:

```sh
sudo dkp-pacman -S --needed switch-dev switch-cmake switch-pkg-config switch-zlib switch-curl switch-glfw switch-mesa switch-libdrm_nouveau switch-glm
```

Затем в корне репозитория:

```sh
cmake -B build -DPLATFORM_SWITCH=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Результат — `build/ModKitNX.nro`. Сторонние исходники (zstd 1.5.7, LZ4 1.10.0, nlohmann/json 3.11.3, Borealis —
форк xfangfang на закреплённом коммите) CMake скачивает в папку сборки и сверяет каждый архив с закреплённым SHA-256
([`cmake/ModkitDeps.cmake`](../../cmake/ModkitDeps.cmake)). Без сети: `-DMODKIT_DEPS_DIR=<папка>` с `zstd/`, `lz4/`, `nlohmann/nlohmann/json.hpp` и
`borealis/` внутри.

Через Docker вместо devkitPro на машине:

```sh
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64:20260219 sh -c 'dkp-pacman -Sy --needed --noconfirm switch-dev switch-cmake switch-pkg-config switch-zlib switch-curl switch-glfw switch-mesa switch-libdrm_nouveau switch-glm && cmake -B build -DPLATFORM_SWITCH=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build -j'
```

Тесты ядра на ПК: `cmake -S tests/host -B build-host && cmake --build build-host -j && ctest --test-dir build-host`.

Исходники модуля для игр — в `nxredirect/` (GPL-2.0). Модули из `romfs/modules/` можно пересобрать и побайтно
сверить с поставленными (Linux или WSL, devkitA64, make, rsync):
`bash nxredirect/build-romfs-modules.sh <папка-выхода> --check romfs/modules`.

## Языки интерфейса

English, Русский, Українська. Переводы лежат в `resources/i18n/<язык>/`. Как добавить свой язык —
[`TRANSLATING.md`](../../TRANSLATING.md).
