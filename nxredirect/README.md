# nxredirect

**English** · [Русский](#русский)

nxredirect is an Atmosphère **exefs module** (`subsdkN`) for Nintendo Switch games: it intercepts the game's
file opens on its romfs and serves the file from the SD card instead —
`atmosphere/contents/<TitleID>/files/<path>` — when such a file exists, without LayeredFS.
It is part of ModKitNX: the prebuilt modules ship in `romfs/modules/<variant>/subsdk`, and the application
copies the chosen variant to `atmosphere/contents/<TitleID>/exefs/subsdkN` on the console.
The TitleID is not compiled in: the module asks the kernel at run time.

## License

GPL-2.0 — see [`LICENSE`](LICENSE). The module is based on
[exlaunch](https://github.com/shadowninja108/exlaunch) (GPL-2.0); `exlaunch/` is a copy of it with our changes.
`src/nxr_autofix_catalog.h` is GPL-2.0-or-later: the app ModKitNX (GPL-3.0-or-later) includes it too.
Bundled third-party parts keep their own notices: `exlaunch/source/rtld/` (oss-rtld, ISC — `LICENSE.txt` there),
`exlaunch/source/lib/hook/nx64/hook_impl.cpp` (And64InlineHook, MIT — header of the file).
This folder is the complete corresponding source of the binaries in `romfs/modules/`.

## Layout

| path | what |
|---|---|
| `src/` | the module itself (`main.cpp` and headers) |
| `exlaunch/` | exlaunch framework, Makefile, linker script; `exlaunch/source/program/main.cpp` includes `../../../src/main.cpp` |
| `build-romfs-modules.sh` | builds every variant of `romfs/modules/` and can compare them with its `manifest.json` |
| `scripts/`, `hosttest/` | NSO checks (`scripts/check_nso.py`) and host tests of the module logic |
| `nxr_build.py`, `build.py`, `build-diag.py` | the original build-and-gate scripts (Windows + WSL) the modules were produced with |

## Build

Requirements: Linux or WSL (Ubuntu), [devkitPro](https://devkitpro.org/wiki/Getting_Started) with devkitA64
(`DEVKITPRO`, default `/opt/devkitpro`), `make`, `rsync`; Python 3 for the checks.

All variants, bit-identical to `romfs/modules/` (run from the repository root):

```sh
bash nxredirect/build-romfs-modules.sh ~/nxr-out --check romfs/modules
bash nxredirect/build-romfs-modules.sh ~/nxr-out autofix          # one variant
python3 nxredirect/scripts/check_nso.py ~/nxr-out/autofix/subsdk --game-exefs <unpacked game exefs> --compat-nvrm
```

By hand, one variant (the build directory must not contain spaces; use a fresh one per variant —
make compares file times, not flags):

```sh
export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
mkdir -p ~/nxr-build && cp -r nxredirect/exlaunch nxredirect/src ~/nxr-build/
cd ~/nxr-build/exlaunch
make MODULE_NAME=subsdk9 "C_FLAGS=-Wno-error=non-c-typedef-for-linkage <variant flags>"
# result: deploy/subsdk9 — the NSO module (deploy/main.npdm is the exlaunch template one, not used)
```

Code and data do not depend on the build directory, but the GNU build-id (NSO `ModuleId`) does: it hashes
the whole ELF including debug paths. The script maps its temporary directory to the original one
(`-ffile-prefix-map`, `--debug-prefix-map`), so its output matches `romfs/modules/` byte for byte;
a plain manual build differs from it only in those 20 bytes of build-id.

## Variants

| `romfs/modules/` | flags | what for |
|---|---|---|
| `autofix/subsdk` | — | main module: on the stock sdk calls go to its functions, on a replaced sdk its own fixes are used; writes a console passport to the SD |
| `autofix-earlyfs/subsdk` | `-DNXR_EARLY_FS_HOOK` | the same, with the file hook installed at load time — for mods that bring their own module |
| `plain/subsdk` | `-DNXR_NO_AUTOFIX` | no auto-fix, no passport — troubleshooting |
| `diag/subsdk` | `-DNXR_DIAG -DNXR_NO_AUTOFIX` | diagnostic: report `sd:/switch/ModKitNX/logs/<TitleID>/diag-NNNN.txt` |
| `diag-gfx/subsdk` | `-DNXR_DIAG -DNXR_NO_AUTOFIX -DNXR_DIAG_GFX` | diagnostic + graphics report `…/logs/<TitleID>/gfx-NNNN.txt` (black screen) |
| `plain-gfx/subsdk` | `-DNXR_NO_AUTOFIX -DNXR_DIAG_GFX` | graphics report only |
| `autofix-diag/subsdk` | `-DNXR_DIAG` | main module + diagnostic report |
| `autofix-diag-gfx/subsdk` | `-DNXR_DIAG -DNXR_DIAG_GFX` | main module + both reports |
| `autofix-diag-earlyfs/subsdk` | `-DNXR_DIAG -DNXR_EARLY_FS_HOOK` | early hook + diagnostic report |

All are built with `MODULE_NAME=subsdk9`; the file on the console gets the free slot name `subsdkN`.

Reports of the diagnostic variants (`src/nxr_logs.hpp`): one new file per game start in
`sd:/switch/ModKitNX/logs/<TitleID>/` — `diag-NNNN.txt` and `gfx-NNNN.txt`, the last 5 of each kind are kept; the
first line is `TitleID=<16 HEX> BuildID=<40 HEX|?> report=<kind>-<NNNN>`, the time is written inside. Before that
date the module wrote one file to the card root (`sd:/nxredirect-diag.txt`, `sd:/nxredirect-gfx.txt`).
Sizes and sha256 are in `romfs/modules/manifest.json` and `romfs/modules/SOURCE.md`.

---

## Русский

nxredirect — **exefs-модуль** Atmosphère (`subsdkN`) для игр Nintendo Switch: перехватывает открытие файлов
игры в её romfs и, если на карте есть файл `atmosphere/contents/<TitleID>/files/<путь>`, отдаёт его — без LayeredFS.
Модуль входит в ModKitNX: готовые модули лежат в `romfs/modules/<вариант>/subsdk`, приложение копирует выбранный
вариант в `atmosphere/contents/<TitleID>/exefs/subsdkN` на консоли. TitleID в модуль не вшит — он спрашивает его у ядра.

**Лицензия** — GPL-2.0 ([`LICENSE`](LICENSE)); основано на [exlaunch](https://github.com/shadowninja108/exlaunch)
(GPL-2.0), `exlaunch/` — его копия с нашими правками. `src/nxr_autofix_catalog.h` — GPL-2.0-or-later: его включает и
приложение ModKitNX (GPL-3.0-or-later). Сторонние части сохраняют свои уведомления:
`exlaunch/source/rtld/` (oss-rtld, ISC), `exlaunch/source/lib/hook/nx64/hook_impl.cpp` (And64InlineHook, MIT).
Эта папка — полный исходный код двоичных файлов из `romfs/modules/`.

**Сборка** — Linux или WSL (Ubuntu), devkitPro с devkitA64 (`DEVKITPRO`, по умолчанию `/opt/devkitpro`), `make`,
`rsync`; для проверок — Python 3. Из корня репозитория:

```sh
bash nxredirect/build-romfs-modules.sh ~/nxr-out --check romfs/modules   # все варианты, сверка с описью
bash nxredirect/build-romfs-modules.sh ~/nxr-out autofix                 # один вариант
```

Вручную — как в английском разделе: `make MODULE_NAME=subsdk9 "C_FLAGS=-Wno-error=non-c-typedef-for-linkage <флаги>"`
в копии `exlaunch/` + `src/` (путь без пробелов, своя свежая папка на каждый вариант), результат — `deploy/subsdk9`.
Код и данные от папки сборки не зависят, а GNU build-id (`ModuleId` в заголовке NSO) зависит: он считается по ELF
вместе с путями отладочной информации. Скрипт подменяет путь временной папки на исходный, поэтому его результат
совпадает с `romfs/modules/` побайтно; ручная сборка отличается только 20 байтами build-id.

**Варианты** и их флаги — таблица выше: `autofix` (основной), `autofix-earlyfs` (ранний перехват, для мода со своим
модулем), `plain` (без автоисправления), `diag`, `diag-gfx`, `plain-gfx`, `autofix-diag`, `autofix-diag-gfx`,
`autofix-diag-earlyfs` (диагностические: отчёт на каждый запуск игры в `sd:/switch/ModKitNX/logs/<TitleID>/` —
`diag-NNNN.txt` и `gfx-NNNN.txt`, последние 5 каждого вида, первая строка `TitleID=… BuildID=… report=…`, время —
внутри; `src/nxr_logs.hpp`; в прежних сборках — один файл в корне карты `sd:/nxredirect-diag.txt`). Каждый
лежит в `romfs/modules/<вариант>/subsdk`; размеры и sha256 — в `romfs/modules/manifest.json` и `SOURCE.md`.
