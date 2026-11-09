#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build-diag — ДИАГНОСТИЧЕСКАЯ сборка nxredirect (-DNXR_DIAG).

Зачем. Обычная сборка на консоли запускается, игра работает, но файлы мода не
подхватываются. Диагностическая версия — тот же модуль, собранный с -DNXR_DIAG:
за один запуск игры она пишет на SD отчёт о том, что именно не сработало.

Отчёт — в папке игры на SD, новый на каждый запуск:
    sd:/switch/ModKitNX/logs/<TitleID>/diag-NNNN.txt
(NNNN — номер запуска 0001, 0002, …; хранятся последние 5; первая строка —
TitleID, BuildID main и номер). Прежние сборки писали nxredirect-diag.txt в корень.

В отчёте: слот, из которого загрузился модуль; TitleID от ядра; собранный путь
папки мода; все модули, найденные своим обходом памяти; адреса символов;
результат MountSdCard; проба папки мода; первые 60 путей nn::fs::OpenFile с
пометкой «подменено ДА/НЕТ».

Если отчёт писать нечем, модуль НАМЕРЕННО падает по узнаваемому адресу, видимому
в crash report Atmosphère (0xDEAD0001…0xDEAD0005, 0xD1MMPPSS) — таблица кодов
кладётся в архив в ЧИТАТЬ.txt. Этих аварий в обычной сборке нет.

Шаги те же семь, что у build.py (общие, живут в nxr_build.py), но:
  - модуль собирается с -DNXR_DIAG и кладётся в out-diag/;
  - гейт check_nso.py запускается с --diag (требует символы записи и папок и шаблон
    пути отчёта);
  - архив называется
    dist/nxredirect_<игра>_DIAG_ЛИЧНАЯ-с-npdm-из-дампа_<ГГГГММДД-ЧЧММ>.zip
    (в имени сказано, что внутри main.npdm из дампа игры — такой архив не раздают).
Обычная сборка (out/, архив без DIAG) при этом не трогается.

Запуск:
    python build-diag.py --game-exefs <папка exefs игры> --mod-files <папка мода>
    python build-diag.py ... --skip-build / --no-zip / --slot subsdkN
    python build-diag.py ... --diag-gfx     # плюс ДИАГНОСТИКА ГРАФИКИ: отдельный
                                            # отчёт .../logs/<TitleID>/gfx-NNNN.txt про путь
                                            # вывода картинки (nn::vi/EGL/NVN).
                                            # Ключ независим от -DNXR_DIAG и есть
                                            # также у build.py
    python build-diag.py --self-test        # пробы гейтов + «сборки не перепутаны»
    python build-diag.py --quick-self-test  # то же, короче

Требуется: Windows + WSL Ubuntu с devkitPro в /opt/devkitpro, Python 3.8+.
Ничего не пишет на SD и на консоль, в сеть не ходит.
"""

import argparse
import sys

from nxr_build import (DEFAULT_SLOT, DIAG_CRASH_CODES, DIAG_REPORT_NAME, GFX_FAIL_MARK,
                       GFX_REPORT_NAME, NXR_VERSION,
                       add_common_args, emit, events_open, say, self_test_common,
                       self_test_not_mixed, self_test_verdict, setup_stdio,
                       variant_from_args)


def main():
    # Первой строкой: вывод по-русски не должен падать, когда stdout — канал.
    setup_stdio()
    ap = argparse.ArgumentParser(description="Диагностическая сборка nxredirect (-DNXR_DIAG)")
    add_common_args(ap)
    args = ap.parse_args()

    events_open(args.events)
    emit("RUN_START", "запуск диагностической сборки", tool="build-diag.py",
         version=NXR_VERSION,
         mode=("self-test" if (args.self_test or args.quick_self_test) else "build"),
         diag=True, diag_gfx=bool(args.diag_gfx), share=bool(args.share),
         slot=(args.slot or DEFAULT_SLOT))

    if args.self_test or args.quick_self_test:
        say("=== самопроверка гейтов диагностической сборки ===", code="SELFTEST_START",
            full=bool(args.self_test and not args.quick_self_test), diag=True)
        ok = self_test_common(args.game_exefs, full_hosttest=not args.quick_self_test)
        ok = self_test_not_mixed(args.game_exefs) and ok
        ok = self_test_verdict(ok)
        sys.exit(0 if ok else 1)

    variant = variant_from_args(args, diag=True)
    sha = variant.run(skip_build=args.skip_build, no_zip=args.no_zip)
    if args.no_zip:
        emit("DONE", "раскладка готова, архив не собирался", action="build-diag",
             applied=True, zip=None, rc=0)
        return
    say("")
    say("ГОТОВО — это ДИАГНОСТИЧЕСКАЯ сборка, не рабочая.")
    say("Архив: " + str(variant.zip_path))
    say("Распаковать в КОРЕНЬ SD-карты, запустить игру, выйти из неё и забрать")
    say("с карты последний (с наибольшим номером) файл "
        + DIAG_REPORT_NAME.replace("<TitleID>", variant.tid or "<TitleID>") + ".")
    if variant.diag_gfx:
        say("Рядом появится второй отчёт — "
            + GFX_REPORT_NAME.replace("<TitleID>", variant.tid or "<TitleID>") + " (диагностика")
        say("графики). Строки, начинающиеся с «" + GFX_FAIL_MARK + "», — подозреваемые.")
    if not variant.share:
        say("АРХИВ НЕЛЬЗЯ РАЗДАВАТЬ: внутри exefs/main.npdm из дампа игры.")
    say("Если игра упала — нужен crash report Atmosphère; коды аварий:")
    for code, meaning in DIAG_CRASH_CODES:
        say(f"    {code} — {meaning}")
    say("sha256 архива: " + (sha or "-"), code="DONE", action="build-diag",
        zip=str(variant.zip_path), sha256=sha, tid=variant.tid, slot=variant.slot,
        share=variant.share, diag=True, diag_gfx=variant.diag_gfx, rc=0)


if __name__ == "__main__":
    main()
