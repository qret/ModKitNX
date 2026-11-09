#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_nso_hash — сверка ГОТОВОГО МОДУЛЯ с оригиналом, конец в конец.

run_hashtest.py сверяет исходник, собранный обычным g++ на ПК. Здесь проверяется
то, что реально поедет на консоль: берётся собранный NSO модуля, его сегменты
кладутся в эмулятор ARM64, вызывается экспорт _ZNSt3__113__hash_memoryEPKvm по
адресу из .dynsym, и значение сравнивается с настоящим кодом того же символа из
NintendoSdk_nnSdk-22_2_0-Release, выполненным там же.

Это ловит то, что проверка по исходнику поймать не может: неверный адрес символа,
выброшенное тело (например, экспорт длиной 4 байта, оказавшийся переходом в
никуда), сломанную сборку.

Заглушка LDN проверяется так же: вызывается её экспорт, ожидается 0 (успех).

Запуск:
    python run_nso_hash.py <nso-модуля> [--sdk <NintendoSdk_nnSdk-22_2_0-Release>]
    python run_nso_hash.py <nso> --self-test    # негативные пробы
"""

import argparse
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import nso_min

# Эталон nnSdk 22.2.0: --sdk, NXR_REF_SDK или папка $NXR_SDK_DIR (путь не зашит).
DEFAULT_SDK = Path(os.environ.get("NXR_SDK_DIR") or "NXR_SDK_DIR-не-задана") / "NintendoSdk_nnSdk-22_2_0-Release"
SYM_HASH = "_ZNSt3__113__hash_memoryEPKvm"
SYM_LDN = "_ZN2nn3ldn11SetProtocolENS0_8ProtocolE"


def setup_stdio():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def say(msg=""):
    print(msg, flush=True)


class Emu:
    """Один образ NSO в эмуляторе + вызов экспорта по имени."""

    def __init__(self, path):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
        self.path = str(path)
        self.m = nso_min.NSO(self.path)
        self.base, self.stack, self.data, self.ret = (0x100000000, 0x200000000,
                                                      0x300000000, 0x400000000)
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        size = ((self.m.DMEM + self.m.DSZ + self.m.bss + 0xFFFF) // 0x10000) * 0x10000
        uc.mem_map(self.base, max(size, 0x10000))
        uc.mem_write(self.base + self.m.TMEM, self.m.text)
        uc.mem_write(self.base + self.m.RMEM, self.m.ro)
        uc.mem_write(self.base + self.m.DMEM, self.m.data)
        uc.mem_map(self.stack, 0x100000)
        uc.mem_map(self.data, 0x8000)
        uc.mem_map(self.ret, 0x1000)
        self.uc = uc
        self.exports = self.m.exports()
        # Экспорты модуля — автоисправление: при первом вызове они
        # обходят процесс через svcQueryMemory (svc 0x6). Здесь в «процессе»
        # нет ни одного модуля (вся память — неотображённая), sdk не находится,
        # и модуль работает СВОЕЙ реализацией — её и сверяем с оригиналом.
        # Выбор «своя/прокси» на настоящих sdk проверяет run_autofix.py.
        from unicorn import UC_HOOK_INTR
        uc.hook_add(UC_HOOK_INTR, self._on_svc)
        self._relocate()

    def _relocate(self):
        """Связать образ, как rtld: RELATIVE и ссылки на СВОИ символы (GOT).
        Без этого экспорт с обращением к своей глобальной переменной через GOT
        читал бы по нулевому адресу. Чужие символы — ноль, как без модуля."""
        import struct
        n, base = self.m, self.base
        syms = n.syms()
        for tag_a, tag_s in ((7, 8), (23, 2)):
            if tag_a not in n.DT:
                continue
            at, sz = n.DT[tag_a][0], n.DT[tag_s][0]
            for k in range(0, sz, 24):
                off, info, add = struct.unpack("<QQq", n.mem(at + k, 24))
                t, si = info & 0xFFFFFFFF, info >> 32
                if t == 1027:
                    val = base + add
                elif t in (257, 1025, 1026):
                    _i, _nm, _inf, _o, shndx, value, _sz = syms[si]
                    if shndx == 0 or value == 0:
                        continue
                    val = base + value + (add if t == 257 else 0)
                else:
                    continue
                self.uc.mem_write(base + off, struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF))

    def _on_svc(self, uc, _intno, _ud):
        import struct
        from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                         UC_ARM64_REG_X2)
        out = uc.reg_read(UC_ARM64_REG_X0)
        addr = uc.reg_read(UC_ARM64_REG_X2)
        top = 1 << 39
        start, size = (0, top) if addr < top else (addr, 0)
        uc.mem_write(out, struct.pack("<QQIIIIII", start, size, 0, 0, 0, 0, 0, 0))
        uc.reg_write(UC_ARM64_REG_X0, 0)
        uc.reg_write(UC_ARM64_REG_X1, 0)

    def has(self, name):
        return name in self.exports

    def call_hash(self, payload, name=SYM_HASH):
        from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                         UC_ARM64_REG_SP, UC_ARM64_REG_LR)
        addr = self.exports[name][0]
        self.uc.mem_write(self.data, b"\x00" * 0x8000)
        self.uc.mem_write(self.data, payload)
        self.uc.reg_write(UC_ARM64_REG_X0, self.data)
        self.uc.reg_write(UC_ARM64_REG_X1, len(payload))
        self.uc.reg_write(UC_ARM64_REG_SP, self.stack + 0x80000)
        self.uc.reg_write(UC_ARM64_REG_LR, self.ret)
        self.uc.emu_start(self.base + addr, self.ret)
        return self.uc.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFFFFFFFFFF

    def call_ldn(self, protocol):
        from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_SP,
                                         UC_ARM64_REG_LR)
        addr = self.exports[SYM_LDN][0]
        self.uc.reg_write(UC_ARM64_REG_X0, protocol)
        self.uc.reg_write(UC_ARM64_REG_SP, self.stack + 0x80000)
        self.uc.reg_write(UC_ARM64_REG_LR, self.ret)
        self.uc.emu_start(self.base + addr, self.ret)
        return self.uc.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFF


def cases():
    """Те же входы, что и в hash_test.cpp, плюс строка camera_create."""
    buf = bytearray(8192)
    s = 123456789
    for i in range(len(buf)):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        buf[i] = (s >> 16) & 0xFF
    buf = bytes(buf)
    out = [("camera_create", b"camera_create")]
    for ln in range(0, 65):
        out.append(("buf0+%d" % ln, buf[:ln]))
    for off in (1, 3, 7, 9):
        for ln in (0, 1, 4, 8, 9, 13, 16, 17, 31, 32, 33, 64, 65):
            out.append(("buf%d+%d" % (off, ln), buf[off:off + ln]))
    for ln in (65, 96, 127, 128, 129, 255, 256, 257, 1000, 4096, 8000):
        out.append(("buf0+%d" % ln, buf[:ln]))
        out.append(("buf5+%d" % ln, buf[5:5 + ln]))
    return out


def compare(mod, ref, quiet=False):
    bad = []
    for label, payload in cases():
        got = mod.call_hash(payload)
        want = ref.call_hash(payload)
        if got != want:
            bad.append((label, got, want))
    if not quiet:
        for label, got, want in bad[:10]:
            say("   РАСХОЖДЕНИЕ %s: модуль %016x, оригинал %016x" % (label, got, want))
    return bad


def main():
    setup_stdio()
    ap = argparse.ArgumentParser(description="Сверка готового NSO с оригиналом")
    ap.add_argument("nso")
    ap.add_argument("--sdk", default=os.environ.get("NXR_REF_SDK", str(DEFAULT_SDK)))
    ap.add_argument("--self-test", action="store_true",
                    help="негативные пробы: порча кода модуля обязана покраснеть")
    args = ap.parse_args()

    try:
        import unicorn  # noqa: F401
    except ImportError:
        raise SystemExit("нет пакета unicorn: `pip install unicorn`")

    ref = Emu(args.sdk)
    mod = Emu(args.nso)
    say("модуль: %s (экспортов %d)" % (Path(args.nso).name, len(mod.exports)))
    say("эталон: BuildId %s" % ref.m.buildid[:40])

    rc = 0
    if not mod.has(SYM_HASH):
        say("в модуле нет %s — сверять нечего (это обычная сборка?)" % SYM_HASH)
        rc = 1
    else:
        bad = compare(mod, ref)
        say("хеш: сошлось %d из %d" % (len(cases()) - len(bad), len(cases())))
        if bad:
            rc = 1

    if mod.has(SYM_LDN):
        codes = [mod.call_ldn(p) for p in (0, 1, 3, 7)]
        say("заглушка LDN вернула: %s" % codes)
        if any(c != 0 for c in codes):
            say("   ПРЕТЕНЗИЯ: заглушка должна возвращать 0 (успех)")
            rc = 1

    if args.self_test and mod.has(SYM_HASH):
        say("--- негативные пробы по готовому NSO ---")
        # Проба 1: сверяем модуль сам с собой — обязано быть зелено (затвор смысла).
        bad = compare(mod, mod, quiet=True)
        say("   сам с собой: расхождений %d (ожидалось 0)" % len(bad))
        if bad:
            rc = 1
        # Проба 2: портим один байт кода по адресу экспорта — обязано покраснеть.
        from unicorn.arm64_const import UC_ARM64_REG_X0  # noqa: F401
        addr = mod.base + mod.exports[SYM_HASH][0]
        saved = mod.uc.mem_read(addr, 4)
        mod.uc.mem_write(addr, b"\xc0\x03\x5f\xd6")  # ret: функция ничего не считает
        # Без сброса кэша трансляции Unicorn продолжал бы выполнять СТАРЫЙ код, и
        # проба осталась бы зелёной при уже испорченной памяти.
        mod.uc.ctl_remove_cache(addr, addr + 8)
        bad = compare(mod, ref, quiet=True)
        mod.uc.mem_write(addr, bytes(saved))
        mod.uc.ctl_remove_cache(addr, addr + 8)
        if bad:
            say("   подменили тело на `ret`: КРАСНОЕ (расхождений %d). Хорошо." % len(bad))
        else:
            say("   подменили тело на `ret`, но сверка осталась ЗЕЛЁНОЙ — она ничего не ловит!")
            rc = 1
        # Проба 3 (затвор нуля): проверяем, что эталон выдаёт не одни нули.
        vals = {ref.call_hash(p) for _l, p in cases()[:20]}
        if len(vals) > 1 and 0 not in vals:
            say("   затвор нуля: у эталона %d разных ненулевых значений. Хорошо." % len(vals))
        else:
            say("   затвор нуля: эталон выдаёт вырожденные значения — сверка бессмысленна!")
            rc = 1

    say("ИТОГ: " + ("сошлось" if rc == 0 else "ЕСТЬ ПРОВАЛЫ"))
    sys.exit(rc)


if __name__ == "__main__":
    main()
