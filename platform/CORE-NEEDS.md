# Что платформе и UI нужно от ядра (`core/`)

Первая версия этого файла описывала **предполагаемый** API
(когда `core/include/` ещё не было); после появления `core/include/mk/*.hpp` платформа и UI переписаны под
настоящий API ядра, а файл — под то, что осталось нужным.

## 1. Как платформа и UI пользуются ядром (сделано)

| Что | Где у нас | Функция ядра |
|---|---|---|
| ввод-вывод | `platform/src/core_fs.cpp` (`PosixFs`, `NxHash` — аппаратный SHA-256, `NxClock`) | `mk::IFileSystem`, `IHash`, `IClock`, `Env` |
| название, издатель, версия, сжатая таблица SDK 20+ | `platform/src/titles.cpp` | `mk::parse_nacp` |
| найденные sdk (два места, без повторов) | `platform/src/sdk_scan.cpp` | `mk::scan_sdk` (метки мест `modkit`, `dbi`) |
| вывод блока «sdk», рекомендация | `platform/src/sdk_compat.cpp` | `mk::evaluate_sdk_status`, таблица `mk::fw_sdk_table` (ОДНО место правила) |
| снимать ли ZBIC | `platform/src/zbic_policy.cpp` | `mk::zbic_need` (ОДНО место правила) |
| состояние игры | `source/core_bridge.cpp` `inspectGame` | `inspect_installed`, `zbic_scan`, `identify_sdk` (своя sdk игры и `exefs/sdk`) |
| моды к игре | `source/core_bridge.cpp` `findMods` | `find_mod_tree` по папкам `sdmc:/switch/<APP>/mods/` |
| планы | `source/core_bridge.cpp` `makePlan` | `plan_install` (модуль и ранний модуль — из `romfs:/modules/manifest.json` по свойствам), `plan_uninstall`, `plan_swap_sdk`, `plan_unswap_sdk` |
| запись | `source/core_bridge.cpp` `applyPending` | `apply_plan`; подмена sdk вместе с установкой — второй план, строится заново после записи установки |

Ошибки ядра (`mk::Error`) ловятся в мосте; код ищется в i18n `app/error/<КОД>` (переведены частые `FAIL_*`),
иначе показывается русский текст ядра.

## 2. Что ещё нужно от ядра

Сейчас — ничего: все семь прежних просьб исполнены (прогресс записи, строки ядра ключами, «игра обновилась
после установки», отбракованные файлы и глубина обхода sdk, вид сборки в квитанции, `installed_by`). Прежний текст —
`docs/archive/CORE-NEEDS-2026-11-09b.md`.

## 3. Что делает платформа, а не ядро

Список игр (ns), control data и иконка, ncm (program NCA, патч приоритетнее базы, required_system_version),
расшифровка заголовка NCA ключом от `spl` и `sdk_addon_version`, открытие exefs (Code FS → с патчем → сырой NCA через службы консоли
без `prod.keys`, `platform/exefs_raw`), прошивка, Atmosphère, emuMMC, наличие `prod.keys` (без чтения), журнал, разбор отчётов отладки
(`platform/diag_report`).
