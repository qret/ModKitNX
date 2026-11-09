#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
npdm_syscalls.py — добавляет в main.npdm игры системные вызовы, нужные нашему модулю.

Зачем. Рядом с модулем мы кладём main.npdm игры (плюс бит SdCard). Но exlaunch ставит
перехватчики через svc 0x74/0x75 (MapProcessMemory/UnmapProcessMemory), а игре они не
разрешены: в её правах есть только группы 0..3 (номера 0x00..0x5F). Результат —
«Undefined System Call» и закрытие игры. Проба на консоли 2026-09-22,
logs/01790108607_01007a2027548000.log: Result 0x10A8 (2168-0008), Type «Undefined System
Call», Svc Id 0x74, Address = наш модуль + 0x7c8c.

Скрипт разбирает npdm, добавляет недостающие вызовы в разрешения ЯДРА в обеих частях
(ACI0 — что просит процесс, ACID — что ему позволено) и пересобирает файл со всеми
смещениями. Всё остальное (права FS, список сервисов, program id, прочие возможности)
переносится байт в байт.

Почему правятся обе части. Atmosphère при создании процесса зовёт
TestCapability(acid_kac, aci_kac) (stratosphere/loader/source/ldr_process_creation.cpp),
а для масок вызовов сверка ТОЧНАЯ: ldr_capabilities.cpp, CapabilitySyscallMask::IsValid —
«this->GetIndex() == restriction.GetIndex() && this->GetMask() == restriction.GetMask()».
Значит слова масок в ACI0 и ACID должны совпадать слово в слово, иначе процесс не создастся.

Подпись ACID у нашего файла никто не проверяет: ldr_meta.cpp читает npdm с SD, а подпись
сверяет у ОРИГИНАЛЬНОГО npdm из NCA (m_original_meta_cache), забирая оттуда только modulus.
Program id он всё равно переписывает своим (acid->program_id_min/max, aci->program_id).

Формат (switchbrew.org/wiki/NPDM):
  META : 0x70 ACI0 off, 0x74 ACI0 size, 0x78 ACID off, 0x7C ACID size
  ACID : 0x000 подпись RSA-2048, 0x100 открытый ключ, 0x200 «ACID», 0x204 размер
         (= размер секции - 0x100), 0x20C флаги, 0x210/0x218 диапазон program id,
         0x220 Fac off/size, 0x228 Sac off/size, 0x230 Kac off/size
  ACI0 : 0x000 «ACI0», 0x010 program id, 0x20 Fah off/size, 0x28 Sac off/size,
         0x30 Kac off/size
  Возможность ядра «EnableSystemCalls»: (v & 0x1F) == 0x0F, биты 5..28 — маска из 24
  номеров, биты 29..31 — индекс группы; номер вызова = группа * 24 + бит.

Использование:
    python npdm_syscalls.py <вход main.npdm> [-o выход] [--svc 0x74 0x75]
    python npdm_syscalls.py <main.npdm> --check [--program-id 01007A2027548000]
    python npdm_syscalls.py <main.npdm> --self-test   (или NXR_GAME_NPDM=<путь>)

--check ничего не пишет и краснеет (код 1), если вызовов нет, маски ACI0/ACID разошлись,
пропал бит SdCard или не совпал program id. Это и есть гейт для сборки.
"""

import argparse
import os
import struct
import sys
from pathlib import Path

# Вызовы, которые нужны нашему модулю сверх прав игры (найдены сканированием его кода).
DEFAULT_SVCS = [0x74, 0x75]

CAP_SYSCALL = 0x0F          # (value & 0x1F) == 0x0F — это маска разрешённых вызовов
SYSCALLS_PER_GROUP = 24     # одна запись покрывает 24 номера
MAX_SVC = 8 * SYSCALLS_PER_GROUP - 1        # индекс группы — 3 бита

SDCARD_BIT = 1 << 21        # право «монтировать SD-карту» в маске FS-permissions

META_HDR_SIZE = 0x80
SECTION_ALIGN = 0x10

# (имя, смещение таблицы подблоков, минимальный размер заголовка секции)
LAYOUT = {
    "ACID": (0x220, 0x240),
    "ACI0": (0x020, 0x040),
}


def setup_stdio():
    """
    stdout/stderr — UTF-8, даже когда это КАНАЛ, а не консоль.

    Замер: без этого скрипт не падал (say() гасит ошибку заменой),
    но весь русский вывод в канале превращался в «?????» — а по нему сверяют
    приговор и внешние программы, и пробы. Локаль машины cp1250; `chcp 65001`
    помогает только настоящей консоли. Скрипт раздаётся в архиве «для раздачи»,
    поэтому чинится он сам, а не только вызов из конвейера.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def say(msg):
    enc = sys.stdout.encoding or "utf-8"
    sys.stdout.write(msg.encode(enc, errors="replace").decode(enc) + "\n")


def align(value, mult=SECTION_ALIGN):
    return (value + mult - 1) & ~(mult - 1)


class Npdm:
    """Разбор и пересборка npdm (META + ACID + ACI0)."""

    def __init__(self, raw):
        raw = bytes(raw)
        if len(raw) < META_HDR_SIZE or raw[:4] != b"META":
            raise ValueError("это не npdm: нет сигнатуры META")
        self.raw = raw
        self.aci_off, self.aci_size, self.acid_off, self.acid_size = struct.unpack_from("<IIII", raw, 0x70)
        for name, off, size, hdr in (("ACI0", self.aci_off, self.aci_size, 0x40),
                                     ("ACID", self.acid_off, self.acid_size, 0x240)):
            if off < META_HDR_SIZE or size < hdr or off + size > len(raw):
                raise ValueError(f"секция {name} не помещается в файл")

        self.acid = bytes(raw[self.acid_off:self.acid_off + self.acid_size])
        self.aci = bytes(raw[self.aci_off:self.aci_off + self.aci_size])
        if self.acid[0x200:0x204] != b"ACID":
            raise ValueError("нет сигнатуры ACID по смещению +0x200")
        if self.aci[:4] != b"ACI0":
            raise ValueError("нет сигнатуры ACI0")

        self.sections = {"ACID": self.acid, "ACI0": self.aci}
        self.blocks = {name: self._blocks(self.sections[name], name) for name in LAYOUT}

    @staticmethod
    def _blocks(sec, name):
        base, hdr = LAYOUT[name]
        out = {}
        for i, key in enumerate(("fs", "srv", "kern")):
            off, size = struct.unpack_from("<II", sec, base + i * 8)
            # то же, что ValidateSubregion в ldr_meta.cpp
            if off < hdr or off + size > len(sec):
                raise ValueError(f"{name}: подблок {key} выходит за пределы секции")
            out[key] = (off, size)
        if out["kern"][1] % 4:
            raise ValueError(f"{name}: размер kernel caps не кратен 4")
        return out

    def block(self, name, key):
        off, size = self.blocks[name][key]
        return self.sections[name][off:off + size]

    def caps(self, name):
        data = self.block(name, "kern")
        return list(struct.unpack(f"<{len(data)//4}I", data))

    def fs_mask(self, name):
        # FsAccessHeader (ACI0) и FsAccessControl (ACID): маска прав — u64 по +0x04
        return struct.unpack_from("<Q", self.block(name, "fs"), 4)[0]

    @property
    def program_id(self):
        return struct.unpack_from("<Q", self.aci, 0x10)[0]

    # ---- работа с возможностями ядра ----

    @staticmethod
    def syscalls_of(caps):
        """Разрешённые номера вызовов и прочие возможности (в исходном порядке)."""
        allowed, others = set(), []
        for v in caps:
            if (v & 0x1F) == CAP_SYSCALL:
                mask, group = (v >> 5) & 0xFFFFFF, v >> 29
                allowed |= {group * SYSCALLS_PER_GROUP + b for b in range(SYSCALLS_PER_GROUP)
                            if mask & (1 << b)}
            else:
                others.append(v)
        return allowed, others

    @staticmethod
    def syscall_words(caps):
        """Словарь группа -> слово-маска (для сверки ACI0 с ACID слово в слово)."""
        return {v >> 29: v for v in caps if (v & 0x1F) == CAP_SYSCALL}

    @staticmethod
    def add_syscalls(caps, svcs):
        """Дописывает номера в маски, сохраняя порядок остальных возможностей.

        Существующие группы дополняются на месте, отсутствующие добавляются в конец —
        так не разрывается пара слов MapRange (ldr_capabilities.cpp требует, чтобы они
        шли подряд) и не меняется порядок прочих возможностей.
        """
        need = {}
        for s in svcs:
            if not 0 <= s <= MAX_SVC:
                raise ValueError(f"номер вызова {s:#x} вне диапазона 0..{MAX_SVC:#x}")
            need.setdefault(s // SYSCALLS_PER_GROUP, 0)
            need[s // SYSCALLS_PER_GROUP] |= 1 << (s % SYSCALLS_PER_GROUP)
        out = list(caps)
        for i, v in enumerate(out):
            if (v & 0x1F) == CAP_SYSCALL and (v >> 29) in need:
                group = v >> 29
                mask = ((v >> 5) & 0xFFFFFF) | need.pop(group)
                out[i] = (group << 29) | (mask << 5) | CAP_SYSCALL
        for group in sorted(need):
            out.append((group << 29) | (need[group] << 5) | CAP_SYSCALL)
        return out

    # ---- пересборка ----

    def _rebuild_section(self, name, new_caps):
        sec, blocks = self.sections[name], self.blocks[name]
        base = LAYOUT[name][0]
        head_len = min(off for off, _ in blocks.values())
        body = bytearray(sec[:head_len])
        new_blocks = {}
        for key in sorted(blocks, key=lambda k: blocks[k][0]):
            off, size = blocks[key]
            data = struct.pack(f"<{len(new_caps)}I", *new_caps) if key == "kern" else sec[off:off + size]
            body += b"\0" * (align(len(body)) - len(body))
            new_blocks[key] = (len(body), len(data))
            body += data
        for i, key in enumerate(("fs", "srv", "kern")):
            struct.pack_into("<II", body, base + i * 8, *new_blocks[key])
        if name == "ACID":
            # размер, которым пользуется подпись: всё после подписи RSA (0x100 байт)
            struct.pack_into("<I", body, 0x204, len(body) - 0x100)
        return bytes(body)

    def with_syscalls(self, svcs):
        """Новый npdm с добавленными вызовами + отчёт {секция: (было, добавлено)}."""
        report, rebuilt = {}, {}
        for name in ("ACID", "ACI0"):
            caps = self.caps(name)
            allowed, _ = self.syscalls_of(caps)
            report[name] = (sorted(allowed), sorted(set(svcs) - allowed))
            rebuilt[name] = self._rebuild_section(name, self.add_syscalls(caps, svcs))

        # Секции кладутся в том же порядке, что были, с выравниванием на 0x10.
        order = sorted(("ACID", "ACI0"), key=lambda n: self.acid_off if n == "ACID" else self.aci_off)
        first_off = min(self.acid_off, self.aci_off)
        out = bytearray(self.raw[:first_off])       # шапка META и её выравнивание — как были
        offsets = {}
        for name in order:
            out += b"\0" * (align(len(out)) - len(out))
            offsets[name] = len(out)
            out += rebuilt[name]
        struct.pack_into("<IIII", out, 0x70,
                         offsets["ACI0"], len(rebuilt["ACI0"]),
                         offsets["ACID"], len(rebuilt["ACID"]))
        return bytes(out), report


def check(npdm, svcs, program_id=None, need_sdcard=True):
    """Гейт на готовый npdm. Возвращает список проблем (пусто — всё хорошо)."""
    problems = []
    words = {name: Npdm.syscall_words(npdm.caps(name)) for name in ("ACID", "ACI0")}
    if words["ACID"] != words["ACI0"]:
        problems.append("маски вызовов в ACI0 и ACID не совпадают слово в слово "
                        "(Atmosphère сверяет их точно — процесс не создастся)")
    for name in ("ACID", "ACI0"):
        allowed, _ = Npdm.syscalls_of(npdm.caps(name))
        missing = sorted(set(svcs) - allowed)
        if missing:
            problems.append(f"{name}: не разрешены вызовы " + ", ".join(f"{s:#x}" for s in missing))
        if need_sdcard and not npdm.fs_mask(name) & SDCARD_BIT:
            problems.append(f"{name}: в правах FS нет бита SdCard ({SDCARD_BIT:#x})")
    if program_id is not None and npdm.program_id != program_id:
        problems.append(f"program id {npdm.program_id:#018x}, ожидался {program_id:#018x}")
    return problems


def compare(src, new, svcs):
    """Сверка «новый npdm против исходного»: меняться должны только маски вызовов."""
    problems = []
    for name in ("ACID", "ACI0"):
        old_allowed, old_others = Npdm.syscalls_of(src.caps(name))
        new_allowed, new_others = Npdm.syscalls_of(new.caps(name))
        if not old_allowed <= new_allowed:
            problems.append(f"{name}: пропали разрешённые вызовы {sorted(old_allowed - new_allowed)}")
        if new_allowed - old_allowed - set(svcs):
            problems.append(f"{name}: добавились лишние вызовы "
                            f"{sorted(new_allowed - old_allowed - set(svcs))}")
        if old_others != new_others:
            problems.append(f"{name}: изменились прочие возможности ядра")
        for key in ("fs", "srv"):
            if src.block(name, key) != new.block(name, key):
                problems.append(f"{name}: изменился блок {key} (должен остаться как в игре)")
    if src.program_id != new.program_id:
        problems.append("изменился program id")
    if src.acid[0x208:0x220] != new.acid[0x208:0x220]:
        problems.append("изменились флаги ACID или диапазон program id")
    if src.acid[:0x200] != new.acid[:0x200]:
        problems.append("изменилась подпись/ключ ACID")
    if src.raw[:0x70] != new.raw[:0x70]:
        problems.append("изменилась шапка META")
    return problems


def patch_file(in_path, out_path, svcs):
    """Правка с гейтом: без записи, если что-то разошлось. Возвращает отчёт."""
    src = Npdm(Path(in_path).read_bytes())
    data, report = src.with_syscalls(svcs)
    new = Npdm(data)
    problems = compare(src, new, svcs) + check(new, svcs, program_id=src.program_id,
                                               need_sdcard=bool(src.fs_mask("ACI0") & SDCARD_BIT))
    if problems:
        raise ValueError("; ".join(problems))
    Path(out_path).write_bytes(data)
    return report, len(src.raw), len(data)


def describe(npdm, svcs):
    for name in ("ACID", "ACI0"):
        allowed, others = Npdm.syscalls_of(npdm.caps(name))
        groups = sorted({s // SYSCALLS_PER_GROUP for s in allowed})
        say(f"{name}: разрешено вызовов {len(allowed)}, группы {groups}, "
            f"прочих возможностей {len(others)}, FS-права {npdm.fs_mask(name):#018x}"
            f"{' (SdCard есть)' if npdm.fs_mask(name) & SDCARD_BIT else ' (SdCard НЕТ)'}")
        say("   нужные нам: " + ", ".join(f"{s:#x}" + ("" if s in allowed else " — НЕТ") for s in svcs))
    say(f"program id: {npdm.program_id:#018x}")


def _with_sdcard_bit(npdm):
    """Копия npdm с битом SdCard в правах FS обеих секций (для самопроверки)."""
    raw = bytearray(npdm.raw)
    for name, sec_off in (("ACI0", npdm.aci_off), ("ACID", npdm.acid_off)):
        fo, _ = npdm.blocks[name]["fs"]
        struct.pack_into("<Q", raw, sec_off + fo + 4, npdm.fs_mask(name) | SDCARD_BIT)
    return Npdm(bytes(raw))


def self_test(npdm_path=None):
    """Негативные пробы: гейт обязан краснеть там, где должен.

    Нужен main.npdm ЛЮБОЙ игры (из своего дампа): путь передаётся позиционным
    аргументом или переменной среды NXR_GAME_NPDM. Если файла нет, самопроверка
    КРАСНЕЕТ (код 2), а не «пропускается»: гейт, который ничего не проверил, не
    должен выглядеть пройденным.
    """
    src_path = npdm_path or os.environ.get("NXR_GAME_NPDM")
    src_path = Path(src_path) if src_path else None
    if src_path is None or not src_path.is_file():
        say("НЕЧЕМ ПРОВЕРЯТЬ: нужен main.npdm игры.")
        say("  python npdm_syscalls.py <main.npdm> --self-test")
        say("  или: NXR_GAME_NPDM=<путь> python npdm_syscalls.py --self-test")
        return 2
    src = Npdm(src_path.read_bytes())

    # В main.npdm из дампа игры бита SdCard нет — его ставит ДРУГОЙ скрипт
    # (npdm_sdcard.py). Проба 2 («правленый npdm проходит») требует этот бит, а
    # проба 5 («снят бит SdCard») без него вырождается: снимать нечего. Поэтому
    # пробы гоняются на копии, где бит уже выставлен, — ровно так, как файл
    # придёт сюда в настоящем конвейере. Исходный файл не изменяется.
    src = _with_sdcard_bit(src)
    ok = True
    ran = 0

    def probe(label, condition):
        nonlocal ok, ran
        ran += 1
        say(f"      проба «{label}» — " + ("поймана" if condition else "НЕ поймана"))
        ok = ok and condition

    # 1) исходный npdm (без вызовов) гейт обязан отвергнуть
    probe("исходный npdm без 0x74/0x75", bool(check(src, DEFAULT_SVCS)))
    # 2) правленый — принять
    data, _ = src.with_syscalls(DEFAULT_SVCS)
    new = Npdm(data)
    probe("правленый npdm проходит", not check(new, DEFAULT_SVCS, program_id=src.program_id)
          and not compare(src, new, DEFAULT_SVCS))
    # 3) пустой список вызовов обязан дать файл байт в байт
    same, _ = src.with_syscalls([])
    probe("пересборка без правок даёт тот же файл", same == src.raw)
    # 4) расхождение ACI0/ACID гейт обязан заметить: в ACI0 оставляем только 0x74
    skewed = bytearray(data)
    ko, _ = new.blocks["ACI0"]["kern"]
    caps = new.caps("ACI0")
    for i, v in enumerate(caps):
        if (v & 0x1F) == CAP_SYSCALL and v >> 29 == DEFAULT_SVCS[0] // SYSCALLS_PER_GROUP:
            caps[i] = (v >> 29 << 29) | ((1 << (DEFAULT_SVCS[0] % SYSCALLS_PER_GROUP)) << 5) | CAP_SYSCALL
    struct.pack_into(f"<{len(caps)}I", skewed, new.aci_off + ko, *caps)
    probe("маски ACI0 и ACID разошлись", bool(check(Npdm(bytes(skewed)), DEFAULT_SVCS)))
    # 5) снятый бит SdCard гейт обязан заметить
    no_sd = bytearray(data)
    fo, _ = new.blocks["ACI0"]["fs"]
    struct.pack_into("<Q", no_sd, new.aci_off + fo + 4, new.fs_mask("ACI0") & ~SDCARD_BIT)
    probe("снят бит SdCard", bool(check(Npdm(bytes(no_sd)), DEFAULT_SVCS)))
    # 6) чужой program id
    probe("чужой program id", bool(check(new, DEFAULT_SVCS, program_id=0x0102030405060708)))
    # Затвор нуля: самопроверка, не запустившая ни одной пробы, о гейте ничего не
    # доказала и пройденной считаться не может (тот же код 2, что и «нечем проверять»).
    if not ran:
        say("НИ ОДНА ПРОБА НЕ ЗАПУЩЕНА — проверять гейт нечем")
        return 2
    say("      пробы гейта npdm: запущено %d, %s" % (ran, "все сработали" if ok
                                                     else "ЕСТЬ НЕ СРАБОТАВШИЕ"))
    return 0 if ok else 1


def main():
    setup_stdio()
    ap = argparse.ArgumentParser(description="Права на системные вызовы в main.npdm")
    ap.add_argument("npdm", type=Path, nargs="?")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--svc", type=lambda s: int(s, 0), nargs="*", default=DEFAULT_SVCS)
    ap.add_argument("--check", action="store_true", help="только проверить, ничего не писать")
    ap.add_argument("--program-id", type=lambda s: int(s, 16), help="ожидаемый program id (hex)")
    ap.add_argument("--no-sdcard-check", action="store_true", help="не требовать бита SdCard")
    ap.add_argument("--self-test", action="store_true", help="негативные пробы гейта")
    args = ap.parse_args()

    if args.self_test:
        return self_test(args.npdm)
    if args.npdm is None:
        ap.error("нужен путь к main.npdm")
    # Нет входа — НАХОДКА гейта, а не его авария (иначе FileNotFoundError обрывал
    # прогон трассировкой Python, и код возврата 1 ничего не говорил о причине).
    if not args.npdm.is_file():
        say("ОТКАЗ: нет файла main.npdm: " + str(args.npdm))
        return 2

    npdm = Npdm(args.npdm.read_bytes())
    if args.check:
        describe(npdm, args.svc)
        problems = check(npdm, args.svc, args.program_id, not args.no_sdcard_check)
        for p in problems:
            say("ПРОБЛЕМА: " + p)
        say("гейт npdm: " + ("НЕ ПРОЙДЕН" if problems else "пройден"))
        return 1 if problems else 0

    out_path = args.out or args.npdm.with_suffix(args.npdm.suffix + ".svc")
    report, old_len, new_len = patch_file(args.npdm, out_path, args.svc)
    for name, (allowed, added) in report.items():
        say(f"{name}: было разрешено {len(allowed)}, добавлено " +
            (", ".join(f"{s:#x}" for s in added) if added else "ничего"))
    say(f"записан {out_path} ({new_len} Б, исходный {old_len} Б)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ValueError as e:
        say("ОТКАЗ: " + str(e))
        sys.exit(2)
