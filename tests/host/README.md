# tests/host — хост-тесты ядра и побайтная сверка с эталоном

## Что здесь

| Файл | Что |
|---|---|
| `CMakeLists.txt` | библиотека ядра `mkcore` (+ zstd, lz4 — `cmake/ModkitDeps.cmake`), `mkhost` (ФС ПК), `core_cli`, `unit_tests`, `nca_tests`, `app_update_tests`; ctest — `unit`, `nca` и `app_update` |
| `host_fs.*` | `IFileSystem`/`IClock` на `std::filesystem` (запись через `.part` → rename; поверх существующего — только `*_replace`, как на FAT консоли) |
| `unit_tests.cpp` | синтетика без файлов игр: SHA-256, таблица BIC, кадры zstd/ZBIC, LZ4, npdm, квитанция (текст как `json.dumps` эталона), DEFLATE и NACP, имена папок, правила ZBIC и sdk, исполнитель и снятие, свои без `.bak` и вид сборки, логи (отчёты, выбор последнего отчёта о падении по датам и именам, сборка и zip), sdk из установленных игр (`mk::harvest`: кэш, выбор кандидатов, имя папки, сохранение); негативные пробы; затвор нуля — 80 проверок (сейчас — 146) |
| `app_update_tests.cpp` | обновление приложения и настройки — переносимая часть `platform/` (`app_update.cpp`, `app_config.cpp`) на подставных ответах GitHub и собранных в памяти `.nro`, без сети: версии, разбор релиза и sha256, «что нового», решение показать окно, проверка `.nro` (NACP, размер, sha256), `.new`/`.new.part`/`.old`/`.new.stale`, `config.json` (сейчас — 88 проверок) |
| `fixture_deflate.h` | эталон raw DEFLATE (динамический Хаффман), сгенерирован Python zlib из псевдослучайного текста |
| `core_cli.cpp` | ядро как команда: `install` (+ `--diag`, `--diag-gfx`), `uninstall`, `status`, `zbic-*`, `npdm-*`, `forecast`/`detect`/`profile`, `sdk-scan`, `sdk-swap`, `nacp`, `classify`, `nca-sdk` |
| `compare_pc.py` | сверка `core_cli` с эталоном (`install.py`/`zbic_nso.py`/`npdm_*.py`/`stubdiff.py`) на настоящих входах |

## Запуск

```sh
# зависимости (zstd, lz4, json) CMake скачивает сам — закреплённые версии с SHA-256 (cmake/ModkitDeps.cmake);
# уже скачанные: -DMODKIT_DEPS_DIR=<папка> (тогда без сети)
cmake -S tests/host -B <папка сборки> && cmake --build <папка сборки> -j && ctest --test-dir <папка сборки>
```
Папку сборки в WSL лучше держать в домашней папке дистрибутива: `/tmp` WSL
очищается при перезапуске.

Сверка с эталоном — эталонной реализацией на Python (`install.py` и скрипты `nxredirect/scripts/`), по которой выверено
ядро (`install.py` в репо нет; путь задаётся переменной `MK_PIPELINE`; входы — только чтение, пути задаются средой):
```sh
MK_PIPELINE=<эталон: папка nxredirect> MK_MODULES=<эталон: папка modules> MK_GAMES=<эталон: game-exefs-autofix> \
MK_SDK=<папка nnSdk> MK_MOD=<мод TotK UltraCam> MK_EXTRA=<ещё exefs> MK_WEB_NCA=<эталон: dump/nca.py> \
MK_WORK=<временная папка> MK_CLI=<путь к core_cli внутри WSL> py -3 tests/host/compare_pc.py
```
Скрипт копирует эталон в `MK_WORK/run-<время>/pipeline` (его `install.py --apply` пишет в свой `out/`),
ставит одни и те же случаи эталоном и ядром в разные папки и сравнивает: деревья (пути + sha256, `.bak-<время>`
нормализуется), квитанцию **текстом** (время берётся из квитанции эталона; `mod.source` — путь в разных ОС),
квитанцию консоли (= эталон + `installed_by` в конце; случаи «обычная → отладочная → обычная» и «отладочная с графикой
→ обычная»: ни одного `.bak`, `own_backups` пуст, `module_variant` как у эталона), выход ядра — гейтами эталона (`zbic_nso.py --check`,
`npdm_syscalls.py --check`). Ничего не удаляет; код 0 — всё сошлось, 1 — расхождения, 2 — нет входов.
