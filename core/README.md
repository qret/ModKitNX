# core — переносимое ядро ModKitNX (C++17, без libnx)

Выход ядра побайтно совпадает с эталонной реализацией на Python (её скрипты проверки — `nxredirect/scripts/`;
сверка — `tests/host/compare_pc.py`).
Ввод-вывод — только через интерфейсы `mk::IFileSystem`, `mk::IHash`, `mk::IClock` (`include/mk/core.hpp`).
Ошибки — исключение `mk::Error` с постоянным кодом (`code()`, коды как у событий эталона: `FAIL_NO_FREE_SLOT` и т. п.).

## Что даёт платформе и UI (заголовки `include/mk/`)

| Заголовок | Что |
|---|---|
| `core.hpp` | `Bytes`, `Error`, `IFileSystem` (read/list/write атомарно/copy; поверх существующего — только явные `write_file_replace`/`copy_file_replace`; rename/remove_file/remove_empty_dir), `IHash`, `IClock`, `Env`, `soft_hash()` |
| `nso.hpp` | заголовок NSO0, сегменты (LZ4/ZBIC), `NsoImage` (символы, `module_name`, `mw_version`) |
| `zbic.hpp` | `zbic_scan` (по содержимому), `zbic_rebuild` (несжатый NSO как `zbic_nso.rebuild`), `zbic_check` (13 проверок), `zbic_need` (нужно ли снимать: HOS/AMS) |
| `npdm.hpp` | `make_game_npdm` (SdCard + svc 0x74/0x75), `make_mod_npdm` (as-is/merged), `Npdm` |
| `mod.hpp` | `find_mod_tree`, `analyze_mod` (romfs → files/, модуль мода, npdm мода, IPS, прочее) |
| `install.hpp` | `plan_install` (вкл. `swap_sdk`, `diag`, `diag_gfx`; строка плана «отладочный модуль заменится обычным» и обратно), `module_variant`, `plan_zbic_only`, `inspect_installed`, `installed_matches_game` («игра обновилась после установки»), `plan_uninstall`, `classify_nso` |
| `plan.hpp` | `Plan`/`Action` и исполнитель `apply_plan` (+ прогресс `ProgressFn`; учёт своего как `Ledger` эталона; гейты после записи) |
| `receipt.hpp` | квитанция `nxredirect.json`: формат 1 эталона 1:1 (вплоть до `sdk_swap`, `diag`, `diag_gfx`, `module_variant`, `owned`, `own_backups`, `foreign_backups`, `created_dirs`) + `installed_by` в конце |
| `logs.hpp` | логи по играм: отчёты модуля `logs/<TID>/diag-NNNN.txt`/`gfx-NNNN.txt` (`list_reports`, первая строка `parse_report_head`), последний отчёт Atmosphère о падении (`pick_last_crash`: по дате изменения, при равенстве или без даты — по числу в начале имени; TitleID — из имени, иначе из текста), «Собрать логи для отправки» (`plan_logs_bundle` / `write_logs_bundle`: папка `<Игра> v<версия>/` и zip без сжатия, `zip_store`) |
| `sdk.hpp` | `identify_sdk`, `scan_sdk` (два места, без повторов), `sdk_profile/forecast/detect`, прогноз по частям `forecast_game` × `forecast_sdk` → `forecast_fixes` (экран «Что исправит модуль»; тот же `decide`, `sdk_forecast` считается через них), `evaluate_sdk_status`, `plan_swap_sdk/plan_unswap_sdk` |
| `sdk_harvest.hpp` | «Найти sdk в других играх» (`mk::harvest`): sdk одной игры (`probe_sdk_file`), кэш проверенных игр `cache/sdk-scan.json` (`load_cache`/`save_cache`, `cache_hit` по признаку содержимого игры — изменилась игра, проверяется снова), выбор кандидатов для прошивки (`select_candidates`: рекомендованная по таблице ядра первой, затем ближайшие; одинаковые — одной строкой; есть sdk с мажором прошивки — младшие не показываются), имя папки `nnSdk <версия> (<игра>)` (`folder_name`), сохранение `sdk` + `source.json` через `.part` (`save_candidate`, sha256 сверяется с найденным) и подпись «из игры …» (`read_source`). Ничего не скачивается |
| `nacp.hpp` | `parse_nacp` (+ сжатая таблица SDK 20+), `nca_sdk_addon_version`, `safe_folder_name` |

Свои файлы при переустановке перезаписываются начисто,
без `.bak`; `own_backups` новых записей не получает, записи прежних квитанций переносятся и снимаются удалением по
sha256. Чужой файл, как и прежде, сначала копируется в `.bak-<время>` (`foreign_backups`), затем перезаписывается.
`main.npdm` при сохранённом нами `main.npdm.orig` — свой (`Action::ours_if_owned`).

Правило «план, потом запись»: функции `plan_*` ничего не пишут, возвращают `Plan` (`lines` — что будет
сделано, `warnings` — что не трогаем и почему). UI показывает план и после подтверждения зовёт `apply_plan`.

Снятие (настоящее удаление): удаляется только
названное в квитанции и только при совпавшем sha256 (`owned`, `module_sha256`, `npdm_sha256`, `sdk_swap.sha256`,
`own_backups`); без sha256 или изменённое — не трогается и попадает в `warnings`; `main.npdm.orig` → `main.npdm`;
чужие файлы из `foreign_backups` (и `sdk_swap.foreign_backup`) возвращаются на место; пустые `created_dirs`
убираются; квитанция удаляется последней.

Что платформа должна реализовать: `IFileSystem` поверх fsdev/`fsFs*` (пути с префиксом тома: `sdmc:/…`,
смонтированная Code FS игры, `romfs:/…`), запись через `<путь>.part` → rename (цель есть — ошибка; `*_replace` —
прежний убирается после полной записи `.part`: на FAT/exFAT rename поверх файла не работает); `IClock` по `timeGetCurrentTime`;
по желанию — `IHash` на `sha256CalculateHash`. Расшифровка заголовка NCA — платформа (spl), ядро разбирает
уже расшифрованный.

## Зависимости (в git не лежат; закреплённые версии с SHA-256 скачивает CMake — `cmake/ModkitDeps.cmake`)

- zstd 1.5.7 (BSD): `lib/common/*.c` кроме `entropy_common.c` + `lib/decompress/*.c`, с `-DZSTD_DISABLE_ASM`.
  `entropy_common.c` включается в `src/zstd_bic_glue.c` с переименованием `FSE_readNCount*` — так все чтения
  таблиц частот проходят через наш диспетчер (BIC для кадров ZBIC, обычный разбор для zstd). Upstream не правится.
- lz4 1.10.0 (BSD-2): `lib/lz4.c`.
- nlohmann/json 3.11.3 (MIT): `nlohmann/json.hpp`.

Каталог автоисправления `src/nxr_autofix_catalog.h` — копия файла модуля
(`nxredirect/src/nxr_autofix_catalog.h` этого репо, GPL-2.0, участники nxredirect); хост-тест сверяет копию с оригиналом (одно отличие — личный путь в комментарии
заменён нейтральным текстом).

## Сборка и проверка на ПК

`tests/host/` (CMake): библиотека ядра, `core_cli`, юнит-тесты и побайтная сверка с эталоном
(`tests/host/compare_pc.py`). Как запускать — `tests/host/README.md`.
