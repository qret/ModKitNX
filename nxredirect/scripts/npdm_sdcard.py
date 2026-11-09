#!/usr/bin/env python3
"""
npdm_sdcard.py — добавляет игре право доступа к SD-карте в её собственном main.npdm.

Зачем: exefs-модуль nxredirect монтирует SD через nn::fs::MountSdCard. У Trails in the
Sky 2nd Chapter в main.npdm стоит FS-permission 0x4000000000000000 — это бит 62
(Debug), который Atmosphère вырезает, если не включён debugmode_user. Права на SD
(бит 21, MountSdCard) у игры нет, поэтому рядом с модулем в exefs-мод кладётся
main.npdm с этим битом. Подпись npdm, взятого с SD, Atmosphère не проверяет
(ldr_meta.cpp проверяет ACID только у оригинального npdm из NCA).

main.npdm — файл из дампа игры, его нельзя раздавать. Поэтому вместо готового файла
раздаётся этот скрипт: каждый делает правку на своём npdm.

Это ПЕРВЫЙ шаг конвейера прав. Второй — npdm_syscalls.py: он добавляет системные
вызовы 0x74/0x75, без которых модуль падает с «Undefined System Call» при установке
хуков. Полный конвейер (оригинальный npdm игры на входе):
    python npdm_sdcard.py   <main.npdm>        -o main.npdm.sdcard   # бит SdCard
    python npdm_syscalls.py main.npdm.sdcard   -o main.npdm.ready    # вызовы 0x74/0x75
В exefs-мод кладётся main.npdm.ready под именем main.npdm.

Использование:
    python npdm_sdcard.py <main.npdm> [-o <main.npdm.sdcard>]
Без -o результат пишется рядом, с суффиксом .sdcard.

Скрипт меняет ровно два байта (по одному в ACI0/FsAccessHeader и ACID/FsAccessControl)
и отказывается работать, если структура файла не похожа на META/ACI0/ACID.
"""

import argparse
import struct
import sys
from pathlib import Path

# Бит права «монтировать SD-карту» в маске FS-permissions.
SDCARD_BIT = 1 << 21


def setup_stdio():
    """
    stdout/stderr — UTF-8, даже когда это КАНАЛ, а не консоль.

    Замер: скрипт печатает по-русски, и при выводе в файл или канал
    (`python npdm_sdcard.py ... > log.txt`) без PYTHONIOENCODING падал на
    UnicodeEncodeError — код возврата 2 на ГОДНОМ main.npdm. Локаль машины cp1250;
    `chcp 65001` помогает только настоящей консоли. Скрипт раздаётся в архиве
    «для раздачи», поэтому чинится он сам, а не только вызов из конвейера.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def read_u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def read_u64(data, off):
    return struct.unpack_from("<Q", data, off)[0]


def locate_permission_fields(data):
    """Возвращает [(имя, абсолютное смещение u64-маски)] для ACI0 и ACID."""
    if len(data) < 0x80 or data[0:4] != b"META":
        raise ValueError("это не npdm: в начале файла нет сигнатуры META")

    aci0_off = read_u32(data, 0x70)
    aci0_size = read_u32(data, 0x74)
    acid_off = read_u32(data, 0x78)
    acid_size = read_u32(data, 0x7C)

    for name, off, size in (("ACI0", aci0_off, aci0_size), ("ACID", acid_off, acid_size)):
        if off + size > len(data):
            raise ValueError(f"секция {name} выходит за пределы файла")

    if data[aci0_off:aci0_off + 4] != b"ACI0":
        raise ValueError("нет сигнатуры ACI0")
    # В ACID первые 0x200 байт — подпись RSA и модуль, магия идёт после них.
    if data[acid_off + 0x200:acid_off + 0x204] != b"ACID":
        raise ValueError("нет сигнатуры ACID")

    # FsAccessHeader внутри ACI0 и FsAccessControl внутри ACID:
    # у обоих u64-маска прав лежит по смещению +0x04 от начала структуры.
    fah_off = aci0_off + read_u32(data, aci0_off + 0x20)
    fac_off = acid_off + read_u32(data, acid_off + 0x220)

    for name, off in (("ACI0/FsAccessHeader", fah_off), ("ACID/FsAccessControl", fac_off)):
        if off + 12 > len(data):
            raise ValueError(f"{name} выходит за пределы файла")

    return [("ACI0/FsAccessHeader", fah_off + 4), ("ACID/FsAccessControl", fac_off + 4)]


def main():
    setup_stdio()
    ap = argparse.ArgumentParser()
    ap.add_argument("npdm", type=Path, help="main.npdm из exefs обновления/игры")
    ap.add_argument("-o", "--out", type=Path, default=None)
    args = ap.parse_args()

    # Нет входа — это НАХОДКА, а не авария: раньше отсутствующий файл улетал
    # FileNotFoundError и обрывал прогон трассировкой Python.
    if not args.npdm.is_file():
        raise SystemExit(f"ОТКАЗ: нет файла main.npdm: {args.npdm}")
    src = args.npdm.read_bytes()
    out_path = args.out or args.npdm.with_suffix(args.npdm.suffix + ".sdcard")

    fields = locate_permission_fields(src)

    patched = bytearray(src)
    for name, off in fields:
        old = read_u64(patched, off)
        new = old | SDCARD_BIT
        struct.pack_into("<Q", patched, off, new)
        state = "уже стоял" if old == new else "добавлен"
        print(f"{name}: маска {off:#x}: {old:#018x} -> {new:#018x} (бит SdCard {state})")

    changed = [i for i in range(len(src)) if src[i] != patched[i]]
    if len(changed) != 2:
        raise SystemExit(
            f"ОТКАЗ: ожидалось ровно 2 изменённых байта, получилось {len(changed)} "
            f"({[hex(i) for i in changed]}). Файл не записан."
        )

    print("изменённые байты:", ", ".join(hex(i) for i in changed))
    out_path.write_bytes(bytes(patched))
    print(f"записано: {out_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ValueError as e:
        print(f"ОТКАЗ: {e}", file=sys.stderr)
        sys.exit(2)
