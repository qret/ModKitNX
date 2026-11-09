#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
zbic_nso.py — распознавание ZBIC в exefs игры и сборка НЕСЖАТОГО NSO вместо него.

Зачем это нужно. С прошивки 22.0.0 Nintendo сжимает сегменты модуля `main`
не LZ4, а своим вариантом zstd: магия кадра "ZBIC" (5A 42 49 43) и таблицы
частот FSE в binary interpolative coding. Включается это БИТОМ 7 флагов
заголовка NSO0 (`Flag_UseZbicCompression`). Atmosphère до 1.12.0 такой NSO
распаковать не умеет: загрузчик возвращает ResultInvalidNso и игра не стартует.

Наш путь (без подмены прошивки): распаковать сегменты НА ПК, собрать из них
несжатый NSO0 (flags = 0x38: сжатия нет, все три хеша проверяются) и положить
его через LayeredFS в atmosphere/contents/<TID>/exefs/main. Загрузчику тогда
нечего распаковывать, а sha256 сегментов остаются ОРИГИНАЛЬНЫМИ — их считала
Nintendo, они лежат в заголовке NSO, и проверка загрузчика на них сходится.

ЧЕСТНО: на консоли этот путь НЕ пробован. На ПК доказано всё, что на ПК
доказуемо (разбор — в этом файле ниже).

Что здесь есть:
  --scan <папка exefs>          распознавание ПО СОДЕРЖИМОМУ: какие файлы exefs
                                несут бит ZBIC (имя игры и имя файла ни на что
                                не влияют — читается заголовок каждого NSO);
  --rebuild <файл> -o <выход>   собрать несжатый NSO из ZBIC-NSO и сразу
                                проверить результат гейтом --check;
  --check <выход> --orig <файл> гейт: 13 проверок пересобранного NSO против
                                заголовка ОРИГИНАЛА (хеши, флаги, ModuleId,
                                раскладка сегментов, размеры, смещения);
  --self-test                   негативные пробы самого гейта на синтетическом
                                ZBIC-NSO (настоящая игра для них НЕ нужна).

Коды возврата (они постоянные; коды возврата ДРУГИХ гейтов не менялись):
    0  всё сошлось
    1  находки: негодный вход, хеш не сошёлся, гейт красный
    2  нет входа или проверено НОЛЬ проверок («затвор нуля»)
    3  проба не поймана — гейт лжёт о своей работе (--self-test)
    4  распаковывать нечем: нет ни пакета `zbic`, ни соседнего zbic_pure.py

Чем распаковывается ZBIC (два пути, выбор автоматический):
  * pip-пакет `zbic` (kinnay, https://github.com/kinnay/zbic, GPL-2.0,
    C-обёртка вокруг zstd.c из Atmosphere-NX) — быстрый путь, ~700 МБ/с;
  * scripts/zbic_pure.py — НАШ разбор формата на чистом Python, без единой
    внешней зависимости, ~4 МБ/с. Он лежит рядом и работает всегда.
Пакет берётся, когда он есть; иначе молча берётся наш путь, и в отчёте честно
пишется, каким путём пошли. Прежний отказ «распаковать нечем» больше не
возникает: код 4 остался только на случай, когда рядом нет и zbic_pure.py.
Закрепить путь для проб: NXR_ZBIC_BACKEND=pure|pkg|auto.

Ничего не пишет, кроме указанного выхода и своих временных файлов.
Ничего не удаляет.
"""

import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

# --------------------------------------------------------------- константы

NSO_MAGIC = b"NSO0"
ZBIC_MAGIC = b"ZBIC"                 # 0x4349425A little-endian
FLAG_COMPRESSED = (1, 2, 4)          # сжат text / rodata / data
FLAG_HASHED = (8, 16, 32)            # хеш проверяется для text / rodata / data
FLAG_ZBIC = 0x80                     # бит 7 — Flag_UseZbicCompression
FLAGS_UNCOMPRESSED = 0x38            # чего мы добиваемся: сжатия нет, хеши есть
SEG_NAMES = ("text", "rodata", "data")
PAGE = 0x1000
HEADER_SIZE = 0x100

# Затвор нуля: гейт, выполнивший меньше проверок, ничего не доказал.
MIN_ZBIC_CHECKS = 13

PURE_NAME = "zbic_pure.py"           # наш распаковщик, лежит рядом с этим файлом

ZBIC_PKG_HINT = ("распаковывать ZBIC нечем: нет ни пакета `zbic`, ни соседнего "
                 "%s.\n"
                 "  Обычно хватает второго — он и есть наш путь без зависимостей;\n"
                 "  положите его рядом с этим скриптом (он приходит в том же архиве).\n"
                 "  Быстрый путь по желанию: pip install zbic (kinnay, GPL-2.0,\n"
                 "  https://github.com/kinnay/zbic)." % PURE_NAME)


def setup_stdio():
    """stdout/stderr — UTF-8, даже когда это КАНАЛ, а не консоль (локаль cp1250)."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def say(msg):
    enc = sys.stdout.encoding or "utf-8"
    sys.stdout.write(str(msg).encode(enc, errors="replace").decode(enc) + "\n")
    sys.stdout.flush()


# --------------------------------------------------------------- заголовок NSO


class NotNso(Exception):
    """Файл не NSO0 — для распознавания это не ошибка, а ответ «не наш случай»."""


def read_header(path):
    """
    Разбор заголовка NSO0 (0x100 байт) БЕЗ распаковки сегментов.

    Возвращает словарь; на не-NSO бросает NotNso. Читается только заголовок:
    распознавание не должно тащить в память 60-МБ сегменты.
    """
    path = Path(path)
    with open(path, "rb") as fh:
        raw = fh.read(HEADER_SIZE)
    if len(raw) < HEADER_SIZE or raw[:4] != NSO_MAGIC:
        raise NotNso("%s: нет магии NSO0" % path.name)
    flags = struct.unpack_from("<I", raw, 0x0C)[0]
    segs = []
    for n in range(3):
        foff, moff, size = struct.unpack_from("<III", raw, 0x10 + n * 0x10)
        csize = struct.unpack_from("<I", raw, 0x60 + n * 4)[0]
        segs.append({
            "name": SEG_NAMES[n],
            "file_offset": foff,
            "memory_offset": moff,
            "size": size,
            "compressed_size": csize,
            "compressed": bool(flags & FLAG_COMPRESSED[n]),
            "hashed": bool(flags & FLAG_HASHED[n]),
            "hash": raw[0xA0 + n * 0x20:0xC0 + n * 0x20].hex(),
        })
    return {
        "path": str(path),
        "name": path.name,
        "file_size": path.stat().st_size,
        "flags": flags,
        "zbic": bool(flags & FLAG_ZBIC),
        "module_id": raw[0x40:0x60].hex(),
        "bss_size": struct.unpack_from("<I", raw, 0x3C)[0],
        "module_name_offset": struct.unpack_from("<I", raw, 0x1C)[0],
        "module_name_size": struct.unpack_from("<I", raw, 0x2C)[0],
        "segments": segs,
        "header": raw,
    }


def segment_magic(path, header, n):
    """Первые 4 байта сжатого сегмента — для ZBIC это ASCII «ZBIC»."""
    with open(path, "rb") as fh:
        fh.seek(header["segments"][n]["file_offset"])
        return fh.read(4)


# --------------------------------------------------------------- распознавание


def scan(exefs):
    """
    Распознавание ПО СОДЕРЖИМОМУ: какие файлы папки exefs — NSO и у кого бит ZBIC.

    Имя игры, имя файла и TitleID тут ни при чём: у каждого файла читается
    заголовок. Поэтому ZBIC найдётся и там, где его по имени никто не ждал
    (например не у `main`, а у субмодуля), — а это ровно тот случай, ради
    которого проверка и сделана.
    """
    exefs = Path(exefs)
    if not exefs.is_dir():
        return {"ok": False, "reason": "папки нет или это не папка", "exefs": str(exefs)}
    files, zbic_files, not_nso = [], [], []
    for p in sorted(exefs.iterdir()):
        if not p.is_file():
            continue
        try:
            h = read_header(p)
        except NotNso:
            not_nso.append(p.name)
            continue
        row = {
            "name": p.name,
            "size": h["file_size"],
            "flags": h["flags"],
            "zbic": h["zbic"],
            "module_id": h["module_id"],
            "segments": [{k: s[k] for k in
                          ("name", "size", "compressed_size", "compressed", "hash")}
                         for s in h["segments"]],
        }
        if h["zbic"]:
            row["segment_magic"] = [segment_magic(p, h, n).hex() for n in range(3)]
            zbic_files.append(row)
        files.append(row)
    return {
        "ok": True,
        "exefs": str(exefs),
        "nso_files": [f["name"] for f in files],
        "not_nso": not_nso,
        "zbic_files": [f["name"] for f in zbic_files],
        "zbic": bool(zbic_files),
        "details": files,
        "zbic_details": zbic_files,
    }


# --------------------------------------------------------------- пересборка


def have_zbic():
    """(есть ли пакет, версия). Импорт отложен: без --rebuild пакет не нужен."""
    try:
        import zbic  # noqa: F401
    except Exception:
        return False, None
    try:
        import importlib.metadata as md
        return True, md.version("zbic")
    except Exception:
        return True, "?"


_PURE = None


def pure_module():
    """
    Наш распаковщик на чистом Python (scripts/zbic_pure.py), лежащий РЯДОМ.

    Импорт по пути, а не по имени: скрипт запускают откуда угодно, и `import`
    по sys.path нашёл бы чужой модуль или не нашёл бы ничего. Импорт отложенный
    и мягкий: гейту --check распаковщик не нужен, и его отсутствие не должно
    ломать проверку хешей.
    """
    global _PURE
    if _PURE is None:
        import importlib.util
        p = Path(__file__).resolve().with_name(PURE_NAME)
        if not p.is_file():
            return None
        spec = importlib.util.spec_from_file_location("nxr_zbic_pure", p)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _PURE = mod
    return _PURE


def have_pure():
    """Есть ли наш путь (файл рядом и он импортируется)."""
    try:
        return pure_module() is not None
    except Exception:
        return False


def backend():
    """
    Каким путём распаковывать ZBIC: ("zbic", версия) или ("pure", None).

    По умолчанию берётся пакет `zbic` (он в сотни раз быстрее), а когда его нет —
    наш чистый Python. NXR_ZBIC_BACKEND=pure|pkg закрепляет путь: без этого
    пробы не смогли бы прогнать ОБА пути на одной машине, а значит и доказать,
    что переключение работает в обе стороны.
    """
    want = (os.environ.get("NXR_ZBIC_BACKEND") or "auto").strip().lower()
    have_pkg, ver = have_zbic()
    if want == "pure":
        return ("pure", None) if have_pure() else (None, None)
    if want == "pkg":
        return ("zbic", ver) if have_pkg else (None, None)
    if have_pkg:
        return "zbic", ver
    return ("pure", None) if have_pure() else (None, None)


def backend_line(name, ver):
    """Честная строка в отчёт: каким путём пошли и чего это стоит."""
    if name == "zbic":
        return "распаковка ZBIC: пакет zbic %s (C, быстрый путь)" % (ver or "?")
    if name == "pure":
        return ("распаковка ZBIC: %s — наш чистый Python, без внешних пакетов "
                "(медленнее пакета, но работает везде)" % PURE_NAME)
    return "распаковывать ZBIC нечем"


def zbic_ready():
    """Умеем ли распаковывать хоть чем-нибудь. Обычно да — на то и наш путь."""
    return backend()[0] is not None


def decompress_blob(blob):
    """Распаковать один сжатый сегмент -> (данные, каким путём, версия)."""
    name, ver = backend()
    if name is None:
        raise ImportError(ZBIC_PKG_HINT)
    if name == "zbic":
        import zbic
        return zbic.decompress(blob), name, ver
    return pure_module().decompress(blob), name, ver


class RebuildError(Exception):
    """Пересборка не удалась: распаковка или сверка хеша."""


def rebuild(src, dst):
    """
    Собрать НЕСЖАТЫЙ NSO0 из ZBIC-NSO.

    Что берётся из оригинала и НЕ меняется: ModuleId, bss, memory_offset'ы,
    имя модуля, ApiInfo/DynStr/DynSym и все три sha256 сегментов. Меняются
    только flags (0x38), file_offset'ы и CompressedSize = размер сегмента.

    Каждый распакованный сегмент сверяется с sha256 ИЗ ЗАГОЛОВКА до того, как
    попадёт в выход: несовпадение — отказ, а не предупреждение.
    """
    back, back_ver = backend()
    if back is None:
        raise ImportError(ZBIC_PKG_HINT)

    src, dst = Path(src), Path(dst)
    h = read_header(src)
    if not h["zbic"]:
        raise RebuildError("у %s нет бита ZBIC (flags = 0x%02x) — пересобирать нечего"
                           % (src.name, h["flags"]))
    raw = src.read_bytes()
    mno, mns = h["module_name_offset"], h["module_name_size"]
    name = raw[mno:mno + mns]

    out = bytearray(raw[:HEADER_SIZE])
    out += name
    struct.pack_into("<I", out, 0x1C, HEADER_SIZE)          # имя модуля сразу за заголовком
    struct.pack_into("<I", out, 0x2C, mns)
    struct.pack_into("<I", out, 0x0C, FLAGS_UNCOMPRESSED)   # сжатия нет, три хеша

    pos = HEADER_SIZE + mns
    report = []
    for n, seg in enumerate(h["segments"]):
        blob = raw[seg["file_offset"]:seg["file_offset"] + seg["compressed_size"]]
        if seg["compressed"]:
            try:
                data, _b, _v = decompress_blob(blob)
            except Exception as e:
                raise RebuildError("сегмент %s не распаковался: %s: %s"
                                   % (seg["name"], type(e).__name__, e))
        else:
            data = blob
        if len(data) != seg["size"]:
            raise RebuildError("сегмент %s: распаковано %d байт, а в заголовке %d"
                               % (seg["name"], len(data), seg["size"]))
        got = hashlib.sha256(data).hexdigest()
        if got != seg["hash"]:
            raise RebuildError("сегмент %s: sha256 не сошлась с заголовком NSO\n"
                               "  в заголовке: %s\n  получилось:  %s"
                               % (seg["name"], seg["hash"], got))
        struct.pack_into("<III", out, 0x10 + n * 0x10, pos, seg["memory_offset"], seg["size"])
        struct.pack_into("<I", out, 0x60 + n * 4, seg["size"])
        out += data
        pos += seg["size"]
        report.append("  %-6s %9d Б, sha256 сошлась с заголовком (%s…)"
                      % (seg["name"], seg["size"], seg["hash"][:16]))
    report.insert(0, "  " + backend_line(back, back_ver))

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(bytes(out))
    return {
        "src": str(src),
        "dst": str(dst),
        "backend": back,
        "backend_version": back_ver,
        "backend_line": backend_line(back, back_ver),
        "size_before": h["file_size"],
        "size_after": len(out),
        "module_id": h["module_id"],
        "flags_before": h["flags"],
        "flags_after": FLAGS_UNCOMPRESSED,
        "src_sha256": hashlib.sha256(raw).hexdigest(),
        "dst_sha256": hashlib.sha256(bytes(out)).hexdigest(),
        "segments": [{"name": s["name"], "size": s["size"], "hash": s["hash"]}
                     for s in h["segments"]],
        "report": report,
    }


# --------------------------------------------------------------- гейт


def check(rebuilt, orig):
    """
    Гейт пересобранного NSO. Возвращает (список находок, число проверок).

    Пакет `zbic` тут НЕ нужен: всё проверяется по заголовкам и sha256. Значит
    гейт работает и на машине без зависимости — он не «доверяет» пересборке,
    а перепроверяет её независимо.
    """
    problems, checks = [], 0
    rebuilt, orig = Path(rebuilt), Path(orig)

    if not rebuilt.is_file():
        return ["нет файла пересобранного NSO: %s" % rebuilt], 0
    if not orig.is_file():
        return ["нет файла оригинала: %s" % orig], 0

    # 1. оригинал — NSO с битом ZBIC (иначе сравнивать не с чем)
    try:
        o = read_header(orig)
    except NotNso as e:
        return ["оригинал не NSO: %s" % e], 0
    checks += 1
    if not o["zbic"]:
        problems.append("у оригинала %s нет бита ZBIC (flags = 0x%02x): этот гейт "
                        "проверяет снятие ZBIC" % (orig.name, o["flags"]))

    # 2. результат — NSO
    try:
        r = read_header(rebuilt)
    except NotNso as e:
        return ["пересобранный файл не NSO: %s" % e], checks
    checks += 1

    raw = rebuilt.read_bytes()

    # 3. флаги ровно 0x38
    checks += 1
    if r["flags"] != FLAGS_UNCOMPRESSED:
        problems.append("флаги пересобранного NSO 0x%02x, а должны быть 0x%02x "
                        "(сжатия нет, все три хеша проверяются)"
                        % (r["flags"], FLAGS_UNCOMPRESSED))

    # 4. бита ZBIC нет
    checks += 1
    if r["zbic"]:
        problems.append("в пересобранном NSO остался бит ZBIC — загрузчик снова "
                        "полезет распаковывать")

    # 5. ни один сегмент не помечен сжатым
    checks += 1
    comp = [s["name"] for s in r["segments"] if s["compressed"]]
    if comp:
        problems.append("сегменты помечены сжатыми: " + ", ".join(comp))

    # 6. CompressedSize == размер сегмента
    checks += 1
    for s in r["segments"]:
        if s["compressed_size"] != s["size"]:
            problems.append("сегмент %s: CompressedSize %d != размер %d"
                            % (s["name"], s["compressed_size"], s["size"]))

    # 7. размеры сегментов те же, что у оригинала
    checks += 1
    for so, sr in zip(o["segments"], r["segments"]):
        if so["size"] != sr["size"]:
            problems.append("сегмент %s: размер %d, а в оригинале %d"
                            % (sr["name"], sr["size"], so["size"]))

    # 8. хеши в заголовке не подменены: они из оригинала
    checks += 1
    for so, sr in zip(o["segments"], r["segments"]):
        if so["hash"] != sr["hash"]:
            problems.append("сегмент %s: хеш в заголовке подменён (%s… вместо %s…)"
                            % (sr["name"], sr["hash"][:16], so["hash"][:16]))

    # 9. СОДЕРЖИМОЕ сегментов сходится с хешами Nintendo — главная проверка
    checks += 1
    for sr in r["segments"]:
        blob = raw[sr["file_offset"]:sr["file_offset"] + sr["size"]]
        if len(blob) != sr["size"]:
            problems.append("сегмент %s: в файле %d байт вместо %d"
                            % (sr["name"], len(blob), sr["size"]))
            continue
        got = hashlib.sha256(blob).hexdigest()
        if got != sr["hash"]:
            problems.append("сегмент %s: sha256 содержимого не сошлась с заголовком "
                            "(%s… вместо %s…)" % (sr["name"], got[:16], sr["hash"][:16]))

    # 10. ModuleId и bss — из оригинала (защита от подмены версии игры)
    checks += 1
    if r["module_id"] != o["module_id"]:
        problems.append("ModuleId не совпал с оригиналом: %s… вместо %s… — это другая "
                        "версия игры" % (r["module_id"][:16], o["module_id"][:16]))
    if r["bss_size"] != o["bss_size"]:
        problems.append("bss 0x%x, а в оригинале 0x%x" % (r["bss_size"], o["bss_size"]))

    # 11. раскладка в памяти не тронута: memory_offset'ы те же
    checks += 1
    for so, sr in zip(o["segments"], r["segments"]):
        if so["memory_offset"] != sr["memory_offset"]:
            problems.append("сегмент %s: memory_offset 0x%x, а в оригинале 0x%x"
                            % (sr["name"], sr["memory_offset"], so["memory_offset"]))

    # 12. ограничения загрузчика (CheckAutoLoad): text с нуля, страницы, без нахлёста
    checks += 1
    segs = r["segments"]
    if segs[0]["memory_offset"] != 0:
        problems.append("text начинается не с нуля (0x%x) — загрузчик такой NSO не примет"
                        % segs[0]["memory_offset"])
    for s in segs:
        if s["memory_offset"] % PAGE:
            problems.append("сегмент %s не выровнен на страницу: 0x%x"
                            % (s["name"], s["memory_offset"]))
    for a, b in zip(segs, segs[1:]):
        end = a["memory_offset"] + a["size"]
        if end > b["memory_offset"]:
            problems.append("сегмент %s (конец 0x%x) налезает на %s (0x%x)"
                            % (a["name"], end, b["name"], b["memory_offset"]))

    # 13. файл собран без дыр и мусора: сегменты идут подряд за именем модуля
    checks += 1
    pos = HEADER_SIZE + r["module_name_size"]
    if r["module_name_offset"] != HEADER_SIZE:
        problems.append("имя модуля лежит по 0x%x, а ожидалось сразу за заголовком (0x%x)"
                        % (r["module_name_offset"], HEADER_SIZE))
    for s in segs:
        if s["file_offset"] != pos:
            problems.append("сегмент %s начинается с 0x%x, а ожидалось 0x%x "
                            "(между сегментами дыра или нахлёст)"
                            % (s["name"], s["file_offset"], pos))
        pos += s["size"]
    if len(raw) != pos:
        problems.append("размер файла %d, а по раскладке должно быть %d "
                        "(лишний хвост или обрезан)" % (len(raw), pos))

    return problems, checks


def run_check(rebuilt, orig, quiet=False):
    """Гейт как команда: печать и код возврата (0 / 1 / 2 при затворе нуля)."""
    problems, checks = check(rebuilt, orig)
    if checks < MIN_ZBIC_CHECKS:
        if not quiet:
            say("checks passed: 0 of %d · critical: %d · new warnings: 0"
                % (MIN_ZBIC_CHECKS, len(problems) or 1))
            for p in problems:
                say("X " + p)
            say("X гейт закончился, выполнив %d проверок из %d — он ничего (или почти "
                "ничего) не проверил; это отказ, а не успех" % (checks, MIN_ZBIC_CHECKS))
        return 2
    if problems:
        if not quiet:
            say("checks passed: %d of %d · critical: %d · new warnings: 0"
                % (checks - len(problems) if checks > len(problems) else 0,
                   checks, len(problems)))
            for p in problems:
                say("X " + p)
        return 1
    if not quiet:
        say("checks passed: %d of %d · critical: 0 · new warnings: 0" % (checks, checks))
        say("  ok несжатый NSO годен: флаги 0x%02x, хеши сегментов совпали с заголовком "
            "оригинала, ModuleId и раскладка не тронуты" % FLAGS_UNCOMPRESSED)
    return 0


# --------------------------------------------------------------- синтетика для проб


def fixture_body(seed, size):
    """
    Тело сегмента фикстуры: слова из короткого словаря в порядке, который задаёт
    простой линейный генератор. Такие данные реально сжимаются ТАБЛИЦАМИ (а не
    вырождаются в RLE-блоки), то есть проходят через настоящие FSE-таблицы в
    binary interpolative coding — ровно то, что мы и проверяем.
    """
    words = [b"nxredirect", b"zbic", b"fixture", b"segment", b"atmosphere", b"exefs",
             b"main", b"npdm", b"layeredfs", b"switch", b"module", b"offset",
             b"0123456789", b"abcdefghij", b"\x00\x01\x02\x03", b"hash"]
    out = bytearray()
    s = seed
    while len(out) < size:
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        out += words[(s >> 16) % len(words)]
        out += b" " if (s >> 8) & 1 else b"-"
    return bytes(out[:size])


FIXTURE_SIZES = (0x2000, 0x1000, 0x800)

# Готовые кадры ZBIC для тех же тел (сжаты пакетом zbic уровнем 3 один раз и
# записаны сюда). Так фикстура НЕ ЗАВИСИТ от пакета: пробы идут и на машине, где
# его нет, — иначе самопроверка там доказывала бы ноль. Кадры настоящие: в них
# есть и сжатые литералы Huffman, и таблицы частот FSE в BIC.
FIXTURE_FRAMES = (
    # сегмент 1: 8192 Б -> 1749 Б кадра ZBIC
    (
        "T0%)fU;rOoHUP2<As+>1F5(Ud7{SP<Y&VncwJUe?|G$_1|DQpR>i-k118q_0Uv+oY|JQZjSJ"
        "&6Q9jB;s%A&3+L$_Q*ZYhq<&^hHG%;H-#PO?l#Qx9xihk@9@u%M8Da9~hCaAGFypppXJITA)"
        "hne{1`7%W-Md`hD&3T-=vQ1YP?5f2Ci0E{TJ0)s%P33q_{0#(ygJSPgX+OvN%bb5ElRt+Yo&"
        "ENLO|KFm&9u+?z30FaYCyEAGpKyi2ryg;mlJA+DkIy-=X_g!bkdm(QUiO*RF;59Nl6)FpPXT"
        "~VAzb`9rRE6mR2(TNeXjY;ju4|HW)cM0$^IHVV8V2)C3VWJmUr56x_GZGb%Fv_jlu9y7VSau"
        "{+beRuJYOHK2|2iL6%m%PR%4GvyMW4s3R#rcYdTCh1`tfzzFX)lyvbOELH7XK1|h#KI3viF&"
        "5fB&_F2n7*{3m3A5w7{*6UUu-I4~^@R8lslqrgt{wn-s4<CNlzcd%r8bp{4e%{&W#s2T_SGl"
        "#l)@rQ4eYClAhTLb$}9cQoC?%$x6O+oLsX^rTbNDf#N)UL8OC=Uo|01WjPNNjt;hijatS5r!"
        "DR!6)^o~0y3QrJ#g1ZXk*Ud%`Y&b$<2F%WZmQ^(baOF*YPW~ZIJgGkZ3>2~Y#A9?pEWLsIPa"
        "07IiX#fNO0kJ3#2OHC=WuQ4^!A<Y{$`J!j{+rKSROf`_76sij4ZSj+Ch;LA~Voj?n1coaeLi"
        "8M59cWGcwFF8|36(w#eZ3Rr`~I{EHZ|C!ml4vY~8a5L=e=bj<IRfkM`v)}34PIWL{0pl@%59"
        "-Bo!Y@jVz6e$sAr>VxzereN@BiMd6QtNJgFepY$Hu*k;pW$dDkD{A<G&vlk!n>=V<w~*LD;|"
        "`FeiealFl$dbm-&9TDNg3!MO&ocv6<O9{@+1nL;QrY}*;hu`C~=)yb&ya@nxWK-*be?cK7l0"
        "62buBGEbwi;@1;2`LBYp$;cq#M70hWKNL#oWH+H@lqXO`IaK69>S2$)AYN7aLW1y_9z7zgB<"
        "anhbd|)R}E0`jb3Qq*kMiF4w^bDT^$hHa<XUKJ1XC~nW&>792e#Gk2z`@+X|yj8)mY=<{@z%"
        "-pUW<#wKGF`JRN)X(JpyX`oxo$~%4xjmexNGo{^a-@pZH`1W8i<^Al0uU<MVHuadSP&R=BRX"
        "6lNtn@U2gjl}aVxe9EEwhs_akTf>ZD1`CetGCI001kIFTlzOH*!sQ`aRe*202uk)aJ4dnXJ0"
        ">q(APWIwuPq8_1Wm>Q@>D2?Ah%dL%jMl903QC`?%*qe*YQl0pjVm%uSI`ArNy`AU@tJ6@M(i"
        "{H~zU#Jz{kp;9^1s>o(E|$q_RG|XKPTA$Faf0v2o{u)pHPwh03zRtI>N%F7`j{HXuOqD~j$q"
        "%pS^y~eQcU(YfU$neG`GaQl9@k!_Ke~Bef)^AZNX|4IZJ_?{(^QR_hGI&C|OfTxe6J;_pr=~"
        "$@yO;nq(*H|A&u~%96-Iz2)L!QFHhL%7mjUHdk_I#m~@zIp7|Etbt7Vs8AET3||eE@#oP<Sy"
        "1D*MWDA~1^1r~DpA$gz*8)pYB%ycVC~=_b2D7b_o%!KB6<ge$iT|W@&Jk`Wsv4NWp7--j_JO"
        "z0|^p*t`yEUr_&*(L$MIuK3P<AW0Voge}N`ORtj@)nCcUtLH;fX#E~n#Z3hQbt-bDTkLoPxa"
        "B{1GLtIk~u)IVjZ4hBWRM%iQBUEh#<Rd+|Zj+b3wFO3_P`57je)$iIR>H>fLSGVt$&Qnt(;Z"
        "xdqc@>DD85ob+7&J^`gdW6mLg3sba+s6OtT;O$7s`zE>pBL-2ccg7B=X?YH&Oh8I=MQ1syGl"
        "$vawA@Db@P=|maNid?hr6>G#@SEmm$)Ldz4S|1q1n*P=Pb8uMB%#X2qKef$h<GL)YewZVq`M"
        "ES>XjQ>lUcw=o$27$1(PRdDG|cOeLe)G>RpQ=oY{M}Mn%aBFw6zEUGVNK^P^s~-9^Pzj+_F%"
        ">G-KpP`UcY$VDXN1aqR~K9eIJbfJ`<Zcs)aG_z;yj7Qq-476Un9No#f^6gw!;y*xh&s9rPKJ"
        "d@8lN-?mHf05oV$D|aPQ@#KRe0CsRePUr0H+z66Jp=`8HF+Q&$L@k`DREs2hx`F7X;I}abCy"
        "FWL*E!vrtnOXF98IXx2h7d;<Kv-M5Jc2W|CdQ>R#oVZKd&~QbCyQXONM}W~fgW%a4GXyZ7FD"
        "<qR3{;iswF+7{is&InpS=`azBhX"
    ),
    # сегмент 2: 4096 Б -> 959 Б кадра ZBIC
    (
        "T0%)fU;q!T9ROlVA0P#0mhWC1!$Fzf2wg<fH7<7)MPF@X)d){+IEG;mp>@cywzvJ>{{P?a{r"
        "mlVsS@Z|G=CFOkYkM}-$MD8)^E$Om@p;RVag?sE!mN<3B<urb>d2Zg$d<L1%u2`(u%OukrEx"
        "loKK0%N>$LPo@Ffbz@UI&Kp+5Mp$LJus0fw$wB;_veFX@>zrKvlSYA8|z5FGAWt%SK2Q^xIN"
        "g;4_d83{TqdAy&u7K3tLtNfgOP*Jwxjl%Q;znlmn4hdKo+3^;jCIfo|6RS|f?*CaOCnmDQ+("
        "pkfZDcqI=>EFNqf}+kU34k2*QuiMC@;$1wByTjW2fu-$@v;LnB;d5DS9Bs<kW*4S2(kV@8g-"
        "(kBE4c`C5XNGKA-nKDH@A1hD!qF4fG6hoqLF`cb*9&Rs=&)QTe<((2G&;+*)4^&O;XC*j8*{"
        "PVvB7nPCN9ENK-XwUPqJ3i!0$n^IoyH9&F;YsrrW9Q~QpuAgoeT9$Cf&!Sax6<yjS(odxm!g"
        "AtU?K#Hv|}dwmR6=5L2SmT7cKSj6Rondan@fG+oOwi9Ud8I0A+3dfoTGnVe*Y1LNR<h$P}BV"
        "@LD|;B1-?q;_&ArYA6r#g76V*W#HqD6o4;E4?LDOF3c#f#aZv@cHWz$Y@wt8gCSuy-weP?4D"
        "2riQsGuAeX0vK(6i|dBGa>GG2CFw*hAkQ-dzdoJ`HI)N`di_D?EU{Onn;Gsu+<-xCDw?NN$g"
        "d-1C3ek8*oJOiw9m4+a_6aYHxZN9=2h3Nx&MkRMKn3xD7#|OHzG+L<n)(3=>7~B`7GbdLib+"
        "NpjfVWeUEa?mi_(|D19K``S9|X!$?5a+~4W}~DvCCpl7%4Y^A0snIidYB;`mqA29`?o3YP9e"
        "hMj1gY{*@zsU?56)?r5cBm>nV3|H;NBTq0qHFqk&3X8Nv|<|5#oK7<{c0SIgkATj8V)*>+-s"
        "Y78T0ccOsb26()Y`vN!Iw}4g^MTmOFZPK4mh?DJ5N-jq546JAx{g>9TOtjzN<FIjw6d4n#FT"
        "R6MwtOIA$5!N`Tys*H4N;hiFQ~mOvSL8c6<Uf*bqZ^F5Ufc5H>mkJY+K5SgL#NwpiW}Wm5SH"
        "<MzPyiZ21-&wE*B9tLLQqb`Q*Xt%@ig2`v(V*^1mX4(si?fJ>9<H_Jy00q(JEwl_ar0oEFgl"
        "6tGZbu1r4>MEk3#mLvP7Qv*%odyhGJpUR)Y7&<az{&17(x{"
    ),
    # сегмент 3: 2048 Б -> 537 Б кадра ZBIC
    (
        "T0%)fU;qbw5CDP*93BN^0wS#gGqR<#C*KhMX7@l41V&L5Rk#2Fz`2YVb=$f`c5DAR@yS)EOd"
        "^Y;m{!NbwU{;-3q=BPFcb(e{iXVQoZ<TOvJ~?H?GiNx5wn|Woc(v!K&D01fgk`VT<Y%-wA7J"
        "ISyw4RjqXi341nFJ2;{$M6>K1kQ3v>e*$|9#0t(2>z4?V%MfMT}3y00Qz13nw7n15e-6$a2T"
        "(;b^A-PQsFSaB^+?#YHgI$uRG|zXRI+4ql<QjVXbavxn<QzCo>!WJF09<y~#2Z5vT*+1V6$_"
        "qqR*ogp97lg32@W`r!Bc!?nZ0(`QUrmJIWm!vv^8<eI5l$;!na}Y`Gs>U!9rf#UjY<Qy6X_{"
        "dIU$!2YKEwwQqE!twHJu-}D~Gx{HJXo!eB}D8*8?5<+^_haoxJx!|qF&Ub@ablZGl^|ITY)W"
        "9o5U_HR_9OOAD@^QRF2F)iuW2z2n<%=NLB9B63g0%kyr#+|$t~HNipNqOlSVAuc3QqRyOyZ("
        "GYZKhatO-MLZwFRN<$LH*nKe5762SEwrYY_ekdZcq5{0#MdPM?k84AyE=FFny$FBV3aRJQKo"
        "nKyS`DZie+zDjNaF-8WNFwm(rVe=Bfag~X3PUYO&ch_bf>hve40rdumrE~Fknpoz>YyW4jer"
        "X6uh|di)Qd2QiQk5_Xs+6KsQ"
    ),
)


def fixture_frames():
    """Кадры ZBIC фикстуры (распакованные из base85 констант)."""
    import base64
    return [base64.b85decode(f) for f in FIXTURE_FRAMES]


def make_fixture(path, level=3):
    """
    Синтетический ZBIC-NSO: три маленьких сегмента в настоящем ZBIC,
    честные sha256 в заголовке, flags = 0xBF.

    Зачем. Негативные пробы гейта не должны зависеть ни от дампа игры, ни от
    пакета `zbic`: иначе самопроверка на чужой машине всегда «неполная», и о
    гейте там не доказано ничего. Поэтому вход собирается из встроенных кадров.
    """
    name = b"fixture\0"
    bodies = [fixture_body(n + 1, sz) for n, sz in enumerate(FIXTURE_SIZES)]
    frames = fixture_frames()
    mem = [0x0, 0x5000, 0x8000]      # выровнено на страницу, без нахлёста
    hdr = bytearray(b"\0" * HEADER_SIZE)
    hdr[0:4] = NSO_MAGIC
    struct.pack_into("<I", hdr, 0x0C, 0x3F | FLAG_ZBIC)
    struct.pack_into("<I", hdr, 0x1C, HEADER_SIZE)
    struct.pack_into("<I", hdr, 0x2C, len(name))
    struct.pack_into("<I", hdr, 0x3C, 0x1000)                 # bss
    hdr[0x40:0x60] = hashlib.sha256(b"module-id").digest()[:0x14] + b"\0" * 12

    body = bytearray(name)
    pos = HEADER_SIZE + len(name)
    for n, data in enumerate(bodies):
        blob = frames[n]
        if blob[:4] != ZBIC_MAGIC:
            raise RuntimeError("кадр фикстуры без магии ZBIC: %r" % blob[:4])
        struct.pack_into("<III", hdr, 0x10 + n * 0x10, pos, mem[n], len(data))
        struct.pack_into("<I", hdr, 0x60 + n * 4, len(blob))
        hdr[0xA0 + n * 0x20:0xC0 + n * 0x20] = hashlib.sha256(bytes(data)).digest()
        body += blob
        pos += len(blob)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(hdr) + bytes(body))
    return path


# --------------------------------------------------------------- негативные пробы


def self_test():
    """
    Пробы гейта --check: он обязан краснеть на каждой подделке и быть зелёным
    на честном результате. Плюс «затвор нуля» и проба распознавания --scan.

    Приём тот же, что у прочих гейтов проекта: берётся ЗАВЕДОМО ГОДНЫЙ вход,
    портится ровно одна вещь, и смотрится, покраснел ли гейт именно на ней.
    """
    import tempfile

    ok_pkg, ver = have_zbic()
    if not zbic_ready():
        say("нет входа: " + ZBIC_PKG_HINT.splitlines()[0])
        say("  пробы гейта ZBIC НЕ ЗАПУСКАЛИСЬ — о гейте не доказано ничего")
        return 2

    good = bad = 0
    with tempfile.TemporaryDirectory(prefix="zbic-selftest-") as td:
        td = Path(td)
        exefs = td / "exefs"
        exefs.mkdir()
        src = make_fixture(exefs / "main")
        (exefs / "rtld").write_bytes(b"NSO0" + b"\0" * (HEADER_SIZE - 4))
        (exefs / "main.npdm").write_bytes(b"META" + b"\0" * 100)
        dst = td / "main-uncompressed"
        try:
            info = rebuild(src, dst)
        except Exception as e:
            # Первый же честный вход не пересобрался — дальше проверять нечего:
            # это не «проба покраснела», это сломан сам распаковщик.
            say("  проба «честная пересборка фикстуры» — НЕ СРАБОТАЛА: %s: %s"
                % (type(e).__name__, e))
            say("проба не получилась: 0 из 1")
            return 3

        def probe(label, red, negative=True):
            nonlocal good, bad
            want = "покраснел" if negative else "зелёный"
            fine = red if negative else not red
            say("  проба «%s» — %s" % (label, want if fine else "НЕ СРАБОТАЛА"))
            if fine:
                good += 1
            else:
                bad += 1

        # положительные: на честном входе гейт и распознавание зелёные
        probe("честный пересобранный NSO", run_check(dst, src, quiet=True) != 0,
              negative=False)
        sc = scan(exefs)
        probe("распознавание нашло ZBIC по содержимому (%s)" % ", ".join(sc["zbic_files"]),
              not (sc["ok"] and sc["zbic_files"] == ["main"]), negative=False)
        probe("распознавание не путается в не-NSO (main.npdm)",
              "main.npdm" not in sc["not_nso"], negative=False)

        raw = dst.read_bytes()

        def mutant(label, mutate, negative=True):
            m = bytearray(raw)
            mutate(m)
            p = td / "mutant"
            p.write_bytes(bytes(m))
            probe(label, run_check(p, src, quiet=True) != 0, negative=negative)

        # негативные: каждая подделка обязана быть поймана
        mutant("бит ZBIC вернули в флаги",
               lambda m: struct.pack_into("<I", m, 0x0C, FLAGS_UNCOMPRESSED | FLAG_ZBIC))
        mutant("сегмент помечен сжатым",
               lambda m: struct.pack_into("<I", m, 0x0C, FLAGS_UNCOMPRESSED | 1))
        mutant("байт в сегменте испорчен",
               lambda m: m.__setitem__(HEADER_SIZE + 8 + 16, m[HEADER_SIZE + 8 + 16] ^ 0xFF))
        mutant("ModuleId подменён (другая версия игры)",
               lambda m: m.__setitem__(slice(0x40, 0x60), b"\xAA" * 0x20))
        mutant("хеш в заголовке подогнан под испорченный сегмент",
               lambda m: (m.__setitem__(HEADER_SIZE + 8 + 16,
                                        m[HEADER_SIZE + 8 + 16] ^ 0xFF),
                          m.__setitem__(slice(0xA0, 0xC0),
                                        hashlib.sha256(
                                            bytes(m[HEADER_SIZE + 8:
                                                    HEADER_SIZE + 8 +
                                                    struct.unpack_from('<I', m, 0x18)[0]])
                                        ).digest())))
        mutant("memory_offset сегмента сдвинут",
               lambda m: struct.pack_into("<I", m, 0x24, 0x6000))
        mutant("сегмент не выровнен на страницу",
               lambda m: struct.pack_into("<I", m, 0x24, 0x5001))
        mutant("CompressedSize не равен размеру",
               lambda m: struct.pack_into("<I", m, 0x60, 123))
        mutant("файл обрезан с конца", lambda m: m.__delitem__(slice(len(m) - 64, len(m))))
        mutant("к файлу приписан хвост", lambda m: m.extend(b"\xCC" * 32))
        mutant("bss подменён", lambda m: struct.pack_into("<I", m, 0x3C, 0x999))

        # «затвор нуля»: на входе, где сравнивать не с чем, гейт обязан дать 2
        rc = run_check(dst, td / "нет-такого-файла", quiet=True)
        probe("оригинала нет — затвор нуля (код 2)", rc == 2)
        rc = run_check(td / "нет-такого-файла", src, quiet=True)
        probe("пересобранного нет — затвор нуля (код 2)", rc == 2)

        # --- пробы самой РАСПАКОВКИ: путей два, и оба обязаны быть верны
        pure = pure_module()
        bodies = [fixture_body(n + 1, sz) for n, sz in enumerate(FIXTURE_SIZES)]
        frames = fixture_frames()

        if pure is None:
            # файла рядом нет — это не «проба прошла», это провал: значит
            # у человека без пакета распаковывать было бы нечем
            probe("наш путь (%s) лежит рядом" % PURE_NAME, True, negative=False)
        else:
            same = all(pure.decompress(frames[n]) == bodies[n]
                       for n in range(len(frames)))
            probe("чистый Python распаковал все кадры фикстуры байт в байт",
                  not same, negative=False)

            # негативная: испорченный байт кадра не должен «распаковаться верно»
            broken = bytearray(frames[0])
            broken[len(broken) // 2] ^= 0xFF
            try:
                got = pure.decompress(bytes(broken))
                caught = got != bodies[0]
            except Exception:
                caught = True
            probe("испорченный байт кадра: чистый Python не выдал прежние данные",
                  caught)

            # негативная: подмена магии на обычный zstd НЕ лечит BIC-таблицы —
            # если бы лечила, весь наш разбор BIC был бы не нужен
            swapped = b"\x28\xb5\x2f\xfd" + frames[0][4:]
            try:
                got = pure.decompress(swapped)
                caught = got != bodies[0]
            except Exception:
                caught = True
            probe("подмена магии ZBIC->zstd не даёт прежних данных "
                  "(значит BIC действительно нужен)", caught)

            if ok_pkg:
                import zbic as _pkg
                both = all(_pkg.decompress(frames[n]) == pure.decompress(frames[n])
                           for n in range(len(frames)))
                probe("наш путь и пакет zbic совпали байт в байт", not both,
                      negative=False)

        # --- пробы ПЕРЕКЛЮЧЕНИЯ путей: оно обязано работать в обе стороны
        keep = os.environ.get("NXR_ZBIC_BACKEND")
        try:
            os.environ["NXR_ZBIC_BACKEND"] = "pure"
            probe("принудительный выбор нашего пути (NXR_ZBIC_BACKEND=pure)",
                  backend()[0] != "pure", negative=False)
            dst_pure = td / "main-pure"
            info_pure = rebuild(src, dst_pure)
            probe("пересборка НАШИМ путём прошла гейт и дала тот же файл",
                  run_check(dst_pure, src, quiet=True) != 0
                  or dst_pure.read_bytes() != dst.read_bytes(), negative=False)
            if ok_pkg:
                os.environ["NXR_ZBIC_BACKEND"] = "pkg"
                probe("принудительный выбор пакета (NXR_ZBIC_BACKEND=pkg)",
                      backend()[0] != "zbic", negative=False)
                dst_pkg = td / "main-pkg"
                rebuild(src, dst_pkg)
                probe("оба пути дали одинаковый несжатый NSO",
                      dst_pkg.read_bytes() != dst_pure.read_bytes(), negative=False)
            os.environ.pop("NXR_ZBIC_BACKEND", None)
            probe("без указания путь выбирается сам (%s)" % (backend()[0] or "никакой"),
                  backend()[0] is None, negative=False)
            probe("путь в отчёте назван честно: %s" % info_pure["backend"],
                  info_pure["backend"] != "pure", negative=False)
        finally:
            if keep is None:
                os.environ.pop("NXR_ZBIC_BACKEND", None)
            else:
                os.environ["NXR_ZBIC_BACKEND"] = keep

        # негативная: испорченный кадр внутри ZBIC-NSO обязан быть пойман по sha
        bad_src = td / "main-bad-frame"
        m = bytearray(src.read_bytes())
        foff = struct.unpack_from("<I", m, 0x10)[0]
        m[foff + 40] ^= 0xFF
        bad_src.write_bytes(bytes(m))
        try:
            rebuild(bad_src, td / "out-bad")
            probe("испорченный кадр в NSO пойман при пересборке", False)
        except Exception:
            probe("испорченный кадр в NSO пойман при пересборке", True)

        # пересборка не-ZBIC входа обязана отказать, а не «сделать вид»
        plain = td / "plain"
        plain.write_bytes(bytes(bytearray(raw)))
        try:
            rebuild(plain, td / "out2")
            probe("пересборка NSO без бита ZBIC", False)
        except RebuildError:
            probe("пересборка NSO без бита ZBIC", True)

        say("  (синтетика: %d Б ZBIC -> %d Б несжатого; %s)"
            % (info["size_before"], info["size_after"], info["backend_line"]))

    total = good + bad
    if not total:
        say("ничего не проверив: ни одна проба не запускалась")
        return 2
    if bad:
        say("проба не получилась: %d из %d" % (good, total))
        return 3
    say("checks passed: %d of %d · critical: 0 · new warnings: 0" % (good, total))
    say("  ok гейт ZBIC умеет краснеть на каждой подделке и зелен на честном входе")
    return 0


# --------------------------------------------------------------- CLI


def main():
    setup_stdio()
    ap = argparse.ArgumentParser(
        description="ZBIC в exefs: распознавание и сборка несжатого NSO")
    ap.add_argument("--scan", metavar="EXEFS", help="осмотреть папку exefs игры")
    ap.add_argument("--rebuild", metavar="NSO", help="собрать несжатый NSO из ZBIC-NSO")
    ap.add_argument("-o", "--out", metavar="ФАЙЛ", help="куда положить результат --rebuild")
    ap.add_argument("--check", metavar="NSO", help="гейт пересобранного NSO")
    ap.add_argument("--orig", metavar="NSO", help="оригинал для --check")
    ap.add_argument("--self-test", action="store_true", help="негативные пробы гейта")
    ap.add_argument("--json", action="store_true", help="машинный вывод для --scan")
    args = ap.parse_args()

    if args.self_test:
        sys.exit(self_test())

    if args.scan:
        res = scan(args.scan)
        if args.json:
            say(json.dumps(res, ensure_ascii=False))
        if not res["ok"]:
            if not args.json:
                say("нет входа: %s (%s)" % (res["exefs"], res["reason"]))
            sys.exit(2)
        if not args.json:
            say("exefs: %s" % res["exefs"])
            say("  NSO: %s" % (", ".join(res["nso_files"]) or "(ни одного)"))
            if res["not_nso"]:
                say("  не NSO: %s" % ", ".join(res["not_nso"]))
            if res["zbic"]:
                say("  СЖАТИЕ ZBIC (бит 7 флагов NSO) у: %s" % ", ".join(res["zbic_files"]))
                for f in res["zbic_details"]:
                    say("    %s: flags 0x%02x, магия сегментов %s"
                        % (f["name"], f["flags"],
                           " ".join(bytes.fromhex(m).decode("latin1")
                                    for m in f["segment_magic"])))
                say("  Такой NSO Atmosphère младше 1.12.0 не загрузит "
                    "(ResultInvalidNso). Снять ZBIC: --rebuild")
            else:
                say("  ZBIC ни у одного файла нет — обычный exefs")
        sys.exit(0)

    if args.rebuild:
        if not args.out:
            say("нет входа: для --rebuild нужен -o <файл>")
            sys.exit(2)
        try:
            info = rebuild(args.rebuild, args.out)
        except ImportError as e:
            say(str(e))
            sys.exit(4)
        except (RebuildError, NotNso) as e:
            say("X " + str(e))
            sys.exit(1)
        for line in info["report"]:
            say(line)
        say("собран несжатый NSO: %s (%d Б вместо %d), flags 0x%02x, sha256 %s"
            % (info["dst"], info["size_after"], info["size_before"],
               info["flags_after"], info["dst_sha256"]))
        # Пересборка сама себя не хвалит: результат сразу идёт через тот же гейт.
        sys.exit(run_check(args.out, args.rebuild))

    if args.check:
        if not args.orig:
            say("нет входа: для --check нужен --orig <исходный NSO>")
            sys.exit(2)
        sys.exit(run_check(args.check, args.orig))

    ap.print_help()
    sys.exit(2)


if __name__ == "__main__":
    main()
