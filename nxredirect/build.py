#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build — обычная (рабочая) сборка универсального мод-редиректора nxredirect.

Что делает по шагам (сами шаги общие, живут в nxr_build.py):
  1. собирает модуль в WSL (devkitPro /opt/devkitpro), слот задаётся --slot;
  2. гоняет хост-тесты hosttest/run_hosttest.sh — гейт;
  3. гоняет scripts/check_nso.py по собранному NSO — гейт;
  4. делает main.npdm: бит SdCard (npdm_sdcard.py) + вызовы 0x74/0x75
     (npdm_syscalls.py), затем гейт --check;
  5. раскладывает out/atmosphere/contents/<TID>/{exefs,files};
  6. пакует dist/nxredirect_<игра>_<ГГГГММДД-ЧЧММ>.zip с паспортом СБОРКА.txt;
  7. проверяет архив: нет romfs/romfs.bin, есть exefs и files, размеры совпали.

TitleID нигде не вписывается: модуль спрашивает его у ядра во время работы
(svcGetInfo ProgramId). Здесь TID нужен только для имени папки в раскладке и
берётся из main.npdm дампа игры.

Любой гейт при провале роняет сборку (код возврата 1).

Запуск:
    python build.py --game-exefs <папка exefs игры> --mod-files <папка файлов мода>
    python build.py ... --game tits2          # имя игры в имени архива
    python build.py ... --slot subsdk5        # другой слот
    python build.py ... --skip-build          # без пересборки (раскладка и архив)
    python build.py ... --no-zip              # только раскладка
    python build.py ... --share               # архив ДЛЯ РАЗДАЧИ: без main.npdm
                                              # (он из дампа игры), но со скриптами
                                              # правки прав и инструкцией
    python build.py ... --sdk <библиотека>     # сверить импорты игры с её экспортами:
                                              # нужен ли вариант для подменённой sdk
    python build.py ... --swapped-sdk         # КОМПЛЕКТ для консоли с ПОДМЕНЁННОЙ
                                              # библиотекой sdk: модуль + main.npdm +
                                              # несжатый main (если ZBIC у игры есть)
    python build.py ... --no-autofix          # модуль БЕЗ автоисправления
                                              # (по умолчанию оно ВКЛЮЧЕНО — экспорты-прокси
                                              # сами решают на консоли, своя функция или
                                              # настоящая из sdk; пишется паспорт консоли)
    python build.py ... --cnmt <файл>         # required_system_version игры в профиль
    (--compat-hash, --compat-ldn, --compat-nvrm — УСТАРЕЛИ, ничего не меняют)
    python build.py ... --diag-gfx            # ДИАГНОСТИКА ГРАФИКИ: модуль
                                              # перехватит nn::vi/EGL/NVN и напишет
                                              # отчёт sd:/switch/ModKitNX/logs/<TID>/
                                              # gfx-NNNN.txt. Это проба, а не рабочий мод;
                                              # собирается в ОТДЕЛЬНУЮ папку
                                              # (out/module...-gfxdiag), архив идёт
                                              # с пометкой GFXDIAG.
    python build.py ... --diag-gfx --diag-lock  # ДОБАВИТЬ узкую пробу блокировок:
                                              # один перехватчик pthread_rwlock_rdlock,
                                              # работающий только внутри
                                              # eglCreateWindowSurface и только в том
                                              # же потоке. По умолчанию ВЫКЛЮЧЕНА:
                                              # прежняя, широкая проба (пять целей)
                                              # однажды сломала запуск игры —
                                              # модуль вышел за пул трамплинов
                                              # exlaunch, и тот аварийно завершил её
    python build.py --self-test               # негативные пробы гейтов
    python build.py --quick-self-test         # то же, короче (первые 2 поломки хост-теста вместо всех)

Требуется: Windows + WSL Ubuntu с devkitPro в /opt/devkitpro, Python 3.8+.
Ничего не пишет на SD и на консоль, в сеть не ходит.
"""

import argparse
import sys

from nxr_build import (DEFAULT_SLOT, GFX_FAIL_MARK, GFX_REPORT_NAME, NXR_VERSION,
                       add_common_args, emit, events_open, say, self_test_common,
                       self_test_verdict, setup_stdio, variant_from_args)


def main():
    # Первой строкой: вывод по-русски не должен падать, когда stdout — канал.
    setup_stdio()
    ap = argparse.ArgumentParser(description="Сборка nxredirect (рабочая версия)")
    add_common_args(ap)
    args = ap.parse_args()

    events_open(args.events)
    emit("RUN_START", "запуск сборки", tool="build.py", version=NXR_VERSION,
         mode=("self-test" if (args.self_test or args.quick_self_test) else "build"),
         diag=False, diag_gfx=bool(args.diag_gfx), share=bool(args.share),
         slot=(args.slot or DEFAULT_SLOT))

    if args.self_test or args.quick_self_test:
        say("=== самопроверка гейтов (негативные пробы) ===", code="SELFTEST_START",
            full=bool(args.self_test and not args.quick_self_test), diag=False)
        ok = self_test_common(args.game_exefs, full_hosttest=not args.quick_self_test)
        ok = self_test_verdict(ok)
        sys.exit(0 if ok else 1)

    variant = variant_from_args(args, diag=False)
    sha = variant.run(skip_build=args.skip_build, no_zip=args.no_zip)
    if args.no_zip:
        emit("DONE", "раскладка готова, архив не собирался", action="build",
             applied=True, zip=None, rc=0)
        return
    say("")
    say("ГОТОВО. Архив: " + str(variant.zip_path))
    say("Его содержимое распаковать в КОРЕНЬ SD-карты.")
    if variant.diag_gfx:
        say("")
        say("ВНИМАНИЕ: в сборке ВКЛЮЧЕНА ДИАГНОСТИКА ГРАФИКИ — это проба, а не")
        say("рабочий мод. Запустить игру, выйти и забрать с карты последний файл")
        say(GFX_REPORT_NAME.replace("<TitleID>", variant.tid or "<TitleID>") + ". Строки, начинающиеся с «" + GFX_FAIL_MARK
            + "», — подозреваемые:")
        say("вызов вернул неуспех; для EGL рядом дописан код eglGetError.")
        say("Когда проба закончена, поставьте обычную сборку (без --diag-gfx).")
    if variant.share:
        say("Это архив ДЛЯ РАЗДАЧИ: main.npdm внутрь НЕ вложен (он из дампа игры),")
        say("вместо него — скрипты правки прав и инструкция, как сделать свой.")
    else:
        say("ЭТОТ АРХИВ НЕЛЬЗЯ РАЗДАВАТЬ: внутри exefs/main.npdm из дампа игры.")
        say("Для раздачи соберите вариант без него: python build.py --share")
    say(f"Папки atmosphere/contents/{variant.tid}/romfs и файла romfs.bin на карте")
    say("быть не должно — иначе включится LayeredFS и тяжёлые игры зависнут.")
    say("sha256 архива: " + (sha or "-"), code="DONE", action="build",
        zip=str(variant.zip_path), sha256=sha, tid=variant.tid, slot=variant.slot,
        share=variant.share, diag=False, diag_gfx=variant.diag_gfx, rc=0)


if __name__ == "__main__":
    main()
