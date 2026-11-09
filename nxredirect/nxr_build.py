#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
nxr_build — общие шаги сборки универсального мод-редиректора nxredirect.

Сам по себе ничего не собирает: это библиотека для двух запускаемых скриптов,
которые отличаются только тем, ЧТО собирают и куда кладут архив:

    build.py        обычная сборка              -> out/,      dist/nxredirect_<игра>[_…]_ЛИЧНАЯ-с-npdm-из-дампа_<время>.zip
    build-diag.py   диагностическая (-DNXR_DIAG) -> out-diag/, dist/nxredirect_<игра>_DIAG_ЛИЧНАЯ-с-npdm-из-дампа_<время>.zip

Семь шагов, одинаковых для обеих сборок (различия собраны в классе Variant,
чтобы проверки не расходились):

    1. сборка модуля в WSL (devkitPro в /opt/devkitpro, временная папка БЕЗ пробелов:
       путь проекта пробел содержит, а Makefile exlaunch его не переживает);
    2. хост-тесты hosttest/run_hosttest.sh — гейт;
    3. scripts/check_nso.py — гейт по собранному NSO;
    4. main.npdm игры: npdm_sdcard.py -> npdm_syscalls.py, затем гейт --check;
    5. раскладка out[-diag]/atmosphere/contents/<TID>/{exefs,files};
    6. упаковка архива от корня SD + СБОРКА.txt внутри;
    7. проверка архива: нет папки romfs, есть exefs и files, размеры совпадают.

Каждый гейт при провале РОНЯЕТ сборку (код возврата 1). Негативные пробы самих
гейтов — `--self-test` у build.py и build-diag.py.

Ничего не пишет на SD и на консоль, в сеть не ходит.
"""

import atexit
import datetime
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent

EXLAUNCH = ROOT / "exlaunch"
SRC = ROOT / "src"
SCRIPTS = ROOT / "scripts"
HOSTTEST = ROOT / "hosttest"

CHECK_NSO = SCRIPTS / "check_nso.py"
NPDM_SDCARD = SCRIPTS / "npdm_sdcard.py"
NPDM_SYSCALLS = SCRIPTS / "npdm_syscalls.py"
ZBIC_NSO = SCRIPTS / "zbic_nso.py"
# Наш распаковщик ZBIC на чистом Python: лежит рядом с zbic_nso.py и едет с ним
# в раздачу — иначе у получателя без pip-пакета распаковывать было бы нечем.
ZBIC_PURE = SCRIPTS / "zbic_pure.py"
RUN_HOSTTEST = HOSTTEST / "run_hosttest.sh"
CHECK_COMPAT_SYMS = HOSTTEST / "check_compat_syms.py"
NSO_MIN = HOSTTEST / "nso_min.py"
RUN_GFXTEST = HOSTTEST / "run_gfxtest.sh"
# Хост-тест ЗАТВОРА ФАЙЛОВОЙ СИСТЕМЫ (src/nxr_fsgate.hpp): модуль не трогает
# nn::fs раньше игры. Появился после того, как раннее монтирование SD
# сломало Well Dweller (Result 0x327602 на её nn::fs::SetAllocator).
RUN_FSGATETEST = HOSTTEST / "run_fsgatetest.sh"
GFX_TARGETS = HOSTTEST / "gfx_targets.py"
GFX_UNCHANGED = HOSTTEST / "gfx_unchanged.py"
GFX_HEADER = SRC / "nxr_gfx.hpp"

# ------------------------------------------------- совместимость с чужой `sdk`
#
# Когда на консоли вместо родной библиотеки игры грузится системная `sdk` другой
# версии, игре может не хватить символов, и связыватель роняет процесс.
#
# Сейчас — АВТОИСПРАВЛЕНИЕ (src/nxr_autofix.hpp, каталог
# src/nxr_autofix_catalog.h): каждая сборка модуля экспортирует все исправления
# каталога, но каждый экспорт — прокси, который в рантайме сам решает, звать
# настоящую функцию загруженной sdk или свою. Поэтому один модуль годится и для
# родной, и для подменённой библиотеки; выключается только целиком (--no-autofix,
# флаг -DNXR_NO_AUTOFIX) — тогда модуль как прежняя сборка без ключей.
# *Было:* «доэкспорт ТОЛЬКО по явному ключу: subsdkN ищется раньше sdk и
# перекрывает одноимённый символ у того, у кого библиотека родная». Опасность
# настоящая; теперь её снимает прокси, а не ключ. Ключи --compat-hash,
# --compat-ldn, --compat-nvrm приняты по-прежнему, но УСТАРЕЛИ (ничего не меняют,
# дают предупреждение COMPAT_DEPRECATED).
#
# Ключ -> (имя символа, флаг компиляции). Имена символов здесь и в
# hosttest/check_compat_syms.py обязаны совпадать; сверка — в self_test_compat().
COMPAT_SYMS = {
    "hash": "_ZNSt3__113__hash_memoryEPKvm",
    "ldn": "_ZN2nn3ldn11SetProtocolENS0_8ProtocolE",
}
# Прежние флаги компиляции по ключам. Модулем больше НЕ управляют (их ставит
# только хост-тест hash_test.cpp); оставлены как справка для старых событий.
COMPAT_CFLAGS = {
    "hash": "-DNXR_COMPAT_HASH_MEMORY",
    "ldn": "-DNXR_COMPAT_LDN_STUB",
}
# Автоисправление: по умолчанию ВКЛЮЧЕНО в C++; этот флаг его выключает.
NO_AUTOFIX_CFLAG = "-DNXR_NO_AUTOFIX"
AUTOFIX_CATALOG = SRC / "nxr_autofix_catalog.h"
RUN_AUTOFIX = HOSTTEST / "run_autofix.py"
RUN_AUTOFIXTEST = HOSTTEST / "run_autofixtest.py"
STUBDIFF = SCRIPTS / "stubdiff.py"
# Исправление NvRmMemQueryHandleParams для подменённой sdk (
# src/nxr_compat.hpp). Отдельно от COMPAT_SYMS: этот символ ЕСТЬ в любой sdk (у
# 19.3.13 — заглушка, у родной 22.2 — настоящая функция), то есть это не
# «недостающий символ», а замена поведения. Включается --compat-nvrm или в
# комплекте --swapped-sdk; гейт check_compat_syms.py --nvrm yes|no — в обе стороны.
NVRM_SYM = "NvRmMemQueryHandleParams"
# Справка: прежний флаг. Теперь модуль определяет его сам при включённом
# автоисправлении (src/nxr_compat_exports.hpp); конвейер его не передаёт.
NVRM_CFLAG = "-DNXR_COMPAT_NVRM_QUERY"
RUN_NVRMTEST = HOSTTEST / "run_nvrmtest.py"

# Что ждёт hosttest/check_compat_syms.py --expect для набора ключей.
COMPAT_EXPECT = {(): "none", ("hash",): "hash", ("ldn",): "ldn",
                 ("hash", "ldn"): "both"}

# Сжатие ZBIC (бит 7 флагов заголовка NSO0, HOS 22.0.0+): Atmosphère младше
# 1.12.0 такой модуль не загружает. Подробности и доказательства — в
# scripts/zbic_nso.py.
ZBIC_FLAG = 0x80

# Необязательная зависимость для снятия ZBIC. Записана здесь одним местом,
# чтобы версия/лицензия в документации и в подсказках не разъехались.
ZBIC_PKG = {"name": "zbic", "version": "1.0.1.post1", "license": "GPL-2.0",
            "author": "kinnay", "url": "https://github.com/kinnay/zbic"}

# Вызовы, которые exlaunch делает при установке хуков (Map/UnmapProcessMemory).
NPDM_SVCS = ["0x74", "0x75"]

# Слот по умолчанию: игры занимают subsdkN с начала, поэтому берём старший.
DEFAULT_SLOT = "subsdk9"

# Версия инструмента. Пишется в квитанцию установки (nxredirect.json), чтобы
# --status мог сказать, чем именно установлен модуль на карте.
NXR_VERSION = "1.1"

# Имя квитанции об установке внутри atmosphere/contents/<TID>/.
RECEIPT_NAME = "nxredirect.json"

# Флаг, который приходится повторять при переопределении C_FLAGS из командной
# строки make: у шаблона включён -Werror, а GCC 14 ругается на hook_impl.cpp.
BASE_C_FLAGS = "-Wno-error=non-c-typedef-for-linkage"

# Намеренные поломки логики, которые понимает run_hosttest.sh. Каждая обязана
# сделать хост-тест КРАСНЫМ — иначе тест «всегда зелёный».
HOSTTEST_BREAKS = ["mod0-limit", "no-mapped-check", "wrong-perm",
                   # общее имя "sd" (конфликт 0x7802 с UltraCam),
                   # перевёрнутое правило rtld, слот импорта без сравнения имени.
                   "shared-mount-name", "rtld-last-wins", "import-slot-any-symbol",
                   "fixed-prefix", "lower-hex", "no-self-guard",
                   "no-mount-blacklist", "no-mode-check", "import-ignores-shndx",
                   # узнавание точки romfs через MountRom — "content"
                   # всегда / реестр не читается / реестр берёт "save" / Unmount
                   # не вычёркивает.
                   "content-always", "rom-mount-ignored", "rom-mount-takes-system",
                   "unmount-keeps",
                   # поиск символов требует DT_HASH (причина №3). Был в
                   # run_hosttest.sh и раньше, но в этот список не попал —
                   # самопроверка его не гоняла. Краснеет на любой игре (блок [6]
                   # вырезает DT_HASH; на Trails ещё и блок [1]).
                   "needs-dt-hash"]
# weak-hash-check сюда НЕ входит сознательно: это не поломка логики, а
# ослабленный тест (блок [6] не собирается). Один он обязан быть ЗЕЛЁНЫМ (логика
# цела) — проба «должен покраснеть» на нём падала бы. Он нужен только в паре
# «needs-dt-hash+weak-hash-check» для ручной проверки, что на игре с DT_HASH у
# всех модулей (TotK) поломку ловит именно блок [6]; итог пары зависит от игры
# (на Trails/WD она красная из-за блока [1]), поэтому в общую самопроверку не годится.

# Имя архива со временем сборки: иначе рядом копятся одинаковые на вид файлы и
# непонятно, какой из них поставлен на карту. Время локальное, ГГГГММДД-ЧЧММ.
BUILD_STAMP = datetime.datetime.now().strftime("%Y%m%d-%H%M")

# Отчёты отладочных сборок: не в корень карты, а
# по играм, новый на каждый запуск, последние 5 каждого вида (src/nxr_logs.hpp):
#   sd:/switch/ModKitNX/logs/<TitleID>/diag-NNNN.txt и gfx-NNNN.txt
# NNNN — номер запуска (0001, 0002, …). Первая строка отчёта — TitleID, BuildID
# main и номер. Прежние сборки — nxredirect-diag.txt / -gfx.txt в корне карты.
LOGS_DIR = "switch/ModKitNX/logs/<TitleID>/"
DIAG_REPORT_NAME = LOGS_DIR + "diag-NNNN.txt"

# ------------------------------------------------- диагностика ГРАФИЧЕСКОГО пути
#
# Отдельная от -DNXR_DIAG диагностика (src/nxr_gfx.hpp): перехватывает цепочку
# вывода картинки (nn::vi -> EGL -> NVN) и пишет СВОЙ отчёт (gfx-NNNN.txt в папке
# игры под switch/ModKitNX/logs, рядом с diag-NNNN.txt).
# Включается ключом --diag-gfx, то есть одним флагом компиляции; без него ни
# байта этого кода в модуле нет (гейт check_nso.py проверяет это в обе стороны).
GFX_CFLAG = "-DNXR_DIAG_GFX"

# Узкая проба блокировок (ШАГ-8) — ОТДЕЛЬНЫЙ ключ поверх --diag-gfx, по
# умолчанию выключенный. Отдельный он потому, что прежняя, широкая проба
# (пять целей pthread_rwlock_*) однажды не дала игре запуститься: модуль
# вышел за пул трамплинов exlaunch, и тот аварийно завершил процесс. Всё
# новое включается только по явной просьбе — откат к рабочей сборке это
# просто сборка без ключа.
LOCK_CFLAG = "-DNXR_DIAG_LOCK"
GFX_REPORT_NAME = LOGS_DIR + "gfx-NNNN.txt"
# Метка строки-подозреваемого в отчёте: с неё начинается каждая неудача.
GFX_FAIL_MARK = "!!"

# Папка внутри архива для раздачи, куда кладутся скрипты правки прав main.npdm.
SHARE_SCRIPTS_DIR = "СДЕЛАТЬ-main.npdm-САМОМУ"

# Коды намеренных аварий диагностического модуля (видны в crash report Atmosphère).
DIAG_CRASH_CODES = [
    ("0xDEAD0001", "не найден символ nnMain в модуле main — хук ставить некуда"),
    ("0xDEAD0002", "не найдены функции записи nn::fs в модуле sdk"),
    ("0xDEAD0003", "nn::fs::MountSdCard(\"nxrsd\") вернул ошибку — SD модулю недоступна"),
    ("0xDEAD0004", "SD смонтирована, но файл отчёта создать не удалось"),
    ("0xDEAD0005", "svcGetInfo(ProgramId) отказал — путь мода собрать не из чего"),
    ("0xD1MMPPSS", "символы nn::fs не найдены: MM модулей видно, у PP разобран .dynsym, "
                   "SS символов из двух найдено"),
]

WSL_DISTRO = os.environ.get("NXR_WSL_DISTRO", "Ubuntu")


# ------------------------------------------------- машинный протокол событий

# Зачем он есть. Человеческий вывод say() — русский текст, и внешнему интерфейсу
# (веб-оболочке) с ним делать нечего: ни перевести, ни надёжно разобрать. Поэтому
# РЯДОМ с прежним выводом идёт машинный канал: NDJSON, одна строка — одно событие,
# у каждого события СТАБИЛЬНЫЙ код (EVENT_CODES ниже).
#
# Прежний вывод не меняется ни на байт: события пишутся в ДРУГОЙ канал (файл, stderr
# или дескриптор), а без ключа --events не пишутся вовсе. Точка сцепления одна —
# функции say() и fail(); отдельные print по коду не правились и править их не надо.

EVENTS_SCHEMA = 1

_events_fh = None      # куда писать события; None — канал выключен
_events_owned = False  # файл открыли мы, значит нам и закрывать
_events_ascii = True   # ensure_ascii; для консоли обязателен: cp866 кириллицу не примет
_events_count = 0      # сколько событий выпущено (для «затвора нуля» у проверки)


def events_open(target):
    """
    Включить канал событий. target:
        <путь>   файл (перезапись); родительские папки создаются;
        "-"      stdout — ОДИН канал с человеческим логом (коды — EVENT_CODES);
        "stderr" или "2" — stderr;
        "fd:N"   уже открытый дескриптор N (для запуска из другой программы).
    Пустое значение или None — канал остаётся выключенным.
    """
    global _events_fh, _events_owned, _events_ascii
    events_close()
    if not target:
        return None
    target = str(target)
    if target == "-":
        _events_fh, _events_owned, _events_ascii = sys.stdout, False, True
    elif target in ("stderr", "2"):
        _events_fh, _events_owned, _events_ascii = sys.stderr, False, True
    elif target.startswith("fd:"):
        _events_fh = os.fdopen(int(target[3:]), "w", encoding="utf-8", newline="\n")
        _events_owned, _events_ascii = True, False
    else:
        p = Path(target)
        if p.parent and str(p.parent) not in ("", "."):
            p.parent.mkdir(parents=True, exist_ok=True)
        _events_fh = open(p, "w", encoding="utf-8", newline="\n")
        _events_owned, _events_ascii = True, False
    atexit.register(events_close)
    return _events_fh


def events_close():
    """Закрыть канал. Вызывается и через atexit: sys.exit(1) в fail() не теряет хвост."""
    global _events_fh, _events_owned
    fh, owned = _events_fh, _events_owned
    _events_fh, _events_owned = None, False
    if fh is None:
        return
    try:
        fh.flush()
    except Exception:
        pass
    if owned:
        try:
            fh.close()
        except Exception:
            pass


def events_flush():
    """
    Сбросить буфер, НЕ закрывая канал.

    Закрывать в fail() нельзя: негативные пробы ловят SystemExit внутри того же
    процесса (`_zip_probe`, `self_test_*`), и закрытый канал молча терял бы все
    события после первой пробы. Закрытие делает atexit при настоящем выходе.
    """
    if _events_fh is not None:
        try:
            _events_fh.flush()
        except Exception:
            pass


def events_on():
    return _events_fh is not None


def events_count():
    return _events_count


def _jsonable(v):
    """Path, множества и прочее — в то, что json переварит без сюрпризов."""
    if isinstance(v, Path):
        return str(v)
    if isinstance(v, (str, int, float, bool)) or v is None:
        return v
    if isinstance(v, dict):
        return {str(k): _jsonable(x) for k, x in v.items()}
    if isinstance(v, (list, tuple, set, frozenset)):
        return [_jsonable(x) for x in v]
    return str(v)


def emit(code, msg=None, level="info", **data):
    """
    Выпустить событие. Поля: schema, ts, level, code, msg, data.

    События БЕЗ кода не бывает: пустой code — дефект вызывающего, и он роняет
    прогон ValueError'ом даже при выключенном канале, чтобы дефект не дожил до
    случая, когда канал включат.
    """
    global _events_count
    if not isinstance(code, str) or not code.strip():
        raise ValueError("событие без кода не допускается (code=%r, msg=%r)" % (code, msg))
    if _events_fh is None:
        return None
    ev = {
        "schema": EVENTS_SCHEMA,
        "ts": datetime.datetime.now().astimezone().isoformat(timespec="milliseconds"),
        "level": level,
        "code": code,
        "msg": "" if msg is None else str(msg),
        "data": {str(k): _jsonable(v) for k, v in data.items()},
    }
    line = json.dumps(ev, ensure_ascii=_events_ascii)
    try:
        _events_fh.write(line + "\n")
        _events_fh.flush()
    except Exception:
        # Канал событий не имеет права ронять сборку: человеческий вывод главнее.
        return None
    _events_count += 1
    return ev



# ------------------------------------------------------- реестр кодов событий

# Единственный источник истины по кодам. Описание событий порождается из него
# (scripts/check_events.py --registry-md) и сверяется с ним (--check-docs), чтобы
# документ и код не разъехались.
#
# ПРАВИЛО: код — постоянный адрес. Коды НЕ переиспользуются и НЕ удаляются;
# вышедший из употребления переносится в EVENT_CODES_RETIRED с датой и причиной.
#
# Столбцы: код, уровень, когда выпускается, поля data, что значит для пользователя.

EVENT_CODES = (
 # --- общее
 ("LOG", "info", "любая строка say() без своего кода",
  "—",
  "сырой лог конвейера (по-русски); внешнему интерфейсу переводить его не надо"),
 ("RUN_START", "info", "сразу после разбора аргументов, первым событием прогона",
  "tool, version, mode, diag, share, slot, dest, apply",
  "инструмент запущен; видно, что именно и в каком режиме"),
 ("DONE", "info", "прогон закончен без отказа",
  "action, applied, rc=0, zip, sha256, tid, slot, dest, warnings",
  "работа доведена до конца; артефакты и предупреждения перечислены"),
 ("WARNING", "warn", "каждая строка итогового списка ПРЕДУПРЕЖДЕНИЯ в install.py",
  "text",
  "не отказ, но требует внимания перед запуском игры"),
 ("GATE_PASS", "info", "гейт пройден (gate: hosttest, fsgate, check_nso, compat_syms, "
  "kit, mod, zbic, zip, foreign, sdk_swap; гейт npdm отмечается событием NPDM_PATCHED)",
  "gate, detail, path, sha256 (zip), expect (compat_syms)",
  "проверка зелёная; видно, какая именно"),
 ("STEP_START", "info", "начало шага конвейера",
  "n, of, name, плюс поля шага",
  "честный прогресс «шаг n из of» без разбора текста"),
 ("STEP_DONE", "info", "шаг закончен",
  "n, of, name",
  "шаг пройден целиком"),
 ("STEP_SKIPPED", "info", "шаг пропущен ключом (--skip-build, --no-zip)",
  "n, of, name, why",
  "шаг не выполнялся; это не отказ и не успех"),
 # --- сборка
 ("BUILD_START", "info", "начало сборки (build.py / build-diag.py)",
  "kind (plain|diag), title, slot, game, share, version, steps",
  "сборка началась; вид сборки виден сразу"),
 ("GAME_INSPECTED", "info", "exefs игры прочитан, TitleID известен",
  "exefs, tid, busy_slots, tid_from_npdm",
  "TitleID — прочитанный факт, а не ввод пользователя"),
 ("SLOT_CHOSEN", "info", "слот определён (аргумент, квитанция, образ, старший свободный)",
  "slot, source, updating, busy_slots, receipt_slot, ours",
  "в какой subsdkN ляжет модуль и почему именно в него"),
 ("MODULE_BUILT", "info", "модуль собран в WSL и забран на ПК",
  "path, size, sha256, slot, diag",
  "бинарник готов; sha256 годится для сверки с квитанцией"),
 ("NPDM_PATCHED", "info", "правленый main.npdm готов и прошёл гейт",
  "path, size, sha256, program_id, svc, sdcard_bit, source",
  "права игры дополнены (бит SdCard и вызовы 0x74/0x75)"),
 ("LAYOUT_DONE", "info", "раскладка atmosphere/contents/<TID>/{exefs,files} собрана",
  "exefs, files, count, tid, slot, stub",
  "файлы разложены; count — сколько файлов мода легло"),
 ("ZIP_WRITTEN", "info", "архив упакован (до проверки)",
  "path, entries, size, share, diag",
  "архив создан, но ещё не проверен"),
 ("ARTIFACT", "info", "готовый артефакт прогона",
  "kind, path, sha256, size, share, diag, tid, slot",
  "что именно получилось и где лежит"),
 # --- ZBIC (сжатие сегментов NSO, бит 7 флагов заголовка; HOS 22.0.0+)
 ("ZBIC_SCANNED", "info", "exefs осмотрен на сжатие ZBIC (всегда, по содержимому файлов)",
  "exefs, nso_files, zbic_files, zbic (да/нет), not_nso",
  "видно, читался ли каждый файл exefs и нашлось ли в нём сжатие ZBIC"),
 ("ZBIC_DETECTED", "warn", "в exefs найден NSO с битом ZBIC",
  "exefs, files, flags, segments, fix (готовим ли несжатый)",
  "игра не запустится на Atmosphère младше 1.12.0, пока ZBIC не снят"),
 ("ZBIC_REBUILT", "info", "несжатый NSO собран и прошёл гейт",
  "src, dst, name, size_before, size_after, module_id, flags_after, "
  "src_sha256, dst_sha256, segments",
  "ZBIC снят: сегменты распакованы, их sha256 совпали с заголовком оригинала"),
 ("ZBIC_LAYOUT_DONE", "info", "несжатые NSO разложены в atmosphere/contents/<TID>/exefs/",
  "exefs, files, count, tid, source_exefs",
  "файлы без ZBIC лежат рядом с subsdkN мода, в том же exefs"),
 ("ZBIC_ONLY_START", "info", "начало режима «только снять ZBIC» (install.py --zbic-only)",
  "dest, tid, exefs, apply, version, steps",
  "ставится ТОЛЬКО снятие сжатия: ни модуля мода, ни main.npdm, ни files/"),
 # --- совместимость с подменённой библиотекой sdk (доэкспорт символов)
 ("COMPAT_SELECTED", "info", "выбран набор символов совместимости (перед сборкой модуля)",
  "compat[], expect (none|hash|ldn|both), swapped_sdk, cflags[], symbols[]",
  "видно, какие символы модуль доэкспортирует и каким ключом это включено"),
 # --- диагностика графического пути (--diag-gfx)
 ("GFX_DIAG_SELECTED", "info", "решено, собирается ли диагностика графики "
  "(перед сборкой модуля; событие выходит ВСЕГДА, в том числе с enabled=false)",
  "enabled, cflag, report, build_dir",
  "видно, какой диагностический вариант собран и куда модуль положит отчёт на карте"),
 ("LOCK_DIAG_SELECTED", "info", "решено, собирается ли УЗКАЯ проба блокировок "
  "(--diag-lock; событие выходит ВСЕГДА, в том числе с enabled=false)",
  "enabled, cflag",
  "видно, есть ли в сборке перехват pthread_rwlock_rdlock: широкая проба "
  "28.09.2026 сломала запуск игры, поэтому это отдельный, по умолчанию "
  "выключенный ключ"),
 ("SWAPPED_SDK_KIT", "info", "комплект «для консоли с подменённым sdk» разложен (--swapped-sdk)",
  "tid, slot, exefs, compat[], symbols[], zbic_files[], zbic_found, npdm",
  "что именно вошло в комплект: модуль с символами, main.npdm и (если ZBIC был) "
  "несжатый main"),
 # --- подмена библиотеки sdk игры (install.py --swap-sdk; проверено на консоли)
 ("SWAP_SDK_CHECKED", "info", "файл --swap-sdk проверен: это nnSdk (NSO0, имя модуля, "
  "строка SDK MW)",
  "path, module, version, sha256, size, zbic, game_sdk_version, same_as_game",
  "какая nnSdk ляжет в exefs/sdk вместо библиотеки игры и чем она отличается от родной"),
 ("SWAP_SDK_WRITTEN", "info", "nnSdk записана в exefs/sdk и сверена байт в байт (--apply)",
  "dst, version, sha256, foreign_backup, updating",
  "подмена sdk лежит в сборке; foreign_backup — отложенная копия прежнего чужого файла"),
 ("SWAP_SDK_REMOVED", "info", "--uninstall --apply: наша exefs/sdk удалена по квитанции "
  "(sha256 совпал), чужая возвращена на место, если была (с 08.11.2026; до того — "
  "откладывалась .removed-)",
  "removed, restored, version, kept_changed",
  "подмена sdk удалена; restored — из какой копии вернули прежний файл (или null); "
  "kept_changed — файл изменён после установки и не тронут"),
 ("FAIL_SWAP_SDK_BAD_ARG", "error", "--swap-sdk указывает на путь, которого нет (или не на файл)",
  "rc=1, path, reason",
  "подменять sdk нечем; молча пропустить нельзя — человек решил бы, что подмена есть"),
 ("FAIL_SWAP_SDK_NOT_NNSDK", "error", "файл --swap-sdk — не библиотека nnSdk",
  "rc=1, path, reason, magic, module, version",
  "в exefs/sdk ляжет только nnSdk: другой NSO (movie, OpenGL, main) или не-NSO игру уронит"),
 ("FAIL_SWAP_SDK_ARGS", "error", "--swap-sdk задан вместе с --status или --uninstall",
  "rc=1, extra",
  "осмотр и снятие ничего не кладут; подмена sdk ставится обычной установкой"),
 ("FAIL_SWAP_SDK_MOD_CONFLICT", "error", "мод несёт свой exefs/sdk, и --swap-sdk тоже кладёт sdk",
  "rc=1, names[]",
  "двум файлам sdk в exefs не лечь — решите, чья библиотека нужна"),
 ("FAIL_GATE_SDK_SWAP", "error", "гейт подмены sdk: записанный exefs/sdk не совпал с источником",
  "rc=1, gate=sdk_swap, dst, sha256, want",
  "в сборке не та библиотека, что выбрана; ставить такое нельзя"),
 ("COMPAT_DEPRECATED", "warn", "задан устаревший ключ --compat-hash/--compat-ldn/"
  "--compat-nvrm (04.10.2026)",
  "keys[], replacement",
  "ключ ничего не меняет: автоисправление включено в каждой сборке и решает на консоли само"),
 ("AUTOFIX_FORECAST", "info", "прогноз автоисправления для указанной sdk консоли (--sdk)",
  "sdk, sdk_version, game_version, swapped, vulkan, fixes{id: {choice, why}}",
  "что модуль выберет на консоли с этой библиотекой — СВОЯ или ПРОКСИ — тем же "
  "правилом, что nxr::autofix::Decide"),
 ("STUBS_DETECTED", "info", "детектор заглушек: sdk консоли против родной sdk игры по "
  "импортам игры (уровень warn, если есть НОВЫЕ)",
  "old, new, old_version, new_version, known[], new_unfixed[]",
  "известные расхождения модуль закроет сам; новые найдены, но исправления для них нет"),
 ("GAME_PROFILE", "info", "профиль игры: графика, родная sdk, ZBIC, требуемая прошивка",
  "exefs, ok, sdk_version, sdk_buildid, graphics{}, main_imports{}, zbic_files[], "
  "required_system_version, required_text",
  "required_system_version — только если дан --cnmt; иначе null (честно: не знаем)"),
 ("COMPAT_SYMS_SCANNED", "info", "импорты модулей игры сверены с экспортами указанной "
  "библиотеки (--sdk)",
  "exefs, sdk[], libs[], game_nso[], imports, missing[], missing_weak[], "
  "compat_missing[], unreadable[]",
  "перечислено, каких символов игре не хватит с этой библиотекой (WEAK — отдельно: "
  "они процесс не роняют)"),
 ("COMPAT_SDK_UNKNOWN", "warn", "сверить не с чем: --sdk не задан или файл не разобрался",
  "exefs, sdk, reason, how",
  "честный ответ «проверить нечем»: конвейер не гадает, нужен ли доэкспорт"),
 # --- соседи в exefs: подменённый sdk, чужие модули и прочее не наше
 ("EXEFS_NEIGHBOURS", "info", "осмотр exefs назначения ПЕРЕД записью",
  "exefs, ours, foreign[{name,size,sha256}], replacing_foreign",
  "видно, что в папке уже лежит и чьё оно; наше — по квитанции, остальное чужое"),
 ("FOREIGN_KEPT", "info", "чужие файлы exefs пережили запись без изменений",
  "exefs, files[{name,sha256}], checked",
  "файлы других модов и прежних установок остались байт в байт теми же"),
 # --- установка
 ("INSTALL_START", "info", "начало установки (install.py без --status/--uninstall)",
  "dest, tid, apply, version, steps",
  "установка началась; apply=false значит «только план»"),
 ("DEST_SCANNED", "info", "назначение осмотрено перед выбором слота",
  "base, slots, ours, foreign, receipt, busy_game",
  "что уже лежит на карте: карта слотов с классами"),
 ("WRITE_PLAN", "info", "план записи готов (печатается и при --apply)",
  "dest, tid, slot, updating, apply, mode, items[{dst,src,kind,count,bytes}] "
  "(kind sdk-swap — подмена sdk, с version)",
  "что куда ляжет; без --apply на этом всё и заканчивается"),
 ("WRITE_APPLIED", "info", "запись выполнена",
  "dest, tid, slot, mod_files_written, module_sha256, npdm_sha256, updating",
  "файлы записаны в назначение"),
 ("RECEIPT_WRITTEN", "info", "квитанция nxredirect.json записана",
  "path, schema, tool, version, game_tid, slot, installed, module_sha256, "
  "npdm_sha256, module_name, zbic_files, neighbours, modes, compat, mod, sdk_swap",
  "источник истины «что установлено» обновлён"),
 ("ROOT_CONFIRM_NEEDED", "warn", "назначение — КОРЕНЬ диска",
  "dest, anchor, removable, allowed_by_flag",
  "нужно подтверждение: так выглядит и SD-карта, и системный диск"),
 ("ROOT_CONFIRMED", "info", "запись в корень подтверждена флагом --yes-root",
  "dest, how",
  "подтверждение получено, запись продолжается"),
 ("STATUS_START", "info", "начало осмотра (--status)",
  "dest, tid, base",
  "смотрим, что стоит в назначении"),
 ("STATUS_REPORT", "info", "осмотр (--status) закончен",
  "dest, tid, installed, slots, ours, receipt, mod_files, romfs, npdm, npdm_orig, reason",
  "полный машинный снимок назначения: карта слотов, квитанция, romfs"),
 ("UNINSTALL_START", "info", "начало снятия (--uninstall)",
  "dest, tid, apply, also_files",
  "снятие началось"),
 ("UNINSTALL_PLAN", "info", "план снятия готов",
  "dest, tid, slot, apply, also_files (устарел 08.11.2026), items, stamp, npdm_orig, "
  "npdm_ours, zbic_files, sdk_swap, delete[], kept_changed[], kept_no_sha[], restore[], "
  "npdm_restore",
  "что будет УДАЛЕНО по квитанции (с 08.11.2026; до того — отложено .removed-): "
  "только файлы с совпавшим sha256; изменённые и без sha256 — не трогаются"),
 ("UNINSTALL_APPLIED", "info", "удаление установленного выполнено",
  "dest, tid, slot, stamp, also_files, npdm_restored, zbic_files, deleted[], restored[], "
  "kept_changed[], kept_no_sha[], removed_dirs[]",
  "наши файлы удалены, main.npdm и отложенные чужие файлы возвращены, пустые созданные "
  "установкой папки убраны"),
 # --- самопроверка
 ("SELFTEST_START", "info", "начало самопроверки гейтов (--self-test/--quick-self-test)",
  "full, diag",
  "проверяем, что проверки умеют краснеть"),
 ("SELFTEST_DONE", "info|error", "самопроверка закончена",
  "passed, rc, ran (запущено проб), caught (сработали), missed (не сработали), "
  "skipped (блоков не запускалось)",
  "итог самопроверки одной строкой"),
 ("PROBE_CAUGHT", "info", "негативная проба поймана (гейт покраснел, как обязан)",
  "probe, kind=negative",
  "гейт умеет отказывать"),
 ("PROBE_MISSED", "error", "негативная проба НЕ поймана",
  "probe, kind=negative",
  "гейт лжёт о своей работе: его зелёный итог ничего не значит"),
 ("PROBE_GREEN", "info", "положительная проба зелёная (гейт не краснеет на годном)",
  "probe, kind=positive",
  "гейт не ложно-срабатывает"),
 ("PROBE_RED", "error", "положительная проба КРАСНАЯ",
  "probe, kind=positive",
  "гейт краснеет на заведомо годном входе — дефект гейта"),
 ("PROBE_SKIPPED", "warn", "блок проб НЕ запускался: нет входа",
  "probe, kind=skipped, reason, how",
  "о гейте не доказано ничего: это не «проба не сработала», а «нечем проверять»"),
 # --- отказы: у каждого свой код, КОД ВОЗВРАТА прежний (1)
 ("FAIL_GENERIC", "error", "отказ, которому ещё не назначен свой код",
  "rc=1",
  "отказ без машинного пояснения; такой код — повод завести свой"),
 ("FAIL_EXEFS_BAD_ARG", "error", "--game-exefs / --exefs указывает на негодную папку",
  "rc=1, path, reason",
  "путь к exefs игры не годится; другой путь НЕ подставляется"),
 ("FAIL_EXEFS_BAD_ENV", "error", "NXR_GAME_EXEFS указывает на негодную папку",
  "rc=1, path, reason",
  "то же, но путь пришёл из переменной среды"),
 ("FAIL_NO_GAME_EXEFS", "error", "exefs игры не найден ни в одном из трёх мест",
  "rc=1",
  "без модулей игры гейты 2 и 3 проверять нечем — сборка красная, а не «пропущенная»"),
 ("FAIL_EXEFS_MISSING_FILE", "error", "в папке exefs нет обязательного файла",
  "rc=1, path, missing",
  "дамп неполный: нужны main, rtld, sdk, main.npdm"),
 ("FAIL_NO_MAKEFILE", "error", "нет exlaunch/Makefile",
  "rc=1, path",
  "дерево проекта неполное"),
 ("FAIL_NO_SOURCE", "error", "нет src/main.cpp",
  "rc=1, path",
  "дерево проекта неполное"),
 ("FAIL_WSL_PATH_SPACES", "error", "путь сборки в WSL содержит пробел",
  "rc=1, work",
  "Makefile exlaunch пробела не переживает"),
 ("FAIL_WSL_BUILD", "error", "сборка в WSL не удалась",
  "rc=1, wsl_rc, devkitpro_missing, slot, diag, work",
  "devkitpro_missing=true значит «нет devkitPro в /opt/devkitpro» (код 3 скрипта)"),
 ("FAIL_NO_PYTHON", "error", "не найден интерпретатор Python для вспомогательных скриптов",
  "rc=1, sys_executable",
  "sys.executable не указывает на python и в PATH его нет; гейты запускать нечем"),
 ("FAIL_WSL_TIMEOUT", "error", "прогон в WSL прерван по времени (сборка или хост-тест)",
  "rc=1, timeout, distro",
  "проверка СОРВАНА и результат не получен; это находка гейта, а не его авария"),
 ("FAIL_WSL_MISSING", "error", "wsl.exe не найден или не запускается",
  "rc=1, distro",
  "запускать сборку и хост-тесты негде: нужна WSL с Ubuntu и devkitPro"),
 ("FAIL_WSL_FETCH", "error", "собранный модуль не удалось забрать из WSL",
  "rc=1, path, work",
  "модуль собрался, но не скопировался на ПК"),
 ("FAIL_NO_HOSTTEST_SCRIPT", "error", "нет hosttest/run_hosttest.sh",
  "rc=1, path",
  "хост-тесты запускать нечем"),
 ("FAIL_NO_GAME_NPDM", "error", "нет main.npdm игры",
  "rc=1, path",
  "права править не из чего"),
 ("FAIL_NPDM_SDCARD", "error", "npdm_sdcard.py не поставил бит SdCard",
  "rc=1, path",
  "правка прав не удалась"),
 ("FAIL_NPDM_SYSCALLS", "error", "npdm_syscalls.py не добавил вызовы",
  "rc=1, path, svc",
  "правка прав не удалась"),
 ("FAIL_GATE_NPDM", "error", "гейт npdm --check не пройден",
  "rc=1, gate=npdm, path, program_id, svc",
  "правленый npdm негоден: игра упала бы на «Undefined System Call»"),
 ("FAIL_GATE_HOSTTEST", "error", "гейт хост-тестов не пройден",
  "rc=1, gate=hosttest|fsgate, gate_rc, detail",
  "логика подмены путей (gate=hosttest) или порядок работы с nn::fs (gate=fsgate) сломаны — ставить нельзя"),
 ("FAIL_GATE_CHECK_NSO", "error", "гейт check_nso.py не пройден",
  "rc=1, gate=check_nso, gate_rc, path, detail",
  "собранный модуль не соответствует требованиям к NSO"),
 ("FAIL_GATE_ZIP", "error", "гейт архива не пройден",
  "rc=1, gate=zip, path, problems[], share",
  "архив негоден: перечислено, что именно не так"),
 ("FAIL_ZBIC_TOOL_MISSING", "error", "распаковывать ZBIC нечем",
  "rc=1, package, version, license, url, how",
  "рядом нет scripts/zbic_pure.py и не установлен пакет zbic; в обычной работе "
  "не возникает — наш чистый Python зависимостей не требует"),
 ("FAIL_ZBIC_NOT_FOUND", "error", "задан --zbic-fix или --zbic-only, а сжатия ZBIC в exefs нет",
  "rc=1, exefs, nso_files",
  "снимать нечего: ключ задан по ошибке либо exefs не от той версии игры"),
 ("FAIL_ZBIC_ONLY_ARGS", "error", "--zbic-only задан вместе с ключами установки мода "
  "(в том числе --swap-sdk)",
  "rc=1, extra",
  "режим ничего не ставит, кроме несжатых NSO; лишние ключи обещали бы мод, "
  "которого не будет"),
 ("FAIL_ZBIC_REBUILD", "error", "пересборка несжатого NSO не удалась",
  "rc=1, src, dst, gate_rc, detail",
  "сегмент не распаковался или его sha256 не сошлась с заголовком NSO"),
 ("FAIL_GATE_ZBIC", "error", "гейт пересобранного NSO не пройден",
  "rc=1, gate=zbic, gate_rc, path, orig, detail",
  "несжатый NSO негоден: игра упала бы при запуске"),
 ("FAIL_GATE_COMPAT_SYMS", "error", "гейт символов совместимости не пройден",
  "rc=1, gate=compat_syms, gate_rc, path, expect, detail",
  "в .dynsym модуля не те символы, что обещаны ключами: лишний экспорт перекрыл бы "
  "рабочий символ sdk, недостающий не спас бы от падения"),
 ("FAIL_GATE_KIT", "error", "гейт согласованности комплекта не пройден",
  "rc=1, gate=kit, exefs, tid, problems[], checks",
  "в раскладке файлы разных игр или разных версий (или гейт ничего не проверил)"),
 ("FAIL_AUTOFIX_ARGS", "error", "--no-autofix задан вместе с --swapped-sdk или --compat-*",
  "rc=1, extra[]",
  "просили одновременно «без исправлений» и «с исправлениями» — выбрать одно"),
 ("FAIL_SWAPPED_SDK_ARGS", "error", "--swapped-sdk задан вместе с несочетаемым ключом",
  "rc=1, extra",
  "комплект — это модуль вместе с правами и несжатым main; режимы без модуля с ним "
  "не сочетаются"),
 ("FAIL_SDK_BAD_ARG", "error", "--sdk указывает на путь, которого нет",
  "rc=1, path",
  "сверять импорты игры не с чем; молча пропустить нельзя — это была бы тишина "
  "вместо ответа"),
 ("FAIL_GATE_FOREIGN_TOUCHED", "error", "запись задела чужой файл в exefs назначения",
  "rc=1, gate=foreign, exefs, problems[]",
  "в exefs рядом с нашими файлами могут лежать файлы других модов; "
  "чужое молча менять или удалять нельзя"),
 ("FAIL_NO_MOD_FILES_DIR", "error", "папки файлов мода нет",
  "rc=1, path",
  "подменять было бы нечем"),
 ("FAIL_MOD_FILES_EMPTY", "error", "папка файлов мода пуста",
  "rc=1, path",
  "подменять было бы нечем"),
 ("FAIL_NO_PREBUILT_MODULE", "error", "нужен готовый модуль, а его нет (--skip-build/--no-build)",
  "rc=1, path, slot",
  "соберите модуль или снимите ключ запрета сборки"),
 ("FAIL_NO_MODULE_FILE", "error", "--module указывает на несуществующий файл",
  "rc=1, path",
  "путь к готовому модулю неверен"),
 ("FAIL_MODULE_NOT_OURS", "error", "classify_nso не признал файл нашим модулем",
  "rc=1, path, found, detail",
  "это не nxredirect — ставить не будем"),
 ("FAIL_SLOT_BUSY_GAME", "error", "выбранный слот занят самой игрой",
  "rc=1, slot, busy_slots, free",
  "возьмите свободный слот (подсказан в free)"),
 ("FAIL_SLOT_OCCUPIED_DEST", "error", "в выбранном слоте назначения лежит чужой файл",
  "rc=1, slot, found, detail",
  "перезаписывать чужое не будем"),
 ("FAIL_NO_FREE_SLOT", "error", "все слоты subsdk0..subsdk9 заняты",
  "rc=1, busy_slots",
  "свободного слота нет"),
 ("FAIL_FOREIGN_SD_MOD", "error", "в назначении стоит чужой мод, читающий SD",
  "rc=1, slots",
  "два таких модуля хукают одни и те же функции — снимите старый вручную"),
 ("FAIL_FOREIGN_IN_OUR_SLOT", "error", "в слоте из квитанции лежит НЕ наш модуль",
  "rc=1, slot, found, detail",
  "кто-то поставил мод поверх нашего"),
 ("FAIL_RECEIPT_MISMATCH", "error", "квитанция и карта расходятся",
  "rc=1, receipt_slot, ours",
  "второй модуль не ставится: грузились бы оба"),
 ("FAIL_SLOT_CONFLICT_RECEIPT", "error", "--slot не совпал со слотом из квитанции",
  "rc=1, receipt_slot, asked",
  "сначала снимите прежний модуль"),
 ("FAIL_SLOT_CONFLICT_CONTENT", "error", "--slot не совпал со слотом, опознанным по образу",
  "rc=1, found_slot, asked",
  "сначала снимите прежний модуль"),
 ("FAIL_MANY_OURS", "error", "в назначении больше одного нашего модуля",
  "rc=1, ours",
  "грузятся оба; лишний снимите вручную"),
 ("FAIL_BAD_TID", "error", "--tid не 16 шестнадцатеричных цифр",
  "rc=1, tid",
  "неверный TitleID"),
 ("FAIL_NO_TID", "error", "TitleID взять не из чего",
  "rc=1",
  "задайте --exefs или --tid"),
 ("FAIL_NO_EXEFS_ARG", "error", "для установки не задан --exefs",
  "rc=1",
  "установка без exefs игры невозможна"),
 ("FAIL_ROOT_NOT_CONFIRMED", "error", "запись в корень диска не подтверждена",
  "rc=1, dest",
  "ответа не было (не интерактивный запуск) — молчание согласием не считается"),
 ("FAIL_ROOT_DECLINED", "error", "пользователь отказался писать в корень диска",
  "rc=1, dest",
  "ничего не записано"),
 ("FAIL_UNINSTALL_NO_DIR", "error", "--uninstall: папки назначения нет",
  "rc=1, path",
  "снимать нечего"),
 ("FAIL_UNINSTALL_FOREIGN_IN_SLOT", "error", "--uninstall: в слоте квитанции чужой модуль",
  "rc=1, slot, found, detail",
  "ничего не трогаем"),
 ("FAIL_UNINSTALL_NOTHING", "error", "--uninstall: квитанции нет (или не читается)",
  "rc=1, slots, ours, receipt",
  "снимать нечего: без квитанции ничего не удаляется (с 08.11.2026; наш модуль, "
  "опознанный по содержимому, — в ours, убрать его вручную)"),
 # --- перенос мода целиком: свой модуль, свой main.npdm, IPS, romfs
 ("MOD_ANALYZED", "info", "мод разобран (--mod или --mod-files с деревом мода)",
  "tid, root, title_dir, modules[] (name, size, sha256, build_id, hooks_openfile, "
  "replaces_game, path), exefs[], replaces[], npdm, files, files_bytes, patches[] "
  "(path, target), extra[], skipped[], warnings[]",
  "полный план переноса: что из мода куда поедет и что НЕ переносится"),
 ("MOD_TREE_AS_FILES", "warn", "--mod-files указывает на дерево мода (atmosphere/…)",
  "path, as_mod",
  "папка понята как мод целиком (как --mod), а не как файлы romfs"),
 ("MOD_PARTS_LEFT", "warn", "--mod-files — это romfs внутри мода, а рядом есть "
  "exefs/патчи",
  "path, mod_root, left[]",
  "файлы перенесутся, а модуль/патчи/npdm мода — нет; укажите --mod <корень мода>"),
 ("EARLY_FS_SELECTED", "info", "решено, ставить ли перехват nn::fs рано (exl_main)",
  "early, why, cflag",
  "у мода свой модуль — наш перехват должен встать раньше его кода"),
 ("MOD_SLOT_ORDER", "info", "наш слот относительно модулей мода",
  "slot, mod_slots[], ok, why",
  "ok=false: модуль мода инициализируется раньше нашего (подмена его ранних файлов не гарантирована)"),
 ("NPDM_FROM_MOD", "info", "main.npdm из мода готов и прошёл гейт",
  "path, source, mode (as-is|merged), added[], header_diffs[], sha256, program_id, svc",
  "права взяты из файла мода; as-is — байт в байт, merged — дописано недостающее"),
 ("MOD_WRITTEN", "info", "части мода записаны в назначение",
  "dest, tid, exefs[], files, patches[], extra[]",
  "модуль мода, патчи и файлы лежат на месте (проверено гейтом mod)"),
 ("FAIL_MOD_NOT_TREE", "error", "--mod указывает не на мод",
  "rc=1, path",
  "в папке нет atmosphere/contents/<TID>/ — это не мод (или это просто файлы: --mod-files)"),
 ("FAIL_MOD_OTHER_TITLE", "error", "в моде нет папки этой игры",
  "rc=1, path, tid, mod_titles[]",
  "мод для другой игры или другой программы"),
 ("FAIL_MOD_ROMFS_BIN", "error", "мод несёт romfs.bin",
  "rc=1, path",
  "упакованный romfs надо распаковать в romfs/ — иначе это снова LayeredFS"),
 ("FAIL_MOD_FILES_CLASH", "error", "один файл и в romfs/, и в files/ мода — разный",
  "rc=1, rel",
  "какой настоящий — решать автору мода"),
 ("FAIL_MOD_EMPTY", "error", "в моде для этой игры нечего переносить",
  "rc=1, path, tid",
  "мод пустой"),
 ("FAIL_MOD_NPDM_BAD", "error", "main.npdm мода не разбирается",
  "rc=1, path",
  "файл прав мода битый"),
 ("FAIL_MOD_NPDM_TID", "error", "main.npdm мода от другой программы",
  "rc=1, path, program_id, tid",
  "такой npdm игру не запустит"),
 ("FAIL_MOD_SLOT_CONFLICT", "error", "наш слот совпал со слотом модуля мода",
  "rc=1, slot, mod_slots[]",
  "два модуля в одном файле не уживутся — выберите другой слот"),
 ("FAIL_MOD_ZBIC_CONFLICT", "error", "мод несёт свой main, а снятие ZBIC тоже кладёт main",
  "rc=1, names[]",
  "двум файлам одного имени в exefs не лечь — решите, чей main нужен"),
 ("FAIL_MOD_ARGS", "error", "--mod вместе с --mod-files",
  "rc=1",
  "мод задаётся одним ключом: --mod <корень мода>"),
 ("FAIL_GATE_MOD", "error", "гейт переноса мода не пройден",
  "rc=1, gate=mod, tid, problems[], checks",
  "часть мода не перенесена или перенесена не байт в байт"),
)

# Коды, выведенные из употребления: не переиспользуются.
EVENT_CODES_RETIRED = {
    "FAIL_UNINSTALL_MANY_OURS": "08.11.2026: снятие удаляет только названное в квитанции "
                                "(сверка sha256), поиск «наших» модулей по содержимому для "
                                "снятия больше не нужен; без квитанции — FAIL_UNINSTALL_NOTHING",
}


def event_codes():
    """Множество живых кодов — для проверки NDJSON (scripts/check_events.py)."""
    return {row[0] for row in EVENT_CODES}


# ------------------------------------------------------------------ мелочи


def setup_stdio():
    """
    stdout и stderr — UTF-8, даже когда это КАНАЛ, а не консоль.

    Зачем. Конвейер печатает по-русски, а когда вывод перенаправлен (в файл, в
    канал вызывающей программы, в git-hook), Python берёт кодировку не из консоли,
    а из локали машины. На этой машине локаль — cp1250, кириллицу она не кодирует,
    и исправный скрипт падал на UnicodeEncodeError ещё в argparse (`--help`) —
    то есть ДО первой своей строки. `chcp 65001` тут не спасает: он меняет только
    настоящую консоль. Замер: `python build.py --help` с выводом в
    канал и без PYTHONIOENCODING — код возврата 1 и UnicodeEncodeError; после
    reconfigure — 0.

    errors="replace" оставлен запасным путём: если поток нельзя перенастроить
    (подменён на объект без reconfigure), печать всё равно не должна ронять прогон.
    Вызывается ПЕРВОЙ строкой точки входа, до разбора аргументов.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def _print(msg):
    """Печать, устойчивая к консоли с cp866/cp1250."""
    enc = sys.stdout.encoding or "utf-8"
    sys.stdout.write(msg.encode(enc, errors="replace").decode(enc) + "\n")
    sys.stdout.flush()


def say(msg, code=None, level="info", **data):
    """
    Прежняя печать плюс событие в машинный канал.

    Текст в stdout остаётся тем же, что и до появления событий. Строка без своего
    кода едет как LOG: это «сырой лог», переводить его внешнему интерфейсу не надо.
    """
    _print(msg)
    emit(code or "LOG", msg, level=level, **data)


def fail(msg, code="FAIL_GENERIC", **data):
    """
    Отказ: событие уровня error со своим кодом, та же строка в stdout, КОД ВОЗВРАТА 1.

    Код возврата не менялся и не меняется: машинное различение отказов идёт по
    полю code, а не по числу (внешние скрипты на 1 уже опираются).
    """
    emit(code, "ОШИБКА: " + msg, level="error", rc=1, **data)
    _print("ОШИБКА: " + msg)
    events_flush()
    sys.exit(1)


def _python_runs(exe):
    """Пробный запуск: настоящий Python 3.8+ отвечает кодом 0, заглушка — нет."""
    try:
        r = subprocess.run([exe, "-c", "import sys; sys.exit(sys.version_info < (3, 8))"],
                           stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return False
    return r.returncode == 0


def python_exe():
    """
    Чем запускать наши вспомогательные скрипты.

    Обычно это sys.executable, но полагаться на него молча нельзя: в особом
    окружении (собранный .exe, встроенный интерпретатор, запуск из чужой
    программы) он указывает не на python, а на саму программу или вовсе пуст —
    и тогда subprocess падает OSError'ом без объяснения (замер:
    пустая строка -> [WinError 87]). Поэтому: проверяем, и если не годится —
    честно ищем python в PATH, а не делаем вид, что всё хорошо.
    """
    exe = sys.executable
    if exe and Path(exe).is_file() and Path(exe).stem.lower().startswith("python"):
        return exe
    for name in ("python", "python3"):
        found = shutil.which(name)
        # На Windows в PATH бывает заглушка Microsoft Store (WindowsApps\python.exe):
        # which её находит, а запуск даёт 9009. Берём только то, что реально запускается.
        if found and _python_runs(found):
            return found
    fail("не найден интерпретатор Python для запуска вспомогательных скриптов "
         f"(sys.executable = {sys.executable!r}, в PATH нет python/python3).",
         code="FAIL_NO_PYTHON", sys_executable=str(sys.executable))


def run_py(args, env=None):
    """
    Запуск вспомогательного скрипта этим же Python.

    PYTHONIOENCODING оставлен как ремень поверх подтяжек: сами скрипты теперь
    настраивают свой вывод на UTF-8 в точке входа (setup_stdio), но вызов из
    конвейера не должен зависеть от того, починен ли конкретный скрипт.
    `env` подменяет окружение целиком — это нужно пробам, которые закрывают
    пакет `zbic` подставным модулем и смотрят, что конвейер не отказал.
    """
    env = dict(env if env is not None else os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    return subprocess.run([python_exe()] + [str(a) for a in args],
                          capture_output=True, text=True,
                          encoding="utf-8", errors="replace", env=env)


def file_sha256(path):
    sha = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            sha.update(chunk)
    return sha.hexdigest()


def wsl_path(win_path):
    """X:\\a b\\c -> /mnt/<буква диска>/a b/c (кавычки навешивает вызывающий)."""
    p = str(Path(win_path).resolve()).replace("\\", "/")
    return "/mnt/" + p[0].lower() + p[2:]


def run_wsl(script, timeout=1800):
    """
    Прогон bash-скрипта в WSL; возвращает CompletedProcess.

    Скрипт передаётся через stdin (bash -l -s), а НЕ через bash -lc "...":
    многострочная строка в командной строке wsl.exe теряется — проверено,
    исполнялась только первая строка, из-за чего DEVKITPRO оказывался пустым.

    Байтами, а не text=True: в текстовом режиме Python на Windows переводит \\n
    в CRLF, и bash спотыкается на \\r («set: -: invalid option»).

    СОРВАННЫЙ прогон — находка гейта, а не авария гейта. Раньше истёкший timeout
    улетал наружу как subprocess.TimeoutExpired, а отсутствие wsl.exe — как
    FileNotFoundError: прогон умирал трассировкой Python, без своего кода события
    и без объяснения (замер: run_wsl("sleep 20", timeout=1) ->
    TimeoutExpired). Теперь оба случая проходят через fail(): прежний код возврата
    1, свой код события, понятная строка.
    """
    try:
        res = subprocess.run(["wsl", "-d", WSL_DISTRO, "--", "bash", "-l", "-s"],
                             input=script.encode("utf-8"), capture_output=True,
                             timeout=timeout)
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace")
        err = (e.stderr or b"").decode("utf-8", "replace")
        if out or err:
            say(tail(out) + "\n" + tail(err))
        fail(f"прогон в WSL не уложился в {timeout} с и был прерван.\n"
             "        Это находка проверки, а не её авария: сборка/тест не доведены\n"
             "        до конца, поэтому результат считается НЕ полученным.",
             code="FAIL_WSL_TIMEOUT", timeout=timeout, distro=WSL_DISTRO)
    except FileNotFoundError:
        fail("не найдена программа wsl (wsl.exe) — запускать сборку и хост-тесты негде.\n"
             "        Нужна WSL с Ubuntu и devkitPro в /opt/devkitpro.\n"
             "        Проверьте: wsl -l -v",
             code="FAIL_WSL_MISSING", distro=WSL_DISTRO)
    except OSError as e:
        fail(f"не удалось запустить wsl: {e}", code="FAIL_WSL_MISSING",
             distro=WSL_DISTRO)
    return subprocess.CompletedProcess(
        res.args, res.returncode,
        res.stdout.decode("utf-8", "replace"),
        res.stderr.decode("utf-8", "replace"))


def tail(text, n=3000):
    text = (text or "").strip()
    return text[-n:]


def last_line(text):
    lines = [l for l in (text or "").strip().splitlines() if l.strip()]
    return lines[-1] if lines else "(нет вывода)"


def load_script(path, name):
    """Импорт нашего скрипта из scripts/ как модуля (нужен Npdm.program_id)."""
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def npdm_program_id(npdm_path):
    """TitleID из main.npdm игры — 16 hex ЗАГЛАВНЫМИ (как в имени папки contents)."""
    mod = load_script(NPDM_SYSCALLS, "nxr_npdm_syscalls")
    npdm = mod.Npdm(Path(npdm_path).read_bytes())
    return "%016X" % npdm.program_id


# ------------------------------------------------------------------ вход


def exefs_problem(path):
    """Чем именно папка не годится как exefs игры; None — годится."""
    p = Path(path)
    if not p.exists():
        return "папки нет"
    if not p.is_dir():
        return "это не папка"
    missing = [n for n in ("main.npdm", "main", "rtld", "sdk") if not (p / n).is_file()]
    if missing:
        return "нет обязательных файлов: " + ", ".join(missing)
    return None


def find_game_exefs(explicit=None):
    """
    Папка с распакованным exefs игры (main, subsdk0.., sdk, rtld, main.npdm).

    Ищется ТОЛЬКО в трёх местах, по порядку:
      1) явный аргумент --game-exefs;
      2) переменная среды NXR_GAME_EXEFS;
      3) папка game-exefs рядом с проектом (nxredirect/game-exefs).

    Никаких путей, привязанных к машине автора, здесь нет и быть не должно: это
    универсальный инструмент, а молчаливая подстановка чужого дампа означала бы
    сборку под ДРУГУЮ игру (дефект гейта, найденный при проверке).

    Если указанный путь не годится — КРАСНЫЙ отказ с объяснением, а не подстановка
    другого пути. Без exefs игры гейты 2 и 3 проверять нечем.
    """
    if explicit:
        problem = exefs_problem(explicit)
        if problem:
            fail(f"указанная папка exefs игры не годится: {explicit}\n"
                 f"        причина: {problem}\n"
                 "        Другой путь НЕ подставляется: соберите полный распакованный exefs\n"
                 "        своей игры (main, rtld, sdk, subsdk0.., main.npdm) из своего дампа\n"
                 "        с учётом установленного обновления.",
                 code="FAIL_EXEFS_BAD_ARG", path=str(explicit), reason=problem)
        return Path(explicit)

    env = os.environ.get("NXR_GAME_EXEFS")
    if env:
        problem = exefs_problem(env)
        if problem:
            fail(f"NXR_GAME_EXEFS указывает на негодную папку: {env}\n"
                 f"        причина: {problem}",
                 code="FAIL_EXEFS_BAD_ENV", path=str(env), reason=problem)
        return Path(env)

    near = ROOT / "game-exefs"
    if exefs_problem(near) is None:
        return near
    return None


def classify_nso(path):
    """
    Чей это модуль в слоте exefs. Возвращает (класс, пояснение).

    Классы:
      "ours"      — наш nxredirect: в образе есть куски пути мода
                    "sd:/atmosphere/contents/" и "/files/", и нет импортов
                    (импортов у нашего модуля не бывает — см. check_nso.py);
      "old-sd-mod"— чужой мод, который сам читает SD по своему пути (у прежнего
                    проекта это "sd:/tits2rus/"): в образе есть "sd:/", но не наш
                    путь мода. Такой модуль будет хукать те же функции, что и мы;
      "alien"     — какой-то другой модуль (или субмодуль игры);
      "not-nso"   — не NSO вовсе (или разобрать не удалось).
    Разбор NSO берём из scripts/check_nso.py — того же, что служит гейтом, чтобы
    распознавание и проверка не разъехались.
    """
    path = Path(path)
    try:
        chk = load_script(CHECK_NSO, "nxr_check_nso")
        nso = chk.Nso(path)
    except Exception as e:
        return "not-nso", f"не разобрался как NSO ({e})"
    image = nso.image
    try:
        imports = [nm for nm, is_import, _v, _b in nso.symbols() if is_import]
    except Exception:
        imports = []
    ours_parts = [p.encode() for p in ("sd:/atmosphere/contents/", "/files/")]
    if all(p in image for p in ours_parts):
        if imports:
            return "alien", ("похож на наш путь мода, но у модуля есть импорты "
                             f"({len(imports)} шт.) — наш собирается без импортов")
        return "ours", "в образе путь мода sd:/atmosphere/contents/<TID>/files/, импортов нет"
    if b"sd:/" in image:
        marks = []
        for known in (b"sd:/tits2rus/", b"sd:/atmosphere/"):
            if known in image:
                marks.append(known.decode())
        return "old-sd-mod", ("модуль сам читает SD" +
                              (" (" + ", ".join(marks) + ")" if marks else ""))
    return "alien", "ни пути мода, ни обращений к sd:/ — чужой модуль"


def occupied_slots(game_exefs):
    """Какие subsdkN уже заняты самой игрой."""
    return [f"subsdk{n}" for n in range(10) if (Path(game_exefs) / f"subsdk{n}").is_file()]


def pick_free_slot(game_exefs):
    """
    Свободный слот: берём СТАРШИЙ свободный (subsdk9 и вниз).

    Игры занимают слоты с начала, поэтому конфликт со штатным субмодулём игры
    или с чужим модом у старшего слота менее вероятен.
    """
    busy = set(occupied_slots(game_exefs))
    for n in range(9, -1, -1):
        name = f"subsdk{n}"
        if name not in busy:
            return name, sorted(busy)
    return None, sorted(busy)


# ----------------------------------------------- соседи в exefs назначения


def snapshot_dir(path):
    """
    Снимок папки: {имя файла: {"size":…, "sha256":…}}.

    Нужен, чтобы после записи можно было доказать (а не пообещать), что чужие
    файлы не тронуты. Папки внутрь не раскрываются: в exefs их не бывает.
    """
    path = Path(path)
    snap = {}
    if not path.is_dir():
        return snap
    for p in sorted(path.iterdir()):
        if p.is_file():
            snap[p.name] = {"size": p.stat().st_size, "sha256": file_sha256(p)}
    return snap


def foreign_files(snapshot, ours):
    """Что в папке НЕ наше: всё, чего нет в списке наших имён."""
    return {n: v for n, v in snapshot.items() if n not in set(ours)}


def foreign_untouched(before, after, ours, weak=False):
    """
    Гейт: каждый чужой файл, лежавший в exefs ДО записи, остался тем же.

    Зачем. В atmosphere/contents/<TID>/exefs/ у файлов может быть несколько
    хозяев: наш subsdkN (мод), наш main без сжатия ZBIC, а рядом — файлы других
    модов и прежних установок. Мы кладём СВОИ файлы рядом, а не «раскладываем
    папку»: ничего чужого ни удалять, ни перезаписывать молча нельзя. Наше
    опознаётся по квитанции, всё остальное считается чужим.

    weak=True — НАРОЧНО ослабленная проверка для негативной пробы: смотрит
    только, что файл на месте, и не сравнивает содержимое. В работе не
    используется (её зелёный итог ничего не значит — это и доказывает проба).
    """
    problems = []
    for name, was in foreign_files(before, ours).items():
        now = after.get(name)
        if now is None:
            problems.append("чужой файл %s исчез из exefs назначения" % name)
            continue
        if weak:
            continue
        if now["sha256"] != was["sha256"]:
            problems.append("чужой файл %s изменён: sha256 %s… вместо %s… "
                            "(размер %d вместо %d)"
                            % (name, now["sha256"][:16], was["sha256"][:16],
                               now["size"], was["size"]))
    return problems


# ------------------------------------------- квитанция: что в exefs наше


def receipt_sane(rec):
    """Квитанция, которой можно верить: она есть и читается."""
    return isinstance(rec, dict) and "__битая__" not in rec and bool(rec)


def exefs_ours_known(receipt):
    """
    Имена файлов в exefs назначения, которые положили МЫ, — по квитанции.

    Одно место на оба режима. Всё, чего здесь нет, считается ЧУЖИМ: в
    atmosphere/contents/<TID>/exefs/ могут лежать файлы других модов и прежних
    установок, и молча менять их нельзя.

    main.npdm попадает в «наше» НЕ по факту существования квитанции, а только
    когда в ней есть npdm_sha256. Режим --zbic-only npdm не кладёт вовсе, и
    считать чужой npdm своим было бы враньём (именно так и появлялось бы
    «заменяем не наше» задом наперёд: предупреждения нет там, где оно нужно).
    """
    rec = receipt if receipt_sane(receipt) else {}
    known = set(filter(None, [rec.get("slot"), rec.get("module_name")]))
    known |= set(rec.get("zbic_files") or [])
    if rec.get("npdm_sha256"):
        known |= {"main.npdm", "main.npdm.orig"}
    # Файлы exefs, перенесённые из МОДА: их тоже положили мы.
    known |= set((rec.get("mod") or {}).get("exefs") or [])
    # Подменённая библиотека sdk (install.py --swap-sdk): её exefs/sdk
    # положили мы. Отложенная копия прежнего (чужого) файла — тоже наша забота.
    swap = rec.get("sdk_swap") or {}
    if swap.get("file"):
        known.add(swap["file"])
    if swap.get("foreign_backup"):
        known.add(swap["foreign_backup"])
    return known


def zbic_only_layout(dest, tid, expected, allowed=(), weak=False):
    """
    Гейт режима «только снять ZBIC»: в раскладке РОВНО то, что обещано.

    Обещано буквально следующее: в atmosphere/contents/<TID>/exefs/ появляются
    несжатые NSO и больше ничего — ни subsdkN мода, ни main.npdm, ни files/.
    Проверять это нужно именно гейтом: режим тем и ценен, что даёт человеку с
    незапускающейся игрой ОДИН файл, а не мод целиком, и «заодно положили модуль»
    было бы не мелочью, а другой установкой.

    allowed — имена, которые лежали в папке ДО нас (чужие файлы и наши прежние):
    они не «лишнее». expected — что мы обещали положить.

    weak=True — НАРОЧНО ослабленная проверка для негативной пробы: смотрит только,
    что обещанные файлы на месте, и не смотрит, не легло ли лишнее. В работе не
    используется: её зелёный итог ничего не значит, и это доказывает проба.
    """
    base = Path(dest) / "atmosphere" / "contents" / str(tid).upper()
    exefs = base / "exefs"
    expected = sorted(expected)
    if not expected:
        # Затвор нуля: гейт, которому нечего проверять, обязан краснеть, а не
        # радостно молчать. Иначе пустой список делал бы его вечно зелёным.
        return ["затвор нуля: список обещанных файлов пуст — гейт ничего не проверил"]
    names = sorted(p.name for p in exefs.iterdir() if p.is_file()) if exefs.is_dir() else []
    problems = ["обещанный файл %s не разложен в %s" % (n, exefs)
                for n in expected if n not in names]
    if weak:
        return problems
    known = set(expected) | set(allowed)
    extra = [n for n in names
             if n not in known and ".bak-" not in n and ".removed-" not in n]
    if extra:
        problems.append("в exefs легло лишнее (режим обещал только несжатые NSO): "
                        + ", ".join(extra))
    if (base / "files").is_dir():
        problems.append("появилась папка files/ — в режиме «только ZBIC» файлов мода "
                        "быть не должно")
    return problems


def receipt_lists_zbic(receipt, expected, weak=False):
    """
    Гейт квитанции: каждый снятый файл в ней ОТМЕЧЕН как наш.

    Зачем отдельной проверкой. Именно этот список решает, чьим считать main в
    exefs. Нет его — и следующая установка мода объявит НАШ ЖЕ файл чужим и
    испугает человека предупреждением «заменяем не наше», а --uninstall не
    снимет то, что положил.

    weak=True — ослабленная проверка: довольствуется тем, что квитанция вообще
    есть, и в список не смотрит. Негативная проба показывает, что её зелёный
    итог ничего не стоит.
    """
    expected = sorted(expected)
    if not expected:
        return ["затвор нуля: список снятых файлов пуст — проверять было нечего"]
    if not isinstance(receipt, dict) or not receipt or "__битая__" in receipt:
        return ["квитанции нет или она не читается"]
    if weak:
        return []
    listed = set(receipt.get("zbic_files") or [])
    return ["в квитанции не отмечен снятый файл %s (следующая установка сочтёт его "
            "чужим и напугает предупреждением «заменяем не наше»)" % n
            for n in expected if n not in listed]


# -------------------------------------------------------------------- ZBIC


def zbic_module():
    """scripts/zbic_nso.py как модуль: распознавание и гейт — один и тот же код."""
    return load_script(ZBIC_NSO, "nxr_zbic_nso")


def zbic_scan(exefs, fix=False):
    """
    Осмотр exefs на сжатие ZBIC. Делается ВСЕГДА, независимо от ключей.

    Распознавание идёт ПО СОДЕРЖИМОМУ: у каждого файла папки читается заголовок
    NSO0 и бит 7 флагов. Ни имя игры, ни имя файла, ни TitleID на вывод не
    влияют — поэтому ZBIC найдётся и там, где его по имени никто не ждал
    (у Nintendo он пока только у `main`, но проверка этого не предполагает).
    """
    res = zbic_module().scan(exefs)
    if not res.get("ok"):
        emit("ZBIC_SCANNED", "exefs осмотреть не удалось", level="warn",
             exefs=str(exefs), reason=res.get("reason"), zbic=False)
        return res
    say("      сжатие ZBIC: " + (", ".join(res["zbic_files"]) if res["zbic"]
                                 else "не найдено (обычный exefs)"),
        code="ZBIC_SCANNED", exefs=res["exefs"], nso_files=res["nso_files"],
        zbic_files=res["zbic_files"], zbic=res["zbic"], not_nso=res["not_nso"])
    if res["zbic"]:
        emit("ZBIC_DETECTED", "в exefs есть NSO со сжатием ZBIC", level="warn",
             exefs=res["exefs"], files=res["zbic_files"], fix=bool(fix),
             flags=[f["flags"] for f in res["zbic_details"]],
             segments=[f["segments"] for f in res["zbic_details"]])
        say("      ВНИМАНИЕ: у %s стоит бит ZBIC (сжатие zstd-ZBIC, HOS 22.0.0+)."
            % ", ".join(res["zbic_files"]), level="warn")
        if not fix:
            say("      Atmosphère младше 1.12.0 такой модуль не загрузит "
                "(«Could not start the software»).", level="warn")
            say("      Снять ZBIC: тот же запуск с ключом --zbic-fix.", level="warn")
    return res


def zbic_available():
    """
    (умеем ли снимать ZBIC, чем именно).

    Раньше это значило «есть ли пакет zbic», и на машине без него конвейер
    отказывал. Теперь рядом лежит наш scripts/zbic_pure.py — разбор формата на
    чистом Python, без внешних пакетов, — поэтому ответ практически всегда «да»,
    а второе значение говорит, какой путь выбран: быстрый пакет или наш.
    """
    mod = zbic_module()
    name, ver = mod.backend()
    if name == "zbic":
        return True, "пакет zbic %s" % (ver or "?")
    if name == "pure":
        return True, "чистый Python (%s)" % mod.PURE_NAME
    return False, None


def zbic_rebuild(src, dst):
    """
    Собрать несжатый NSO вместо ZBIC-NSO и ПРОВЕРИТЬ результат гейтом.

    Гейт запускается отдельным процессом (scripts/zbic_nso.py --check): его код
    возврата — настоящий, а не «мы сами у себя спросили». Пересборка без гейта
    наружу не выходит.
    """
    mod = zbic_module()
    src, dst = Path(src), Path(dst)
    if not mod.zbic_ready():
        # Сюда можно попасть, только если рядом со скриптом нет zbic_pure.py
        # И нет пакета: наш путь зависимостей не требует, поэтому отказ
        # «распаковать нечем» из рабочего сценария исчез.
        fail("снять сжатие ZBIC нечем: рядом с %s нет %s, и пакет `zbic` тоже "
             "не установлен.\n        Верните %s в scripts/ (он идёт в том же "
             "архиве) или поставьте быстрый путь: pip install zbic\n"
             "        (%s/%s, %s, лицензия %s)."
             % (ZBIC_NSO.name, mod.PURE_NAME, mod.PURE_NAME, ZBIC_PKG["author"],
                ZBIC_PKG["name"], ZBIC_PKG["url"], ZBIC_PKG["license"]),
             code="FAIL_ZBIC_TOOL_MISSING", package=ZBIC_PKG["name"],
             version=ZBIC_PKG["version"], license=ZBIC_PKG["license"],
             url=ZBIC_PKG["url"], how="вернуть " + mod.PURE_NAME)
    try:
        info = mod.rebuild(src, dst)
    except Exception as e:
        fail("не удалось собрать несжатый NSO из %s: %s: %s" % (src.name, type(e).__name__, e),
             code="FAIL_ZBIC_REBUILD", src=str(src), dst=str(dst),
             detail="%s: %s" % (type(e).__name__, e))
    res = run_py([ZBIC_NSO, "--check", dst, "--orig", src])
    if res.returncode != 0:
        say(tail(res.stdout + res.stderr, 4000))
        fail("гейт пересобранного NSO не пройден: " + str(dst), code="FAIL_GATE_ZBIC",
             gate="zbic", gate_rc=res.returncode, path=str(dst), orig=str(src),
             detail=last_line(res.stdout + res.stderr))
    for line in info["report"]:
        say("      " + line.strip())
    say("      %s: %d Б -> %d Б без сжатия (flags 0x%02x)"
        % (src.name, info["size_before"], info["size_after"], info["flags_after"]),
        code="ZBIC_REBUILT", src=info["src"], dst=info["dst"], name=src.name,
        size_before=info["size_before"], size_after=info["size_after"],
        module_id=info["module_id"], flags_after=info["flags_after"],
        src_sha256=info["src_sha256"], dst_sha256=info["dst_sha256"],
        segments=info["segments"])
    say("      гейт zbic пройден: " + last_line(res.stdout + res.stderr),
        code="GATE_PASS", gate="zbic", path=str(dst),
        detail=last_line(res.stdout + res.stderr))
    return info


def zbic_prepare_exefs(game_exefs, out_dir, scan=None, required=True):
    """
    Подготовить несжатые NSO для всех файлов exefs с битом ZBIC.

    Источник — ТОЛЬКО папка game_exefs, та же самая, из которой взяты main.npdm
    (а значит TitleID и права) и модули для гейтов. Отдельного ключа «взять main
    откуда-то ещё» НЕТ и быть не должно: если у пользователя установлено
    обновление, exefs процесса берётся из патча, и подложить main от базы значит
    получить неработающую игру. Перепутать нельзя ровно потому, что выбирать
    нечего — берётся то, что лежит рядом с main.npdm этого же дампа, а гейт
    сверяет ModuleId и sha256 сегментов с заголовком ЭТОГО же файла.

    Возвращает {имя файла: (путь к несжатому, info)}.
    """
    game_exefs, out_dir = Path(game_exefs), Path(out_dir)
    scan = scan if scan is not None else zbic_module().scan(game_exefs)
    if not scan.get("ok") or not scan["zbic_files"]:
        if not required:
            # Мягкий путь для комплекта: ZBIC — свойство КОНКРЕТНОЙ игры, а не
            # выбора человека. Комплект собирается одним ключом, и отказывать
            # из-за того, что у этой игры сжатия нет, значило бы требовать от
            # человека знать заранее то, что конвейер и так видит сам.
            say("      сжатия ZBIC в этой игре нет — несжатый main в комплект "
                "не кладётся (это не отказ)")
            return {}
        fail("сжатия ZBIC в %s нет — снимать нечего (ключ --zbic-fix задан зря либо "
             "exefs не от той версии игры)" % game_exefs,
             code="FAIL_ZBIC_NOT_FOUND", exefs=str(game_exefs),
             nso_files=scan.get("nso_files", []))
    out_dir.mkdir(parents=True, exist_ok=True)
    made = {}
    for name in scan["zbic_files"]:
        made[name] = (out_dir / name, zbic_rebuild(game_exefs / name, out_dir / name))
    return made


# ------------------------------- совместимость: ключи, гейт символов, комплект


def compat_norm(compat):
    """
    Набор совместимости: () — без автоисправления, ("hash", "ldn") — с ним.

    Модуль умеет только «всё или ничего»: экспорты автоисправления
    включаются одним ключом C++ (их выключает -DNXR_NO_AUTOFIX). Поэтому любой
    непустой набор (в том числе от устаревших --compat-hash / --compat-ldn)
    означает полный. Неизвестное имя — по-прежнему ошибка.
    """
    got = set(compat or ())
    unknown = sorted(got - set(COMPAT_SYMS))
    if unknown:
        raise ValueError("неизвестный ключ совместимости: " + ", ".join(unknown))
    return ("hash", "ldn") if got else ()


def compat_expect(compat):
    """Что должен ждать hosttest/check_compat_syms.py: none|hash|ldn|both."""
    return COMPAT_EXPECT[compat_norm(compat)]


def compat_suffix(compat):
    """
    Кусок имени папки сборки: "" (без автоисправления) | "-autofix".

    *Прежние сборки:* "" | "-compat-hash" | "-compat-ldn" | "-compat-both" плюс
    отдельный "-nvrmfix"; старые папки в out/ остаются как есть и конвейером
    больше не выбираются (кроме проб, где это сказано явно).

    Зачем отдельная папка. Модуль с доэкспортом и без него — РАЗНЫЕ бинарники с
    одним и тем же именем файла (имя файла — это слот). Клади мы их в одну папку,
    `--skip-build` и «взять уже собранный» молча подсунули бы не тот модуль, и
    человек с родной библиотекой получил бы перекрытые символы, ничего об этом не
    зная. Разные папки делают путаницу невозможной, а не маловероятной.
    """
    return "-autofix" if compat_norm(compat) else ""


def gfx_suffix(diag_gfx):
    """
    Кусок имени папки сборки для диагностики графики: "" | "-gfxdiag".

    Зачем отдельная папка — ровно то же, что у compat_suffix(), и обожглись на
    этом уже дважды: make сравнивает ВРЕМЯ файлов, а не строку флагов. Собери мы
    модуль с -DNXR_DIAG_GFX в той же папке, где лежит свежий обычный, make сказал
    бы «up to date» и отдал прежний бинарник — без единой строки диагностики,
    которую мы только что попросили. Разные папки делают это невозможным, а не
    маловероятным. То же самое и в обратную сторону: рабочая сборка после
    диагностической не должна получить диагностический бинарник.
    """
    return "-gfxdiag" if diag_gfx else ""


def lock_suffix(diag_lock):
    """Кусок имени папки сборки для узкой пробы блокировок: "" | "-lockdiag".

    Та же причина, что у gfx_suffix(): make сравнивает время файлов, а не
    строку флагов, и сборка с другим -D в той же папке молча отдала бы
    прежний бинарник.
    """
    return "-lockdiag" if diag_lock else ""


def nvrm_suffix(nvrm):
    """Кусок имени папки для исправления NvRm. Теперь всегда "": NvRm входит
    в автоисправление и в имени папки виден как "-autofix" (compat_suffix).
    *Было:* "" | "-nvrmfix"."""
    return ""


def is_autofix_dir(path):
    """Модуль из папки с автоисправлением (или прежней «-nvrmfix», где NvRm был)."""
    name = Path(path).parent.name
    return "-autofix" in name or "-nvrmfix" in name


def module_dir_name(compat=(), diag_gfx=False, diag_lock=False, nvrm=False, early=False):
    """Имя папки модуля: module[-autofix][-earlyfs][-gfxdiag][-lockdiag]. Одно место.
    -earlyfs — ранний перехват nn::fs для мода со своим модулем."""
    return ("module" + compat_suffix(compat) + nvrm_suffix(nvrm) + early_suffix(early)
            + gfx_suffix(diag_gfx) + lock_suffix(diag_lock))


def compat_cflags(compat):
    """Флаги компиляции: автоисправление в C++ включено по умолчанию, поэтому
    флаг нужен только чтобы его ВЫКЛЮЧИТЬ."""
    return [] if compat_norm(compat) else [NO_AUTOFIX_CFLAG]


def compat_human(compat):
    """Человеческое перечисление включённых символов (для лога и паспорта)."""
    got = compat_norm(compat)
    if not got:
        return "ни одного (сборка --no-autofix)"
    return ("АВТОИСПРАВЛЕНИЕ: " + ", ".join(autofix_symbols())
            + " — каждый прокси: на родной sdk зовёт её функцию, на подменённой — свою")


def autofix_catalog():
    """Каталог исправлений (тот же файл, что компилируется в модуль)."""
    try:
        return load_script(STUBDIFF, "nxr_stubdiff").load_catalog(AUTOFIX_CATALOG)
    except Exception:
        return []


def autofix_symbols():
    cat = autofix_catalog()
    return [c["symbol"] for c in cat] or list(COMPAT_SYMS.values()) + [NVRM_SYM]


def check_compat_syms(nso, compat, nvrm=None):
    """
    Гейт .dynsym собранного модуля: символы совместимости есть РОВНО там, где ждём.

    Проверка идёт в обе стороны и это не перестраховка: «символа нет в обычной
    сборке» — такое же требование, как «символ есть в специальной». Обычный
    модуль с лишним экспортом перекрыл бы рабочую функцию `sdk` у человека с
    родной библиотекой.
    """
    if not CHECK_COMPAT_SYMS.is_file():
        return 2, "нет " + str(CHECK_COMPAT_SYMS)
    # NvRm входит в автоисправление: ожидание по умолчанию следует за набором.
    if nvrm is None:
        nvrm = bool(compat_norm(compat))
    res = run_py([CHECK_COMPAT_SYMS, nso, "--expect", compat_expect(compat),
                  "--nvrm", "yes" if nvrm else "no"])
    return res.returncode, res.stdout + res.stderr


def gate_compat_syms(nso, compat, nvrm=None):
    """Тот же гейт, но роняющий прогон: одна точка отказа на все сборки."""
    rc, out = check_compat_syms(nso, compat, nvrm=nvrm)
    if rc != 0:
        say(tail(out, 4000))
        fail("гейт символов совместимости не пройден: " + str(nso),
             code="FAIL_GATE_COMPAT_SYMS", gate="compat_syms", gate_rc=rc,
             path=str(nso), expect=compat_expect(compat),
             detail=last_line(out))
    say("      гейт символов совместимости (ждали «%s»): %s"
        % (compat_expect(compat), last_line(out)),
        code="GATE_PASS", gate="compat_syms", path=str(nso),
        expect=compat_expect(compat), detail=last_line(out))
    return out


# ------------------------------------------------- распознавание: чего не хватает


def nso_min_module():
    """hosttest/nso_min.py как модуль: разбор NSO один и тот же везде."""
    return load_script(NSO_MIN, "nxr_nso_min")


def sdk_library_files(path):
    """
    Файлы библиотек, которые указал пользователь: файл или все файлы папки.

    Никаких имён конкретных библиотек и версий: что дали, то и читаем.
    """
    p = Path(path)
    if p.is_file():
        return [p]
    if p.is_dir():
        return [q for q in sorted(p.iterdir()) if q.is_file()]
    return []


def validate_sdk_paths(sdk_paths):
    """
    Путь, указанный в --sdk, обязан существовать.

    Молча пропустить опечатку нельзя: человек получил бы тишину вместо ответа
    «хватает символов или нет» и решил бы, что проверка прошла.
    """
    for p in sdk_paths or ():
        if not Path(p).exists():
            fail("--sdk указывает на путь, которого нет: %s" % p,
                 code="FAIL_SDK_BAD_ARG", path=str(p))
    return list(sdk_paths or [])


def nnsdk_identify(path):
    """
    Это библиотека nnSdk? Для install.py --swap-sdk.

    Признаки — те же, что у scripts/stubdiff.py (один разбор на весь конвейер):
    заголовок NSO0, имя модуля в начале rodata — «nnSdk», и строка
    «SDK MW+Nintendo+NintendoSdk_…-X_Y_Z» в хвосте rodata (из неё — версия).
    Любой другой NSO (movie, OpenGL, main игры) и не-NSO — не nnSdk.

    Возвращает {ok, reason, magic, module, version, version_text, sha256, size, zbic}.
    """
    p = Path(path)
    info = {"ok": False, "reason": None, "path": str(p), "magic": None, "module": None,
            "version": None, "version_text": None, "sha256": None, "size": None,
            "zbic": False}
    if not p.is_file():
        info["reason"] = "файла нет" if not p.exists() else "это не файл"
        return info
    info["size"] = p.stat().st_size
    info["sha256"] = file_sha256(p)
    with open(p, "rb") as f:
        head = f.read(0x10)
    info["magic"] = head[:4].decode("latin-1")
    if head[:4] != b"NSO0":
        info["reason"] = "это не NSO (заголовок %r вместо NSO0)" % head[:4]
        return info
    info["zbic"] = len(head) >= 0x10 and bool(struct.unpack_from("<I", head, 0xC)[0] & 0x80)
    try:
        sd = load_script(STUBDIFF, "nxr_stubdiff")
        n = sd.NSO(p)
        info["module"] = sd.module_name(n)
        ver = sd.mw_version(n)
        sdk_name = sd.SDK_NAME
    except Exception as e:
        info["reason"] = "NSO не разбирается: %s: %s" % (type(e).__name__, e)
        return info
    if ver:
        info["version"] = list(ver)
        info["version_text"] = ".".join(str(x) for x in ver)
    if info["module"] != sdk_name:
        info["reason"] = "имя модуля %r, а не %s" % (info["module"], sdk_name)
        return info
    if not ver:
        info["reason"] = "нет строки «SDK MW…» с версией — это не сборка nnSdk"
        return info
    info["ok"] = True
    return info


def missing_symbols(game_exefs, sdk_paths):
    """
    Чего не хватит игре, если её модули связать с УКАЗАННЫМИ библиотеками.

    Считается честно и без единого имени игры: берутся импорты всех NSO из exefs
    игры, из них вычитаются экспорты этих же NSO (модули игры разрешают символы
    друг друга) и экспорты каждого файла, который назвал пользователь. Что
    осталось — то rtld не найдёт, и процесс упадёт на неразрешённом символе.

    `sdk` из самого exefs игры в расчёт НЕ берётся: смысл проверки ровно в том,
    что на консоли вместо него грузится другая библиотека.

    Возвращает словарь с полями ok/reason/missing/compat_missing/...
    """
    game_exefs = Path(game_exefs)
    files = []
    for p in sdk_paths or ():
        files += sdk_library_files(p)
    if not files:
        return {"ok": False, "reason": "библиотека не указана",
                "sdk": [], "missing": [], "missing_weak": [], "compat_missing": []}
    try:
        nso = nso_min_module()
    except Exception as e:
        return {"ok": False, "reason": "не удалось загрузить разбор NSO: %s" % e,
                "sdk": [], "missing": [], "missing_weak": [], "compat_missing": []}

    # Импорт запоминается вместе со СВЯЗЫВАНИЕМ: WEAK-символ (bind = 2), которого
    # нет ни у кого, процесс не роняет — rtld оставляет ноль, и код это переживает
    # (так устроены, например, ZSTD_trace_*, которые не находятся даже с родной
    # библиотекой игры). Сваливать их в одну кучу с обязательными значило бы
    # пугать человека находками там, где всё в порядке.
    imports, exports, read, unreadable = {}, set(), [], []
    for p in sorted(game_exefs.iterdir()) if game_exefs.is_dir() else []:
        if not p.is_file() or p.name == "sdk" or p.suffix:
            continue
        try:
            n = nso.NSO(str(p))
        except Exception as e:
            unreadable.append({"file": p.name, "why": "%s: %s" % (type(e).__name__, e)})
            continue
        for nm, (info,) in n.imports().items():
            imports[nm] = info
        exports |= set(n.exports())
        read.append(p.name)
    if not read:
        return {"ok": False, "reason": "в exefs игры не нашлось ни одного читаемого NSO",
                "sdk": [str(p) for p in files], "missing": [], "missing_weak": [],
                "compat_missing": [], "unreadable": unreadable}

    libs = []
    for p in files:
        try:
            n = nso.NSO(str(p))
        except Exception as e:
            unreadable.append({"file": p.name, "why": "%s: %s" % (type(e).__name__, e)})
            continue
        ex = set(n.exports())
        exports |= ex
        libs.append({"file": str(p), "name": p.name, "exports": len(ex),
                     "buildid": n.buildid[:40]})
    if not libs:
        return {"ok": False, "reason": "ни один указанный файл не разобрался как NSO",
                "sdk": [str(p) for p in files], "missing": [], "missing_weak": [],
                "compat_missing": [], "unreadable": unreadable}

    not_found = {nm: info for nm, info in imports.items() if nm not in exports}
    # bind: 1 = GLOBAL (обязателен), 2 = WEAK (не найден — не беда).
    missing = sorted(nm for nm, info in not_found.items() if (info >> 4) != 2)
    missing_weak = sorted(nm for nm, info in not_found.items() if (info >> 4) == 2)
    # Из недостающего отдельно называем то, что наш модуль УМЕЕТ доэкспортировать.
    compat_missing = sorted(k for k, sym in COMPAT_SYMS.items() if sym in set(missing))
    return {"ok": True, "reason": None, "exefs": str(game_exefs),
            "game_nso": read, "sdk": [str(p) for p in files], "libs": libs,
            "imports": len(imports), "exports": len(exports),
            "missing": missing, "missing_weak": missing_weak,
            "compat_missing": compat_missing, "unreadable": unreadable}


AUTOFIX_PASSPORT = "nxredirect-passport.txt"


def report_game_profile(game_exefs, cnmt=None):
    """Профиль игры (п.2 автоисправления): графика, родная sdk, ZBIC, CNMT."""
    try:
        sd = load_script(STUBDIFF, "nxr_stubdiff")
        pr = sd.profile(game_exefs, cnmt)
    except Exception as e:
        say("      профиль игры не снят: %s: %s" % (type(e).__name__, e), level="warn",
            code="GAME_PROFILE", exefs=str(game_exefs), ok=False, reason=str(e))
        return None
    lines = sd.profile_lines(pr)
    for line in lines:
        say("      " + line)
    c = pr.get("cnmt") or {}
    emit("GAME_PROFILE", "профиль игры", exefs=str(game_exefs), ok=True,
         sdk_version=pr["sdk_version"], sdk_buildid=pr["sdk_buildid"],
         graphics=pr["graphics_libraries"], main_imports=pr["main_imports_graphics"],
         zbic_files=pr["zbic_files"],
         required_system_version=c.get("required_system_version"),
         required_text=c.get("text"))
    return pr


def report_autofix_forecast(game_exefs, sdk_paths):
    """
    Что модуль выберет на консоли с указанной sdk (своя/прокси) и что нашёл
    детектор заглушек. Только сообщение: решает модуль на консоли сам.
    """
    files = []
    for p in sdk_paths or ():
        files += sdk_library_files(p)
    try:
        sd = load_script(STUBDIFF, "nxr_stubdiff")
    except Exception as e:
        say("      прогноз автоисправления не сделан: %s" % e, level="warn")
        return None
    out = None
    for f in files:
        try:
            nso = sd.NSO(f)
            if sd.module_name(nso) != "nnSdk":
                continue
            fc = sd.forecast(f, game_exefs)
        except Exception:
            continue
        say("      прогноз автоисправления на консоли с %s (sdk %s, библиотеки игры %s, "
            "подмена %s, Vulkan %s):"
            % (Path(f).name, fc["sdk_version"], fc["game_version"],
               "ДА" if fc["swapped"] else "нет", "да" if fc["vulkan"] else "нет"),
            code="AUTOFIX_FORECAST", sdk=str(f), sdk_version=fc["sdk_version"],
            game_version=fc["game_version"], swapped=fc["swapped"], vulkan=fc["vulkan"],
            fixes={k: {"choice": v["choice"], "why": v["why"]} for k, v in fc["fixes"].items()})
        for k, v in fc["fixes"].items():
            say("        %-17s %-6s (%s)" % (k, "СВОЯ" if v["choice"] == "own" else "ПРОКСИ", v["why"]))
        own_sdk = Path(game_exefs) / "sdk"
        if own_sdk.is_file():
            d = sd.detect(f, own_sdk, game_exefs)
            say("      детектор заглушек (%s -> родная %s): известных %d, НОВЫХ без исправления %d"
                % (d["old_version"], d["new_version"], len(d["known"]), len(d["new_unfixed"])),
                code="STUBS_DETECTED", level="warn" if d["new_unfixed"] else "info",
                old=str(f), new=str(own_sdk), old_version=d["old_version"],
                new_version=d["new_version"],
                known=[x["symbol"] for x in d["known"]],
                new_unfixed=[x["symbol"] for x in d["new_unfixed"]])
            for x in d["new_unfixed"]:
                say("        НОВАЯ — найдено, исправления НЕТ: %s (%s%s), импортируют %s"
                    % (x["symbol"], x["kind"], ", WEAK" if x["weak"] else "",
                       ",".join(x["importers"])), level="warn")
        out = fc
    return out


def report_missing_symbols(game_exefs, sdk_paths):
    """
    Сказать человеку, НУЖЕН ли ему вариант для подменённой библиотеки.

    Молчание здесь хуже ошибки: если библиотеку не указали, честный ответ —
    «проверить нечем», а не «всё в порядке». Отказом прогон не роняется: это
    распознавание, а не гейт.
    """
    if not sdk_paths:
        say("      библиотека для сверки не указана (--sdk): проверить, хватит ли игре "
            "символов, НЕЧЕМ — ни подтвердить, ни опровергнуть",
            code="COMPAT_SDK_UNKNOWN", level="warn", exefs=str(game_exefs),
            how="--sdk <файл или папка с библиотекой вашей консоли>")
        return None
    res = missing_symbols(game_exefs, sdk_paths)
    if not res.get("ok"):
        say("      сверка с указанной библиотекой не удалась: " + str(res.get("reason")),
            code="COMPAT_SDK_UNKNOWN", level="warn", exefs=str(game_exefs),
            reason=res.get("reason"), sdk=res.get("sdk"))
        return res
    report_autofix_forecast(game_exefs, sdk_paths)
    names = ", ".join(l["name"] for l in res["libs"])
    say("      сверка с библиотекой (%s): импортов у модулей игры %d, обязательных не "
        "находится %d (плюс %d необязательных, WEAK — они процесс не роняют)"
        % (names, res["imports"], len(res["missing"]), len(res["missing_weak"])),
        code="COMPAT_SYMS_SCANNED", exefs=res["exefs"], sdk=res["sdk"],
        libs=res["libs"], game_nso=res["game_nso"], imports=res["imports"],
        missing=res["missing"], missing_weak=res["missing_weak"],
        compat_missing=res["compat_missing"], unreadable=res["unreadable"])
    if not res["missing"]:
        say("      все обязательные символы в этой библиотеке есть — вариант для "
            "подменённой библиотеки с НЕЙ этой игре НЕ НУЖЕН")
        return res
    for sym in res["missing"][:20]:
        knows = [k for k, s in COMPAT_SYMS.items() if s == sym]
        say("        не находится: %s%s"
            % (sym, "   <- это умеет доэкспортировать наш модуль (%s)" % knows[0]
               if knows else ""), level="warn")
    if len(res["missing"]) > 20:
        say("        … и ещё %d" % (len(res["missing"]) - 20), level="warn")
    unknown = [m for m in res["missing"] if m not in set(autofix_symbols())]
    if not unknown:
        say("      всё недостающее закрывает АВТОИСПРАВЛЕНИЕ модуля — ключей не нужно "
            "(модуль включит своё сам; см. прогноз ниже)")
    else:
        say("      ВНИМАНИЕ: не хватает символов, которых в каталоге исправлений НЕТ: "
            + ", ".join(unknown[:8]) + " — модуль их не закроет", level="warn")
    return res


# --------------------------------------------- гейт согласованности комплекта


# Затвор нуля комплекта: гейт, сделавший меньше проверок, ничего не доказал.
MIN_KIT_CHECKS = 4


def kit_problems(exefs_dir, tid, source_exefs=None, slot=None, compat=(),
                 zbic_names=(), npdm_expected=True, weak=False, nvrm=False):
    """
    Гейт: всё в раскладке — от ОДНОЙ игры и ОДНОЙ версии.

    Что сверяется (каждая проверка считается, см. затвор нуля):
      1. program id внутри main.npdm раскладки совпадает с TID самой раскладки —
         иначе комплект лежит в папке чужой игры и не подхватится (или, хуже,
         подхватится не той игрой);
      2. main.npdm раскладки сделан из main.npdm ТОГО ЖЕ exefs-источника (тот же
         program id, что у источника);
      3. каждый несжатый NSO — та же версия, что в exefs-источнике: гейт
         scripts/zbic_nso.py --check --orig сверяет ModuleId и sha256 сегментов с
         заголовком ЭТОГО же исходного файла;
      4. модуль лежит под именем слота (имя файла на SD и есть слот) и опознаётся
         своим распознаванием как наш;
      5. символы совместимости в модуле — ровно те, что обещаны ключами.

    weak=True — НАРОЧНО ослабленный вариант для негативной пробы: смотрит только,
    что обещанные файлы лежат на месте, и ничего не сверяет. В работе не
    используется: её зелёный итог ничего не значит, и это доказывает проба.

    Возвращает (список находок, число выполненных проверок).
    """
    exefs_dir = Path(exefs_dir)
    problems, checks = [], 0
    tid = (tid or "").upper()

    npdm = exefs_dir / "main.npdm"
    if npdm_expected:
        checks += 1
        if not npdm.is_file():
            problems.append("в раскладке нет main.npdm, а комплект его обещал")
        elif not weak:
            checks += 1
            try:
                got = npdm_program_id(npdm)
            except Exception as e:
                got = None
                problems.append("main.npdm раскладки не разбирается: %s: %s"
                                % (type(e).__name__, e))
            if got and got != tid:
                problems.append("program id в main.npdm = %s, а раскладка лежит в папке "
                                "игры %s — это файлы РАЗНЫХ игр" % (got, tid))
            if got and source_exefs:
                checks += 1
                src_npdm = Path(source_exefs) / "main.npdm"
                try:
                    src_id = npdm_program_id(src_npdm) if src_npdm.is_file() else None
                except Exception:
                    src_id = None
                if src_id is None:
                    problems.append("не из чего сверить версию прав: нет %s" % src_npdm)
                elif src_id != got:
                    problems.append("main.npdm раскладки от игры %s, а exefs-источник "
                                    "от %s" % (got, src_id))

    for name in sorted(zbic_names or ()):
        checks += 1
        dst = exefs_dir / name
        if not dst.is_file():
            problems.append("в раскладке нет несжатого %s, а он обещан" % name)
            continue
        if weak:
            continue
        checks += 1
        src = Path(source_exefs) / name if source_exefs else None
        if src is None or not src.is_file():
            problems.append("не с чем сверить версию %s: нет исходного файла в %s"
                            % (name, source_exefs))
            continue
        res = run_py([ZBIC_NSO, "--check", dst, "--orig", src])
        if res.returncode != 0:
            problems.append("%s в раскладке не сходится с тем же файлом exefs-источника "
                            "(ModuleId/хеши сегментов): %s"
                            % (name, last_line(res.stdout + res.stderr)))

    if slot:
        checks += 1
        mod = exefs_dir / slot
        if not mod.is_file():
            problems.append("в раскладке нет модуля %s" % slot)
        elif not weak:
            checks += 1
            cls, why = classify_nso(mod)
            if cls != "ours":
                problems.append("файл %s не опознан как наш модуль (%s: %s)"
                                % (slot, cls, why))
            # Имя файла на SD И ЕСТЬ слот: модуль под другой слот переименовывать
            # нельзя. В диагностической сборке слот виден прямо в образе (модуль
            # печатает его в отчёт) — тогда сверяем дословно.
            checks += 1
            try:
                img = load_script(CHECK_NSO, "nxr_check_nso").Nso(mod).image
            except Exception as e:
                img = b""
                problems.append("модуль %s не разбирается как NSO: %s: %s"
                                % (slot, type(e).__name__, e))
            found = sorted({("subsdk%d" % n) for n in range(10)
                            if ("subsdk%d" % n).encode() in img})
            if found and slot not in found:
                problems.append("модуль лежит как %s, а собран под %s (имя файла на SD "
                                "и есть слот, переименовывать нельзя)"
                                % (slot, ", ".join(found)))
            checks += 1
            rc, out = check_compat_syms(mod, compat, nvrm=nvrm)
            if rc != 0:
                problems.append("символы совместимости в %s не те, что обещаны "
                                "ключами (ждали «%s»): %s"
                                % (slot, compat_expect(compat), last_line(out)))

    return problems, checks


def gate_kit(exefs_dir, tid, source_exefs=None, slot=None, compat=(),
             zbic_names=(), npdm_expected=True, nvrm=False):
    """
    Тот же гейт, но роняющий прогон, с затвором нуля.

    Затвор обязателен: гейт, которому нечего было проверить, не «пройден», а
    красный — иначе пустая раскладка выглядела бы как согласованный комплект.
    """
    problems, checks = kit_problems(exefs_dir, tid, source_exefs=source_exefs,
                                    slot=slot, compat=compat, zbic_names=zbic_names,
                                    npdm_expected=npdm_expected, nvrm=nvrm)
    if checks < MIN_KIT_CHECKS:
        problems = problems + [
            "затвор нуля: гейт комплекта выполнил %d проверок из %d — он ничего "
            "(или почти ничего) не проверил" % (checks, MIN_KIT_CHECKS)]
    if problems:
        for p in problems:
            say("      ПРОБЛЕМА: " + p, level="warn")
        fail("комплект не согласован: в раскладке файлы разных игр/версий",
             code="FAIL_GATE_KIT", gate="kit", exefs=str(exefs_dir), tid=tid,
             problems=problems, checks=checks)
    say("      гейт комплекта пройден: проверок %d (TitleID, версия main, слот, "
        "символы)" % checks, code="GATE_PASS", gate="kit", path=str(exefs_dir),
        detail="проверок %d" % checks)
    return checks


# ------------------------------------------------------------------ шаги


def build_module(slot, diag, out_nso, work_dir=None, compat=(), diag_gfx=False,
                 diag_lock=False, nvrm=False, early=False):
    """
    Шаг 1: сборка модуля в WSL.

    Копируем во временную папку БЕЗ пробелов И exlaunch/, И src/ — в
    exlaunch/source/program/main.cpp лежит прокладка с #include "../../../src/main.cpp",
    без src/ сборка не найдёт исходник.
    """
    if not (EXLAUNCH / "Makefile").is_file():
        fail(f"нет {EXLAUNCH / 'Makefile'}", code="FAIL_NO_MAKEFILE",
             path=str(EXLAUNCH / "Makefile"))
    if not (SRC / "main.cpp").is_file():
        fail(f"нет {SRC / 'main.cpp'}", code="FAIL_NO_SOURCE",
             path=str(SRC / "main.cpp"))

    # Временная папка сборки зависит и от ключей совместимости. Причина не в
    # аккуратности: make сравнивает ВРЕМЯ файлов, а не строку флагов, и сборка с
    # другим -D в той же папке молча сказала бы «up to date» и отдала прежний
    # бинарник — без единого символа, который мы только что попросили.
    work = work_dir or ("/tmp/nxr-build-" + slot + ("-diag" if diag else "")
                        + compat_suffix(compat) + nvrm_suffix(nvrm) + early_suffix(early)
                        + gfx_suffix(diag_gfx) + lock_suffix(diag_lock))
    if " " in work:
        fail("путь сборки в WSL не должен содержать пробелов: " + work,
             code="FAIL_WSL_PATH_SPACES", work=work)

    # Ключи совместимости идут теми же C_FLAGS, что и -DNXR_DIAG: другого способа
    # донести -D до Makefile exlaunch нет, а собранный результат потом всё равно
    # проверяется по .dynsym (гейт compat_syms), а не по строке команды.
    c_flags = " ".join([BASE_C_FLAGS] + (["-DNXR_DIAG"] if diag else [])
                       + compat_cflags(compat)
                       + ([EARLY_FS_CFLAG] if early else [])
                       + ([GFX_CFLAG] if diag_gfx else [])
                       + ([LOCK_CFLAG] if diag_lock else []))
    script = (
        'set -e\n'
        'export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"\n'
        'export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"\n'
        'export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"\n'
        '[ -d "$DEVKITPRO" ] || { echo "нет devkitPro в $DEVKITPRO"; exit 3; }\n'
        f'WORK="{work}"\n'
        'mkdir -p "$WORK"\n'
        f'rsync -a --delete --exclude build --exclude deploy --exclude .git "{wsl_path(EXLAUNCH)}/" "$WORK/exlaunch/"\n'
        f'rsync -a --delete "{wsl_path(SRC)}/" "$WORK/src/"\n'
        'cd "$WORK/exlaunch"\n'
        f'make MODULE_NAME={slot} "C_FLAGS={c_flags}"\n'
        f'ls -l "$WORK/exlaunch/deploy/{slot}"\n'
    )
    res = run_wsl(script)
    if res.returncode != 0:
        say(tail(res.stdout) + "\n" + tail(res.stderr))
        # Код 3 из скрипта означает «нет devkitPro в /opt/devkitpro» — внешнему
        # интерфейсу это надо отличать от прочих поломок сборки, поэтому он в data.
        fail("сборка в WSL не удалась", code="FAIL_WSL_BUILD", wsl_rc=res.returncode,
             devkitpro_missing=(res.returncode == 3), slot=slot, diag=bool(diag),
             diag_gfx=bool(diag_gfx), diag_lock=bool(diag_lock), work=work)

    built = f"{work}/exlaunch/deploy/{slot}"
    out_nso = Path(out_nso)
    out_nso.parent.mkdir(parents=True, exist_ok=True)
    res = run_wsl(f'cp "{built}" "{wsl_path(out_nso)}"')
    if res.returncode != 0 or not out_nso.is_file():
        say(tail(res.stdout) + "\n" + tail(res.stderr))
        fail("собранный модуль не удалось забрать из WSL", code="FAIL_WSL_FETCH",
             path=str(out_nso), work=work)
    return out_nso


def run_hosttest(game_exefs, nso, breakage=None, work_suffix=""):
    """Шаг 2: хост-тесты (та же логика, что в модуле, компилируется g++ на ПК)."""
    if not RUN_HOSTTEST.is_file():
        fail(f"нет {RUN_HOSTTEST}", code="FAIL_NO_HOSTTEST_SCRIPT",
             path=str(RUN_HOSTTEST))
    work = "/tmp/nxr-hosttest" + work_suffix
    script = (
        f'NXR_HOSTTEST_DIR="{work}" bash "{wsl_path(RUN_HOSTTEST)}" '
        f'"{wsl_path(game_exefs)}" "{wsl_path(nso)}" {breakage or "none"}'
    )
    res = run_wsl(script, timeout=900)
    return res.returncode, res.stdout + res.stderr


def check_nso(nso, game_exefs, diag, diag_gfx=False, diag_lock=False, nvrm=False,
              early=False):
    """Шаг 3: гейт по собранному NSO. early — ждём ранний перехват (мод со своим модулем)."""
    cmd = [CHECK_NSO, nso, "--game-exefs", game_exefs]
    if early:
        cmd.append("--early-fs")
    if nvrm:
        cmd.append("--compat-nvrm")
    if diag:
        cmd.append("--diag")
    if diag_gfx:
        cmd.append("--diag-gfx")
    if diag_lock:
        cmd.append("--diag-lock")
    res = run_py(cmd)
    return res.returncode, res.stdout + res.stderr


def make_npdm(game_npdm, out_npdm, tid):
    """
    Шаг 4: main.npdm игры -> бит SdCard -> системные вызовы -> гейт --check.

    Возвращает список строк отчёта. Исходный main.npdm игры не изменяется.
    """
    game_npdm = Path(game_npdm)
    out_npdm = Path(out_npdm)
    if not game_npdm.is_file():
        fail(f"нет main.npdm игры: {game_npdm}", code="FAIL_NO_GAME_NPDM",
             path=str(game_npdm))
    out_npdm.parent.mkdir(parents=True, exist_ok=True)
    mid = out_npdm.with_suffix(out_npdm.suffix + ".sdcard")

    res = run_py([NPDM_SDCARD, game_npdm, "-o", mid])
    if res.returncode != 0:
        say(tail(res.stdout) + "\n" + tail(res.stderr))
        fail("npdm_sdcard.py: не удалось поставить бит SdCard",
             code="FAIL_NPDM_SDCARD", path=str(game_npdm))

    res = run_py([NPDM_SYSCALLS, mid, "-o", out_npdm, "--svc"] + NPDM_SVCS)
    if res.returncode != 0:
        say(tail(res.stdout) + "\n" + tail(res.stderr))
        fail("npdm_syscalls.py: не удалось добавить системные вызовы",
             code="FAIL_NPDM_SYSCALLS", path=str(mid), svc=NPDM_SVCS)

    rc, out = npdm_check(out_npdm, tid)
    if rc != 0:
        say(tail(out))
        fail("гейт npdm не пройден: " + str(out_npdm), code="FAIL_GATE_NPDM",
             gate="npdm", path=str(out_npdm), program_id=tid, svc=NPDM_SVCS)
    emit("NPDM_PATCHED", "правленый main.npdm готов", path=str(out_npdm),
         size=out_npdm.stat().st_size, sha256=file_sha256(out_npdm),
         program_id=tid, svc=NPDM_SVCS, sdcard_bit=True, source=str(game_npdm))
    return ["npdm: бит SdCard + вызовы " + ", ".join(NPDM_SVCS) +
            " в ACI0 и ACID; program id " + tid]


def npdm_check(npdm, tid):
    cmd = [NPDM_SYSCALLS, npdm, "--check", "--program-id", tid, "--svc"] + NPDM_SVCS
    res = run_py(cmd)
    return res.returncode, res.stdout + res.stderr


def layout_mod(out_root, tid, nso, slot, npdm, mod_files, zbic_files=None,
               mod_items=None, mod_exefs=None):
    """
    Шаг 5: раскладка мода.

        <out_root>/atmosphere/contents/<TID>/exefs/{subsdkN, main.npdm}
        <out_root>/atmosphere/contents/<TID>/files/...

    Папки romfs/romfs.bin здесь быть не должно — иначе Atmosphère включит
    LayeredFS и потратит 12-МБ кучу ams_mitm (проверено по amsmitm_fs_utils.cpp).
    """
    out_root = Path(out_root)
    base = out_root / "atmosphere" / "contents" / tid
    exefs = base / "exefs"
    files = base / "files"
    if base.exists():
        # Пересобираем свою раскладку с нуля, но чужого рядом не трогаем:
        # удаляются только exefs/ и files/ внутри нашей папки out[-diag].
        for d in (exefs, files):
            if d.exists():
                shutil.rmtree(d)
    exefs.mkdir(parents=True, exist_ok=True)
    files.mkdir(parents=True, exist_ok=True)

    shutil.copy2(nso, exefs / slot)
    shutil.copy2(npdm, exefs / "main.npdm")

    # Несжатые NSO (снятый ZBIC) ложатся В ТОТ ЖЕ exefs, где лежит subsdkN мода:
    # это одна и та же папка LayeredFS, и обе вещи друг другу не мешают —
    # Atmosphère подменяет файлы exefs поимённо.
    for name, src in sorted((zbic_files or {}).items()):
        shutil.copy2(src, exefs / name)
    # Файлы exefs МОДА: его модуль и прочее — байт в байт.
    for name, src in sorted((mod_exefs or {}).items()):
        shutil.copy2(src, exefs / name)

    count = 0
    if mod_items:
        # Мод целиком: файлы его romfs (и files/) — в нашу раскладку files/.
        for rel, p in mod_items:
            dst = files / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)
            count += 1
    elif mod_files:
        mod_files = Path(mod_files)
        if not mod_files.is_dir():
            fail(f"нет папки файлов мода: {mod_files}", code="FAIL_NO_MOD_FILES_DIR",
                 path=str(mod_files))
        for p in sorted(mod_files.rglob("*")):
            if p.is_file():
                dst = files / p.relative_to(mod_files)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(p, dst)
                count += 1
        if count == 0:
            fail(f"папка файлов мода пуста: {mod_files}", code="FAIL_MOD_FILES_EMPTY",
                 path=str(mod_files))
    else:
        (files / "ПОЛОЖИТЕ-СЮДА-ФАЙЛЫ-МОДА.txt").write_bytes(
            ("Сюда кладутся файлы, которыми подменяются файлы игры.\r\n"
             "Пути внутри этой папки повторяют пути внутри romfs игры:\r\n"
             "файл игры rom:/table/t_item.tbl -> files/table/t_item.tbl\r\n"
             "\r\nПапок romfs и romfs.bin здесь быть не должно.\r\n").encode("utf-8"))
        count = 1
    return exefs, files, count


# ------------------------------------------------ перенос мода целиком
#
# Задача: человек указывает игру и МОД (папку, как её раздал автор), дальше всё
# само. Мод может нести не только файлы romfs, но и:
#   - свой модуль exefs (atmosphere/contents/<TID>/exefs/subsdkN, например UltraCam
#     для TotK на exlaunch);
#   - свой main.npdm (права под свой модуль);
#   - IPS-патчи (atmosphere/exefs_patches/<имя>/<BuildId>.ips).
# Раньше конвейер брал только файлы (--mod-files) и молча терял остальное: сборка
# проходила с кодом 0, а мод получался хуже рабочей ручной сборки (живая проверка:
# TotK + UltraCam). Теперь:
#   - файлы romfs мода -> НАША раскладка files/ (смысл проекта — убрать romfs);
#   - модуль мода -> exefs/ под своим именем, байт в байт;
#   - IPS -> atmosphere/exefs_patches/... байт в байт;
#   - main.npdm мода -> за основу; наше (бит SdCard, svc 0x74/0x75) дописывается,
#     только если его не хватает (иначе файл мода как есть);
#   - наш модуль -> свободный слот СТАРШЕ модулей мода, а сборка — с ранним
#     перехватом nn::fs (-DNXR_EARLY_FS_HOOK). Почему так — architecture §«мод со
#     своим модулем»: по коду rtld модули инициализируются от старшего адреса к
#     младшему (sdk, subsdk9, …, subsdk0, main), и ранний перехват ставится
#     раньше, чем модуль мода начнёт работать.

import re as _re

EARLY_FS_CFLAG = "-DNXR_EARLY_FS_HOOK"
_HEX16 = _re.compile(r"^[0-9A-Fa-f]{16}$")
_HEX_IPS = _re.compile(r"^[0-9A-Fa-f]{1,64}$")
# Манглинг nn::fs::OpenFile: если строка лежит в образе ЧУЖОГО модуля вне таблицы
# имён импорта, модуль, скорее всего, ищет OpenFile по имени, чтобы перехватить.
OPENFILE_MANGLED = b"_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"
# Файлы exefs, которые Loader грузит как модули (имя файла = место в процессе).
EXEFS_MODULE_NAMES = (["main", "rtld", "sdk"] + ["subsdk%d" % n for n in range(10)])
# Что в папке игры мода точно не переносится как «лишнее»: наша квитанция и копии.
_TITLE_DIR_OURS = {"nxredirect.json"}


def early_suffix(early):
    """Кусок имени папки сборки для раннего перехвата nn::fs: "" | "-earlyfs".

    Та же причина, что у gfx_suffix(): make сравнивает время файлов, а не строку
    флагов, и модуль с -DNXR_EARLY_FS_HOOK в той же папке молча стал бы прежним."""
    return "-earlyfs" if early else ""


def slot_number(name):
    """subsdkN -> N, иначе None."""
    m = _re.match(r"^subsdk(\d)$", str(name))
    return int(m.group(1)) if m else None


def _nso_build_id(path):
    """Build id NSO (32 байта заголовка по 0x40) в hex ЗАГЛАВНЫМИ или None."""
    try:
        raw = Path(path).read_bytes()[:0x100]
    except OSError:
        return None
    if raw[:4] != b"NSO0" or len(raw) < 0x60:
        return None
    return raw[0x40:0x60].hex().upper()


def find_mod_tree(path):
    """
    Опознать папку мода. Возвращает {"root", "atmosphere", "titles": {TID: Path}}
    или None, если это не дерево мода (тогда папка — просто файлы, как --mod-files).

    Принимается то, что раздают авторы модов, и его части:
      <корень>/atmosphere/contents/<TID>/...    — типичный архив мода;
      <...>/atmosphere                          — сама папка atmosphere;
      <...>/contents/<TID>                       — без atmosphere;
      <...>/<TID>                                — одна папка игры (exefs/, romfs/).
    """
    p = Path(path)
    if not p.is_dir():
        return None
    atm = None
    if (p / "atmosphere").is_dir():
        atm = p / "atmosphere"
    elif p.name.lower() == "atmosphere":
        atm = p
    elif (p / "contents").is_dir() and any(_HEX16.match(c.name) for c in (p / "contents").iterdir()
                                           if c.is_dir()):
        atm = p
    if atm is not None:
        contents = atm / "contents"
        titles = {}
        if contents.is_dir():
            for c in sorted(contents.iterdir()):
                if c.is_dir() and _HEX16.match(c.name):
                    titles[c.name.upper()] = c
        return {"root": p, "atmosphere": atm, "titles": titles}
    if _HEX16.match(p.name) and any((p / d).exists() for d in ("exefs", "romfs", "romfs.bin")):
        return {"root": p, "atmosphere": None, "titles": {p.name.upper(): p}}
    return None


def mod_module_info(path, game_exefs=None):
    """
    Что за модуль лежит в exefs мода: NSO ли он, ищет ли nn::fs::OpenFile по имени
    (признак перехвата), занимает ли слот, который есть у самой игры.

    Признак перехвата — эвристика, а не доказательство: строка манглинга OpenFile
    в образе ВНЕ таблицы имён импорта (.dynstr). Обычный вызов OpenFile даёт только
    импорт; поиск по имени (exlaunch, skyline) кладёт строку в rodata.
    """
    path = Path(path)
    info = {"name": path.name, "size": path.stat().st_size, "sha256": file_sha256(path),
            "nso": False, "build_id": None, "hooks_openfile": False,
            "imports_openfile": False, "replaces_game": False, "problem": None}
    if game_exefs is not None and (Path(game_exefs) / path.name).is_file():
        info["replaces_game"] = True
    try:
        chk = load_script(CHECK_NSO, "nxr_check_nso")
        nso = chk.Nso(path)
    except Exception as e:
        info["problem"] = "не разбирается как NSO: %s" % e
        return info
    info["nso"] = True
    info["build_id"] = _nso_build_id(path)
    img = nso.image
    strtab, strsz = nso.tags.get(5, 0), nso.tags.get(10, 0)
    for m in _re.finditer(_re.escape(OPENFILE_MANGLED), img):
        if strtab <= m.start() < strtab + strsz:
            info["imports_openfile"] = True
        else:
            info["hooks_openfile"] = True
    return info


def analyze_mod(mod_path, tid, game_exefs=None):
    """
    Разобрать мод целиком: что из него куда поедет. Ничего не пишет.

    Возвращает словарь плана:
      title_dir   — папка игры в моде (atmosphere/contents/<TID>);
      exefs       — {имя: Path} все файлы exefs мода, КРОМЕ main.npdm;
      modules     — {subsdkN: info} модули мода (mod_module_info);
      replaces    — имена exefs мода, которые есть у самой игры (main, sdk, subsdkN);
      npdm        — Path main.npdm мода или None;
      files       — [(относительный путь, Path)] файлы romfs (и files/) мода;
      patches     — [(путь от atmosphere/exefs_patches, Path, цель)] IPS и прочее;
      extra       — [(путь от папки игры, Path)] прочее в папке игры (cheats/ и т. п.);
      skipped     — [строка] что НЕ переносится и почему (другие TID, kips, switch/…);
      warnings    — [строка] предупреждения.
    """
    tid = str(tid).upper()
    tree = find_mod_tree(mod_path)
    if tree is None:
        fail("папка мода не похожа на мод: в ней нет atmosphere/contents/<TID>/ (или "
             "contents/<TID>/, или папки <TID> с exefs/romfs): %s\n"
             "        Если это просто файлы для подмены — укажите её как --mod-files."
             % mod_path, code="FAIL_MOD_NOT_TREE", path=str(mod_path))
    titles = tree["titles"]
    if tid not in titles:
        fail("в моде нет папки этой игры (%s): в нём %s.\n"
             "        Мод сделан для другой игры или другой части приложения — "
             "проверьте, что выбрали нужный." % (tid, ", ".join(sorted(titles)) or "ничего"),
             code="FAIL_MOD_OTHER_TITLE", path=str(mod_path), tid=tid,
             mod_titles=sorted(titles))
    tdir = titles[tid]
    plan = {"root": tree["root"], "title_dir": tdir, "exefs": {}, "modules": {},
            "replaces": [], "npdm": None, "files": [], "patches": [], "extra": [],
            "skipped": [], "warnings": []}
    if (tdir / "romfs.bin").is_file():
        fail("в моде лежит romfs.bin (упакованный образ romfs): разложить его по файлам "
             "конвейер не умеет, а положить как есть — значит включить LayeredFS, от "
             "которого мы и уходим. Распакуйте его (например, hactoolnet --romfsdir) "
             "в папку romfs/ рядом и повторите.", code="FAIL_MOD_ROMFS_BIN",
             path=str(tdir / "romfs.bin"))

    exefs_dir = tdir / "exefs"
    if exefs_dir.is_dir():
        for p in sorted(exefs_dir.iterdir()):
            if p.is_dir():
                plan["skipped"].append("exefs/%s/ — папка внутри exefs (Loader её не "
                                       "читает), не переносится" % p.name)
                continue
            if p.name == "main.npdm":
                plan["npdm"] = p
                continue
            plan["exefs"][p.name] = p
            if game_exefs is not None and (Path(game_exefs) / p.name).is_file():
                plan["replaces"].append(p.name)
            if slot_number(p.name) is not None:
                plan["modules"][p.name] = mod_module_info(p, game_exefs)
            elif p.name not in EXEFS_MODULE_NAMES:
                plan["warnings"].append("в exefs мода лежит %s — это не имя модуля "
                                        "(Loader его не грузит); переносится как есть"
                                        % p.name)

    for sub in ("romfs", "files"):
        d = tdir / sub
        if d.is_dir():
            for p in sorted(d.rglob("*")):
                if p.is_file():
                    plan["files"].append((p.relative_to(d).as_posix(), p))
    seen = {}
    for rel, p in plan["files"]:
        if rel in seen and file_sha256(seen[rel]) != file_sha256(p):
            fail("в моде один и тот же файл лежит и в romfs/, и в files/, и они "
                 "РАЗНЫЕ: %s. Какой из них настоящий — решать автору мода."
                 % rel, code="FAIL_MOD_FILES_CLASH", rel=rel)
        seen[rel] = p
    plan["files"] = sorted(seen.items())

    for p in sorted(tdir.iterdir()):
        if p.name in ("exefs", "romfs", "files", "romfs.bin") or p.name in _TITLE_DIR_OURS:
            continue
        if ".bak-" in p.name or ".removed-" in p.name:
            continue
        if p.is_dir():
            for q in sorted(p.rglob("*")):
                if q.is_file():
                    plan["extra"].append((q.relative_to(tdir).as_posix(), q))
        else:
            plan["extra"].append((p.name, p))
    if plan["extra"]:
        plan["warnings"].append("в папке игры мода есть и другое (%s) — переносится как "
                                "есть, рядом с exefs/ и files/"
                                % ", ".join(sorted({r.split("/")[0] for r, _ in plan["extra"]})))

    # IPS-патчи и что ещё лежит в atmosphere/ мода.
    atm = tree["atmosphere"]
    build_ids = {}
    if game_exefs is not None:
        for name in EXEFS_MODULE_NAMES:
            bid = _nso_build_id(Path(game_exefs) / name)
            if bid:
                build_ids[bid] = "%s игры" % name
    for name, p in plan["exefs"].items():
        bid = _nso_build_id(p)
        if bid:
            build_ids[bid] = "%s мода" % name
    if atm is not None:
        for p in sorted(atm.iterdir()):
            if p.name == "contents":
                for c in sorted(p.iterdir()):
                    if c.name.upper() != tid:
                        plan["skipped"].append("atmosphere/contents/%s — другая программа "
                                               "(не эта игра), не переносится" % c.name)
                continue
            if p.name == "exefs_patches" and p.is_dir():
                for q in sorted(p.rglob("*")):
                    if not q.is_file():
                        continue
                    rel = q.relative_to(p).as_posix()
                    target = None
                    if q.suffix.lower() == ".ips" and _HEX_IPS.match(q.stem):
                        want = q.stem.upper().ljust(64, "0")
                        target = build_ids.get(want)
                        if target is None and build_ids:
                            plan["warnings"].append(
                                "IPS %s не подходит ни к одному модулю этой версии игры "
                                "(build id %s…): Atmosphère его просто не применит — "
                                "возможно, мод для другой версии" % (rel, q.stem[:16]))
                    plan["patches"].append((rel, q, target))
                continue
            plan["skipped"].append("atmosphere/%s%s — не относится к папке игры (системные "
                                   "модули, kip, настройки Atmosphère), не переносится; если "
                                   "он нужен моду, поставьте его сами"
                                   % (p.name, "/" if p.is_dir() else ""))
    if tree["root"] != atm and atm is not None:
        for p in sorted(tree["root"].iterdir()):
            if p == atm:
                continue
            plan["skipped"].append("%s%s — вне atmosphere/, не переносится (homebrew, "
                                   "оверлеи, загрузчик — ставятся отдельно)"
                                   % (p.name, "/" if p.is_dir() else ""))

    for name in plan["replaces"]:
        plan["warnings"].append("мод ЗАМЕНЯЕТ файл игры exefs/%s своим — так задумал автор "
                                "мода; переносится как есть" % name)
    for name, info in sorted(plan["modules"].items()):
        if not info["nso"]:
            plan["warnings"].append("модуль мода %s не разбирается как NSO (%s): Loader "
                                    "на нём споткнётся" % (name, info["problem"]))
        if info["hooks_openfile"]:
            plan["warnings"].append("модуль мода %s, похоже, сам перехватывает "
                                    "nn::fs::OpenFile (ищет его по имени): наш перехват "
                                    "и его встанут цепочкой; если файлы мода не "
                                    "подменяются, дело может быть в этом" % name)
    if not plan["files"] and not plan["exefs"] and not plan["patches"]:
        fail("в моде для %s нет ничего, что можно перенести: ни файлов romfs, ни exefs, "
             "ни патчей" % tid, code="FAIL_MOD_EMPTY", path=str(mod_path), tid=tid)
    return plan


def mod_plan_event(plan, tid):
    """Событие MOD_ANALYZED: что найдено в моде (для веб-мастера — план целиком)."""
    emit("MOD_ANALYZED", "мод разобран", tid=tid, root=str(plan["root"]),
         title_dir=str(plan["title_dir"]),
         modules=[dict(v, path=str(plan["exefs"][k])) for k, v in sorted(plan["modules"].items())],
         exefs=sorted(plan["exefs"]), replaces=sorted(plan["replaces"]),
         npdm=(str(plan["npdm"]) if plan["npdm"] else None),
         files=len(plan["files"]),
         files_bytes=sum(p.stat().st_size for _r, p in plan["files"]),
         patches=[{"path": r, "target": t} for r, _p, t in plan["patches"]],
         extra=[r for r, _p in plan["extra"]], skipped=plan["skipped"],
         warnings=plan["warnings"])


def mod_slot_order(slot, plan):
    """
    Наш слот относительно модулей мода. Возвращает (хорошо_ли, пояснение).

    По коду rtld (TotK, функция 0x860) модули инициализируются от СТАРШЕГО адреса
    к младшему: sdk, subsdk9, …, subsdk0, main. Наш модуль в слоте старше всех
    модулей мода ставит ранний перехват nn::fs раньше, чем модуль мода выполнит
    хоть строку, — значит, файлы, которые тот читает в своей инициализации или в
    своём хуке nnMain, уже проходят через нас.
    """
    mods = [slot_number(s) for s in plan["modules"]]
    mine = slot_number(slot)
    if not mods or mine is None:
        return True, ""
    if mine > max(mods):
        return True, ("наш %s старше модулей мода (%s): инициализируется раньше них"
                      % (slot, ", ".join(sorted(plan["modules"]))))
    return False, ("наш %s МЛАДШЕ модуля мода (%s): тот инициализируется раньше нас. "
                   "Файлы, которые он читает в СВОЕЙ инициализации, подмена не увидит; "
                   "в его хуке nnMain — увидит (ранний перехват)"
                   % (slot, ", ".join(sorted(plan["modules"]))))


def pick_slot_for_mod(busy, plan):
    """Старший свободный слот, не занятый ни игрой, ни модом. None — нет свободного."""
    taken = set(busy) | set(plan["modules"]) if plan else set(busy)
    for n in range(9, -1, -1):
        if "subsdk%d" % n not in taken:
            return "subsdk%d" % n
    return None


def make_mod_npdm(mod_npdm, game_npdm, out_npdm, tid):
    """
    main.npdm для мода, который несёт СВОЙ main.npdm.

    Правило: основа — файл мода. Если его права уже
    покрывают наши (бит SdCard в обеих секциях, svc 0x74/0x75) — файл мода кладётся
    КАК ЕСТЬ, байт в байт. Иначе дописывается только недостающее, остальное — как в
    файле мода. Гейт npdm_syscalls.py --check проходит в обоих случаях.

    Возвращает (режим "as-is" | "merged", строки отчёта).
    """
    mod_npdm, out_npdm = Path(mod_npdm), Path(out_npdm)
    lib = load_script(NPDM_SYSCALLS, "nxr_npdm_syscalls")
    try:
        src = lib.Npdm(mod_npdm.read_bytes())
    except Exception as e:
        fail("main.npdm мода не разбирается: %s: %s" % (type(e).__name__, e),
             code="FAIL_MOD_NPDM_BAD", path=str(mod_npdm))
    got = "%016X" % src.program_id
    if got != str(tid).upper():
        fail("main.npdm мода от другой программы: program id %s, а игра %s. Такой файл "
             "игру не запустит (или запустит не ту)." % (got, tid),
             code="FAIL_MOD_NPDM_TID", path=str(mod_npdm), program_id=got, tid=tid)
    svcs = [int(s, 16) for s in NPDM_SVCS]
    lines = []
    problems = lib.check(src, svcs, program_id=int(str(tid), 16))
    out_npdm.parent.mkdir(parents=True, exist_ok=True)
    added = []
    if not problems:
        shutil.copy2(mod_npdm, out_npdm)
        mode = "as-is"
        lines.append("npdm: файл МОДА как есть — его права уже покрывают наши "
                     "(бит SdCard, вызовы %s)" % ", ".join(NPDM_SVCS))
    else:
        cur = src
        if not all(cur.fs_mask(n) & lib.SDCARD_BIT for n in ("ACI0", "ACID")):
            cur = lib._with_sdcard_bit(cur)
            added.append("бит SdCard")
        missing = sorted(set(svcs) - set.intersection(
            *(lib.Npdm.syscalls_of(cur.caps(n))[0] for n in ("ACI0", "ACID"))))
        data = cur.raw
        if missing:
            data, _report = cur.with_syscalls(svcs)
            new = lib.Npdm(data)
            bad = lib.compare(cur, new, svcs)
            if bad:
                fail("правка main.npdm мода разошлась: " + "; ".join(bad),
                     code="FAIL_NPDM_SYSCALLS", path=str(mod_npdm), svc=NPDM_SVCS)
            added.append("вызовы " + ", ".join("%#x" % s for s in missing))
        out_npdm.write_bytes(data)
        mode = "merged"
        lines.append("npdm: за основу взят файл МОДА, дописано: " + ", ".join(added))
    rc, out = npdm_check(out_npdm, str(tid).upper())
    if rc != 0:
        say(tail(out))
        fail("гейт npdm не пройден: " + str(out_npdm), code="FAIL_GATE_NPDM",
             gate="npdm", path=str(out_npdm), program_id=tid, svc=NPDM_SVCS)
    # Чем npdm мода отличается от npdm игры, кроме прав: шапка META. Не отказ (так
    # решил автор мода), но человек должен это видеть.
    diffs = []
    if game_npdm is not None and Path(game_npdm).is_file():
        g = Path(game_npdm).read_bytes()
        m = mod_npdm.read_bytes()
        import struct as _st
        for off, size, what in ((0x14, 4, "системный ресурс"), (0x1C, 4, "стек главного потока"),
                                (0x0E, 1, "приоритет главного потока"),
                                (0x0F, 1, "ядро главного потока"), (0x0C, 1, "флаги")):
            fmt = "<I" if size == 4 else "<B"
            a, b = _st.unpack_from(fmt, g, off)[0], _st.unpack_from(fmt, m, off)[0]
            if a != b:
                diffs.append("%s: у игры %#x, у мода %#x" % (what, a, b))
        if diffs:
            lines.append("npdm мода отличается от npdm игры и в шапке: " + "; ".join(diffs)
                         + " — так задумал автор мода (или npdm сделан под другую версию "
                           "игры); оставлено как в моде")
    emit("NPDM_FROM_MOD", "main.npdm из мода готов и прошёл гейт", path=str(out_npdm),
         source=str(mod_npdm), mode=mode, added=added, header_diffs=diffs,
         sha256=file_sha256(out_npdm), program_id=str(tid).upper(), svc=NPDM_SVCS)
    return mode, lines


def mod_transfer_problems(dest_root, tid, plan, slot, weak=False):
    """
    Гейт переноса мода: всё, что обещано из мода, лежит в раскладке байт в байт,
    и romfs в раскладке нет. Возвращает (находки, число проверок).

    weak=True — НАРОЧНО ослабленный вариант для негативной пробы: смотрит только,
    что файлы на месте, без сравнения содержимого. В работе не используется.
    """
    dest_root = Path(dest_root)
    base = dest_root / "atmosphere" / "contents" / str(tid).upper()
    problems, checks = [], 0

    def same(dst, src, what):
        nonlocal checks
        checks += 1
        item_checks[0] += 1
        if not dst.is_file():
            problems.append("%s не перенесён: нет %s" % (what, dst))
            return
        if weak:
            return
        checks += 1
        if file_sha256(dst) != file_sha256(src):
            problems.append("%s перенесён НЕ байт в байт: %s" % (what, dst))

    item_checks = [0]
    for name, src in sorted(plan["exefs"].items()):
        same(base / "exefs" / name, src, "exefs/%s мода" % name)
    for rel, src in plan["files"]:
        same(base / "files" / rel, src, "файл мода %s (из romfs -> files/)" % rel)
    for rel, src, _t in plan["patches"]:
        same(dest_root / "atmosphere" / "exefs_patches" / rel, src, "патч %s" % rel)
    for rel, src in plan["extra"]:
        same(base / rel, src, "%s мода" % rel)
    checks += 1
    if (base / "romfs").exists() or (base / "romfs.bin").exists():
        problems.append("в раскладке есть romfs/romfs.bin — включится LayeredFS")
    if plan["modules"] and slot:
        checks += 1
        if slot in plan["modules"]:
            problems.append("наш модуль лёг в слот модуля мода %s" % slot)
    # Затвор нуля: гейт, не сверивший ни одного предмета мода, ничего не доказал.
    if item_checks[0] == 0:
        problems.append("затвор нуля: гейт переноса мода не сверил ни одного предмета")
    return problems, checks


def gate_mod_transfer(dest_root, tid, plan, slot):
    """Гейт переноса с затвором нуля: проверок меньше, чем предметов, — красный."""
    problems, checks = mod_transfer_problems(dest_root, tid, plan, slot)
    items = len(plan["exefs"]) + len(plan["files"]) + len(plan["patches"]) + len(plan["extra"])
    if checks < items:
        problems = problems + ["затвор нуля: гейт переноса мода выполнил %d проверок на "
                               "%d предметов" % (checks, items)]
    if problems:
        for p in problems:
            say("      ПРОБЛЕМА: " + p, level="warn")
        fail("мод перенесён не целиком или не байт в байт", code="FAIL_GATE_MOD",
             gate="mod", tid=tid, problems=problems, checks=checks)
    say("      гейт переноса мода пройден: проверок %d (модуль мода, патчи, файлы — "
        "байт в байт, romfs нет)" % checks, code="GATE_PASS", gate="mod",
        detail="проверок %d" % checks)
    return checks


def say_mod_plan(plan, tid, slot, npdm_mode=None):
    """Человеческий план переноса мода (одна функция на build.py и install.py)."""
    say("      МОД целиком: %s" % plan["root"])
    for name, info in sorted(plan["modules"].items()):
        say("        модуль мода %s (%d Б, build id %s…)%s%s"
            % (name, info["size"], (info["build_id"] or "?")[:16],
               " — заменяет модуль игры" if info["replaces_game"] else "",
               " — похоже, перехватывает OpenFile" if info["hooks_openfile"] else ""))
    for name in sorted(n for n in plan["exefs"] if n not in plan["modules"]):
        say("        exefs/%s мода — как есть" % name)
    if plan["npdm"]:
        say("        main.npdm мода — за основу" + (" (%s)" % {
            "as-is": "как есть: его права покрывают наши",
            "merged": "дописано недостающее"}.get(npdm_mode, npdm_mode) if npdm_mode else ""))
    else:
        say("        своего main.npdm у мода нет — права дописываются в npdm игры")
    for rel, _p, target in plan["patches"]:
        say("        патч atmosphere/exefs_patches/%s%s"
            % (rel, (" -> " + target) if target else ""))
    say("        файлов romfs -> files/: %d" % len(plan["files"]))
    for rel, _p in plan["extra"]:
        say("        %s — как есть" % rel)
    for line in plan["skipped"]:
        say("        НЕ переносится: " + line, level="warn")
    ok, why = mod_slot_order(slot, plan)
    if why:
        say("        порядок: " + why, level=("info" if ok else "warn"))


def resolve_mod_input(mod, mod_files, tid, game_exefs=None):
    """
    Что дал человек: мод целиком (--mod) или папку файлов (--mod-files).
    Возвращает (план мода или None, папка файлов или None).

    --mod-files, указывающий на ДЕРЕВО мода (atmosphere/…), понимается как мод
    целиком: разложить atmosphere/… в files/ не бывает правильным никогда.
    --mod-files, указывающий на romfs ВНУТРИ мода, остаётся файлами (так его
    передаёт нынешний веб-мастер), но если рядом лежат модуль, npdm или патчи —
    об этом говорится громко (MOD_PARTS_LEFT), а не молча теряется.
    """
    if mod and mod_files:
        fail("мод задаётся одним ключом: --mod <корень мода> (файлы romfs он возьмёт "
             "сам). --mod и --mod-files вместе не сочетаются.", code="FAIL_MOD_ARGS")
    if mod:
        return analyze_mod(mod, tid, game_exefs), None
    if not mod_files:
        return None, None
    p = Path(mod_files)
    if p.is_dir() and find_mod_tree(p) is not None and p.name.lower() not in ("romfs", "files"):
        say("      --mod-files указывает на дерево мода (%s) — беру мод ЦЕЛИКОМ, как --mod"
            % p, code="MOD_TREE_AS_FILES", level="warn", path=str(p), as_mod=True)
        return analyze_mod(p, tid, game_exefs), None
    left = []
    if p.name.lower() == "romfs" and _HEX16.match(p.parent.name):
        tdir = p.parent
        ex = tdir / "exefs"
        if ex.is_dir():
            left += ["exefs/" + q.name for q in sorted(ex.iterdir()) if q.is_file()]
        atm = tdir.parent.parent if tdir.parent.name.lower() == "contents" else None
        if atm is not None and (atm / "exefs_patches").is_dir():
            left += ["exefs_patches/" + q.relative_to(atm / "exefs_patches").as_posix()
                     for q in sorted((atm / "exefs_patches").rglob("*")) if q.is_file()]
        if left:
            root = atm.parent if atm is not None and atm.name.lower() == "atmosphere" else tdir
            say("      ВНИМАНИЕ: --mod-files — это romfs внутри мода, а рядом лежит то, "
                "что так НЕ перенесётся: " + ", ".join(left) + ". Мод целиком: --mod "
                + str(root), code="MOD_PARTS_LEFT", level="warn", path=str(p),
                mod_root=str(root), left=left)
    return None, mod_files



class Variant:
    """Чем отличаются обычная и диагностическая сборки. Остальное — общее."""

    def __init__(self, diag, slot=DEFAULT_SLOT, game="game", game_exefs=None, mod_files=None,
                 share=False, zbic_fix=False, compat=(), swapped_sdk=False, sdk=None,
                 diag_gfx=False, diag_lock=False, nvrm=False, cnmt=None, mod=None,
                 slot_explicit=True):
        self.diag = diag
        # Мод целиком: --mod или --mod-files с деревом мода. План
        # разбирается в prepare(), когда известен TitleID.
        self.mod = Path(mod) if mod else None
        self.mod_plan = None
        self.npdm_mode = "game"
        # Ранний перехват nn::fs — ровно когда у мода свой модуль (решает prepare).
        self.early = False
        # Слот задан человеком или взят по умолчанию: по умолчанию его можно
        # сдвинуть, если он занят модулем мода; заданный — нет (отказ).
        self.slot_explicit = bool(slot_explicit)
        # Диагностика графики — измерение, НЕЗАВИСИМОЕ от -DNXR_DIAG: она включается
        # и к обычной сборке (именно так снят первый отчёт с консоли), и к
        # диагностической.
        self.diag_gfx = bool(diag_gfx)
        # Узкая проба блокировок имеет смысл только вместе с диагностикой
        # графики: окно пробы открывает перехватчик eglCreateWindowSurface.
        self.diag_lock = bool(diag_lock) and self.diag_gfx
        self.share = share
        # Комплект «для консоли с подменённой библиотекой sdk»: мягкое снятие ZBIC
        # и правленый main.npdm одной раскладкой. Модуль в нём тот же, что везде
        # (автоисправление само решает на консоли, что включить).
        self.swapped_sdk = bool(swapped_sdk)
        if self.swapped_sdk:
            compat = tuple(COMPAT_SYMS)
            zbic_fix = True
        self.compat = compat_norm(compat)
        # NvRmMemQueryHandleParams — часть автоисправления: есть
        # ровно тогда, когда оно включено. Параметр nvrm оставлен ради старых
        # вызовов и больше ничего не решает.
        self.nvrm = bool(self.compat)
        self.sdk = list(sdk or [])
        self.cnmt = cnmt
        self.slot = slot
        self.game = game
        self.game_exefs = Path(game_exefs) if game_exefs else None
        self.mod_files = Path(mod_files) if mod_files else None
        # Снятие ZBIC: пересобранные несжатые NSO кладутся в тот же exefs.
        self.zbic_fix = bool(zbic_fix)
        self.zbic_scan = None      # что нашёл осмотр exefs (заполняется в prepare)
        self.zbic_made = {}        # {имя файла: (путь к несжатому, info гейта)}
        self.test_exefs = None     # exefs для гейтов: тот же, но уже без ZBIC
        self.out_root = ROOT / ("out-diag" if diag else "out")
        # Модуль с доэкспортом и без него — разные бинарники под одним именем
        # файла (имя файла и есть слот), поэтому и папки у них разные.
        self.nso = (self.out_root
                    / module_dir_name(self.compat, self.diag_gfx, self.diag_lock, self.nvrm)
                    / slot)
        self.npdm = self.out_root / "module" / "main.npdm"
        suffix = "_DIAG" if diag else ""
        if self.diag_gfx:
            suffix += "_GFXDIAG"
        if self.diag_lock:
            suffix += "_LOCKDIAG"
        if self.swapped_sdk:
            suffix += "_ДЛЯ-ПОДМЕНЁННОГО-SDK"
        if not self.compat:
            suffix += "_БЕЗ-АВТОИСПРАВЛЕНИЯ"
        # Имя архива само говорит, что внутри: в личном архиве лежит main.npdm,
        # сделанный из ДАМПА игры (раздавать его нельзя), в архиве для раздачи его нет.
        kind = "ДЛЯ-РАЗДАЧИ-без-npdm" if share else "ЛИЧНАЯ-с-npdm-из-дампа"
        self.zip_path = ROOT / "dist" / f"nxredirect_{game}{suffix}_{kind}_{BUILD_STAMP}.zip"
        self.title = ("диагностическая сборка (-DNXR_DIAG)" if diag else "обычная сборка")
        if self.diag_gfx:
            self.title += ", С ДИАГНОСТИКОЙ ГРАФИКИ (-DNXR_DIAG_GFX)"
        if self.diag_lock:
            self.title += ", С УЗКОЙ ПРОБОЙ БЛОКИРОВОК (-DNXR_DIAG_LOCK)"
        if self.swapped_sdk:
            self.title += ", КОМПЛЕКТ для консоли с подменённой библиотекой sdk"
        self.title += (", с автоисправлением (своя или прокси — решает модуль на консоли)"
                       if self.compat else
                       ", БЕЗ автоисправления (" + NO_AUTOFIX_CFLAG + ")")
        if share:
            self.title += ", для раздачи (без main.npdm)"
        self.readme_arc = ("ЧИТАТЬ-СНАЧАЛА-нужен-свой-main.npdm.txt" if share
                           else ("ЧИТАТЬ-внутри-main.npdm-из-дампа.txt" if diag
                                 else "ЧИТАТЬ-ПЕРЕД-УСТАНОВКОЙ-внутри-main.npdm-из-дампа.txt"))
        self.tid = None

    # --------------------------------------------------------------- 1
    def step_build(self, skip_build):
        if skip_build:
            say("[1/7] сборка пропущена (--skip-build)", code="STEP_SKIPPED",
                n=1, of=7, name="build", why="--skip-build")
            if not self.nso.is_file():
                fail(f"нет собранного модуля {self.nso} — запустите без --skip-build",
                     code="FAIL_NO_PREBUILT_MODULE", path=str(self.nso), slot=self.slot)
            return
        say(f"[1/7] сборка модуля в WSL (слот {self.slot}"
            + (", диагностическая)…" if self.diag else ")…"),
            code="STEP_START", n=1, of=7, name="build", slot=self.slot, diag=self.diag,
            compat=list(self.compat))
        say("      " + compat_human(self.compat),
            code="COMPAT_SELECTED", compat=list(self.compat),
            expect=compat_expect(self.compat), swapped_sdk=self.swapped_sdk,
            cflags=compat_cflags(self.compat),
            symbols=(autofix_symbols() if self.compat else []))
        say("      диагностика графики: "
            + ("ВКЛЮЧЕНА (%s), отчёт на карте: sd:/%s"
               % (GFX_CFLAG, GFX_REPORT_NAME) if self.diag_gfx else "выключена"),
            code="GFX_DIAG_SELECTED", enabled=self.diag_gfx,
            cflag=(GFX_CFLAG if self.diag_gfx else None),
            report=(("sd:/" + GFX_REPORT_NAME) if self.diag_gfx else None),
            build_dir=str(self.nso.parent))
        say("      узкая проба блокировок: "
            + ("ВКЛЮЧЕНА (%s): один перехватчик pthread_rwlock_rdlock, "
               "работает только внутри eglCreateWindowSurface и только в том же "
               "потоке" % LOCK_CFLAG if self.diag_lock else "выключена"),
            code="LOCK_DIAG_SELECTED", enabled=self.diag_lock,
            cflag=(LOCK_CFLAG if self.diag_lock else None))
        build_module(self.slot, self.diag, self.nso, compat=self.compat,
                     diag_gfx=self.diag_gfx, diag_lock=self.diag_lock, nvrm=self.nvrm,
                     early=self.early)
        say(f"      готово: {self.nso} ({self.nso.stat().st_size} Б)",
            code="MODULE_BUILT", path=str(self.nso), size=self.nso.stat().st_size,
            sha256=file_sha256(self.nso), slot=self.slot, diag=self.diag,
            diag_gfx=self.diag_gfx, diag_lock=self.diag_lock,
            compat=list(self.compat))
        emit("STEP_DONE", "шаг 1 пройден", n=1, of=7, name="build")

    # --------------------------------------------------------------- 2
    def step_hosttest(self):
        say("[2/7] хост-тесты (логика путей и поиска символов на ПК)…",
            code="STEP_START", n=2, of=7, name="hosttest")
        rc, out = run_hosttest(self.test_exefs or self.game_exefs, self.nso,
                               work_suffix="-diag" if self.diag else "")
        if rc != 0:
            say(tail(out, 4000))
            if self.zbic_needed() and not self.zbic_fix:
                # Не гадание: хост-тест разбирает модули игры как LZ4, и модуль
                # со сжатием ZBIC он просто не видит. Сказать это прямо дешевле,
                # чем заставлять искать причину в своей логике.
                say("      ПРИЧИНА, скорее всего, в сжатии ZBIC у %s: хост-тест "
                    "разбирает модули игры как LZ4 и такой модуль не видит. "
                    "Повторите с --zbic-fix — гейты пойдут по exefs с уже снятым ZBIC."
                    % ", ".join(self.zbic_scan["zbic_files"]), level="warn")
            fail("гейт хост-тестов не пройден", code="FAIL_GATE_HOSTTEST",
                 gate="hosttest", gate_rc=rc, detail=last_line(out))
        say("      " + last_line(out), code="GATE_PASS", gate="hosttest",
            detail=last_line(out))

        # Гейт порядка работы с файловой системой: модуль не обращается к nn::fs
        # раньше, чем игра задаст свой распределитель. Здесь гоняется НАСТОЯЩАЯ
        # логика; негативные пробы — в --self-test (self_test_fsgate).
        if RUN_FSGATETEST.is_file():
            # Явное "none" — настоящая логика без намеренных поломок: полный
            # прогон с поломками идёт в --self-test, а сборке довольно одной пробы.
            res = run_wsl('bash "%s" none' % wsl_path(RUN_FSGATETEST), timeout=900)
            fs_out = res.stdout + res.stderr
            if res.returncode != 0:
                say(tail(fs_out, 4000))
                fail("гейт хост-тестов не пройден", code="FAIL_GATE_HOSTTEST",
                     gate="fsgate", gate_rc=res.returncode, detail=last_line(fs_out))
            say("      затвор ФС: " + last_line(fs_out), code="GATE_PASS",
                gate="fsgate", detail=last_line(fs_out))
        else:
            fail("нет %s" % RUN_FSGATETEST, code="FAIL_NO_HOSTTEST_SCRIPT",
                 path=str(RUN_FSGATETEST))
        emit("STEP_DONE", "шаг 2 пройден", n=2, of=7, name="hosttest")

    # --------------------------------------------------------------- 3
    def step_check_nso(self):
        say("[3/7] гейт check_nso.py…", code="STEP_START", n=3, of=7, name="check_nso")
        rc, out = check_nso(self.nso, self.test_exefs or self.game_exefs, self.diag,
                            diag_gfx=self.diag_gfx, diag_lock=self.diag_lock,
                            nvrm=self.nvrm, early=self.early)
        if rc != 0:
            say(tail(out, 4000))
            fail("гейт check_nso.py не пройден", code="FAIL_GATE_CHECK_NSO",
                 gate="check_nso", gate_rc=rc, path=str(self.nso), detail=last_line(out))
        say("      " + last_line(out), code="GATE_PASS", gate="check_nso",
            detail=last_line(out))
        # Гейт .dynsym: символы совместимости есть ровно там, где обещано. В обе
        # стороны — лишний экспорт в обычной сборке перекрыл бы рабочий символ
        # `sdk` у человека с родной библиотекой.
        gate_compat_syms(self.nso, self.compat, nvrm=self.nvrm)
        emit("STEP_DONE", "шаг 3 пройден", n=3, of=7, name="check_nso")

    # --------------------------------------------------------------- 4
    def step_npdm(self):
        say("[4/7] права в main.npdm игры…", code="STEP_START", n=4, of=7, name="npdm")
        if self.mod_plan and self.mod_plan["npdm"]:
            # Мод несёт свой main.npdm — он основа.
            self.npdm_mode, lines = make_mod_npdm(self.mod_plan["npdm"],
                                                  self.game_exefs / "main.npdm",
                                                  self.npdm, self.tid)
        else:
            lines = make_npdm(self.game_exefs / "main.npdm", self.npdm, self.tid)
        for line in lines:
            say("      " + line)
        emit("STEP_DONE", "шаг 4 пройден", n=4, of=7, name="npdm")

    # --------------------------------------------------------------- 5
    def step_layout(self):
        say("[5/7] раскладка…", code="STEP_START", n=5, of=7, name="layout")
        zbic_paths = {name: p for name, (p, _info) in self.zbic_made.items()}
        plan = self.mod_plan
        exefs, files, count = layout_mod(self.out_root, self.tid, self.nso,
                                         self.slot, self.npdm, self.mod_files,
                                         zbic_files=zbic_paths,
                                         mod_items=(plan["files"] if plan else None),
                                         mod_exefs=(plan["exefs"] if plan else None))
        if plan:
            # Патчи и прочее из мода — рядом с раскладкой, байт в байт.
            for rel, src, _t in plan["patches"]:
                dst = self.out_root / "atmosphere" / "exefs_patches" / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
            for rel, src in plan["extra"]:
                dst = self.out_root / "atmosphere" / "contents" / self.tid / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
            say_mod_plan(plan, self.tid, self.slot, self.npdm_mode)
        if zbic_paths:
            say("      без ZBIC в exefs: " + ", ".join(sorted(zbic_paths)),
                code="ZBIC_LAYOUT_DONE", exefs=str(exefs),
                files=sorted(zbic_paths), count=len(zbic_paths), tid=self.tid,
                source_exefs=str(self.game_exefs))
        say(f"      {exefs}")
        stub = not bool(self.mod_files) and not (self.mod_plan and self.mod_plan["files"])
        say(f"      {files} — файлов: {count}"
            + (" (заглушка: мод не задан или в нём нет файлов romfs)" if stub else ""),
            code="LAYOUT_DONE", exefs=str(exefs), files=str(files), count=count,
            tid=self.tid, slot=self.slot, stub=stub)
        # Гейт согласованности комплекта: всё в exefs — от одной игры и одной
        # версии, модуль лежит под тем слотом, под которым собран, символы те,
        # что обещаны ключами.
        gate_kit(exefs, self.tid, source_exefs=self.game_exefs, slot=self.slot,
                 compat=self.compat, zbic_names=sorted(self.zbic_made),
                 npdm_expected=True, nvrm=self.nvrm)
        if self.mod_plan:
            gate_mod_transfer(self.out_root, self.tid, self.mod_plan, self.slot)
        if self.swapped_sdk:
            say("      комплект для подменённой библиотеки собран: "
                + ", ".join(["модуль " + self.slot, "main.npdm"]
                            + sorted(self.zbic_made)),
                code="SWAPPED_SDK_KIT", tid=self.tid, slot=self.slot,
                exefs=str(exefs), compat=list(self.compat),
                symbols=autofix_symbols(),
                zbic_files=sorted(self.zbic_made),
                zbic_found=self.zbic_needed(), npdm=True)
        emit("STEP_DONE", "шаг 5 пройден", n=5, of=7, name="layout")
        return exefs, files

    # --------------------------------------------------------------- 6
    def step_zip(self, exefs, files):
        say("[6/7] упаковка архива…", code="STEP_START", n=6, of=7, name="zip")
        self.zip_path.parent.mkdir(parents=True, exist_ok=True)
        expected = {}
        with zipfile.ZipFile(self.zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            z.writestr(self.readme_arc, self.readme_text().encode("utf-8"))
            z.writestr("СБОРКА.txt", self.passport_text(exefs).encode("utf-8"))
            base = f"atmosphere/contents/{self.tid}"
            # main.npdm сделан из дампа ИГРЫ — это её файл. В архив для раздачи он не
            # попадает; вместо него кладутся скрипты правки прав и инструкция.
            # Несжатые NSO (снятый ZBIC) сделаны ИЗ ДАМПА игры — это её файлы,
            # как и main.npdm. В архив для раздачи они не попадают; вместо них
            # кладётся scripts/zbic_nso.py и раздел инструкции.
            mod_names = tuple(sorted(self.mod_plan["exefs"])) if self.mod_plan else ()
            names = ((self.slot,) + mod_names if self.share
                     else (self.slot, "main.npdm") + tuple(sorted(self.zbic_made)) + mod_names)
            for name in names:
                arc = f"{base}/exefs/{name}"
                z.write(exefs / name, arc)
                expected[arc] = (exefs / name).stat().st_size
            if self.share:
                share_scripts = [NPDM_SDCARD, NPDM_SYSCALLS]
                if self.zbic_needed():
                    share_scripts.append(ZBIC_NSO)
                    share_scripts.append(ZBIC_PURE)
                for script in share_scripts:
                    arc = f"{SHARE_SCRIPTS_DIR}/{script.name}"
                    z.write(script, arc)
                    expected[arc] = script.stat().st_size
                z.writestr(f"{SHARE_SCRIPTS_DIR}/КАК-СДЕЛАТЬ-main.npdm.txt",
                           self.share_howto_text().encode("utf-8"))
            for p in sorted(files.rglob("*")):
                if p.is_file():
                    arc = f"{base}/files/" + p.relative_to(files).as_posix()
                    z.write(p, arc)
                    expected[arc] = p.stat().st_size
            # Патчи и прочее из мода — как в моде.
            if self.mod_plan:
                for rel, src, _t in self.mod_plan["patches"]:
                    arc = "atmosphere/exefs_patches/" + rel
                    z.write(src, arc)
                    expected[arc] = src.stat().st_size
                for rel, src in self.mod_plan["extra"]:
                    arc = f"{base}/{rel}"
                    z.write(src, arc)
                    expected[arc] = src.stat().st_size
        size = f"{self.zip_path.stat().st_size:,}".replace(",", " ")
        say(f"      {self.zip_path}")
        say(f"      файлов: {len(expected)}, размер архива: {size} Б",
            code="ZIP_WRITTEN", path=str(self.zip_path), entries=len(expected),
            size=self.zip_path.stat().st_size, share=self.share, diag=self.diag)
        emit("STEP_DONE", "шаг 6 пройден", n=6, of=7, name="zip")
        return expected

    def zbic_needed(self):
        """Нашёл ли осмотр exefs сжатие ZBIC (ключ --zbic-fix тут ни при чём)."""
        return bool(self.zbic_scan and self.zbic_scan.get("zbic"))

    # --------------------------------------------------------------- 7
    def step_verify(self, expected):
        say("[7/7] проверка архива…", code="STEP_START", n=7, of=7, name="verify_zip")
        verify_zip(self.zip_path, expected, self.tid, self.slot, self.readme_arc,
                   share=self.share, zbic_names=sorted(self.zbic_made),
                   zbic_needed=self.zbic_needed(),
                   mod_exefs=(sorted(self.mod_plan["exefs"]) if self.mod_plan else ()))
        sha = file_sha256(self.zip_path)
        say(f"      проверки пройдены; sha256 архива: {sha}",
            code="GATE_PASS", gate="zip", path=str(self.zip_path), sha256=sha)
        emit("STEP_DONE", "шаг 7 пройден", n=7, of=7, name="verify_zip")
        emit("ARTIFACT", "архив собран", kind="zip", path=str(self.zip_path),
             sha256=sha, size=self.zip_path.stat().st_size, share=self.share,
             diag=self.diag, tid=self.tid, slot=self.slot)
        return sha

    # --------------------------------------------------------------- текст
    def passport_text(self, exefs):
        lines = [
            f"Сборка: {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')} — {self.title}",
            f"Архив: {self.zip_path.name}",
            f"Игра: {self.game}",
            f"Слот модуля: {self.slot}",
            f"TitleID: {self.tid}",
            f"{self.slot}: {(exefs / self.slot).stat().st_size} Б, "
            f"sha256 {file_sha256(exefs / self.slot)}",
            f"main.npdm: {(exefs / 'main.npdm').stat().st_size} Б, "
            f"sha256 {file_sha256(exefs / 'main.npdm')}",
            f"Файлы мода: {self.mod_files if self.mod_files else ((str(self.mod_plan['root']) + ' (мод целиком)') if self.mod_plan else '(заглушка)')}",
            ("Из мода: " + ", ".join(
                ["exefs/" + n for n in sorted(self.mod_plan["exefs"])]
                + ["exefs_patches/" + r for r, _p, _t in self.mod_plan["patches"]]
                + ["main.npdm мода (%s)" % self.npdm_mode if self.mod_plan["npdm"] else
                   "main.npdm игры (своего у мода нет)"])
             if self.mod_plan else "Из мода: только файлы"),
            "Ранний перехват nn::fs: " + ("ДА (%s) — у мода свой модуль" % EARLY_FS_CFLAG
                                          if self.early else "нет (перехват в хуке nnMain)"),
            "Вариант: " + ("КОМПЛЕКТ для консоли с ПОДМЕНЁННОЙ библиотекой sdk"
                           if self.swapped_sdk else "обычный"),
            "Автоисправление: " + compat_human(self.compat),
            ("Паспорт консоли: модуль пишет на карту atmosphere/contents/%s/%s — "
             "что за sdk загружена и что модуль выбрал" % (self.tid, AUTOFIX_PASSPORT)
             if self.compat else "Паспорт консоли: нет (сборка без автоисправления)"),
            # Паспорт обязан говорить, КАКОЙ диагностический вариант собран: по
            # архиву на карте иначе не отличить рабочий модуль от модуля с
            # перехватом всей графической цепочки.
            "Диагностика графики: " + ("ВКЛЮЧЕНА (%s) — отчёт sd:/%s"
                                       % (GFX_CFLAG, GFX_REPORT_NAME)
                                       if self.diag_gfx else "нет"),
            "Узкая проба блокировок: " + ("ВКЛЮЧЕНА (%s) — один перехватчик "
                                          "pthread_rwlock_rdlock, только внутри "
                                          "eglCreateWindowSurface" % LOCK_CFLAG
                                          if self.diag_lock else "нет"),
            "Папка сборки модуля: " + self.nso.parent.name,
            "Вошло в комплект: модуль %s, main.npdm%s" % (
                self.slot,
                (", несжатые (без ZBIC) " + ", ".join(sorted(self.zbic_made)))
                if self.zbic_made else
                (", несжатого main нет: сжатия ZBIC у этой игры не найдено"
                 if self.swapped_sdk else "")),
            ("main.npdm: НЕ ВЛОЖЕН — архив для раздачи" if self.share
             else "ВНИМАНИЕ: main.npdm в архиве сделан из ДАМПА игры — это файл игры, "
                  "раздавать архив нельзя"),
        ]
        if self.zbic_needed():
            lines.append("Сжатие ZBIC в exefs игры: "
                         + ", ".join(self.zbic_scan["zbic_files"]))
        for name, (_p, info) in sorted(self.zbic_made.items()):
            lines += [
                f"Снят ZBIC: {name} — {info['size_before']} Б -> {info['size_after']} Б, "
                f"flags 0x{info['flags_after']:02x}",
                f"  ModuleId {info['module_id'][:40]}",
                f"  sha256 исходного {name}: {info['src_sha256']}",
                f"  sha256 несжатого {name}: {info['dst_sha256']}",
                f"  источник: {self.game_exefs} (та же папка, откуда взят main.npdm)",
            ]
        if self.diag:
            lines += ["",
                      "ЭТО ДИАГНОСТИКА, не рабочая сборка.",
                      "Отчёт появится на SD-карте: " + DIAG_REPORT_NAME.replace("<TitleID>", self.tid or "<TitleID>"),
                      "(новый на каждый запуск, хранятся последние 5; первая строка — TitleID и BuildID)"]
        if self.diag_gfx:
            lines += ["",
                      "ВКЛЮЧЕНА ДИАГНОСТИКА ГРАФИКИ — это не рабочая сборка.",
                      "Отчёт появится на SD-карте: " + GFX_REPORT_NAME.replace("<TitleID>", self.tid or "<TitleID>"),
                      "Строки, начинающиеся с «%s», — подозреваемые (неуспешный вызов)."
                      % GFX_FAIL_MARK]
        lines += ["",
                  "Если игра ведёт себя не так, как ожидалось, назовите эти строки —",
                  "по ним понятно, какая именно сборка проверялась."]
        return "\r\n".join(lines)

    def zbic_readme_lines(self):
        """
        Что написать в ЧИТАТЬ.txt про ZBIC. Пусто, если ZBIC в игре нет.

        Здесь же — единственное место, где пользователю говорится правда о
        непроверенности: на консоли этот путь НЕ пробован.
        """
        if not self.zbic_needed():
            return []
        who = ", ".join(self.zbic_scan["zbic_files"])
        lines = [
            "=================== СЖАТИЕ ZBIC ===================",
            f"В exefs этой игры сжатие ZBIC (бит 7 флагов NSO, HOS 22.0.0+): {who}.",
            "Atmosphère младше 1.12.0 такой файл не распаковывает: игра не стартует",
            "(«Could not start the software»).",
            "",
        ]
        if self.zbic_made and not self.share:
            lines += [
                "В архиве лежит " + ", ".join(sorted(self.zbic_made)) + " — тот же файл",
                "игры, но с распакованными сегментами и флагами 0x38 (сжатия нет, все три",
                "sha256 сегментов остались ОРИГИНАЛЬНЫЕ, их считала Nintendo). Загрузчику",
                "нечего распаковывать, и он грузит файл обычной веткой.",
                "",
                "ЧЕСТНО: на консоли этот путь НЕ пробован. Проверено только на ПК:",
                "распаковка сходится с sha256 из заголовка NSO, пересобранный файл",
                "разбирается как годный NSO. Первую пробу делайте, сохранив возможность",
                "вернуть всё назад (просто убрать файл из exefs).",
                "",
                "Альтернатива без мода — Atmosphère 1.12.0: в ней ZBIC поддержан штатно.",
                "Но она выпущена под HOS 23.0.0, то есть это обновление прошивки, а не",
                "замена одного файла. Сборки вроде borntohonk несут в нагрузку встроенные",
                "патчи и жёстко зашитую блокировку серверов Nintendo — это НЕ нейтральная",
                "замена официальной Atmosphère.",
                "",
                "ВАЖНО про версию: файл взят из ТОГО ЖЕ дампа, что и main.npdm. Если у вас",
                "установлено обновление игры, и exefs, и этот файл должны быть ИЗ ПАТЧА:",
                "подложенный main от базовой версии игру не запустит.",
                "",
            ]
        elif self.share:
            lines += [
                "Файла без ZBIC в этом архиве НЕТ: он делается из дампа игры, как и",
                f"main.npdm. Сделайте его сами — скрипт и инструкция в папке",
                f"{SHARE_SCRIPTS_DIR}.",
                "",
            ]
        return lines

    def readme_text(self):
        lines = [
            "nxredirect — подмена файлов игры файлами с SD, без LayeredFS.",
            "",
            f"Игра: {self.game}   TitleID: {self.tid}   слот модуля: {self.slot}",
            "",
            "Распаковать содержимое архива в КОРЕНЬ SD-карты. Ляжет так:",
            f"  atmosphere/contents/{self.tid}/exefs/{self.slot}",
        ]
        if not self.share:
            lines.append(f"  atmosphere/contents/{self.tid}/exefs/main.npdm")
            for name in sorted(self.zbic_made):
                lines.append(f"  atmosphere/contents/{self.tid}/exefs/{name}"
                             "   (тот же файл игры, но без сжатия ZBIC)")
        lines += [
            f"  atmosphere/contents/{self.tid}/files/...",
            "",
            f"ВАЖНО: папки atmosphere/contents/{self.tid}/romfs и файла romfs.bin",
            "быть не должно — с ними Atmosphère включит LayeredFS, и тяжёлые игры",
            "снова начнут зависать. Старый мод на LayeredFS уберите заранее.",
            "",
        ] + self.zbic_readme_lines() + [
            "Подменяется только ЧТЕНИЕ файлов данных игры. Открытие файла на запись",
            "и точки монтирования save/saveData/bcat/system/user/cache/temp/cal/host/",
            "sdmc/sd/content не подменяются никогда — сохранения игрока не затрагиваются.",
        ]
        if self.share:
            lines += [
                "",
                "=================== ЧЕГО В АРХИВЕ НЕТ ===================",
                "main.npdm СЮДА НЕ ВЛОЖЕН. Это файл самой игры (берётся из её дампа),",
                "и раздавать его нельзя. Без него модуль не запустится: в правах нужен",
                "бит SdCard и системные вызовы 0x74/0x75.",
                "",
                "Сделайте main.npdm сами из СВОЕГО дампа этой игры и этой версии",
                f"обновления — как именно, написано в папке {SHARE_SCRIPTS_DIR}",
                "(там же оба скрипта). Готовый файл положить рядом с модулем:",
                f"  atmosphere/contents/{self.tid}/exefs/main.npdm",
            ]
        else:
            lines += [
                "",
                "============== ЭТОТ АРХИВ НЕЛЬЗЯ РАЗДАВАТЬ ==============",
                "Внутри лежит exefs/main.npdm, сделанный из ДАМПА игры: это файл игры,",
                "а не наш. Архив годится только для СВОЕЙ консоли.",
                "Для раздачи соберите вариант без него: python build.py --share",
                "(в нём будут скрипты правки прав и инструкция, как сделать main.npdm",
                "из своего дампа).",
                "",
                "main.npdm здесь — от ЭТОЙ игры и ЭТОЙ версии обновления, с битом SdCard",
                "и вызовами 0x74/0x75. Для другой версии игры его нужно сделать заново",
                "(scripts/npdm_sdcard.py, scripts/npdm_syscalls.py).",
            ]
        if self.diag:
            lines += ["",
                      "ЭТО ДИАГНОСТИЧЕСКАЯ СБОРКА. Запустить игру, выйти, забрать с SD последний",
                      "(с наибольшим номером) файл " + DIAG_REPORT_NAME.replace("<TitleID>", self.tid or "<TitleID>")
                      + ". Если игра упала — нужен crash report",
                      "Atmosphère; коды намеренных аварий:"]
            lines += [f"    {code} — {meaning}" for code, meaning in DIAG_CRASH_CODES]
        if self.diag_gfx:
            lines += [
                "",
                "============== ДИАГНОСТИКА ГРАФИКИ ==============",
                "Эта сборка собрана с " + GFX_CFLAG + ": модуль перехватывает весь путь",
                "вывода картинки (nn::vi -> EGL -> NVN) и записывает, что вернул каждый",
                "вызов. Игра от этого рисует не быстрее и не лучше — это ПРОБА, а не",
                "рабочий мод, и на постоянно её оставлять не нужно.",
                "",
                "Запустить игру, подождать 10-20 секунд, выйти из неё и забрать с SD-карты",
                "последний (с наибольшим номером) файл " + GFX_REPORT_NAME.replace("<TitleID>", self.tid or "<TitleID>"),
                "(хранятся последние 5). Файл сбрасывается на карту сразу после",
                "каждой записи, поэтому он читаем даже если игра зависла и консоль",
                "пришлось выключить кнопкой.",
                "",
                "Как читать. Строки, начинающиеся с «" + GFX_FAIL_MARK + "», — ПОДОЗРЕВАЕМЫЕ:",
                "вызов вернул неуспех (ноль, false или ошибку). Первая такая строка сверху",
                "и есть место, где картинка потерялась. Для неуспешного вызова EGL модуль",
                "сам зовёт eglGetError (игра этого не делает) и дописывает код ошибки.",
                "Строки без метки — успешные вызовы, они нужны как фон.",
            ]
        return "\r\n".join(lines)

    def zbic_howto_lines(self):
        """Раздел инструкции для раздачи: как снять ZBIC из своего дампа."""
        if not self.zbic_needed():
            return []
        who = ", ".join(self.zbic_scan["zbic_files"])
        return [
            "",
            "================ ЕЩЁ: СНЯТЬ СЖАТИЕ ZBIC ================",
            f"У этой игры в exefs сжатие ZBIC: {who}. Atmosphère младше 1.12.0",
            "такой файл не загружает, и игра не стартует вовсе — ни с модом, ни без.",
            "",
            "Файл без ZBIC делается из ВАШЕГО дампа (это файл игры, поэтому его тут нет):",
            f"  python {ZBIC_NSO.name} --scan <exefs>          # что именно сжато ZBIC",
            f"  python {ZBIC_NSO.name} --rebuild <exefs>/main -o main",
            f"Ничего ставить не нужно: рядом лежит {ZBIC_PURE.name} — распаковщик",
            "на чистом Python. Он медленнее (крупный main — десятки секунд).",
            "Быстрее, если захотите: pip install %(name)s   (%(author)s, %(license)s, %(url)s)"
            % ZBIC_PKG,
            "",
            "Готовый файл положить рядом с модулем:",
            f"  atmosphere/contents/{self.tid}/exefs/main",
            "",
            "Берите exefs ТОЙ версии, которая стоит у вас: если установлено обновление,",
            "и main.npdm, и main нужны ИЗ ПАТЧА, а не из базовой игры.",
            "На консоли этот путь НЕ пробован — проверено только на ПК.",
        ]

    def share_howto_text(self):
        """Инструкция в архиве для раздачи: как сделать main.npdm из своего дампа."""
        return "\r\n".join([
            "КАК СДЕЛАТЬ main.npdm ДЛЯ ЭТОГО МОДА",
            "",
            "Зачем. Модуль ставит хуки (системные вызовы 0x74/0x75) и читает SD-карту",
            "(бит SdCard в правах FS). У игры этих прав нет, поэтому её main.npdm нужно",
            "дополнить. Сам main.npdm — файл игры, поэтому в архиве его нет: сделайте",
            "его из СВОЕГО дампа.",
            "",
            "Что нужно: Python 3.8+ и распакованный exefs ЭТОЙ игры ЭТОЙ версии",
            "обновления (main, rtld, sdk, subsdk0.., main.npdm) из своего дампа.",
            "Взять exefs можно, например, nstool/hactoolnet из своего NSP/XCI и его",
            "обновления. Версия обновления важна: права от другой версии не подойдут.",
            "",
            "Команды (из этой папки):",
            "  python npdm_sdcard.py <exefs>/main.npdm -o main.npdm.sdcard",
            "  python npdm_syscalls.py main.npdm.sdcard -o main.npdm --svc 0x74 0x75",
            "Проверка (обязательно, иначе игра упадёт на «Undefined System Call»):",
            "  python npdm_syscalls.py main.npdm --check --program-id <TID> --svc 0x74 0x75",
            f"TID этой игры: {self.tid}",
            "",
            "Готовый main.npdm положить на SD рядом с модулем:",
            f"  atmosphere/contents/{self.tid}/exefs/main.npdm",
            "",
            "Свой main.npdm никому не пересылайте — это файл игры.",
        ] + self.zbic_howto_lines())

    # --------------------------------------------------------------- всё
    def prepare(self):
        self.game_exefs = find_game_exefs(self.game_exefs)
        if not self.game_exefs:
            fail("не найден распакованный exefs игры (main, subsdk0.., sdk, rtld, main.npdm).\n"
                 "        Укажите --game-exefs <папка>, положите её в nxredirect/game-exefs\n"
                 "        или задайте NXR_GAME_EXEFS. Без модулей игры гейты 2 и 3 проверять\n"
                 "        нечем, поэтому сборка КРАСНАЯ, а не «пропущенная».",
                 code="FAIL_NO_GAME_EXEFS")
        for need in ("rtld", "main", "sdk", "main.npdm"):
            if not (self.game_exefs / need).is_file():
                fail(f"в {self.game_exefs} нет обязательного файла {need}",
                     code="FAIL_EXEFS_MISSING_FILE", path=str(self.game_exefs),
                     missing=need)
        self.tid = npdm_program_id(self.game_exefs / "main.npdm")
        busy = occupied_slots(self.game_exefs)
        # Мод целиком: разобрать до выбора слота — слоты его модулей
        # заняты, а наш модуль собирается с ранним перехватом nn::fs.
        self.mod_plan, files_dir = resolve_mod_input(self.mod, self.mod_files, self.tid,
                                                     self.game_exefs)
        if self.mod_plan:
            self.mod_files = None
            mod_plan_event(self.mod_plan, self.tid)
            if self.slot in self.mod_plan["modules"]:
                if self.slot_explicit:
                    fail(f"слот {self.slot} занят модулем мода — наш модуль туда не "
                         "ляжет. Уберите --slot (выберется свободный) или задайте другой.",
                         code="FAIL_MOD_SLOT_CONFLICT", slot=self.slot,
                         mod_slots=sorted(self.mod_plan["modules"]))
                self.slot = pick_slot_for_mod(busy, self.mod_plan)
                if self.slot is None:
                    fail("все слоты subsdk0..subsdk9 заняты игрой и модом",
                         code="FAIL_NO_FREE_SLOT", busy_slots=sorted(
                             set(busy) | set(self.mod_plan["modules"])))
            self.early = bool(self.mod_plan["modules"])
        elif files_dir:
            self.mod_files = Path(files_dir)
        emit("EARLY_FS_SELECTED", "ранний перехват nn::fs", early=self.early,
             cflag=(EARLY_FS_CFLAG if self.early else None),
             why=("у мода свой модуль: " + ", ".join(sorted(self.mod_plan["modules"]))
                  if self.early else "модуля мода нет — перехват в хуке nnMain, как раньше"))
        if self.mod_plan and self.mod_plan["modules"]:
            ok, why = mod_slot_order(self.slot, self.mod_plan)
            say("      " + why, code="MOD_SLOT_ORDER", level=("info" if ok else "warn"),
                slot=self.slot, mod_slots=sorted(self.mod_plan["modules"]), ok=ok, why=why)
        # Папка модуля зависит от раннего перехвата и слота — пересчитать.
        self.nso = (self.out_root
                    / module_dir_name(self.compat, self.diag_gfx, self.diag_lock, self.nvrm,
                                      self.early)
                    / self.slot)
        say(f"      exefs игры: {self.game_exefs}")
        say(f"      TitleID (из main.npdm игры): {self.tid}")
        say(f"      слоты, занятые игрой: {', '.join(busy) if busy else '(нет)'}",
            code="GAME_INSPECTED", exefs=str(self.game_exefs), tid=self.tid,
            busy_slots=busy)
        if self.slot in busy:
            fail(f"слот {self.slot} занят самой игрой — задайте свободный через --slot "
                 f"(свободен, например, {pick_free_slot(self.game_exefs)[0]})",
                 code="FAIL_SLOT_BUSY_GAME", slot=self.slot, busy_slots=busy,
                 free=pick_free_slot(self.game_exefs)[0])
        emit("SLOT_CHOSEN", "слот сборки", slot=self.slot, source="аргумент/по умолчанию",
             busy_slots=busy)
        # Осмотр на ZBIC делается ВСЕГДА, ключ влияет только на то, чиним ли.
        self.zbic_scan = zbic_scan(self.game_exefs, fix=self.zbic_fix)
        if self.mod_plan and self.zbic_fix:
            clash = sorted(n for n in self.mod_plan["exefs"]
                           if n in (self.zbic_scan.get("zbic_files") or []))
            if clash:
                fail("мод несёт свой " + ", ".join(clash) + ", а снятие ZBIC (--zbic-fix) "
                     "кладёт в exefs файл с тем же именем. Двум файлам не лечь: решите, чей "
                     "нужен (без --zbic-fix останется файл мода).",
                     code="FAIL_MOD_ZBIC_CONFLICT", names=clash)
        # Профиль игры: чем рисует, родная sdk, ZBIC, требуемая прошивка (CNMT).
        report_game_profile(self.game_exefs, self.cnmt)
        # Нужен ли этой игре доэкспорт — не гадаем: либо сверяем с указанной
        # библиотекой, либо честно говорим, что проверить нечем. С --sdk тут же —
        # прогноз автоисправления и детектор заглушек.
        report_missing_symbols(self.game_exefs, self.sdk)
        if self.zbic_fix:
            say("      снимаю сжатие ZBIC (источник — этот же exefs)…")
            self.zbic_made = zbic_prepare_exefs(self.game_exefs,
                                                self.out_root / "module" / "zbic",
                                                scan=self.zbic_scan,
                                                required=not self.swapped_sdk)
            if self.zbic_made:
                self.test_exefs = self.make_test_exefs()

    def make_test_exefs(self):
        """
        Папка exefs для ГЕЙТОВ: те же файлы игры, но `main` — уже без ZBIC.

        Зачем. Хост-тест разбирает модули игры своим разбором NSO (LZ4) и о ZBIC
        не знает: на ZBIC-`main` он просто не видит модуль и краснеет. А проверять
        надо ровно тот набор, который поедет на карту, — с нашим несжатым main.
        Оригинальный exefs игры при этом не меняется: копия лежит в out/.
        """
        dst = self.out_root / "module" / "zbic-exefs"
        dst.mkdir(parents=True, exist_ok=True)
        made = {name: p for name, (p, _i) in self.zbic_made.items()}
        for p in sorted(self.game_exefs.iterdir()):
            if not p.is_file():
                continue
            src = made.get(p.name, p)
            out = dst / p.name
            if out.is_file() and out.stat().st_size == src.stat().st_size \
                    and out.stat().st_mtime >= src.stat().st_mtime:
                continue          # уже скопировано этой же сборкой
            shutil.copy2(src, out)
        say(f"      exefs для гейтов (с несжатым {', '.join(sorted(made))}): {dst}")
        return dst

    def run(self, skip_build=False, no_zip=False):
        say(f"=== nxredirect: {self.title} ===", code="BUILD_START",
            kind="diag" if self.diag else "plain", title=self.title, slot=self.slot,
            game=self.game, share=self.share, version=NXR_VERSION, steps=7,
            diag_gfx=self.diag_gfx)
        self.prepare()
        self.step_build(skip_build)
        self.step_hosttest()
        self.step_check_nso()
        self.step_npdm()
        exefs, files = self.step_layout()
        if no_zip:
            say("[6/7] архив пропущен (--no-zip)", code="STEP_SKIPPED",
                n=6, of=7, name="zip", why="--no-zip")
            say("[7/7] проверка архива пропущена (--no-zip)", code="STEP_SKIPPED",
                n=7, of=7, name="verify_zip", why="--no-zip")
            return None
        expected = self.step_zip(exefs, files)
        return self.step_verify(expected)


# ------------------------------------------------------------------ гейт архива


def verify_zip(zip_path, expected, tid, slot, readme_arc=None, share=False,
               zbic_names=(), zbic_needed=False, mod_exefs=()):
    """
    Шаг 7: гейт архива. Роняет сборку, если:
      - запись битая или размер не совпал с исходником;
      - нет exefs/<slot> или ни одного файла в files/;
      - личный архив без exefs/main.npdm ЛИБО архив для раздачи (share) с ним:
        main.npdm сделан из дампа игры, в раздаваемом архиве его быть не должно,
        зато должны быть скрипты правки прав и инструкция;
      - есть папка romfs или romfs.bin (включат LayeredFS);
      - есть package3 (тут не нужен);
      - нет пояснения readme_arc или паспорта СБОРКА.txt.
    """
    problems = []
    base = f"atmosphere/contents/{tid}"
    with zipfile.ZipFile(zip_path) as z:
        bad = z.testzip()
        if bad:
            problems.append(f"битая запись: {bad}")
        names = z.namelist()
        sizes = {i.filename: i.file_size for i in z.infolist()}
        for arc, size in expected.items():
            if sizes.get(arc) != size:
                problems.append(f"размер не совпал: {arc} "
                                f"({sizes.get(arc)} вместо {size})")
        must_have = [f"{base}/exefs/{slot}"] + [f"{base}/exefs/{n}" for n in mod_exefs]
        if not share:
            must_have.append(f"{base}/exefs/main.npdm")
        for must in must_have:
            if must not in names:
                problems.append(f"в архиве нет {must}")
        # Несжатые NSO (снятый ZBIC): в личном архиве обязаны быть все, что
        # собраны; в архиве для раздачи их быть НЕ ДОЛЖНО — они сделаны из дампа
        # игры ровно так же, как main.npdm.
        for name in zbic_names:
            arc = f"{base}/exefs/{name}"
            if not share and arc not in names:
                problems.append(f"в архиве нет пересобранного без ZBIC {arc}")
        if share:
            npdm_inside = [n for n in names if n.endswith("main.npdm")]
            if npdm_inside:
                problems.append("архив для раздачи содержит main.npdm из дампа игры: "
                                + ", ".join(npdm_inside))
            game_nso = [n for n in names
                        if n.startswith(f"{base}/exefs/")
                        and n.rsplit("/", 1)[-1] not in (slot,) + tuple(mod_exefs)]
            if game_nso:
                problems.append("архив для раздачи содержит файлы exefs из дампа игры: "
                                + ", ".join(game_nso))
            # Библиотека sdk (nnSdk) — файл Nintendo: в раздаваемом архиве её не
            # бывает НИКОГДА, даже если её нёс мод (mod_exefs) —
            # подмену sdk каждый кладёт сам (install.py --swap-sdk).
            sdk_inside = [n for n in names
                          if "/exefs/" in n and n.rsplit("/", 1)[-1] == "sdk"]
            if sdk_inside:
                problems.append("архив для раздачи содержит библиотеку sdk (файл "
                                "Nintendo): " + ", ".join(sdk_inside))
            want_scripts = [NPDM_SDCARD.name, NPDM_SYSCALLS.name,
                            "КАК-СДЕЛАТЬ-main.npdm.txt"]
            if zbic_needed:
                want_scripts.append(ZBIC_NSO.name)
                want_scripts.append(ZBIC_PURE.name)
            for script in want_scripts:
                if not any(n.endswith("/" + script) for n in names):
                    problems.append(f"в архиве для раздачи нет {script} — "
                                    "получателю нечем сделать свой main.npdm"
                                    + (" и снять ZBIC"
                                       if script in (ZBIC_NSO.name, ZBIC_PURE.name)
                                       else ""))
        if not any(n.startswith(f"{base}/files/") and not n.endswith("/") for n in names):
            problems.append(f"в архиве нет файлов мода ({base}/files/)")
        if any("/romfs/" in n or n.endswith("/romfs") for n in names):
            problems.append("в архиве есть папка romfs — она включит LayeredFS")
        if any(n.endswith("romfs.bin") for n in names):
            problems.append("в архиве есть romfs.bin — он включит LayeredFS")
        if any(n.endswith("package3") for n in names):
            problems.append("в архиве есть package3 — он тут не нужен")
        if "СБОРКА.txt" not in names:
            problems.append("в архиве нет паспорта СБОРКА.txt")
        if readme_arc and readme_arc not in names:
            problems.append(f"в архиве нет пояснения {readme_arc}")
    if problems:
        for p in problems:
            say("      ПРОБЛЕМА: " + p, level="warn")
        fail("архив не прошёл проверку", code="FAIL_GATE_ZIP", gate="zip",
             path=str(zip_path), problems=problems, share=bool(share))


# ------------------------------------------------------------------ негативные пробы


# Учёт проб самопроверки. Нужен, чтобы приговор говорил правду: «проба не
# сработала» (гейт не умеет отказывать) и «пробу нечем было запустить» (нет дампа
# игры или несобранного модуля) — РАЗНЫЕ вещи. Раньше оба случая давали одну
# строку «ЕСТЬ НЕ СРАБОТАВШИЕ ПРОБЫ», и самопроверка на машине без дампа выглядела
# как дефект гейта.
PROBE_STATS = {"ran": 0, "good": 0, "bad": 0, "skipped": 0, "reasons": []}


def probe_reset():
    """Обнулить учёт перед новой самопроверкой (в одном процессе она одна)."""
    PROBE_STATS.update(ran=0, good=0, bad=0, skipped=0)
    PROBE_STATS["reasons"] = []


def _probe_count(ok):
    PROBE_STATS["ran"] += 1
    PROBE_STATS["good" if ok else "bad"] += 1


def probe(label, caught):
    """Негативная проба: гейт ОБЯЗАН покраснеть."""
    _probe_count(bool(caught))
    say("      проба «%s» — %s" % (label, "поймана" if caught else "НЕ ПОЙМАНА"),
        code="PROBE_CAUGHT" if caught else "PROBE_MISSED",
        level="info" if caught else "error", probe=label, kind="negative")
    return caught


def probe_green(label, ok):
    """Положительная проба: гейт обязан быть зелёным (иначе пробы ничего не значат)."""
    _probe_count(bool(ok))
    say("      проба «%s» — %s" % (label, "зелёная" if ok else "КРАСНАЯ (это плохо)"),
        code="PROBE_GREEN" if ok else "PROBE_RED",
        level="info" if ok else "error", probe=label, kind="positive")
    return ok


def probe_skip(label, reason, how):
    """
    Блок проб НЕ ЗАПУСКАЛСЯ: входа нет. Возвращает False — итог всё равно красный
    (самопроверка, ничего не проверившая, ничего и не доказала), но в отчёте это
    названо своим именем, а не «проба не сработала».
    """
    PROBE_STATS["skipped"] += 1
    PROBE_STATS["reasons"].append("%s — %s" % (label, reason))
    say("      пробы «%s» НЕ ЗАПУСКАЛИСЬ: %s" % (label, reason),
        code="PROBE_SKIPPED", level="warn", probe=label, kind="skipped",
        reason=reason, how=how)
    say("        чтобы запустить: " + how, level="warn")
    return False


# Как дать самопроверке недостающий вход. Держим одной строкой, чтобы во всех
# пропусках совет был один и тот же и не расходился.
EXEFS_HOW = ("python build.py --quick-self-test --game-exefs <папка exefs игры> "
             "(или переменная NXR_GAME_EXEFS, или папка nxredirect/game-exefs)")
# Самопроверке нужны ОБЕ сборки: out/module/ (без автоисправления, --no-autofix;
# на ней гоняются хост-тест, check_nso и пробы установщика) и out/module-autofix/
# (по умолчанию; на ней — автоисправление). Одна команда без --no-autofix кладёт
# модуль только в out/module-autofix/, и блоки с out/module так и пропускались бы.
MODULE_HOW = ("сначала собрать модуль в обоих видах: "
              "py -3 build.py --game-exefs ... --no-autofix --no-zip "
              "(out/module/) и py -3 build.py --game-exefs ... --no-zip "
              "(out/module-autofix/)")


def _why_no_input(game_exefs, nso):
    why = []
    if not game_exefs:
        why.append("нет распакованного exefs игры")
    if not nso:
        why.append("модуль не собран (нет out/module/subsdkN)")
    return "; ".join(why) or "нет входа"


def _how_no_input(game_exefs, nso):
    how = []
    if not game_exefs:
        how.append(EXEFS_HOW)
    if not nso:
        how.append(MODULE_HOW)
    return "; ".join(how)


def self_test_verdict(ok):
    """
    Приговор самопроверки одной строкой — общий для build.py и build-diag.py.

    Три разных исхода, и они НЕ синонимы:
      * ни одна проба не запускалась — затвор нуля, самопроверка бессмысленна;
      * есть не сработавшие пробы — гейт не умеет отказывать (настоящая беда);
      * все запущенные сработали, но часть блоков пропущена — самопроверка
        НЕПОЛНАЯ: о пропущенных гейтах не доказано ничего.
    Красный код возврата во всех трёх случаях прежний (1): ослаблять затвор
    нельзя, чинится формулировка, а не строгость.
    """
    s = PROBE_STATS
    if not s["ran"]:
        ok = False
        msg = "ИТОГ: НИ ОДНА ПРОБА НЕ ЗАПУСКАЛАСЬ — самопроверка ничего не доказала"
    elif s["bad"]:
        ok = False
        msg = ("ИТОГ: ЕСТЬ НЕ СРАБОТАВШИЕ ПРОБЫ — %d из %d запущенных "
               "(гейт не умеет отказывать)" % (s["bad"], s["ran"]))
    elif s["skipped"]:
        ok = False
        msg = ("ИТОГ: САМОПРОВЕРКА НЕПОЛНАЯ — запущено проб %d, все сработали, "
               "но %d блок(ов) проб не запускались" % (s["ran"], s["skipped"]))
    elif not ok:
        msg = ("ИТОГ: САМОПРОВЕРКА НЕ ПРОЙДЕНА — запущено проб %d, все сработали, "
               "но шаг самопроверки вернул отказ" % s["ran"])
    else:
        msg = "ИТОГ: все пробы сработали (запущено %d)" % s["ran"]
    say(msg, code="SELFTEST_DONE", level="info" if ok else "error", passed=bool(ok),
        rc=0 if ok else 1, ran=s["ran"], caught=s["good"], missed=s["bad"],
        skipped=s["skipped"])
    for r in PROBE_STATS["reasons"]:
        say("  не запускалось: " + r, level="warn")
    return bool(ok)


def _zip_probe(tid, slot, mutate, share=False, expect_red=True,
               zbic_names=(), zbic_needed=False, mod_exefs=()):
    """
    Собирает игрушечный архив, портит его как сказано, и смотрит на гейт.

    expect_red=True: гейт ОБЯЗАН покраснеть (негативная проба).
    expect_red=False: гейт обязан быть зелёным (контроль, что проба вообще о чём-то).
    """
    import tempfile
    base = f"atmosphere/contents/{tid}"
    entries = {
        f"{base}/exefs/{slot}": b"x",
        f"{base}/files/a.dat": b"z",
    }
    if share:
        entries[f"{SHARE_SCRIPTS_DIR}/{NPDM_SDCARD.name}"] = b"s1"
        entries[f"{SHARE_SCRIPTS_DIR}/{NPDM_SYSCALLS.name}"] = b"s2"
        entries[f"{SHARE_SCRIPTS_DIR}/КАК-СДЕЛАТЬ-main.npdm.txt"] = b"howto"
        if zbic_needed:
            entries[f"{SHARE_SCRIPTS_DIR}/{ZBIC_NSO.name}"] = b"s3"
            entries[f"{SHARE_SCRIPTS_DIR}/{ZBIC_PURE.name}"] = b"s4"
    else:
        entries[f"{base}/exefs/main.npdm"] = b"y"
        for name in zbic_names:
            entries[f"{base}/exefs/{name}"] = b"nso"
    extra = {}
    mutate(entries, extra)
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "t.zip"
        with zipfile.ZipFile(p, "w") as z:
            z.writestr("СБОРКА.txt", b"passport")
            z.writestr("ЧИТАТЬ.txt", b"readme")
            for name, data in entries.items():
                z.writestr(name, data)
        expected = {k: len(v) for k, v in entries.items()}
        expected.update(extra)
        try:
            verify_zip(p, expected, tid, slot, "ЧИТАТЬ.txt", share=share,
                       zbic_names=zbic_names, zbic_needed=zbic_needed,
                       mod_exefs=mod_exefs)
        except SystemExit:
            return expect_red
    return not expect_red


def self_test_common(game_exefs=None, full_hosttest=True):
    """
    Негативные пробы гейтов. Возвращает True, если ВСЕ пробы сработали.

    Проверяется, что гейты умеют краснеть:
      1) архив с папкой romfs / с romfs.bin / без files / с неверным размером;
      2) гейт npdm краснеет на нетронутом main.npdm игры (нет вызовов 0x74/0x75);
      3) собственные пробы npdm_syscalls.py --self-test;
      4) гейт check_nso.py краснеет на подделке (не NSO);
      5) хост-тест краснеет на каждой из шести намеренных поломок логики
         и без модулей игры.
    """
    ok = True
    probe_reset()
    game_exefs = find_game_exefs(game_exefs)
    tid = npdm_program_id(game_exefs / "main.npdm") if game_exefs else "0100000000010000"
    slot = DEFAULT_SLOT

    say("  пробы гейта архива:")
    ok &= probe("архив с папкой romfs",
                _zip_probe(tid, slot, lambda e, x: e.update(
                    {f"atmosphere/contents/{tid}/romfs/a.dat": b"q"})))
    ok &= probe("архив с romfs.bin",
                _zip_probe(tid, slot, lambda e, x: e.update(
                    {f"atmosphere/contents/{tid}/romfs.bin": b"q"})))
    ok &= probe("архив без файлов мода",
                _zip_probe(tid, slot, lambda e, x: e.pop(f"atmosphere/contents/{tid}/files/a.dat")))
    ok &= probe("архив без main.npdm",
                _zip_probe(tid, slot, lambda e, x: e.pop(f"atmosphere/contents/{tid}/exefs/main.npdm")))
    ok &= probe("размер записи не совпал",
                _zip_probe(tid, slot, lambda e, x: x.update(
                    {f"atmosphere/contents/{tid}/files/a.dat": 999})))
    ok &= probe("архив с package3",
                _zip_probe(tid, slot, lambda e, x: e.update({"atmosphere/package3": b"q"})))
    # Архив для раздачи (--share): main.npdm из дампа игры внутри быть НЕ должен,
    # а скрипты правки прав и инструкция — должны.
    ok &= probe_green("честный архив для раздачи",
                      _zip_probe(tid, slot, lambda e, x: None, share=True, expect_red=False))
    ok &= probe("архив для раздачи с main.npdm из дампа",
                _zip_probe(tid, slot, lambda e, x: e.update(
                    {f"atmosphere/contents/{tid}/exefs/main.npdm": b"y"}), share=True))
    ok &= probe("архив для раздачи без скриптов правки прав",
                _zip_probe(tid, slot, lambda e, x: [
                    e.pop(f"{SHARE_SCRIPTS_DIR}/{NPDM_SDCARD.name}"),
                    e.pop(f"{SHARE_SCRIPTS_DIR}/{NPDM_SYSCALLS.name}")], share=True))
    # Библиотека sdk (nnSdk, файл Nintendo) в архиве для раздачи —
    # отказ всегда, даже если её «несёт мод» (mod_exefs не спасает).
    ok &= probe("архив для раздачи с exefs/sdk (файл Nintendo)",
                _zip_probe(tid, slot, lambda e, x: e.update(
                    {f"atmosphere/contents/{tid}/exefs/sdk": b"nnsdk"}), share=True))
    ok &= probe("архив для раздачи с exefs/sdk, названной файлом мода",
                _zip_probe(tid, slot, lambda e, x: e.update(
                    {f"atmosphere/contents/{tid}/exefs/sdk": b"nnsdk"}), share=True,
                    mod_exefs=("sdk",)))

    say("  пробы гейта npdm:")
    if game_exefs:
        rc, _ = npdm_check(game_exefs / "main.npdm", tid)
        ok &= probe("нетронутый main.npdm игры (нет вызовов 0x74/0x75)", rc != 0)
    else:
        ok &= probe_skip("гейт npdm", "нет распакованного exefs игры", EXEFS_HOW)
    # Собственные пробы npdm_syscalls.py: им нужен main.npdm любой игры, иначе
    # скрипт справедливо краснеет («нечем проверять»).
    if game_exefs:
        res = run_py([NPDM_SYSCALLS, game_exefs / "main.npdm", "--self-test"])
        for line in (res.stdout + res.stderr).strip().splitlines():
            say("      " + line.strip())
        ok &= probe_green("собственные пробы npdm_syscalls.py", res.returncode == 0)
    else:
        ok &= probe_skip("собственные пробы npdm_syscalls.py",
                         "нет main.npdm игры", EXEFS_HOW)

    say("  пробы гейта check_nso.py:")
    nso_for_gate = next((p for p in (ROOT / "out" / "module" / slot,
                                     ROOT / "out-diag" / "module" / slot) if p.is_file()), None)
    if nso_for_gate is None:
        nso_for_gate = next(iter(sorted((ROOT / "out").glob("module/subsdk*"))), None)
    if game_exefs:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            fake = Path(td) / "subsdk9"
            fake.write_bytes(b"NOTANSO" + b"\0" * 1000)
            rc, out = check_nso(fake, game_exefs, diag=False)
            ok &= probe("подделка вместо NSO", rc != 0)
            # Сорванная проверка обязана быть НАХОДКОЙ гейта, а не его аварией:
            # трассировка Python в выводе означает, что гейт упал сам.
            ok &= probe_green("подделка поймана находкой, а не трассировкой Python",
                              "Traceback" not in out)
            rc, out = check_nso(Path(td) / "нет-такого-файла", game_exefs, diag=False)
            ok &= probe("нет файла модуля", rc != 0)
            ok &= probe_green("отсутствие файла поймано находкой, а не трассировкой",
                              "Traceback" not in out)
            rc, _ = check_nso(fake, Path(td) / "нет-такой-папки", diag=False)
            ok &= probe("нет модулей игры", rc != 0)
    else:
        ok &= probe_skip("гейт check_nso.py", "нет распакованного exefs игры", EXEFS_HOW)

    ok &= self_test_check_nso_mutation(game_exefs, nso_for_gate)

    say("  пробы хост-теста:")
    # Хост-тест разбирает модули игры как LZ4 и модуль со сжатием ZBIC просто не
    # видит: на таком exefs ЧЕСТНЫЙ прогон краснел, и самопроверка объявляла
    # «гейт не умеет отказывать» — неправду о себе самой (замер на
    # SiN Reloaded). Поэтому для проб берётся тот же exefs, но с уже снятым
    # ZBIC — ровно как это делает сборка (make_test_exefs). Оригинал не меняется.
    hosttest_exefs, ht_tmp = game_exefs, None
    if game_exefs:
        scan_ht = zbic_module().scan(game_exefs)
        if scan_ht.get("ok") and scan_ht.get("zbic"):
            have_ht, _v = zbic_available()
            if have_ht:
                import tempfile
                ht_tmp = tempfile.TemporaryDirectory(prefix="nxr-ht-exefs-")
                dst = Path(ht_tmp.name)
                made = zbic_prepare_exefs(game_exefs, dst / "zbic", scan=scan_ht)
                ready = {n: p for n, (p, _i) in made.items()}
                for p in sorted(Path(game_exefs).iterdir()):
                    if p.is_file():
                        shutil.copy2(ready.get(p.name, p), dst / p.name)
                hosttest_exefs = dst
                say("      exefs для проб взят с уже снятым ZBIC (%s): иначе честный "
                    "прогон краснел бы из-за сжатия, а не из-за дефекта"
                    % ", ".join(sorted(ready)))
            else:
                say("      ВНИМАНИЕ: в exefs есть ZBIC, а пакета zbic нет — честный "
                    "прогон хост-теста покраснеет из-за сжатия, а не из-за дефекта "
                    "(pip install zbic)", level="warn")
    nso = next((p for p in (ROOT / "out" / "module" / slot,
                            ROOT / "out-diag" / "module" / slot) if p.is_file()), None)
    if not nso:
        nso = next(iter(sorted((ROOT / "out").glob("module/subsdk*"))
                        + sorted((ROOT / "out-diag").glob("module/subsdk*"))), None)
    if game_exefs and nso:
        rc, out = run_hosttest(hosttest_exefs, nso, None, "-selftest")
        ok &= probe_green("честный прогон хост-теста", rc == 0)
        if rc != 0:
            say(tail(out, 2000))
        breaks = HOSTTEST_BREAKS if full_hosttest else HOSTTEST_BREAKS[:2]
        for br in breaks:
            rc, _ = run_hosttest(hosttest_exefs, nso, br, "-selftest")
            ok &= probe("поломка логики: " + br, rc != 0)
        rc, _ = run_hosttest(hosttest_exefs, "/tmp/нет-модуля-nxr", None, "-selftest")
        ok &= probe("хост-тест без собранного модуля", rc != 0)
    else:
        ok &= probe_skip("хост-тест", _why_no_input(game_exefs, nso),
                         _how_no_input(game_exefs, nso))
    if ht_tmp is not None:
        ht_tmp.cleanup()

    ok &= self_test_zbic()
    ok &= self_test_compat(game_exefs, nso)
    ok &= self_test_install(game_exefs, nso)
    ok &= self_test_zbic_only(game_exefs, nso)
    ok &= self_test_gfx(game_exefs)
    ok &= self_test_fsgate()
    ok &= self_test_nvrm()
    ok &= self_test_autofix()
    ok &= self_test_events()
    ok &= self_test_mod(game_exefs)

    return bool(ok)


def self_test_mod(game_exefs=None):
    """
    Пробы переноса мода целиком. Мод собирается в памяти: свой модуль
    (любой NSO под именем subsdk5), свой main.npdm (копия npdm игры — БЕЗ наших
    прав), файл romfs, IPS под build id main игры, плюс то, что переносить нельзя
    (kip, homebrew). Проверяется:
      1) разбор: модуль, npdm, файл, патч с целью «main игры», пропуски названы;
      2) npdm мода без прав -> дописано (merged), гейт --check зелёный; npdm с
         правами -> как есть, байт в байт; npdm другой игры -> отказ;
      3) гейт переноса: честная раскладка зелёная; порча модуля мода — настоящий
         гейт КРАСНЫЙ, ослабленный (только «файл на месте») — зелёный; затвор нуля;
      4) слот: модуль мода в subsdk9 -> наш в старшем свободном ниже, с
         предупреждением о порядке; явный --slot в слот мода -> отказ;
      5) --mod-files на romfs внутри мода — громкое MOD_PARTS_LEFT, не молчание.
    """
    import tempfile

    ok = True
    game_exefs = find_game_exefs(game_exefs)
    say("  пробы переноса мода целиком (свой модуль, npdm, IPS, romfs):")
    some_nso = next(iter(sorted((ROOT / "out").glob("module*/subsdk*"))), None)
    if not game_exefs or some_nso is None:
        return probe_skip("перенос мода", _why_no_input(game_exefs, some_nso),
                          _how_no_input(game_exefs, some_nso))
    tid = npdm_program_id(game_exefs / "main.npdm")
    main_bid = _nso_build_id(game_exefs / "main")

    def caught(fn):
        try:
            fn()
        except SystemExit:
            return True
        return False

    with tempfile.TemporaryDirectory(prefix="nxr-mod-selftest-") as td:
        root = Path(td)
        mod = root / "mod"
        t = mod / "atmosphere" / "contents" / tid
        (t / "exefs").mkdir(parents=True)
        (t / "romfs" / "cfg").mkdir(parents=True)
        shutil.copy2(some_nso, t / "exefs" / "subsdk5")
        shutil.copy2(game_exefs / "main.npdm", t / "exefs" / "main.npdm")
        (t / "romfs" / "cfg" / "mod.ini").write_bytes(b"[mod]\r\nfov=90\r\n")
        ips = mod / "atmosphere" / "exefs_patches" / "TestPatch" / ((main_bid or "AB") + ".ips")
        ips.parent.mkdir(parents=True)
        ips.write_bytes(b"PATCH" + b"\x00\x10\x00\x00\x04" + b"\x1f\x20\x03\xd5" + b"EOF")
        (mod / "atmosphere" / "kips").mkdir()
        (mod / "atmosphere" / "kips" / "x.kip").write_bytes(b"KIP1")
        (mod / "switch").mkdir()
        (mod / "switch" / "x.nro").write_bytes(b"NRO0")

        # 1) разбор
        plan = analyze_mod(mod, tid, game_exefs)
        ok &= probe_green("разбор мода: модуль subsdk5, npdm, 1 файл romfs, IPS -> main игры, "
                          "kip и homebrew названы непереносимыми",
                          sorted(plan["modules"]) == ["subsdk5"] and plan["npdm"] is not None
                          and [r for r, _p in plan["files"]] == ["cfg/mod.ini"]
                          and len(plan["patches"]) == 1
                          and plan["patches"][0][2] == "main игры"
                          and len(plan["skipped"]) == 2)

        # 2) npdm
        out1 = root / "npdm1" / "main.npdm"
        mode1, _l = make_mod_npdm(plan["npdm"], game_exefs / "main.npdm", out1, tid)
        rc1, _o = npdm_check(out1, tid)
        out2 = root / "npdm2" / "main.npdm"
        mode2, _l = make_mod_npdm(out1, game_exefs / "main.npdm", out2, tid)
        ok &= probe_green("npdm мода без наших прав -> дописано (merged), гейт зелёный; "
                          "с правами -> как есть, байт в байт",
                          mode1 == "merged" and rc1 == 0 and mode2 == "as-is"
                          and out1.read_bytes() == out2.read_bytes())
        ok &= probe("npdm мода от другой игры (program id не тот) — отказ",
                    caught(lambda: make_mod_npdm(plan["npdm"], None, root / "npdm3" / "m",
                                                 "0100000000010000")))

        # 3) гейт переноса
        dest = root / "dest"
        base = dest / "atmosphere" / "contents" / tid
        for name, src in plan["exefs"].items():
            (base / "exefs").mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, base / "exefs" / name)
        for rel, src in plan["files"]:
            (base / "files" / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, base / "files" / rel)
        for rel, src, _t in plan["patches"]:
            d = dest / "atmosphere" / "exefs_patches" / rel
            d.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, d)
        good, _c = mod_transfer_problems(dest, tid, plan, "subsdk9")
        ok &= probe_green("гейт переноса: честная раскладка зелёная", not good)
        bad_mod = base / "exefs" / "subsdk5"
        raw = bytearray(bad_mod.read_bytes())
        raw[-1] ^= 0xFF
        bad_mod.write_bytes(bytes(raw))
        real, _c = mod_transfer_problems(dest, tid, plan, "subsdk9")
        weak, _c = mod_transfer_problems(dest, tid, plan, "subsdk9", weak=True)
        ok &= probe("порченый модуль мода: настоящий гейт переноса", bool(real))
        ok &= probe_green("ОСЛАБЛЕННЫЙ гейт (только «файл на месте») порчу пропускает — "
                          "значит, сверка содержимого не пустая", not weak)
        empty = dict(plan, exefs={}, files=[], patches=[], extra=[], modules={})
        zero, _c = mod_transfer_problems(dest, tid, empty, "subsdk9")
        ok &= probe("затвор нуля: гейт, не сверивший ни одного предмета", bool(zero))
        ok &= probe("наш модуль в слоте модуля мода — гейт переноса",
                    any("слот модуля мода" in x
                        for x in mod_transfer_problems(dest, tid, dict(plan), "subsdk5")[0]))

        # 4) слот
        plan9 = dict(plan, modules={"subsdk9": plan["modules"]["subsdk5"]})
        busy = occupied_slots(game_exefs)
        picked = pick_slot_for_mod(busy, plan9)
        order_ok, _why = mod_slot_order(picked or "", plan9)
        ok &= probe_green("модуль мода в subsdk9: наш — старший свободный ниже (%s), "
                          "о порядке инициализации предупреждено" % picked,
                          picked not in (None, "subsdk9") and picked not in busy
                          and not order_ok)
        ok &= probe_green("модуль мода в subsdk5: наш в subsdk9 старше — порядок хороший",
                          mod_slot_order("subsdk9", plan)[0])
        rc, out = _install_run(["--exefs", str(game_exefs), "--mod", str(mod),
                                "--dest", str(root / "d-slot"), "--slot", "subsdk5",
                                "--no-build", "--events", str(root / "slot.ndjson")])
        ev = (root / "slot.ndjson").read_text(encoding="utf-8") \
            if (root / "slot.ndjson").is_file() else ""
        ok &= probe("--slot в слот модуля мода (установщик)",
                    rc != 0 and "FAIL_MOD_SLOT_CONFLICT" in ev)

        # 5) romfs внутри мода через --mod-files: не молчим о брошенных частях
        rc, out = _install_run(["--exefs", str(game_exefs),
                                "--mod-files", str(t / "romfs"),
                                "--dest", str(root / "d-parts"), "--no-build",
                                "--events", str(root / "parts.ndjson")])
        ev = (root / "parts.ndjson").read_text(encoding="utf-8") \
            if (root / "parts.ndjson").is_file() else ""
        ok &= probe_green("--mod-files на romfs внутри мода: MOD_PARTS_LEFT назван "
                          "(модуль и патч не потеряны молча)",
                          "MOD_PARTS_LEFT" in ev and "subsdk5" in ev)
    return bool(ok)


def self_test_nvrm():
    """
    Пробы исправления NvRmMemQueryHandleParams.

      1) hosttest/run_nvrmtest.py --self-test: наша функция сверяется с НАСТОЯЩИМ
         кодом nnSdk 22.2.0 в эмуляторе ARM64 (7 входов, всё наблюдаемое), и
         каждая из 7 намеренных порч (включая затвор нуля) обязана покраснеть;
      2) check_compat_syms.py --self-test-nvrm: модуль с исправлением и без —
         каждый зелёный под своим ожиданием и красный под чужим (если оба
         собраны: out/module-*-nvrmfix* и out/module).
    """
    ok = True
    say("  пробы исправления NvRmMemQueryHandleParams:")
    if RUN_NVRMTEST.is_file():
        res = run_py([RUN_NVRMTEST, "--self-test"])
        for line in (res.stdout + res.stderr).strip().splitlines()[-3:]:
            say("      " + line.strip())
        if res.returncode == 2:
            ok &= probe_skip("сверка NvRm с nnSdk 22.2.0",
                             "нет эталона NintendoSdk_nnSdk-22_2_0-Release",
                             "задать NXR_REF_SDK=<путь к nnSdk 22.2.0>")
        else:
            ok &= probe_green("run_nvrmtest.py --self-test (сверка с 22.2.0 и 7 порч)",
                              res.returncode == 0)
    else:
        ok &= probe_skip("сверка NvRm", "нет " + str(RUN_NVRMTEST),
                         "вернуть на место hosttest/run_nvrmtest.py")
    yes = next(iter(sorted(list((ROOT / "out").glob("module*-autofix*/subsdk*"))
                           + list((ROOT / "out").glob("module*-nvrmfix*/subsdk*")),
                           key=lambda p: p.stat().st_mtime, reverse=True)), None)
    no = next(iter(sorted((ROOT / "out").glob("module/subsdk*"),
                          key=lambda p: p.stat().st_mtime, reverse=True)), None)
    if yes and no:
        res = run_py([CHECK_COMPAT_SYMS, "--self-test-nvrm", yes, no])
        ok &= probe_green("check_compat_syms.py --self-test-nvrm (%s / %s)"
                          % (yes.parent.name, no.parent.name), res.returncode == 0)
    else:
        ok &= probe_skip("гейт символа NvRm в обе стороны",
                         "не собраны оба модуля (с исправлением и без)",
                         "python build.py --game-exefs ... --no-zip  и  "
                         "python build.py --game-exefs ... --no-autofix --no-zip")
    return bool(ok)


def autofix_exefs_root():
    """Папка с распакованными exefs WD-patch, SIN-patch, TOTK-patch для сверки
    автоисправления на настоящих играх. Модули игр не распространяются: путь
    даёт человек (NXR_AUTOFIX_EXEFS_ROOT) или папка nxredirect/game-exefs-autofix."""
    env = os.environ.get("NXR_AUTOFIX_EXEFS_ROOT")
    for cand in ([Path(env)] if env else []) + [ROOT / "game-exefs-autofix"]:
        if all((cand / g / "main").is_file() for g in ("WD-patch", "SIN-patch", "TOTK-patch")):
            return cand
    return None


def self_test_autofix():
    """
    Пробы АВТОИСПРАВЛЕНИЯ.

      1) hosttest/run_autofixtest.py --self-test: чистая логика (правило,
         потокобезопасность, затвор паспорта, разбор байтов) и 8 намеренных порч;
      2) scripts/stubdiff.py --self-test: каталог, прогноз по файлам против
         таблицы эмуляции, детектор заглушек на настоящих sdk и его порчи;
      3) hosttest/run_autofix.py --self-test: ГОТОВЫЙ модуль в эмуляторе на
         настоящих nnSdk 19.3.13 / 20.5.17 / 22.2.0 с модулями настоящих игр;
         порча входа — настоящая проверка красная, ослабленная зелёная; затвор
         нуля — модуль без автоисправления красный.
    """
    ok = True
    say("  пробы автоисправления:")
    if RUN_AUTOFIXTEST.is_file():
        res = run_py([RUN_AUTOFIXTEST, "--self-test"])
        for line in (res.stdout + res.stderr).strip().splitlines()[-3:]:
            say("      " + line.strip())
        ok &= probe_green("run_autofixtest.py --self-test (логика и 8 порч)", res.returncode == 0)
    else:
        ok &= probe_skip("логика автоисправления", "нет " + str(RUN_AUTOFIXTEST),
                         "вернуть на место hosttest/run_autofixtest.py")
    root = autofix_exefs_root()
    how = ("задать NXR_AUTOFIX_EXEFS_ROOT=<папка, где лежат распакованные exefs "
           "WD-patch, SIN-patch, TOTK-patch>")
    if root is None:
        ok &= probe_skip("автоисправление на настоящих играх",
                         "нет распакованных exefs WD-patch/SIN-patch/TOTK-patch", how)
        return bool(ok)
    res = run_py([STUBDIFF, "--self-test", "--exefs-root", root])
    for line in (res.stdout + res.stderr).strip().splitlines()[-2:]:
        say("      " + line.strip())
    if res.returncode == 2:
        ok &= probe_skip("детектор заглушек", "нет библиотек NintendoSDK (задать NXR_SDK_DIR)", how)
    else:
        ok &= probe_green("stubdiff.py --self-test (прогноз = эмуляция, детектор, порчи)",
                          res.returncode == 0)

    def newest(paths):
        paths = [q for q in paths if q.is_file()]
        return max(paths, key=lambda q: q.stat().st_mtime) if paths else None
    yes = newest(list((ROOT / "out").glob("module-autofix/subsdk9")))
    no = newest(list((ROOT / "out").glob("module/subsdk9")))
    if not yes or not no:
        ok &= probe_skip("готовый модуль в эмуляторе",
                         "не собраны оба модуля subsdk9 (с автоисправлением и без)",
                         "python build.py --game-exefs ... --no-zip  и  "
                         "python build.py --game-exefs ... --no-autofix --no-zip")
        return bool(ok)
    res = run_py([RUN_AUTOFIX, yes, "--exefs-root", root, "--self-test", "--noautofix", no])
    for line in (res.stdout + res.stderr).strip().splitlines()[-3:]:
        say("      " + line.strip())
    if res.returncode == 2:
        ok &= probe_skip("готовый модуль в эмуляторе", "нечем проверять", how)
    else:
        ok &= probe_green("run_autofix.py --self-test (%s на 7 сценариях, порчи, затвор нуля)"
                          % yes.parent.name, res.returncode == 0)
    return bool(ok)


def self_test_fsgate():
    """
    Пробы ЗАТВОРА ФАЙЛОВОЙ СИСТЕМЫ (hosttest/run_fsgatetest.sh --all).

    Что доказывается:
      1) настоящая логика зелёная: до разрешающего момента модуль не делает с
         nn::fs ни одного обращения, после — работает как прежде, а ранние
         события не теряются;
      2) каждая намеренная поломка этой логики — КРАСНАЯ, включая «проверки не
         выполнялись вовсе» (затвор нуля).
    """
    ok = True
    say("  пробы затвора файловой системы (порядок обращений к nn::fs):")
    if RUN_FSGATETEST.is_file():
        res = run_wsl('bash "%s" --all' % wsl_path(RUN_FSGATETEST), timeout=900)
        for line in tail(res.stdout + res.stderr, 1500).splitlines()[-3:]:
            say("      " + line.strip())
        ok &= probe_green("hosttest/run_fsgatetest.sh --all (настоящая логика плюс "
                          "все намеренные поломки)", res.returncode == 0)
    else:
        ok &= probe_skip("хост-тест затвора ФС", "нет " + str(RUN_FSGATETEST),
                         "вернуть на место hosttest/run_fsgatetest.sh")
    return ok


def _check_nso_mutant(nso, game_exefs, edits, extra_args=()):
    """
    Прогон КОПИИ гейта check_nso.py с намеренно изменённым исходником.

    Возвращает (код возврата, вывод) или None, если мутация не применилась
    (текст в гейте изменился — проба обязана об этом сказать, а не промолчать).
    Оригинальный scripts/check_nso.py не трогается: правится только копия во
    временной папке.
    """
    import tempfile

    text = CHECK_NSO.read_text(encoding="utf-8")
    for old, new in edits:
        if old not in text:
            return None
        text = text.replace(old, new, 1)
    with tempfile.TemporaryDirectory(prefix="nxr-mutant-") as td:
        mut = Path(td) / "check_nso.py"
        mut.write_text(text, encoding="utf-8")
        # Рядом с мутантом кладём и распаковщик ZBIC: он ищется по соседству, а
        # без него мутант на игре со сжатием «проверял бы пустоту» не из-за
        # мутации, а из-за того, что нечем распаковать.
        if ZBIC_PURE.is_file():
            shutil.copy2(ZBIC_PURE, Path(td) / ZBIC_PURE.name)
        res = run_py([mut, nso, "--game-exefs", game_exefs] + list(extra_args))
        return res.returncode, res.stdout + res.stderr


# Мутация «выпотрошить содержательные списки»: гейт перебирает пустоту и по сути
# не проверяет ничего. Держим одним местом, чтобы обе пробы ниже били по одному и
# тому же изменению и отличались ТОЛЬКО наличием затвора нуля.
_GUT_LISTS = [
    ("REQUIRED_RUNTIME_SYMBOLS = [",
     "REQUIRED_RUNTIME_SYMBOLS = []\n_ОТКЛЮЧЁННЫЕ_СИМВОЛЫ = ["),
    ('RUNTIME_PATH_PARTS = ["nxrsd:/atmosphere/contents/", "/files/"]',
     'RUNTIME_PATH_PARTS = []'),
]
_DROP_GUARD = [("MIN_LISTED_CHECKS = 5", "MIN_LISTED_CHECKS = 0")]


def self_test_check_nso_mutation(game_exefs=None, nso=None):
    """
    Мутационная проверка гейта check_nso.py: краснеет ли НУЖНАЯ проверка.

    Приём (метод, §31): взять ЗАВЕДОМО ГОДНЫЙ вход и ослабить в гейте ровно одну
    проверку. Если ослабленный гейт остаётся зелёным, а настоящий на том же входе
    краснеет, значит краснеет именно она, а не что-то по соседству.

    Здесь ослабляется «затвор нуля» (MIN_LISTED_CHECKS): гейту выпотрошены списки
    обязательных символов и кусков пути мода — проверять становится нечего.
      * без затвора  — гейт зелёный, хотя не проверил ничего (так было раньше);
      * с затвором   — гейт красный на том же самом входе.
    """
    say("  мутационная проверка гейта check_nso.py (затвор нуля):")
    game_exefs = find_game_exefs(game_exefs)
    if not game_exefs or not nso or not Path(nso).is_file():
        return probe_skip("мутационная проверка check_nso.py",
                          _why_no_input(game_exefs, nso),
                          _how_no_input(game_exefs, nso))
    ok = True
    weakened = _check_nso_mutant(nso, game_exefs, _GUT_LISTS + _DROP_GUARD)
    real = _check_nso_mutant(nso, game_exefs, _GUT_LISTS)
    if weakened is None or real is None:
        return probe_skip("мутационная проверка check_nso.py",
                          "мутация не применилась: текст check_nso.py изменился",
                          "поправьте _GUT_LISTS/_DROP_GUARD в nxr_build.py под новый текст гейта")
    ok &= probe_green("ослабленный гейт (затвор снят) на выпотрошенных списках ЗЕЛЁНЫЙ "
                      "— значит проверять было нечего", weakened[0] == 0)
    ok &= probe("настоящий гейт на том же входе красный (сработал затвор нуля)",
                real[0] != 0 and "ничего (или почти ничего) не проверив" in real[1])
    return bool(ok)


# Мутация гейта ZBIC: «не выполнять ни одной проверки». На ЗАВЕДОМО ГОДНОМ входе
# находок нет ни там, ни там, и разница между ослабленным и настоящим гейтом —
# ровно затвор нуля.
_ZBIC_GUT = ("checks += 1", "checks += 0")
_ZBIC_DROP_GUARD = ("MIN_ZBIC_CHECKS = 13", "MIN_ZBIC_CHECKS = 0")


def _zbic_mutant(rebuilt, orig, edits):
    """
    Прогон КОПИИ гейта zbic_nso.py с изменённым исходником.

    Возвращает (код возврата, вывод) или None, если мутация не применилась —
    значит текст гейта изменился, и проба обязана об этом сказать, а не молчать.
    Оригинальный scripts/zbic_nso.py не трогается.
    """
    import tempfile

    text = ZBIC_NSO.read_text(encoding="utf-8")
    for old, new in edits:
        if old not in text:
            return None
        text = text.replace(old, new)
    with tempfile.TemporaryDirectory(prefix="nxr-zbic-mutant-") as td:
        mut = Path(td) / "zbic_nso.py"
        mut.write_text(text, encoding="utf-8")
        if ZBIC_PURE.is_file():
            shutil.copy2(ZBIC_PURE, Path(td) / ZBIC_PURE.name)
        res = run_py([mut, "--check", rebuilt, "--orig", orig])
        return res.returncode, res.stdout + res.stderr


def self_test_zbic():
    """
    Пробы всего, что связано со сжатием ZBIC.

    Что доказывается:
      1) собственные негативные пробы гейта zbic_nso.py (он краснеет на каждой
         подделке несжатого NSO и зелен на честном) — им хватает синтетического
         ZBIC-NSO, настоящая игра не нужна;
      2) «затвор нуля» гейта: ослабленный гейт (порог снят, проверки выпотрошены)
         на том же входе ЗЕЛЁНЫЙ, а настоящий — КРАСНЫЙ;
      3) распознавание ZBIC идёт по содержимому: файл с битом ZBIC находится под
         ЛЮБЫМ именем, а обычный exefs не объявляется сжатым;
      4) гейт архива знает про несжатые NSO: в личном архиве они обязаны быть, в
         архиве для раздачи их быть не должно (это файлы дампа игры).
    """
    import tempfile

    ok = True
    say("  пробы сжатия ZBIC:")
    if not ZBIC_NSO.is_file():
        return probe_skip("ZBIC", "нет " + str(ZBIC_NSO),
                          "вернуть на место scripts/zbic_nso.py")

    # --- 4. гейт архива (пакет zbic для этого не нужен)
    tid, slot = "0100000000010000", DEFAULT_SLOT
    ok &= probe_green("личный архив с несжатым main — зелёный",
                      _zip_probe(tid, slot, lambda e, x: None, zbic_names=("main",),
                                 expect_red=False))
    ok &= probe("личный архив без несжатого main (а он собран)",
                _zip_probe(tid, slot,
                           lambda e, x: e.pop(f"atmosphere/contents/{tid}/exefs/main"),
                           zbic_names=("main",)))
    ok &= probe("архив для раздачи с main из дампа игры",
                _zip_probe(tid, slot, lambda e, x: e.update(
                    {f"atmosphere/contents/{tid}/exefs/main": b"nso"}),
                    share=True, zbic_needed=True))
    ok &= probe("архив для раздачи без zbic_nso.py, хотя ZBIC в игре есть",
                _zip_probe(tid, slot, lambda e, x: e.pop(
                    f"{SHARE_SCRIPTS_DIR}/{ZBIC_NSO.name}"),
                    share=True, zbic_needed=True))
    ok &= probe("архив для раздачи без zbic_pure.py (распаковывать было бы нечем)",
                _zip_probe(tid, slot, lambda e, x: e.pop(
                    f"{SHARE_SCRIPTS_DIR}/{ZBIC_PURE.name}"),
                    share=True, zbic_needed=True))

    # --- 1..3 нужно, чем распаковывать; обычно это наш zbic_pure.py
    have, ver = zbic_available()
    if not have:
        ok &= probe_skip("гейт zbic_nso.py",
                         "распаковывать ZBIC нечем: нет ни %s, ни пакета zbic"
                         % ZBIC_PURE.name,
                         "вернуть " + ZBIC_PURE.name)
        return bool(ok)

    res = run_py([ZBIC_NSO, "--self-test"])
    for line in (res.stdout + res.stderr).strip().splitlines():
        say("      " + line.strip())
    ok &= probe_green("собственные пробы гейта zbic_nso.py (%s)" % ver,
                      res.returncode == 0)

    # Пробы ПРИ ОТСУТСТВИИ ПАКЕТА: самая суть этой работы — что без него
    # ничего не отказывает. Пакет закрываем подставным модулем в PYTHONPATH:
    # он бросает ImportError, и весь конвейер уходит на наш чистый Python.
    with tempfile.TemporaryDirectory(prefix="nxr-nopkg-") as nod:
        Path(nod, "zbic.py").write_text(
            'raise ImportError("пакет zbic закрыт пробой конвейера")\n',
            encoding="utf-8")
        env = dict(os.environ)
        env["PYTHONPATH"] = nod + os.pathsep + env.get("PYTHONPATH", "")
        env.pop("NXR_ZBIC_BACKEND", None)
        res_np = run_py([ZBIC_NSO, "--self-test"], env=env)
        ok &= probe_green("те же пробы, когда пакета zbic нет вовсе "
                          "(идут нашим чистым Python)", res_np.returncode == 0)
        if res_np.returncode != 0:
            say(tail(res_np.stdout + res_np.stderr, 2000))
        ok &= probe_green("в отчёте без пакета честно назван наш путь",
                          ZBIC_PURE.name in (res_np.stdout + res_np.stderr))

    # Мутация САМОГО распаковщика: если испортить разбор BIC, пробы обязаны
    # это поймать. Иначе «наш путь верен» было бы утверждением без проверки.
    with tempfile.TemporaryDirectory(prefix="nxr-purecut-") as mud:
        text = ZBIC_PURE.read_text(encoding="utf-8")
        old = "char_last = char_table % remaining + 1"
        if old not in text:
            ok &= probe("мутация распаковщика ZBIC", False)
        else:
            Path(mud, ZBIC_PURE.name).write_text(
                text.replace(old, "char_last = char_table % remaining + 2"),
                encoding="utf-8")
            shutil.copy2(ZBIC_NSO, Path(mud, ZBIC_NSO.name))
            env = dict(os.environ)
            env["NXR_ZBIC_BACKEND"] = "pure"
            res_mu = run_py([Path(mud, ZBIC_NSO.name), "--self-test"], env=env)
            ok &= probe("испорченный разбор BIC в %s пойман пробами "
                        "(код %d)" % (ZBIC_PURE.name, res_mu.returncode),
                        res_mu.returncode != 0)

    mod = zbic_module()
    with tempfile.TemporaryDirectory(prefix="nxr-zbic-selftest-") as td:
        td = Path(td)
        exefs = td / "exefs"
        exefs.mkdir()
        # Имя нарочно НЕ «main»: распознавание обязано смотреть в файл, а не на имя.
        src = mod.make_fixture(exefs / "subsdk7")
        (exefs / "readme.txt").write_bytes("это не NSO".encode("utf-8"))
        rebuilt = td / "subsdk7-uncompressed"
        mod.rebuild(src, rebuilt)

        found = mod.scan(exefs)
        ok &= probe_green("ZBIC найден по содержимому под чужим именем (%s)"
                          % ", ".join(found["zbic_files"]),
                          found["zbic_files"] == ["subsdk7"])
        plain = mod.scan(ROOT / "scripts")   # заведомо не exefs: ни одного NSO
        ok &= probe_green("на папке без NSO распознавание молчит",
                          plain["ok"] and not plain["zbic"])

        weakened = _zbic_mutant(rebuilt, src, [_ZBIC_GUT, _ZBIC_DROP_GUARD])
        real = _zbic_mutant(rebuilt, src, [_ZBIC_GUT])
        if weakened is None or real is None:
            ok &= probe_skip("мутационная проверка гейта zbic_nso.py",
                             "мутация не применилась: текст гейта изменился",
                             "поправьте _ZBIC_GUT/_ZBIC_DROP_GUARD в nxr_build.py")
        else:
            ok &= probe_green("ослабленный гейт ZBIC (затвор снят) на выпотрошенных "
                              "проверках ЗЕЛЁНЫЙ — значит проверять было нечего",
                              weakened[0] == 0)
            ok &= probe("настоящий гейт ZBIC на том же входе красный "
                        "(сработал затвор нуля)",
                        real[0] == 2 and "ничего (или почти ничего) не проверил" in real[1])
    return bool(ok)


def _kit_dir(root, name, npdm=None, module=None, slot=None, extra=None):
    """Игрушечная раскладка exefs для проб гейта комплекта."""
    d = Path(root) / name
    d.mkdir(parents=True, exist_ok=True)
    if npdm:
        shutil.copy2(npdm, d / "main.npdm")
    if module and slot:
        shutil.copy2(module, d / slot)
    for nm, src in (extra or {}).items():
        shutil.copy2(src, d / nm)
    return d


def self_test_compat(game_exefs=None, nso=None):
    """
    Пробы всего, что связано с подменённой библиотекой sdk.

    Что доказывается:
      1) имена символов в конвейере и в hosttest/check_compat_syms.py — одни и те
         же (разъедься они, гейт проверял бы не то, что собирает сборка);
      2) гейт символов краснеет на ЧУЖОМ ожидании: обычный модуль под ожиданием
         hash/ldn/both обязан быть красным, под «none» — зелёным. Это и есть
         доказательство, что гейт вообще что-то ловит;
      3) гейт комплекта краснеет на файлах разных игр (program id в main.npdm не
         совпал с TID раскладки) и разных версий (несжатый main не сходится с
         исходным файлом), а ослабленный вариант на том же входе — зелёный;
      4) затвор нуля гейта комплекта: проверять нечего — красный;
      5) распознавание по --sdk отвечает «проверить нечем», когда библиотека не
         указана, и находит недостающие символы, когда указана.
    """
    import tempfile

    ok = True
    say("  пробы совместимости с подменённой библиотекой sdk:")

    # --- 1. имена символов не разъехались
    if CHECK_COMPAT_SYMS.is_file():
        text = CHECK_COMPAT_SYMS.read_text(encoding="utf-8")
        ok &= probe_green("имена символов в конвейере и в check_compat_syms.py совпадают",
                          all(('"%s"' % sym) in text for sym in COMPAT_SYMS.values()))
    else:
        ok &= probe_skip("гейт символов", "нет " + str(CHECK_COMPAT_SYMS),
                         "вернуть на место hosttest/check_compat_syms.py")
        return bool(ok)

    # --- 2. гейт символов на настоящем модуле
    game_exefs = find_game_exefs(game_exefs)
    if nso is None:
        nso = next(iter(sorted((ROOT / "out").glob("module*/subsdk*"))
                        + sorted((ROOT / "out-diag").glob("module*/subsdk*"))), None)
    if not nso or not Path(nso).is_file():
        ok &= probe_skip("гейт символов совместимости",
                         "модуль не собран (нет out/module/subsdkN)", MODULE_HOW)
        ok &= probe_skip("гейт комплекта", "модуль не собран", MODULE_HOW)
    else:
        nso = Path(nso)
        # Чей это модуль на самом деле, узнаём у самого гейта: в out/ может лежать
        # и сборка без автоисправления, и с ним. Состояний два:
        # «none» (--no-autofix) и «autofix» (все экспорты каталога).
        mine = {"none": (), "autofix": ("hash", "ldn")}
        real = next((k for k in mine if check_compat_syms(nso, mine[k])[0] == 0), None)
        ok &= probe_green("гейт символов опознал собранный модуль (%s: %s)"
                          % (nso.name, real), real is not None)
        if real is not None:
            for other, keys in mine.items():
                if other == real:
                    continue
                rc, _out = check_compat_syms(nso, keys)
                ok &= probe("гейт символов под чужим ожиданием «%s» краснеет" % other,
                            rc != 0)

        # --- 3..4. гейт комплекта
        if not game_exefs:
            ok &= probe_skip("гейт комплекта", "нет распакованного exefs игры", EXEFS_HOW)
        else:
            tid = npdm_program_id(Path(game_exefs) / "main.npdm")
            slot = nso.name
            compat = {"none": (), "autofix": ("hash", "ldn")}.get(real or "none", ())
            with tempfile.TemporaryDirectory(prefix="nxr-kit-selftest-") as td:
                td = Path(td)
                good = _kit_dir(td, "good", npdm=Path(game_exefs) / "main.npdm",
                                module=nso, slot=slot)
                p, c = kit_problems(good, tid, source_exefs=game_exefs, slot=slot,
                                    compat=compat)
                ok &= probe_green("честная раскладка: гейт комплекта зелёный "
                                  "(проверок %d)" % c, not p and c >= MIN_KIT_CHECKS)
                if p:
                    for line in p:
                        say("        " + line, level="warn")
                # Файлы РАЗНЫХ игр: тот же вход, но TID раскладки чужой.
                other_tid = ("0" if tid[0] != "0" else "1") + tid[1:]
                ok &= probe_green("ослабленный гейт комплекта (только наличие файлов) "
                                  "на чужом TitleID ЗЕЛЁНЫЙ — значит он не сверяет",
                                  not kit_problems(good, other_tid,
                                                   source_exefs=game_exefs, slot=slot,
                                                   compat=compat, weak=True)[0])
                ok &= probe("настоящий гейт комплекта на том же входе красный "
                            "(program id в main.npdm не от этой игры)",
                            bool(kit_problems(good, other_tid,
                                              source_exefs=game_exefs, slot=slot,
                                              compat=compat)[0]))
                # Символы: тот же комплект, но ожидание чужое.
                if real is not None:
                    wrong = ("hash", "ldn") if real != "autofix" else ()
                    ok &= probe("гейт комплекта ловит чужие символы в модуле",
                                bool(kit_problems(good, tid, source_exefs=game_exefs,
                                                  slot=slot, compat=wrong)[0]))
                # Разные ВЕРСИИ: несжатый NSO, не сходящийся с исходным файлом.
                have, _ver = zbic_available()
                if have:
                    mod = zbic_module()
                    src_dir = td / "src-exefs"
                    src_dir.mkdir()
                    shutil.copy2(Path(game_exefs) / "main.npdm", src_dir / "main.npdm")
                    fixture = mod.make_fixture(src_dir / "main")
                    rebuilt = td / "main-rebuilt"
                    mod.rebuild(fixture, rebuilt)
                    kit_ok = _kit_dir(td, "kit-zbic",
                                      npdm=Path(game_exefs) / "main.npdm",
                                      extra={"main": rebuilt})
                    ok &= probe_green("несжатый main от ТОГО ЖЕ источника — зелёный",
                                      not kit_problems(kit_ok, tid,
                                                       source_exefs=src_dir,
                                                       zbic_names=("main",))[0])
                    # «Другая версия игры» — это другой ModuleId при том же имени
                    # файла. Вторая фикстура тем же make_fixture не годится: у неё
                    # те же сегменты и тот же ModuleId, и гейт справедливо молчал
                    # бы (проба на консоли показала именно это). Поэтому ModuleId
                    # копии меняется прямо в заголовке — ровно то, чем отличаются
                    # сборки разных версий.
                    other_src = td / "other-exefs"
                    other_src.mkdir()
                    shutil.copy2(Path(game_exefs) / "main.npdm", other_src / "main.npdm")
                    raw = bytearray(Path(fixture).read_bytes())
                    raw[0x40] ^= 0xFF          # первый байт ModuleId (BuildId)
                    (other_src / "main").write_bytes(bytes(raw))
                    ok &= probe_green("ослабленный гейт на main от ДРУГОГО источника "
                                      "ЗЕЛЁНЫЙ — значит версию он не сверяет",
                                      not kit_problems(kit_ok, tid,
                                                       source_exefs=other_src,
                                                       zbic_names=("main",),
                                                       weak=True)[0])
                    ok &= probe("настоящий гейт на том же входе красный "
                                "(main не от этой версии игры)",
                                bool(kit_problems(kit_ok, tid, source_exefs=other_src,
                                                  zbic_names=("main",))[0]))
                else:
                    ok &= probe_skip("версия несжатого main в комплекте",
                                     "нет пакета zbic", "pip install zbic")
                # Затвор нуля: проверять нечего.
                empty = _kit_dir(td, "empty")
                _p, c0 = kit_problems(empty, tid, npdm_expected=False)
                ok &= probe("затвор нуля гейта комплекта: проверок %d — меньше порога %d"
                            % (c0, MIN_KIT_CHECKS), c0 < MIN_KIT_CHECKS)

    # --- 5. распознавание по --sdk
    say("  пробы распознавания «нужен ли доэкспорт» (--sdk):")
    if not game_exefs:
        ok &= probe_skip("распознавание по --sdk", "нет распакованного exefs игры",
                         EXEFS_HOW)
        return bool(ok)
    res_none = missing_symbols(game_exefs, [])
    ok &= probe_green("без --sdk честный ответ «проверить нечем», а не «всё хорошо»",
                      not res_none["ok"] and res_none["missing"] == [])
    # Библиотека самой игры: с НЕЙ у игры по определению хватает символов —
    # на этом и проверяется, что счётчик недостающего не выдумывает находок.
    own_sdk = Path(game_exefs) / "sdk"
    if own_sdk.is_file():
        res_own = missing_symbols(game_exefs, [own_sdk])
        ok &= probe_green("с РОДНОЙ библиотекой игры обязательных недостающих нет "
                          "(WEAK-символов, которые процесс не роняют: %d)"
                          % len(res_own.get("missing_weak") or []),
                          res_own.get("ok") and not res_own.get("missing"))
        # Негативная проба того же счёта: библиотеку подменяем на заведомо чужой
        # NSO (модуль игры или наш модуль) — недостающее ОБЯЗАНО появиться.
        alien = next((p for p in (Path(game_exefs) / "rtld",
                                  Path(game_exefs) / "subsdk0") if p.is_file()), None)
        if alien:
            res_alien = missing_symbols(game_exefs, [alien])
            ok &= probe("с подменённой библиотекой недостающие символы находятся "
                        "(%d шт.)" % len(res_alien.get("missing") or []),
                        res_alien.get("ok") and bool(res_alien.get("missing")))
            # И называет своим ключом РОВНО то из недостающего, что мы умеем
            # доэкспортировать: иначе человек увидел бы список имён, но не узнал
            # бы, каким ключом это лечится, — или получил бы совет включить ключ,
            # который его игре не поможет. Проверка не зависит от того, какая это
            # игра: сравниваются два множества, посчитанные по одному и тому же
            # списку недостающего.
            got = set(res_alien.get("compat_missing") or [])
            want = {k for k, s in COMPAT_SYMS.items()
                    if s in set(res_alien.get("missing") or [])}
            ok &= probe_green("ключи доэкспорта названы ровно для тех символов, "
                              "которых не хватает (%s)"
                              % (", ".join(sorted(got)) or "ни одного — у этой игры "
                                 "недостаёт другого"), got == want)
        else:
            ok &= probe_skip("подменённая библиотека", "в exefs игры нет rtld/subsdk0",
                             EXEFS_HOW)
    else:
        ok &= probe_skip("сверка с родной библиотекой", "в exefs игры нет sdk", EXEFS_HOW)
    return bool(ok)


CHECK_EVENTS = SCRIPTS / "check_events.py"


def self_test_events():
    """
    Пробы машинного протокола событий (--events).

    Что доказывается:
      1) реестр кодов и описание событий не разъехались;
      2) проверка событий сама умеет краснеть (её --self-test с отдельным кодом 3);
      3) на настоящем прогоне конвейера (install.py --status, ничего не пишет)
         каждая строка — валидный JSON с непустым кодом из реестра;
      4) ключ --events НЕ меняет человеческий вывод: stdout с ним и без него
         совпадает БАЙТ В БАЙТ (главный инвариант обратной совместимости);
      5) сломанное событие и событие без кода роняют проверку;
      6) «затвор нуля»: проверено 0 событий — красный (свой код возврата 2).
    """
    import tempfile

    ok = True
    say("  пробы машинного протокола событий:")
    if not CHECK_EVENTS.is_file():
        return probe_skip("машинный протокол событий",
                          "нет " + str(CHECK_EVENTS),
                          "вернуть на место scripts/check_events.py")

    res = run_py([CHECK_EVENTS, "--check-docs"])
    ok &= probe_green("реестр кодов совпадает с docs/события.md", res.returncode == 0)
    if res.returncode != 0:
        say(tail(res.stdout + res.stderr, 2000))

    res = run_py([CHECK_EVENTS, "--self-test"])
    ok &= probe_green("проверка событий умеет краснеть (её собственные пробы)",
                      res.returncode == 0)
    if res.returncode != 0:
        say(tail(res.stdout + res.stderr, 3000))

    with tempfile.TemporaryDirectory(prefix="nxr-events-selftest-") as td:
        td = Path(td)
        ndjson = td / "events.ndjson"
        dest = td / "dest"
        tid = "0100000000010000"
        # install.py --status на пустом назначении: ничего не пишет, WSL не нужен.
        args = [ROOT / "install.py", "--dest", dest, "--tid", tid, "--status"]
        plain = run_py(args)
        withev = run_py(args + ["--events", ndjson])
        ok &= probe_green("настоящий прогон с --events закончился так же (код возврата)",
                          plain.returncode == 0 and withev.returncode == 0)
        ok &= probe_green("ключ --events не изменил человеческий вывод (байт в байт)",
                          plain.stdout == withev.stdout and plain.stderr == withev.stderr)
        lines = ndjson.read_text(encoding="utf-8").splitlines() if ndjson.is_file() else []
        ok &= probe_green(f"события записаны ({len(lines)} строк)", len(lines) > 0)

        res = run_py([CHECK_EVENTS, ndjson])
        ok &= probe_green("каждая строка — JSON с непустым кодом из реестра",
                          res.returncode == 0)
        if res.returncode != 0:
            say(tail(res.stdout + res.stderr, 3000))

        # --- негативные пробы: порча настоящего файла событий
        def broken(name, mutate, expect_rc=1):
            p = td / name
            p.write_text(mutate("\n".join(lines) + "\n"), encoding="utf-8", newline="\n")
            r = run_py([CHECK_EVENTS, p])
            return probe("%s (код возврата %d)" % (name, expect_rc),
                         r.returncode == expect_rc)

        ok &= broken("событие без поля code",
                     lambda s: s.replace('"code"', '"кодНеТот"', 1))
        ok &= broken("событие с пустым code",
                     lambda s: json.dumps(
                         {**json.loads(s.splitlines()[0]), "code": ""},
                         ensure_ascii=False) + "\n" + "\n".join(s.splitlines()[1:]) + "\n")
        ok &= broken("сломанный JSON в строке", lambda s: s + '{"ts": "нет закрывающей\n')
        ok &= broken("код вне реестра",
                     lambda s: s + json.dumps(
                         {"schema": EVENTS_SCHEMA, "ts": "2026-01-01T00:00:00.000+00:00",
                          "level": "info", "code": "ВЫДУМАННЫЙ_КОД", "msg": "x",
                          "data": {}}, ensure_ascii=False) + "\n")
        ok &= broken("файл без событий — затвор нуля", lambda s: "", expect_rc=2)

        # Пойманный SystemExit не должен глушить канал: пробы гейтов ловят выход
        # fail() внутри того же процесса, и однажды закрытый в fail() канал молча
        # терял ВСЁ, что шло после первой пробы (дефект, найденный этой пробой).
        after = td / "after-caught-fail.ndjson"
        snippet = os.linesep.join([
            "import sys; sys.path.insert(0, %r)" % str(ROOT),
            "import nxr_build as n",
            "n.events_open(%r)" % str(after),
            "n.emit('RUN_START', 'до пробы', tool='проба')",
            "try:",
            "    n.fail('нарочно', code='FAIL_GENERIC')",
            "except SystemExit:",
            "    pass",
            "n.emit('DONE', 'после пробы', rc=0)",
            "",
        ])
        sp = td / "caught_fail.py"
        sp.write_text(snippet, encoding="utf-8")
        run_py([sp])
        got = []
        if after.is_file():
            got = [json.loads(l)["code"] for l in
                   after.read_text(encoding="utf-8").splitlines() if l.strip()]
        ok &= probe_green("после пойманного отказа канал событий жив "
                          "(коды: %s)" % ", ".join(got),
                          got == ["RUN_START", "FAIL_GENERIC", "DONE"])

    return bool(ok)


def _install_slots(dest, tid):
    """Какие subsdkN реально лежат в назначении (без .bak/.removed)."""
    exefs = Path(dest) / "atmosphere" / "contents" / tid / "exefs"
    if not exefs.is_dir():
        return []
    return sorted(p.name for p in exefs.iterdir()
                  if p.name.startswith("subsdk") and "." not in p.name)


def self_test_install(game_exefs=None, nso=None):
    """
    Пробы установщика: он НЕ ДОЛЖЕН ставить второй модуль, когда квитанция и карта
    расходятся. Оба сценария — из разбора: наш модуль переложен в другой слот вручную и
    квитанция с чужим (устаревшим) слотом. Раньше в обоих появлялось ДВА модуля.
    """
    import json
    import tempfile

    ok = True
    game_exefs = find_game_exefs(game_exefs)
    if nso is None:
        nso = next(iter(sorted((ROOT / "out").glob("module/subsdk*"))
                        + sorted((ROOT / "out-diag").glob("module/subsdk*"))), None)
    say("  пробы установщика (квитанция против карты):")
    if not game_exefs or not nso:
        return probe_skip("установщик", _why_no_input(game_exefs, nso),
                          _how_no_input(game_exefs, nso))
    tid = npdm_program_id(game_exefs / "main.npdm")
    # Слот НЕ угадывается по имени файла модуля: его выбирает сам установщик
    # (старший свободный), и имя готового NSO в out/module может быть любым.
    # Раньше проба брала Path(nso).name и падала не по делу, стоило в out/module
    # появиться модулю под другой слот (так уже было).
    slot = None

    def install(dest, *args):
        res = run_py([ROOT / "install.py", "--exefs", game_exefs, "--dest", dest,
                      "--mod-files", str(Path(dest).parent / "mod"), "--module", nso,
                      "--no-build"] + autofix_args_for(nso) + list(args))
        return res.returncode

    with tempfile.TemporaryDirectory(prefix="nxr-inst-selftest-") as td:
        root = Path(td)
        (root / "mod").mkdir()
        (root / "mod" / "a.dat").write_bytes(b"x")

        # Контроль: честный цикл «установка + обновление» даёт РОВНО один модуль.
        d0 = root / "d0"
        rc1 = install(d0, "--apply")
        rc2 = install(d0, "--apply")
        # Какой слот выбрал установщик — узнаём из квитанции, а не из имени файла.
        slots0 = _install_slots(d0, tid)
        slot = slots0[0] if slots0 else None
        ok &= probe_green("честная повторная установка: один модуль, тот же слот",
                          rc1 == 0 and rc2 == 0 and len(slots0) == 1)
        if slot is None:
            return probe_skip("установщик", "первая установка не создала модуль в exefs",
                              "проверьте python install.py --dest <...> --apply вручную")

        # (а) модуль переложен вручную в другой слот, квитанция прежняя.
        d1 = root / "d1"
        install(d1, "--apply")
        exefs1 = d1 / "atmosphere" / "contents" / tid / "exefs"
        moved = "subsdk7" if slot != "subsdk7" else "subsdk6"
        (exefs1 / slot).rename(exefs1 / moved)
        rc = install(d1, "--apply")
        ok &= probe("наш модуль переложен в другой слот (квитанция расходится)",
                    rc != 0 and _install_slots(d1, tid) == [moved])

        # (б) устаревшая/подделанная квитанция: слот, в котором ничего нет.
        d2 = root / "d2"
        install(d2, "--apply")
        rp = d2 / "atmosphere" / "contents" / tid / RECEIPT_NAME
        data = json.loads(rp.read_text(encoding="utf-8"))
        data["slot"] = "subsdk3"
        rp.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
        rc = install(d2, "--apply")
        ok &= probe("квитанция указывает пустой слот, модуль лежит в другом",
                    rc != 0 and _install_slots(d2, tid) == [slot])

        # Чужой файл в exefs назначения обязан пережить нашу установку.
        # В exefs могут лежать файлы других модов и прежних установок: мы
        # ДОБАВЛЯЕМ своё, а не «раскладываем папку».
        d4 = root / "d4"
        ex4 = d4 / "atmosphere" / "contents" / tid / "exefs"
        ex4.mkdir(parents=True)
        alien = ex4 / "чужой-файл.bin"
        alien_bytes = b"\xAB" * 4096
        alien.write_bytes(alien_bytes)
        before4 = snapshot_dir(ex4)
        rc = install(d4, "--apply")
        after4 = snapshot_dir(ex4)
        ok &= probe_green("чужой файл в exefs пережил установку байт в байт",
                          rc == 0 and alien.is_file()
                          and alien.read_bytes() == alien_bytes)
        ok &= probe_green("гейт сохранности чужих файлов на честной установке зелёный",
                          not foreign_untouched(before4, after4, {slot, "main.npdm"}))
        # Негативная проба того же гейта: тот же вход, но чужой файл подменён.
        # Ослабленная проверка (смотрит только наличие файла) остаётся ЗЕЛЁНОЙ,
        # настоящая — краснеет; значит краснеет именно сравнение содержимого.
        touched = dict(after4)
        touched[alien.name] = {"size": 4096, "sha256": "0" * 64}
        ok &= probe_green("ослабленный гейт (только наличие файла) на подменённом "
                          "чужом файле ЗЕЛЁНЫЙ — значит он ничего не проверяет",
                          not foreign_untouched(before4, touched, {slot, "main.npdm"},
                                                weak=True))
        ok &= probe("настоящий гейт на том же входе красный "
                    "(поймал подмену чужого файла)",
                    bool(foreign_untouched(before4, touched, {slot, "main.npdm"})))
        gone = {n: v for n, v in after4.items() if n != alien.name}
        ok &= probe("настоящий гейт ловит исчезновение чужого файла",
                    bool(foreign_untouched(before4, gone, {slot, "main.npdm"})))

        # --uninstall не должен оставлять ПРАВЛЕНЫЙ main.npdm.
        d3 = root / "d3"
        install(d3, "--apply")
        res = run_py([ROOT / "install.py", "--dest", d3, "--tid", tid,
                      "--uninstall", "--apply"])
        exefs3 = d3 / "atmosphere" / "contents" / tid / "exefs"
        npdm_left = (exefs3 / "main.npdm").is_file()
        ok &= probe_green("--uninstall убрал модуль и правленый main.npdm",
                          res.returncode == 0 and not _install_slots(d3, tid)
                          and not npdm_left)

        # Снятие удаляет только файл с тем же sha256, что в квитанции.
        # Изменённый после установки main.npdm обязан остаться (иначе снятие
        # стёрло бы чужую правку).
        d6 = root / "d6"
        install(d6, "--apply")
        exefs6 = d6 / "atmosphere" / "contents" / tid / "exefs"
        edited = b"npdm, edited after install"
        (exefs6 / "main.npdm").write_bytes(edited)
        res6 = run_py([ROOT / "install.py", "--dest", d6, "--tid", tid,
                       "--uninstall", "--apply"])
        ok &= probe_green("--uninstall не удалил изменённый после установки main.npdm, "
                          "но удалил модуль",
                          res6.returncode == 0 and not _install_slots(d6, tid)
                          and (exefs6 / "main.npdm").is_file()
                          and (exefs6 / "main.npdm").read_bytes() == edited
                          and "изменён после установки" in (res6.stdout or ""))
    return bool(ok)


def _zbic_only_exefs(game_exefs, dst):
    """
    exefs для проб режима «только ZBIC»: настоящие main.npdm/rtld/sdk игры и
    СИНТЕТИЧЕСКИЙ main со сжатием ZBIC.

    Зачем подделка main. Пробы не должны зависеть от того, попалась ли автору
    игра со сжатием: иначе на любой другой машине этот блок всегда «пропущен», и
    о режиме не доказано ничего. Фикстуру делает тот же пакет `zbic`, что и в
    работе, а TitleID и права берутся из настоящего main.npdm — значит раскладка
    и квитанция проверяются на том же TID, что у живой игры.
    """
    dst = Path(dst)
    dst.mkdir(parents=True, exist_ok=True)
    for name in ("main.npdm", "rtld", "sdk"):
        shutil.copy2(Path(game_exefs) / name, dst / name)
    zbic_module().make_fixture(dst / "main")
    return dst


def autofix_args_for(nso):
    """
    Ключ для install.py под ГОТОВЫЙ модуль (--module): если в его .dynsym нет
    экспортов автоисправления (сборка --no-autofix или прежняя обычная), установщик
    обязан ждать именно такой модуль — иначе его гейт символов справедливо
    покраснеет (по умолчанию ждём автоисправление). Решается по
    содержимому файла, а не по имени папки.
    """
    rc, _out = check_compat_syms(nso, ())
    return ["--no-autofix"] if rc == 0 else []


def _install_run(argv):
    """Прогон install.py: (код возврата, весь вывод). Вывод нужен пробам дословно."""
    res = run_py([ROOT / "install.py"] + list(argv))
    return res.returncode, (res.stdout or "") + (res.stderr or "")


# Строка, которой установщик пугает человека, когда заменяет НЕ свой файл.
# Проба ищет её дословно: она и есть предмет проверки.
FOREIGN_WARN = "лежал ДО нас"


def self_test_zbic_only(game_exefs=None, nso=None):
    """
    Пробы штатного режима «только снять ZBIC» (install.py --zbic-only).

    Что доказывается:
      1) в раскладку легло РОВНО обещанное: несжатые NSO и ничего больше —
         ни subsdkN, ни main.npdm, ни files/;
      2) квитанция отмечает снятые файлы как НАШИ;
      3) чужие файлы в exefs назначения целы байт в байт;
      4) файл, который положили МЫ, не объявляется чужим при следующей записи
         (нет ложного «заменяем не наше»), а настоящий чужой — объявляется;
      5) порядок «снять ZBIC → поставить мод» проходит целиком и сливает квитанцию;
      6) обратный порядок «мод → снять ZBIC» не теряет модуль из квитанции;
      7) --uninstall в обоих порядках снимает только названное в квитанции и
         ничего не удаляет.

    У каждой новой проверки есть негативная проба: ослабленный вариант на том же
    входе зелёный, настоящий — красный. Отдельно проверяется затвор нуля: гейт,
    которому нечего проверять, обязан краснеть.
    """
    import json
    import tempfile

    ok = True
    say("  пробы режима «только снять ZBIC» (--zbic-only):")
    game_exefs = find_game_exefs(game_exefs)
    have, _ver = zbic_available()
    if not game_exefs or not have:
        why = []
        how = []
        if not game_exefs:
            why.append("нет распакованного exefs игры")
            how.append(EXEFS_HOW)
        if not have:
            why.append("нет пакета zbic (%s, %s)" % (ZBIC_PKG["author"],
                                                     ZBIC_PKG["license"]))
            how.append("pip install zbic")
        return probe_skip("режим «только ZBIC»", "; ".join(why), "; ".join(how))
    if nso is None:
        nso = next(iter(sorted((ROOT / "out").glob("module/subsdk*"))
                        + sorted((ROOT / "out-diag").glob("module/subsdk*"))), None)
    tid = npdm_program_id(Path(game_exefs) / "main.npdm")

    with tempfile.TemporaryDirectory(prefix="nxr-zbiconly-selftest-") as td:
        root = Path(td)
        src = _zbic_only_exefs(game_exefs, root / "exefs")
        (root / "mod").mkdir()
        (root / "mod" / "a.dat").write_bytes(b"x")

        # --- 1..3. честный прогон: раскладка, квитанция, чужой файл
        d1 = root / "d1"
        ex1 = d1 / "atmosphere" / "contents" / tid / "exefs"
        ex1.mkdir(parents=True)
        alien = ex1 / "чужой-файл.bin"
        alien_bytes = b"\xCD" * 4096
        alien.write_bytes(alien_bytes)
        before1 = snapshot_dir(ex1)
        rc, out1 = _install_run(["--exefs", src, "--dest", d1, "--zbic-only", "--apply"])
        rec1 = json.loads((d1 / "atmosphere" / "contents" / tid / RECEIPT_NAME)
                          .read_text(encoding="utf-8")) if rc == 0 else {}
        expected = ["main"]
        allowed = list(before1)
        ok &= probe_green("режим «только ZBIC» прошёл и разложил ровно обещанное",
                          rc == 0 and not zbic_only_layout(d1, tid, expected, allowed))
        if rc != 0:
            say(tail(out1, 2000))
        ok &= probe_green("квитанция отмечает снятый main как наш",
                          not receipt_lists_zbic(rec1, expected))
        ok &= probe_green("модуля мода и main.npdm в раскладке НЕТ",
                          not (ex1 / "main.npdm").exists()
                          and not any(p.name.startswith("subsdk") for p in ex1.iterdir())
                          and not (d1 / "atmosphere" / "contents" / tid / "files").exists())
        ok &= probe_green("чужой файл пережил снятие ZBIC байт в байт",
                          alien.is_file() and alien.read_bytes() == alien_bytes
                          and not foreign_untouched(before1, snapshot_dir(ex1),
                                                    set(expected)))

        # Негативные пробы гейта раскладки: тот же вход, но в папке лишний файл.
        (ex1 / "subsdk9").write_bytes("лишний модуль".encode("utf-8"))
        ok &= probe_green("ослабленный гейт раскладки (только наличие обещанного) на "
                          "лишнем модуле ЗЕЛЁНЫЙ — значит он ничего не проверяет",
                          not zbic_only_layout(d1, tid, expected, allowed, weak=True))
        ok &= probe("настоящий гейт раскладки на том же входе красный "
                    "(поймал лишний файл в exefs)",
                    bool(zbic_only_layout(d1, tid, expected, allowed)))
        ok &= probe("затвор нуля гейта раскладки: проверять нечего — красный",
                    bool(zbic_only_layout(d1, tid, [], allowed)))

        # Негативные пробы гейта квитанции: список снятых файлов вычеркнут.
        rec_bad = dict(rec1, zbic_files=[])
        ok &= probe_green("ослабленный гейт квитанции (есть ли она вообще) на "
                          "квитанции без списка снятых файлов ЗЕЛЁНЫЙ",
                          not receipt_lists_zbic(rec_bad, expected, weak=True))
        ok &= probe("настоящий гейт квитанции на том же входе красный "
                    "(снятый файл не отмечен нашим)",
                    bool(receipt_lists_zbic(rec_bad, expected)))
        ok &= probe("затвор нуля гейта квитанции: список снятых пуст — красный",
                    bool(receipt_lists_zbic(rec1, [])))

        # --- 4. опознание «наше/чужое» в exefs назначения.
        #
        # Проверяется тем же кодом и той же строкой, которой установщик пугает
        # человека («лежал ДО нас»), но БЕЗ модуля: повторный прогон --zbic-only
        # заменяет ровно тот файл, который мы клали сами. Если квитанция делает
        # своё дело, предупреждения нет; чужой файл на том же месте обязан его
        # вызвать — иначе «предупреждения нет» ничего не значило бы.
        d6 = root / "d6"
        _install_run(["--exefs", src, "--dest", d6, "--zbic-only", "--apply"])
        rc6, out6 = _install_run(["--exefs", src, "--dest", d6, "--zbic-only", "--apply"])
        ex6 = d6 / "atmosphere" / "contents" / tid / "exefs"
        # *Прежде проба ждала и копии .bak- (повторный прогон перезаписывал
        # тот же файл).* С учётом своего байт-в-байт тот же файл не
        # перезаписывается, как и при установке мода: копии нет, а файл записан в
        # квитанции нашим (owned).
        rec6 = json.loads((ex6.parent / RECEIPT_NAME).read_text(encoding="utf-8"))
        ok &= probe_green("повторное снятие ZBIC: НАШ же main не назван чужим",
                          rc6 == 0 and FOREIGN_WARN not in out6
                          and (ex6 / "main").is_file()
                          and any(k.endswith("/exefs/main")
                                  for k in (rec6.get("owned") or {})))
        d7 = root / "d7"
        ex7 = d7 / "atmosphere" / "contents" / tid / "exefs"
        ex7.mkdir(parents=True)
        (ex7 / "main").write_bytes("чужой main, мы его не клали".encode("utf-8"))
        rc7, out7 = _install_run(["--exefs", src, "--dest", d7, "--zbic-only", "--apply"])
        ok &= probe("чужой main (не из квитанции) назван чужим и отложен в .bak",
                    rc7 == 0 and FOREIGN_WARN in out7
                    and any(".bak-" in p.name for p in ex7.iterdir()))

        # --- 5. «сняли ZBIC → ставим мод»: ложного предупреждения быть не должно
        if not nso:
            ok &= probe_skip("порядки «ZBIC → мод» и «мод → ZBIC»",
                             "модуль не собран (нет out/module/subsdkN)", MODULE_HOW)
            return bool(ok)

        def install_mod(dest):
            return _install_run(["--exefs", game_exefs, "--dest", dest,
                                 "--mod-files", root / "mod", "--module", nso,
                                 "--no-build", "--apply"] + autofix_args_for(nso))

        d2 = root / "d2"
        rc_z, _ = _install_run(["--exefs", src, "--dest", d2, "--zbic-only", "--apply"])
        rc_m, out_m = install_mod(d2)
        base2 = d2 / "atmosphere" / "contents" / tid
        rec2 = json.loads((base2 / RECEIPT_NAME).read_text(encoding="utf-8"))
        ok &= probe_green("порядок «ZBIC → мод»: мод встал и НЕ назвал наш main чужим",
                          rc_z == 0 and rc_m == 0 and FOREIGN_WARN not in out_m)
        ok &= probe_green("квитанция после мода помнит и снятый main, и слот модуля",
                          bool(rec2.get("slot")) and "main" in (rec2.get("zbic_files") or [])
                          and bool(rec2.get("module_sha256"))
                          and bool(rec2.get("npdm_sha256")))

        # --- 6. обратный порядок «мод → ZBIC»
        d4 = root / "d4"
        rc_m4, _ = install_mod(d4)
        rc_z4, out_z4 = _install_run(["--exefs", src, "--dest", d4, "--zbic-only",
                                      "--apply"])
        base4 = d4 / "atmosphere" / "contents" / tid
        rec4 = json.loads((base4 / RECEIPT_NAME).read_text(encoding="utf-8"))
        slot4 = rec4.get("slot")
        ok &= probe_green("порядок «мод → ZBIC»: снятие прошло и не тронуло мод",
                          rc_m4 == 0 and rc_z4 == 0 and FOREIGN_WARN not in out_z4
                          and bool(slot4) and (base4 / "exefs" / slot4).is_file()
                          and (base4 / "exefs" / "main.npdm").is_file())
        ok &= probe_green("квитанция после снятия ZBIC не потеряла модуль мода",
                          not receipt_lists_zbic(rec4, ["main"])
                          and rec4.get("module_sha256") == rec2.get("module_sha256"))

        # --- 7. снятие в обоих порядках: только названное в квитанции
        for dest, label in ((d2, "ZBIC → мод"), (d4, "мод → ZBIC")):
            base = dest / "atmosphere" / "contents" / tid
            rc_u, _ = _install_run(["--dest", dest, "--tid", tid, "--uninstall",
                                    "--apply"])
            # *Прежде проба ждала отложенных .removed- («ничего не удалено»).*
            # Теперь снятие удаляет наше по квитанции: ни
            # своих файлов, ни .removed- после него нет; пустая exefs/ убрана.
            left = (sorted(p.name for p in (base / "exefs").iterdir() if p.is_file())
                    if (base / "exefs").is_dir() else [])
            ok &= probe_green("--uninstall после «%s»: удалены модуль, main.npdm и "
                              "несжатый main; ничего не отложено как .removed-" % label,
                              rc_u == 0
                              and not [n for n in left
                                       if n in ("main", "main.npdm")
                                       or (n.startswith("subsdk") and "." not in n)]
                              and not any(".removed-" in n for n in left))

        # Снятие установки, где мода не было вовсе: чужой main.npdm не трогается.
        d5 = root / "d5"
        ex5 = d5 / "atmosphere" / "contents" / tid / "exefs"
        ex5.mkdir(parents=True)
        npdm_alien = "не наш main.npdm".encode("utf-8")
        (ex5 / "main.npdm").write_bytes(npdm_alien)
        _install_run(["--exefs", src, "--dest", d5, "--zbic-only", "--apply"])
        rc_u5, _ = _install_run(["--dest", d5, "--tid", tid, "--uninstall", "--apply"])
        ok &= probe_green("--uninstall после «только ZBIC» не тронул чужой main.npdm",
                          rc_u5 == 0 and (ex5 / "main.npdm").read_bytes() == npdm_alien
                          and not (ex5 / "main").exists())
    return bool(ok)


# Мутация гейта check_nso.py по части диагностики графики: выпотрошить списки
# примет. Тогда «примет нет» и «приметы есть» перестают отличаться — гейт зелен на
# любом модуле. Держим одним местом, чтобы обе пробы ниже били по одному изменению
# и отличались ТОЛЬКО наличием затвора нуля.
_GFX_GUT = [
    ('GFX_REQUIRED_STRINGS = ["nxrsd:/switch/ModKitNX/logs/{TID}/gfx-"]',
     'GFX_REQUIRED_STRINGS = []'),
    ("GFX_TARGET_SYMBOLS = [",
     "GFX_TARGET_SYMBOLS = []\n_ОТКЛЮЧЁННЫЕ_ЦЕЛИ_ГРАФИКИ = ["),
    # С --diag-gfx гейт требует и функции папок отчётов и часы
    # nn::time (src/nxr_logs.hpp) — это тоже приметы отчёта, выпотрошить их же,
    # иначе ослабленный гейт краснел бы на них, а не был бы «пустым».
    ("LOGS_RUNTIME_SYMBOLS = [",
     "LOGS_RUNTIME_SYMBOLS = []\n_ОТКЛЮЧЁННЫЕ_ФУНКЦИИ_ПАПОК = ["),
    ("LOGS_OPTIONAL_SYMBOLS = [",
     "LOGS_OPTIONAL_SYMBOLS = []\n_ОТКЛЮЧЁННЫЕ_ЧАСЫ = ["),
]
_GFX_DROP_GUARD = [("MIN_GFX_CHECKS = 9", "MIN_GFX_CHECKS = 0")]

# То же самое для примет УЗКОЙ пробы блокировок (проверка 10 гейта): выпотрошить
# список и снять затвор. Нужна отдельно, потому что именно эта проверка отличает
# сборку с перехватом функций синхронизации от сборки без него, а такой перехват
# однажды сломал запуск игры.
_LOCK_GUT = [("LOCK_REQUIRED_STRINGS = [",
              "LOCK_REQUIRED_STRINGS = []\n_ОТКЛЮЧЁННЫЕ_ПРИМЕТЫ_БЛОКИРОВОК = [")]
_LOCK_DROP_GUARD = [("MIN_LOCK_CHECKS = 2", "MIN_LOCK_CHECKS = 0")]


def is_diag_build(nso):
    """Модуль из out-diag/ — собран с -DNXR_DIAG (гейт check_nso — с --diag)."""
    return Path(nso).parent.parent.name == "out-diag"


def gfx_modules():
    """
    Собранные модули: (обычный без gfx, с gfx). Любой может быть None.

    Берётся САМЫЙ СВЕЖИЙ по времени файла, а не первый по алфавиту. Причина:
    в out/ накапливаются модули прежних сборок (другие слоты, другие ключи), и
    выбор по алфавиту приносил модуль позапрошлой версии. Тогда пробы краснели
    не из-за дефекта, а из-за устаревшего артефакта — однажды на этом
    потерян час: проверки не видели нового символа nn::fs::SetAllocator.
    """
    def newest(paths):
        paths = [p for p in paths if p.is_file()]
        return max(paths, key=lambda p: p.stat().st_mtime) if paths else None

    plain = newest([p for p in list((ROOT / "out").glob("module/subsdk*"))
                    + list((ROOT / "out").glob("module-compat-*/subsdk*"))
                    + list((ROOT / "out").glob("module-autofix/subsdk*"))
                    if "-gfxdiag" not in p.parent.name])
    gfx = newest(list((ROOT / "out").glob("module*-gfxdiag/subsdk*"))
                 + list((ROOT / "out-diag").glob("module*-gfxdiag/subsdk*")))
    return plain, gfx


def self_test_gfx(game_exefs=None):
    """
    Пробы диагностики графики (--diag-gfx). Каждая — с негативной стороной.

    Что доказывается:
      1) хост-тест самой диагностики (hosttest/run_gfxtest.sh --all) зелёный, и
         каждая из его намеренных поломок логики — красная (включая «проверки не
         выполнялись вовсе», то есть затвор нуля);
      2) гейт целей перехвата (hosttest/gfx_targets.py) умеет краснеть и на своих
         пробах, и на входе без модулей;
      3) приметы, по которым гейт check_nso.py узнаёт графическую сборку, ЕСТЬ в
         таблице целей заголовка src/nxr_gfx.hpp — списки не разошлись;
      4) сравнение сборок (hosttest/gfx_unchanged.py) умеет краснеть;
      5) рабочая и графическая сборки НЕ ПУТАЮТСЯ: каждая красная под чужим гейтом;
      6) затвор нуля проверки 9 в check_nso.py: с выпотрошенными списками примет
         ослабленный гейт зелёный (проверять нечего), настоящий — красный.
    """
    import tempfile

    ok = True
    game_exefs = find_game_exefs(game_exefs)
    plain, gfx = gfx_modules()

    say("  пробы диагностики графики (хост-тест логики):")
    if RUN_GFXTEST.is_file():
        res = run_wsl('bash "%s" --all' % wsl_path(RUN_GFXTEST), timeout=900)
        for line in tail(res.stdout + res.stderr, 1500).splitlines()[-4:]:
            say("      " + line.strip())
        ok &= probe_green("hosttest/run_gfxtest.sh --all (настоящая логика плюс "
                          "все намеренные поломки)", res.returncode == 0)
    else:
        ok &= probe_skip("хост-тест диагностики графики", "нет " + str(RUN_GFXTEST),
                         "вернуть на место hosttest/run_gfxtest.sh")

    say("  пробы гейта целей перехвата:")
    if GFX_TARGETS.is_file():
        res = run_py([GFX_TARGETS, "--self-test"])
        for line in (res.stdout + res.stderr).strip().splitlines():
            say("      " + line.strip())
        ok &= probe_green("собственные пробы gfx_targets.py", res.returncode == 0)
        with tempfile.TemporaryDirectory(prefix="nxr-gfx-empty-") as td:
            res = run_py([GFX_TARGETS, td])
            ok &= probe("gfx_targets.py на папке без модулей игры", res.returncode != 0)
        # Списки не должны разъехаться: приметы графической сборки, по которым
        # краснеет check_nso.py, обязаны быть целями перехвата из заголовка.
        try:
            mod = load_script(GFX_TARGETS, "nxr_gfx_targets")
            table = (set(mod.targets_from_header())
                     | set(mod.lock_targets_from_header())
                     | {"eglGetError"})
            gate = load_script(CHECK_NSO, "nxr_check_nso_for_gfx")
            absent = [s for s in gate.GFX_TARGET_SYMBOLS if s not in table]
            ok &= probe_green("приметы check_nso.py есть в таблице целей nxr_gfx.hpp"
                              + (" (нет: " + ", ".join(absent) + ")" if absent else ""),
                              not absent)
            # То же самое для УЗКОЙ пробы блокировок: её примета в гейте обязана
            # совпадать с единственной целью в заголовке. Разойдись они — гейт
            # перестал бы отличать сборку с перехватом синхронизации от сборки
            # без него, а именно такой перехват и сломал запуск игры.
            lock_syms = set(mod.lock_targets_from_header())
            lock_absent = [s for s in lock_syms
                           if s not in gate.LOCK_REQUIRED_STRINGS]
            ok &= probe_green("цель пробы блокировок из nxr_gfx.hpp есть в приметах "
                              "check_nso.py"
                              + (" (нет: " + ", ".join(lock_absent) + ")"
                                 if lock_absent else ""),
                              bool(lock_syms) and not lock_absent)
        except Exception as exc:
            ok &= probe_skip("сверка примет со списком целей заголовка",
                             "не удалось прочитать списки (%s)" % exc,
                             "проверьте hosttest/gfx_targets.py и scripts/check_nso.py")
    else:
        ok &= probe_skip("гейт целей перехвата", "нет " + str(GFX_TARGETS),
                         "вернуть на место hosttest/gfx_targets.py")

    say("  пробы сравнения сборок:")
    if GFX_UNCHANGED.is_file():
        res = run_py([GFX_UNCHANGED, "--self-test"])
        ok &= probe_green("собственные пробы gfx_unchanged.py", res.returncode == 0)
        if res.returncode != 0:
            say(tail(res.stdout + res.stderr, 2000))
        if plain and gfx:
            res = run_py([GFX_UNCHANGED, plain, gfx])
            ok &= probe("рабочая и графическая сборки различаются по содержимому",
                        res.returncode != 0)
        else:
            ok &= probe_skip("сравнение рабочей и графической сборок",
                             "собраны не обе (нужны out/module*/subsdkN и "
                             "out/module*-gfxdiag/subsdkN)",
                             "python build.py --game-exefs ... --no-zip  и  "
                             "python build.py --game-exefs ... --diag-gfx --no-zip")
    else:
        ok &= probe_skip("сравнение сборок", "нет " + str(GFX_UNCHANGED),
                         "вернуть на место hosttest/gfx_unchanged.py")

    say("  пробы «рабочая и графическая сборки не перепутаны»:")
    if not game_exefs:
        ok &= probe_skip("сборки не перепутаны", "нет распакованного exefs игры",
                         EXEFS_HOW)
    else:
        if gfx:
            # Самым свежим графическим может оказаться ОТЛАДОЧНЫЙ
            # (out-diag/…-gfxdiag, -DNXR_DIAG): его гейт проверяет с --diag, иначе
            # проба краснела бы на строке отчёта, а не на приметах графики.
            gfx_diag = is_diag_build(gfx)
            rc, _ = check_nso(gfx, game_exefs, diag=gfx_diag, diag_gfx=False,
                          nvrm=is_autofix_dir(gfx))
            ok &= probe("графический модуль под гейтом рабочей сборки", rc != 0)
            rc, out = check_nso(gfx, game_exefs, diag=gfx_diag, diag_gfx=True,
                            nvrm=is_autofix_dir(gfx))
            ok &= probe_green("графический модуль под гейтом --diag-gfx", rc == 0)
            if rc != 0:
                say(tail(out, 2000))
        else:
            ok &= probe_skip("графический модуль под гейтами",
                             "графический модуль не собран (нет out/module*-gfxdiag)",
                             "python build.py --game-exefs ... --diag-gfx --no-zip")
        if plain:
            rc, _ = check_nso(plain, game_exefs, diag=False, diag_gfx=True,
                          nvrm=is_autofix_dir(plain))
            ok &= probe("рабочий модуль под гейтом --diag-gfx", rc != 0)
        else:
            ok &= probe_skip("рабочий модуль под гейтом --diag-gfx",
                             "рабочий модуль не собран", MODULE_HOW)

    say("  мутационная проверка гейта check_nso.py (затвор нуля примет графики):")
    if not game_exefs or not plain:
        ok &= probe_skip("мутационная проверка примет графики",
                         _why_no_input(game_exefs, plain),
                         _how_no_input(game_exefs, plain))
        return bool(ok)
    # Вход заведомо годный (рабочий модуль), ключ --diag-gfx требует примет, которых
    # там нет. Ослаблять будем ровно одно: затвор нуля.
    plain_args = ["--diag-gfx"] + (["--compat-nvrm"] if is_autofix_dir(plain) else [])
    weakened = _check_nso_mutant(plain, game_exefs, _GFX_GUT + _GFX_DROP_GUARD,
                                 extra_args=plain_args)
    real = _check_nso_mutant(plain, game_exefs, _GFX_GUT, extra_args=plain_args)
    if weakened is None or real is None:
        return bool(probe_skip("мутационная проверка примет графики",
                               "мутация не применилась: текст check_nso.py изменился",
                               "поправьте _GFX_GUT/_GFX_DROP_GUARD в nxr_build.py "
                               "под новый текст гейта") and ok)
    ok &= probe_green("ослабленный гейт (затвор снят) на выпотрошенных списках примет "
                      "ЗЕЛЁНЫЙ — значит проверять было нечего", weakened[0] == 0)
    ok &= probe("настоящий гейт на том же входе красный (сработал затвор нуля)",
                real[0] != 0 and "приметы диагностики графики не проверялись" in real[1])

    # То же для примет пробы блокировок (проверка 10 гейта). Вход здесь ДРУГОЙ —
    # графический модуль: приметы графики у него есть (проверка 9 зелёная), а
    # примет блокировок нет, потому что --diag-lock по умолчанию выключен. Ровно
    # на таком входе и видно, работает ли затвор нуля проверки 10.
    if not gfx:
        return bool(probe_skip("мутационная проверка примет блокировок",
                               "графический модуль не собран",
                               "python build.py --game-exefs ... --diag-gfx --no-zip")
                    and ok)
    # Модуль с исправлением NvRm (папка …-nvrmfix…) гейт проверяет с --compat-nvrm,
    # иначе мутант краснел бы не из-за затвора, а из-за имён NvRm.
    lock_args = (["--diag-gfx"] + (["--compat-nvrm"] if is_autofix_dir(gfx) else [])
                 + (["--diag"] if is_diag_build(gfx) else []))
    weak_lock = _check_nso_mutant(gfx, game_exefs, _LOCK_GUT + _LOCK_DROP_GUARD,
                                  extra_args=lock_args)
    real_lock = _check_nso_mutant(gfx, game_exefs, _LOCK_GUT,
                                  extra_args=lock_args)
    if weak_lock is None or real_lock is None:
        return bool(probe_skip("мутационная проверка примет блокировок",
                               "мутация не применилась: текст check_nso.py изменился",
                               "поправьте _LOCK_GUT/_LOCK_DROP_GUARD в nxr_build.py "
                               "под новый текст гейта") and ok)
    ok &= probe_green("ослабленный гейт (затвор снят) на пустом списке примет "
                      "блокировок ЗЕЛЁНЫЙ — значит проверять было нечего",
                      weak_lock[0] == 0)
    ok &= probe("настоящий гейт на том же входе красный (сработал затвор нуля)",
                real_lock[0] != 0
                and "приметы пробы блокировок не проверялись" in real_lock[1])
    return bool(ok)


def self_test_not_mixed(game_exefs=None):
    """
    Главная проба диагностической сборки: обычный и диагностический модули
    НЕ перепутаны. check_nso.py обязан отвергнуть диагностический модуль без
    --diag (в образе есть строка пути отчёта) и обычный — с --diag.
    """
    ok = True
    game_exefs = find_game_exefs(game_exefs)
    if not game_exefs:
        say("  пробы «сборки не перепутаны»:")
        return probe_skip("сборки не перепутаны", "нет распакованного exefs игры",
                          EXEFS_HOW)
    # САМЫЙ СВЕЖИЙ по времени файла, а не первый по алфавиту: однажды в
    # out-diag/module лежал устаревший subsdk1 от 27.09 (общее имя "sd", без
    # SetAllocator) — проба краснела из-за артефакта, а не из-за дефекта. Тот же
    # урок, что у gfx_modules().
    def _newest(paths):
        paths = [p for p in paths if p.is_file()]
        return max(paths, key=lambda p: p.stat().st_mtime) if paths else None
    plain = _newest((ROOT / "out").glob("module/subsdk*"))
    diag = _newest((ROOT / "out-diag").glob("module/subsdk*"))
    say("  пробы «сборки не перепутаны»:")
    if diag:
        rc, _ = check_nso(diag, game_exefs, diag=False)
        ok &= probe("диагностический модуль под обычным гейтом", rc != 0)
        rc, out = check_nso(diag, game_exefs, diag=True)
        ok &= probe_green("диагностический модуль под гейтом --diag", rc == 0)
        if rc != 0:
            say(tail(out, 2000))
    else:
        ok &= probe_skip("диагностический модуль под гейтами",
                         "диагностический модуль не собран (нет out-diag/module/subsdkN)",
                         "python build-diag.py --game-exefs ... --no-zip")
    if plain:
        rc, _ = check_nso(plain, game_exefs, diag=True)
        ok &= probe("обычный модуль под гейтом --diag", rc != 0)
    else:
        ok &= probe_skip("обычный модуль под гейтом --diag",
                         "обычный модуль не собран (нет out/module/subsdkN)", MODULE_HOW)
    return bool(ok)


def add_common_args(ap):
    ap.add_argument("--game-exefs", help="папка с распакованным exefs игры "
                                         "(main, subsdk0.., sdk, rtld, main.npdm)")
    ap.add_argument("--game", default=None, help="короткое имя игры для имени архива")
    ap.add_argument("--mod-files", help="папка с файлами мода (ляжет в contents/<TID>/files)")
    ap.add_argument("--mod",
                    help="МОД ЦЕЛИКОМ, как его раздал автор (папка с atmosphere/contents/"
                         "<TID>/…): romfs -> files/, свой модуль и IPS — как есть, свой "
                         "main.npdm — за основу. Вместо --mod-files")
    ap.add_argument("--slot", default=None, help="слот модуля (по умолчанию subsdk9; "
                                                 "если он занят модулем мода — старший свободный)")
    ap.add_argument("--skip-build", action="store_true", help="не пересобирать модуль в WSL")
    ap.add_argument("--no-zip", action="store_true", help="только раскладка, без архива")
    ap.add_argument("--share", action="store_true",
                    help="архив ДЛЯ РАЗДАЧИ: без main.npdm (он из дампа игры), "
                         "но со скриптами правки прав и инструкцией")
    ap.add_argument("--zbic-fix", action="store_true",
                    help="снять сжатие ZBIC: собрать несжатые NSO из exefs игры "
                         "и положить их в contents/<TID>/exefs (зависимостей не "
                         "нужно: распаковщик на чистом Python лежит в scripts/). "
                         "Распознавание ZBIC идёт всегда, ключ включает только починку")
    add_compat_args(ap)
    ap.add_argument("--compat-nvrm", action="store_true",
                    help="УСТАРЕЛ (04.10.2026): исправление NvRmMemQueryHandleParams входит "
                         "в автоисправление каждой сборки и включается модулем само, только "
                         "на консоли с подменённой sdk и без Vulkan в процессе")
    ap.add_argument("--cnmt", metavar="ФАЙЛ",
                    help="CNMT игры (.cnmt или .cnmt.xml): конвейер покажет "
                         "required_system_version — какую прошивку игра требует")
    add_diag_gfx_arg(ap)
    ap.add_argument("--self-test", action="store_true", help="негативные пробы гейтов")
    ap.add_argument("--quick-self-test", action="store_true",
                    help="то же, но только две поломки хост-теста (быстрее)")
    add_events_arg(ap)
    return ap


def add_compat_args(ap):
    """
    Ключи совместимости — общие для сборки и установки, чтобы имена не разошлись.

    Имена выбраны по стилю соседних (--zbic-fix, --zbic-only): предмет и то, что
    с ним делают. --swapped-sdk называет не действие, а СОСТОЯНИЕ КОНСОЛИ, для
    которой собирается комплект: человек выбирает одну вещь («у меня подменён
    sdk»), а не собирает её из трёх ключей.
    """
    ap.add_argument("--compat-hash", action="store_true",
                    help="УСТАРЕЛ (04.10.2026): std::__1::__hash_memory (%s) входит в "
                         "автоисправление каждой сборки; на родной sdk вызов уходит в её "
                         "функцию" % COMPAT_SYMS["hash"])
    ap.add_argument("--compat-ldn", action="store_true",
                    help="УСТАРЕЛ (04.10.2026): заглушка nn::ldn::SetProtocol (%s) входит в "
                         "автоисправление и включается, только если в sdk этого символа "
                         "НЕТ; иначе работает настоящий LDN" % COMPAT_SYMS["ldn"])
    ap.add_argument("--no-autofix", action="store_true",
                    help="собрать модуль БЕЗ автоисправления (%s): ни одного экспорта, "
                         "ни паспорта консоли — как до 04.10.2026 без ключей. Для отката "
                         "и сравнения" % NO_AUTOFIX_CFLAG)
    ap.add_argument("--swapped-sdk", action="store_true",
                    help="ПОЛНЫЙ КОМПЛЕКТ для консоли, где библиотека sdk подменена: "
                         "несжатый main (если ZBIC в игре есть), правленый main.npdm "
                         "и модуль — одной раскладкой. Модуль тот же, что в обычной "
                         "сборке: исправления он включает на консоли сам")
    ap.add_argument("--sdk", action="append", metavar="ПУТЬ",
                    help="библиотека вашей консоли (файл или папка): конвейер сверит "
                         "импорты модулей игры с её экспортами и скажет, нужен ли "
                         "вариант --swapped-sdk. Можно повторять")
    return ap


def add_diag_gfx_arg(ap):
    """
    Ключ диагностики графики — общей функцией, как add_compat_args().

    Имя выбрано по стилю соседних (--zbic-fix, --zbic-only, --compat-hash): сперва
    семейство, потом уточнение. Семейство здесь — диагностика (её сборка и так
    зовётся build-diag.py, а флаг компиляции -DNXR_DIAG_GFX), уточнение — графика.
    Отсюда --diag-gfx, а не --gfx-diag: иначе в конвейере появилось бы второе,
    ни с чем не связанное семейство «gfx-».

    Ключ доступен И build.py, И build-diag.py: диагностика графики не зависит от
    -DNXR_DIAG, и отчёт с живой консоли снят именно ОБЫЧНОЙ сборкой с
    этим ключом (плюс --compat-hash --compat-ldn).

    *Отладочная сборка доступна во всех версиях: у
    install.py есть свои --diag и --diag-gfx: только для готового отладочного
    модуля (out-diag/ или --module), гейт check_nso идёт с теми же ключами. Без них
    рассуждение ниже по-прежнему верно — обычная установка отладочный модуль не примет.*
    Было: в install.py ключа НЕТ намеренно: install.py ставит на карту РАБОЧИЙ мод, а
    проба графики ставится архивом и снимается. Перепутать нельзя не по договорённости,
    а по устройству: install.py берёт готовый модуль из out/module[-compat-...], то
    есть из папки БЕЗ -gfxdiag, а если графический модуль подсунуть туда руками,
    гейт check_nso.py (без --diag-gfx) покраснеет на приметах диагностики.
    """
    ap.add_argument("--diag-gfx", action="store_true",
                    help="собрать с диагностикой ГРАФИКИ (%s): модуль перехватит "
                         "nn::vi/EGL/NVN и напишет отчёт sd:/%s (папка игры, последние 5). "
                         "Это проба, а не рабочий мод: кладётся в отдельную папку "
                         "сборки и в архив с пометкой GFXDIAG"
                         % (GFX_CFLAG, GFX_REPORT_NAME))
    ap.add_argument("--diag-lock", action="store_true",
                    help="ДОБАВИТЬ к --diag-gfx узкую пробу блокировок (%s): один "
                         "перехватчик pthread_rwlock_rdlock, работающий ТОЛЬКО "
                         "внутри eglCreateWindowSurface и только в том же потоке. "
                         "По умолчанию ВЫКЛЮЧЕНА: прежняя, широкая проба (пять "
                         "целей) 28.09.2026 сломала запуск игры на консоли"
                         % LOCK_CFLAG)
    return ap


def compat_from_args(args):
    """
    Набор совместимости из аргументов. Автоисправление по
    умолчанию; --no-autofix выключает; --compat-* устарели (предупреждение).
    --no-autofix вместе с --swapped-sdk или --compat-* — противоречие: отказ.
    """
    old = [k for k in ("hash", "ldn", "nvrm") if getattr(args, "compat_" + k, False)]
    if old:
        say("      ВНИМАНИЕ: " + ", ".join("--compat-" + k for k in old)
            + " устарел(и): автоисправление включено в каждой сборке и само решает на "
              "консоли, своя функция или настоящая из sdk",
            code="COMPAT_DEPRECATED", level="warn", keys=old,
            replacement="ничего (по умолчанию) или --no-autofix")
    if getattr(args, "no_autofix", False):
        extra = old + (["swapped-sdk"] if getattr(args, "swapped_sdk", False) else [])
        if extra:
            fail("--no-autofix нельзя сочетать с " + ", ".join("--" + (k if k == "swapped-sdk"
                                                                     else "compat-" + k)
                                                              for k in extra),
                 code="FAIL_AUTOFIX_ARGS", extra=extra)
        return ()
    return tuple(COMPAT_SYMS)


def add_events_arg(ap):
    """
    Ключ --events: машинный протокол событий. Обратно совместим — без ключа
    поведение и вывод прежние, ни одного события не пишется.
    """
    ap.add_argument("--events", metavar="ФАЙЛ|-",
                    help="машинный протокол событий (NDJSON, одна строка — одно "
                         "событие): путь к файлу, '-' (stdout), 'stderr' или 'fd:N'. "
                         "Человеческий вывод при этом не меняется. "
                         "Реестр кодов — docs/события.md")
    return ap


def variant_from_args(args, diag):
    game = args.game
    if not game:
        exefs = find_game_exefs(args.game_exefs)
        game = npdm_program_id(exefs / "main.npdm") if exefs else "game"
    return Variant(diag=diag, slot=(args.slot or DEFAULT_SLOT), game=game,
                   game_exefs=args.game_exefs, mod_files=args.mod_files,
                   mod=getattr(args, "mod", None), slot_explicit=bool(args.slot),
                   share=getattr(args, "share", False),
                   zbic_fix=getattr(args, "zbic_fix", False),
                   compat=compat_from_args(args),
                   swapped_sdk=getattr(args, "swapped_sdk", False),
                   sdk=validate_sdk_paths(getattr(args, "sdk", None)),
                   diag_gfx=getattr(args, "diag_gfx", False),
                   diag_lock=getattr(args, "diag_lock", False),
                   cnmt=getattr(args, "cnmt", None))
