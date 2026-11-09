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

## 2. Что ещё нужно от ядра (просьбы платформы и UI)

Сверено по заголовкам `core/include/mk/`: п. 1 — есть `ProgressFn` в
`apply_plan` (`plan.hpp`); п. 2 — строки ядра ключами (`mk::Msg`, `core.hpp`); п. 3 — `installed_matches_game`
(`install.hpp`); п. 4–5 — `scan_sdk(…, max_depth = 2, rejected)` (`sdk.hpp`). Подключение в мосте и UI не сверялось.

1. **Прогресс записи.** `apply_plan` без обратного вызова: на экране прогресса полоса шагает только «план 1 из 2».
   Нужен `ProgressFn(size_t i, size_t n, const Action&)` в `apply_plan` (или перегрузка) — снятие ZBIC с `main` в
   80 МБ идёт долго, человек должен видеть, что идёт работа.
2. **Строки плана на английском.** `Plan::lines` / `warnings` и `Error::what()` — только RU. Для EN-интерфейса нужен
   ключ i18n на строку (например `lines_keys` с аргументами) или английский текст рядом. Пока EN-интерфейс
   показывает их по-русски.
3. **«Игра обновилась после установки».** `InstalledState::module_matches` сравнивает модуль, а не игру. Нужен
   признак «ModuleId `main` из квитанции ≠ текущему `main` exefs» — тогда экран скажет «установите ещё раз».
4. **Отбракованные файлы в папках sdk.** `scan_sdk` молча пропускает не-nnSdk (OpenGL, movie…). Нужно
   показывать их в «Подробностях»: нужен список пропущенных (путь + имя модуля), можно отдельной функцией.
5. **Глубина обхода sdk.** Нужны подпапки на 1–2 уровня; `scan_sdk` обходит рекурсивно без предела. На большой
   карте (`DBI/dumps` с полными дампами) это долго — нужен параметр глубины (у нас был бы 2).
6. **Отладочная сборка.** `InstallOptions` не знает о диагностике — мост сам выбирает вариант модуля из описи
   (`autofix && diag`, для мода с модулем — ещё `early`). Если ядру нужно писать в квитанцию, что стоит отладочный
   вариант, — нужно поле (например `module_variant`).
7. **`installed_by`.** Мост передаёт `"console"` — как и ждёт формат квитанции.

## 3. Что делает платформа, а не ядро

Список игр (ns), control data и иконка, ncm (program NCA, патч приоритетнее базы, required_system_version),
расшифровка заголовка NCA ключом от `spl` и `sdk_addon_version`, открытие exefs (Code FS → запасной путь → «нужны
ключи»), прошивка, Atmosphère, emuMMC, наличие `prod.keys` (без чтения), журнал, разбор отчётов отладки
(`platform/diag_report`).
