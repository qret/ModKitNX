#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gfx_unchanged.py — «рабочая сборка не изменилась»: сравнение двух NSO
по ИСПОЛНЯЕМОМУ СОДЕРЖИМОМУ, без идентификатора сборки.

Зачем отдельный инструмент. Добавляя диагностику графики (-DNXR_DIAG_GFX),
надо доказать, что ОБЫЧНАЯ сборка от этого не поменялась. Сравнить файлы
целиком нельзя: линкер кладёт в модуль 20-байтовый GNU build-id, и он
отличается, даже когда все секции совпали байт в байт (проверено: две сборки
одних и тех же исходников в одной папке дают ОДИНАКОВЫЙ файл, а сборки из
разных копий исходников — одинаковые .text/.data и разный build-id).

Поэтому сравниваются распакованные сегменты .text, .rodata и .data, а из
.rodata исключаются ровно те 20 байт, что лежат в записи .note.gnu.build-id
(тип 3, длина 0x14). Эти же 20 байт лежат в заголовке NSO по смещению 0x40 —
совпадение проверяется, иначе исключать было бы нечего.

    python gfx_unchanged.py <NSO-A> <NSO-B>
    python gfx_unchanged.py --self-test

Коды возврата: 0 — содержимое совпало; 1 — нет; 2 — ошибка запуска.
Ничего не пишет и не удаляет.
"""

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gfx_targets import Nso  # тот же минимальный разбор NSO, без дублей

# Заголовок записи note: namesz=4, descsz=0x14, type=3 (NT_GNU_BUILD_ID), "GNU\0"
NOTE_HEADER = bytes.fromhex("040000001400000003000000474e5500")
BUILD_ID_LEN = 0x14


def build_id_range(nso_path):
    """Смещение build-id внутри .rodata и сам идентификатор, или (None, None)."""
    raw = Path(nso_path).read_bytes()
    hdr_id = raw[0x40:0x40 + BUILD_ID_LEN]
    ro = Nso(nso_path).segs[1]
    pos = ro.find(NOTE_HEADER)
    if pos < 0:
        return None, None
    off = pos + len(NOTE_HEADER)
    got = ro[off:off + BUILD_ID_LEN]
    if got != hdr_id:
        # Нашли не то: исключать вслепую нельзя, иначе спрячем настоящее отличие.
        return None, None
    return off, got


def compare(a, b, verbose=True):
    na, nb = Nso(a), Nso(b)
    offa, ida = build_id_range(a)
    offb, idb = build_id_range(b)
    if offa is None or offb is None:
        if verbose:
            print("не найдена запись .note.gnu.build-id — сравнение вслепую "
                  "делать нельзя")
        return 2
    if offa != offb:
        if verbose:
            print(f"build-id лежит по разным смещениям ({offa:#x} и {offb:#x}) — "
                  "секции точно разошлись")
        return 1

    names = (".text", ".rodata", ".data")
    same = True
    for i, name in enumerate(names):
        x, y = na.segs[i], nb.segs[i]
        if len(x) != len(y):
            if verbose:
                print(f"  {name}: РАЗНАЯ ДЛИНА {len(x)} и {len(y)}")
            same = False
            continue
        diff = [k for k in range(len(x)) if x[k] != y[k]]
        if name == ".rodata":
            diff = [k for k in diff if not (offa <= k < offa + BUILD_ID_LEN)]
        if diff:
            if verbose:
                print(f"  {name}: различий {len(diff)}, первое по {diff[0]:#x}")
            same = False
        elif verbose:
            extra = " (без 20 байт build-id)" if name == ".rodata" else ""
            print(f"  {name}: совпал байт в байт, {len(x)} Б{extra}")

    if verbose:
        print(f"  build-id: {ida.hex()} и {idb.hex()}"
              + ("  — совпал" if ida == idb else "  — различается (это норма)"))
    return 0 if same else 1


def self_test():
    """
    Негативная проба: тот же файл, но с одним изменённым байтом ВНЕ build-id,
    обязан дать красный. Иначе сравнение ничего не значит.
    Временный файл кладётся рядом и НЕ удаляется.
    """
    import tempfile
    here = Path(__file__).resolve().parent
    # Ищем любой собранный модуль, чтобы было на чём проверять.
    candidates = sorted((here.parent / "out" / "module").glob("*")) \
        + sorted((here.parent / "out-diag" / "module").glob("*"))
    # Рядом с модулем лежит и main.npdm: берём только настоящие NSO.
    nso = next((p for p in candidates
                if p.is_file() and p.read_bytes()[:4] == b"NSO0"), None)
    if nso is None:
        print("самопроверке не на чем работать: нет собранного модуля в out/")
        return 2

    print(f"образец: {nso}")
    print("проба 1: файл сам с собой — обязан совпасть")
    rc_same = compare(nso, nso)
    print("  результат:", "ЗЕЛЕНО" if rc_same == 0 else "КРАСНО")

    raw = bytearray(Path(nso).read_bytes())
    # Портим байт в заголовке NSO так, чтобы сегмент не совпал: меняем один байт
    # сжатых данных .text. Файл сохраняем во временную папку — свою на прогон,
    # убирается в конце (прежде файл оставался в %TEMP% навсегда).
    # NXR_KEEP_TEMP=1 — оставить для разбора.
    import os
    import shutil
    tmpdir = Path(tempfile.mkdtemp(prefix="nxr-gfx-unchanged-"))
    tmp = tmpdir / "negative.nso"
    text_off, = struct.unpack_from("<I", raw, 0x10)
    raw[text_off + 8] ^= 0xFF
    try:
        tmp.write_bytes(bytes(raw))
        print(f"проба 2 (НЕГАТИВ): испорченная копия {tmp} — обязана не совпасть")
        try:
            rc_diff = compare(nso, tmp)
        except Exception as exc:
            # Порча сжатого блока может сделать файл неразбираемым — это тоже
            # «не совпал», и это честный красный.
            print(f"  копия не разобралась ({exc}) — считаем красным")
            rc_diff = 1
    finally:
        if os.environ.get("NXR_KEEP_TEMP", "").strip() not in ("", "0"):
            print(f"  NXR_KEEP_TEMP: испорченная копия оставлена: {tmp}")
        else:
            shutil.rmtree(tmpdir, ignore_errors=True)
    print("  результат:", "ЗЕЛЕНО (ПЛОХО)" if rc_diff == 0 else "КРАСНО (верно)")

    ok = (rc_same == 0 and rc_diff != 0)
    print("САМОПРОВЕРКА " + ("ЗЕЛЁНАЯ" if ok else "КРАСНАЯ"))
    return 0 if ok else 1



def setup_stdio():
    """
    stdout/stderr — UTF-8, даже когда это КАНАЛ, а не консоль.

    Та же болезнь, что лечили у check_nso.py и check_events.py (замер:
    локаль машины cp1250): при выводе в канал или файл русский текст падал
    UnicodeEncodeError ещё до первой строки отчёта — код возврата 1 на заведомо
    годном входе, то есть ложная находка гейта. Из конвейера вызов идёт с
    PYTHONIOENCODING=utf-8, но скрипт обязан работать и когда его зовут руками.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

def main():
    setup_stdio()
    ap = argparse.ArgumentParser(description="NSO совпадают по содержимому?")
    ap.add_argument("a", nargs="?")
    ap.add_argument("b", nargs="?")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.a or not args.b:
        print("нужны два файла NSO")
        return 2

    print(f"A: {args.a}")
    print(f"B: {args.b}")
    rc = compare(args.a, args.b)
    print("ИТОГ: " + {0: "содержимое СОВПАЛО (отличается только build-id)",
                     1: "содержимое РАЗОШЛОСЬ",
                     2: "сравнить не удалось"}[rc])
    return rc


if __name__ == "__main__":
    sys.exit(main())
