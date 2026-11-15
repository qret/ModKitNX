# hbl — загрузчик ярлыка на домашнем экране

Загрузчик homebrew, который лежит в exefs ярлыка ModKitNX (Р-43): при первом запуске читает из romfs ярлыка
`/nextNroPath` и `/nextArgv` и запускает `sdmc:/switch/ModKitNX/ModKitNX.nro`; поддерживает `envSetNextLoad`
(перезапуск после обновления).

- Откуда: **sphaira** by ITotalJustice (репозиторий `NaGaa95/sphaira`, на него перенаправляет `ITotalJustice/sphaira`),
  папка `hbl/`, коммит `9bb5a32453093a3fb78d5a9add64b0b5010b1ed3` (последняя правка `hbl/` — `87c855a5`, 14.08.2026).
  Сам загрузчик — **nx-hbloader** (switchbrew), доработанный в sphaira чтением romfs.
- Лицензия: **ISC**, «Copyright 2017-2018 nx-hbloader Authors» — `nx-hbloader.LICENSE.md` рядом (уведомление
  обязано идти с копией). ISC совместима с GPL-3.0-or-later.
- Файлы взяты **без изменений** (sha256 совпадает с файлами коммита выше):

| Файл | sha256 |
|---|---|
| `source/main.c` | `ee3ad5918d6883a2d289da83b945928b3cca88f390d698e0e056f67256d5cc13` |
| `source/trampoline.s` | `2f11a79cc9a4a162dcef557274076b66480bb256f353d47cea1ef56ab8e11f4e` |
| `hbl.json` | `9c0f754ae3363aa4890ed9ca0212aec3015686ae0ceca707b79277198667da53` |
| `nx-hbloader.LICENSE.md` | `db88a9c4d4d3148cbe397ca0acb7886e7aebcc8b984d4ce3b949e9caabdc7223` |

- Сборка — верхний `CMakeLists.txt` (цель `modkit_hbl`), как `hbl/CMakeLists.txt` sphaira: C11 с расширениями,
  `VERSION="3.0.0"`, `-Wl,-wrap,exit`; `elf2nso` → `main`, `npdmtool hbl.json` → `main.npdm`; оба кладутся в
  `romfs:/forwarder/`. Бинарников в git нет — каждая сборка .nro собирает их из этих исходников.
- `main.npdm` правит приложение при создании ярлыка (TitleID, 36 бит, три ядра, ForceDebug на Atmosphère ≥ 1.8.0) —
  `core/src/forwarder.cpp` `patch_npdm`, как sphaira `owo.cpp` `patch_npdm`.
