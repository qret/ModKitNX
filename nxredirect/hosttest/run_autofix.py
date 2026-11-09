#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_autofix — сверка АВТОИСПРАВЛЕНИЯ (src/nxr_autofix.hpp, nxr_compat_exports.hpp)
на НАСТОЯЩИХ библиотеках, конец в конец.

Что делается. В эмуляторе ARM64 (Unicorn) собирается процесс так, как его
раскладывает загрузчик Switch: модули игры из её exefs (rtld, main, subsdk*),
НАШ собранный модуль (subsdk9) и библиотека `sdk` — по очереди НАСТОЯЩИЕ
nnSdk 19.3.13, 20.5.17, 22.2.0 из папки $NXR_SDK_DIR (только чтение). Каждый
модуль — три области (код Rx, rodata R, данные Rw) с дырами между модулями;
svcQueryMemory (svc 0x6) отвечает по этой раскладке. Наш модуль связывается
как это делает rtld (RELATIVE, GLOB_DAT, JUMP_SLOT, ABS64).

Затем вызываются ЭКСПОРТЫ модуля по адресам из его .dynsym — ровно то, что
поедет на консоль, — и смотрится, КУДА ушёл вызов:
  - __hash_memory: в sdk 22.2.0 ушёл в её функцию (вход в sdk пойман), в
    19.3.13/20.5.17 — своя; значение сверяется с НАСТОЯЩЕЙ 22.2.0;
  - nn::ldn::SetProtocol: вход в sdk перехватывается, аргумент сверяется;
  - NvRmMemQueryHandleParams: настоящая 22.2.0 — перехват входа, регистры
    x0..x3 сверяются (соглашение о вызове прокси); заглушка — выполняется как
    есть (ответ 2); своя — зовёт NvRm_MemmgrGetIoctlFile и NvOsDrvIoctl этой
    же sdk (перехват: fd 7, ioctl кладёт размер), сверяются аргументы ioctl и
    байты out.
Решение модуля читается из его экспорта nxr_autofix_state_v1 (choice/why/цель):
цель прокси обязана быть адресом символа ИМЕННО в модуле sdk.

Сценарии — таблица EXPECT ниже; главное:
  Well Dweller + 19.3.13  -> hash СВОЯ, ldn СВОЯ, nvrm СВОЯ   (как работает сейчас)
  SiN Reloaded + 19.3.13  -> hash СВОЯ, ldn СВОЯ, nvrm ПРОКСИ в заглушку (Vulkan)
                             — ровно то, что SiN получает сейчас (код 2)
  любая игра + 22.2.0     -> все три ПРОКСИ (родная библиотека не перекрыта)

Негативные пробы (--self-test): порча ВХОДА (не кода модуля) обязана сделать
настоящую проверку КРАСНОЙ, а ослабленная копия проверки (смотрит только, что
вызов вернулся) на том же входе остаётся ЗЕЛЁНОЙ — так видно, что ловит именно
проверка пути. Плюс затвор нуля: модуль без автоисправления — красный.

Запуск:
    python run_autofix.py <наш subsdk9> --exefs-root <папка с WD-patch, SIN-patch, TOTK-patch>
    python run_autofix.py <наш subsdk9> --exefs-root <...> --self-test [--noautofix <nso>]
Код возврата: 0 — всё как надо, 1 — расхождение, 2 — нечем проверять.
Ничего не пишет и не удаляет.
"""

import argparse
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import nso_min  # noqa: E402

SDK_DIR = Path(os.environ.get("NXR_SDK_DIR") or "NXR_SDK_DIR-не-задана")
SDKS = {
    "19.3.13": "NintendoSdk_nnSdk-19_3_13-Release",
    "20.5.17": "NintendoSdk_nnSdk-20_5_17-Release",
    "22.2.0": "NintendoSdk_nnSdk-22_2_0-Release",
}
SYM_HASH = "_ZNSt3__113__hash_memoryEPKvm"
SYM_LDN = "_ZN2nn3ldn11SetProtocolENS0_8ProtocolE"
SYM_NVRM = "NvRmMemQueryHandleParams"
SYM_GETFD = "NvRm_MemmgrGetIoctlFile"
SYM_IOCTL = "NvOsDrvIoctl"
MARKER = "nxr_autofix_state_v1"
FIXES = ["hash_memory", "ldn_set_protocol", "nvrm_query"]
FIX_SYM = {"hash_memory": SYM_HASH, "ldn_set_protocol": SYM_LDN, "nvrm_query": SYM_NVRM}

CHOICE = {0: "unresolved", 1: "own", 2: "proxy"}
WHY = {0: "None", 1: "SdkMissing", 2: "SdkDefined", 3: "SdkReal", 4: "StubOlderSdk",
       5: "StubNative", 6: "StubVulkan", 7: "StubNoVersion", 8: "NoSdkMissing",
       9: "NoSdkDefined", 10: "TargetTooFar"}

# (игра, sdk) -> {исправление: (choice, why)}
EXPECT = {
    ("WD-patch", "19.3.13"): {"hash_memory": ("own", "SdkMissing"),
                              "ldn_set_protocol": ("own", "SdkMissing"),
                              "nvrm_query": ("own", "StubOlderSdk")},
    ("WD-patch", "20.5.17"): {"hash_memory": ("own", "SdkMissing"),
                              "ldn_set_protocol": ("proxy", "SdkDefined"),
                              "nvrm_query": ("own", "StubOlderSdk")},
    ("WD-patch", "22.2.0"): {"hash_memory": ("proxy", "SdkDefined"),
                             "ldn_set_protocol": ("proxy", "SdkDefined"),
                             "nvrm_query": ("proxy", "SdkReal")},
    ("SIN-patch", "19.3.13"): {"hash_memory": ("own", "SdkMissing"),
                               "ldn_set_protocol": ("own", "SdkMissing"),
                               "nvrm_query": ("proxy", "StubVulkan")},
    ("SIN-patch", "20.5.17"): {"hash_memory": ("own", "SdkMissing"),
                               "ldn_set_protocol": ("proxy", "SdkDefined"),
                               "nvrm_query": ("proxy", "StubVulkan")},
    ("SIN-patch", "22.2.0"): {"hash_memory": ("proxy", "SdkDefined"),
                              "ldn_set_protocol": ("proxy", "SdkDefined"),
                              "nvrm_query": ("proxy", "SdkReal")},
    # Родная старая игра (TotK, библиотеки 15.3.1) на sdk НОВЕЕ себя: заглушка
    # NvRm — её родное поведение, своя включаться НЕ должна.
    ("TOTK-patch", "19.3.13"): {"hash_memory": ("own", "SdkMissing"),
                                "ldn_set_protocol": ("own", "SdkMissing"),
                                "nvrm_query": ("proxy", "StubNative")},
}

PAGE = 0x1000
SENT_LDN = 0x0C0DE000
SENT_NVRM = 0xBEEF
NVRM_FD = 7
NVRM_VALUE = 0x3FC000


def say(msg=""):
    print(msg, flush=True)


def setup_stdio():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def page_up(v):
    return (v + PAGE - 1) & ~(PAGE - 1)


class Proc:
    """Процесс из нескольких NSO в эмуляторе + svcQueryMemory по раскладке."""

    BASE = 0x1000000000
    GAP = 0x200000

    def __init__(self, modules, patches=()):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR, UC_PROT_ALL
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.mods = []          # dict(name, nso, base, end, exports)
        self.regions = []       # (start, size, type, perm)
        at = self.BASE
        for name, path in modules:
            n = nso_min.NSO(str(path))
            tsz = n.RMEM - n.TMEM
            rsz = n.DMEM - n.RMEM
            dsz = page_up(n.DSZ + n.bss)
            total = n.DMEM + dsz
            self.uc.mem_map(at, total, UC_PROT_ALL)
            self.uc.mem_write(at + n.TMEM, n.text)
            self.uc.mem_write(at + n.RMEM, n.ro)
            self.uc.mem_write(at + n.DMEM, n.data)
            self.regions += [(at + n.TMEM, tsz, 3, 5), (at + n.RMEM, rsz, 3, 1),
                             (at + n.DMEM, dsz, 4, 3)]
            self.mods.append({"name": name, "nso": n, "base": at, "end": at + total,
                              "exports": n.exports()})
            at = page_up(at + total) + self.GAP
        # Служебные области: стек, данные, точка возврата (тип Normal — не модуль).
        self.stack, self.data, self.ret = at, at + 0x200000, at + 0x300000
        self.uc.mem_map(self.stack, 0x100000, UC_PROT_ALL)
        self.uc.mem_map(self.data, 0x10000, UC_PROT_ALL)
        self.uc.mem_map(self.ret, PAGE, UC_PROT_ALL)
        self.uc.mem_write(self.ret, b"\xc0\x03\x5f\xd6")
        self.regions += [(self.stack, 0x100000, 5, 3), (self.data, 0x10000, 5, 3),
                         (self.ret, PAGE, 5, 5)]
        self.regions.sort()
        for (mod_name, off, data) in patches:
            m = self.mod(mod_name)
            self.uc.mem_write(m["base"] + off, data)
        self.uc.hook_add(UC_HOOK_INTR, self._on_intr)
        self.svc_calls = 0

    def mod(self, name):
        for m in self.mods:
            if m["name"] == name:
                return m
        raise KeyError(name)

    def query(self, addr):
        prev_end = 0
        for (s, sz, t, p) in self.regions:
            if s <= addr < s + sz:
                return s, sz, t, p
            if addr < s:
                return prev_end, s - prev_end, 0, 0
            prev_end = s + sz
        top = 1 << 39
        if addr < top:
            return prev_end, top - prev_end, 0, 0
        return addr, 0, 0, 0

    def _on_intr(self, uc, intno, _ud):
        from unicorn.arm64_const import (UC_ARM64_REG_PC, UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                         UC_ARM64_REG_X2)
        pc = uc.reg_read(UC_ARM64_REG_PC)
        insn = struct.unpack("<I", bytes(uc.mem_read(pc - 4, 4)))[0]
        if (insn & 0xFFE0001F) != 0xD4000001:
            insn = struct.unpack("<I", bytes(uc.mem_read(pc, 4)))[0]
        imm = (insn >> 5) & 0xFFFF
        self.svc_calls += 1
        if imm == 0x6:
            out = uc.reg_read(UC_ARM64_REG_X0)
            addr = uc.reg_read(UC_ARM64_REG_X2)
            s, sz, t, p = self.query(addr)
            uc.mem_write(out, struct.pack("<QQIIIIII", s, sz, t, 0, p, 0, 0, 0))
            uc.reg_write(UC_ARM64_REG_X0, 0)
            uc.reg_write(UC_ARM64_REG_X1, 0)
        else:
            uc.reg_write(UC_ARM64_REG_X0, 0xE401)

    def relocate(self, name):
        """Связать модуль так, как это делает rtld (первое определение по адресу)."""
        m = self.mod(name)
        n, base = m["nso"], m["base"]
        syms = n.syms()
        for tag_a, tag_s in ((7, 8), (23, 2)):
            if tag_a not in n.DT:
                continue
            at, sz = n.DT[tag_a][0], n.DT[tag_s][0]
            for k in range(0, sz, 24):
                off, info, add = struct.unpack("<QQq", n.mem(at + k, 24))
                t, si = info & 0xFFFFFFFF, info >> 32
                if t == 1027:                       # RELATIVE
                    val = base + add
                elif t in (257, 1025, 1026):        # ABS64, GLOB_DAT, JUMP_SLOT
                    _i, nm, info_s, _o, shndx, value, _size = syms[si]
                    if shndx != 0 and value != 0:
                        val = base + value
                    else:
                        val = self.rtld_lookup(nm)
                    if t == 257:
                        val += add
                else:
                    continue
                self.uc.mem_write(base + off, struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF))

    def rtld_lookup(self, nm):
        for m in self.mods:
            e = m["exports"].get(nm)
            if e is not None and (e[2] >> 4) != 0 and e[0] != 0:
                return m["base"] + e[0]
        return 0

    def addr_of(self, mod_name, sym):
        m = self.mod(mod_name)
        e = m["exports"].get(sym)
        return (m["base"] + e[0]) if e else 0

    def call(self, addr, args, limit=80_000_000):
        from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                         UC_ARM64_REG_X3, UC_ARM64_REG_SP, UC_ARM64_REG_LR,
                                         UC_ARM64_REG_PC)
        regs = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3]
        for r, v in zip(regs, args):
            self.uc.reg_write(r, v)
        self.uc.reg_write(UC_ARM64_REG_SP, self.stack + 0xF0000)
        self.uc.reg_write(UC_ARM64_REG_LR, self.ret)
        self.uc.emu_start(addr, self.ret, count=limit)
        if self.uc.reg_read(UC_ARM64_REG_PC) != self.ret:
            raise RuntimeError("вызов не вернулся за %d инструкций (зацикливание?)" % limit)
        return self.uc.reg_read(UC_ARM64_REG_X0)


class Watch:
    """Перехваты входов в функции sdk (и только их — без хука на каждый шаг)."""

    def __init__(self, proc, sdk_name="sdk"):
        from unicorn import UC_HOOK_CODE
        self.p = proc
        self.hits = {}
        self.log = {}
        sdk = proc.mod(sdk_name)
        self.addr = {s: proc.addr_of(sdk_name, s)
                     for s in (SYM_HASH, SYM_LDN, SYM_NVRM, SYM_GETFD, SYM_IOCTL)}
        self.nvrm_is_stub = False
        a = self.addr[SYM_NVRM]
        if a:
            w0, w1 = struct.unpack("<II", bytes(proc.uc.mem_read(a, 8)))
            self.nvrm_is_stub = (w1 == 0xD65F03C0 and (w0 & 0xFF80001F) == 0x52800000)
        for s, a in self.addr.items():
            if a:
                proc.uc.hook_add(UC_HOOK_CODE, self._make(s), begin=a, end=a)
        self.sdk = sdk

    def reset(self):
        self.hits = {}
        self.log = {}

    def _make(self, sym):
        def cb(uc, addr, _size, _ud):
            from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                             UC_ARM64_REG_X2, UC_ARM64_REG_X3,
                                             UC_ARM64_REG_LR, UC_ARM64_REG_PC)
            self.hits[sym] = self.hits.get(sym, 0) + 1
            x = [uc.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                          UC_ARM64_REG_X2, UC_ARM64_REG_X3)]
            lr = uc.reg_read(UC_ARM64_REG_LR)

            def ret(v):
                uc.reg_write(UC_ARM64_REG_X0, v)
                uc.reg_write(UC_ARM64_REG_PC, lr)

            if sym == SYM_LDN:
                self.log[sym] = x[0] & 0xFFFFFFFF
                ret(SENT_LDN | (x[0] & 0xFFF))
            elif sym == SYM_NVRM and not self.nvrm_is_stub:
                self.log[sym] = (x[0] & 0xFFFFFFFF, x[1], x[2], x[3] & 0xFFFFFFFF)
                ret(SENT_NVRM)
            elif sym == SYM_GETFD:
                ret(NVRM_FD)
            elif sym == SYM_IOCTL:
                arg = bytes(uc.mem_read(x[2], 12))
                self.log[sym] = (x[0] & 0xFFFFFFFF, x[1] & 0xFFFFFFFF, arg, x[3] & 0xFFFFFFFF)
                uc.mem_write(x[2] + 8, struct.pack("<I", NVRM_VALUE))
                ret(0)
            # __hash_memory и заглушка NvRm выполняются как есть.
        return cb


def hash_cases():
    import random
    rnd = random.Random(20261004)
    out = [b"camera_create", b"", b"a", b"abcd", b"abcdefgh", b"0123456789abcdef"]
    for ln in (3, 7, 9, 16, 17, 31, 32, 33, 63, 64, 65, 100, 255, 1000):
        out.append(bytes(rnd.getrandbits(8) for _ in range(ln)))
    return out


def reference_hashes():
    from run_nso_hash import Emu
    ref = Emu(SDK_DIR / SDKS["22.2.0"])
    return [ref.call_hash(p) for p in hash_cases()]


def game_modules(exefs):
    names = ["rtld", "main"] + ["subsdk%d" % i for i in range(9)]
    return [(nm, exefs / nm) for nm in names if (exefs / nm).is_file()]


def build_proc(module, exefs, sdk_path, patches=(), twin=False):
    mods = game_modules(exefs)
    if twin:
        mods.append(("subsdk8", module))
    mods.append(("subsdk9", module))
    mods.append(("sdk", sdk_path))
    p = Proc(mods, patches)
    p.relocate("subsdk9")
    if twin:
        p.relocate("subsdk8")
    return p


def read_state(p, mod="subsdk9"):
    a = p.addr_of(mod, MARKER)
    if not a:
        return None
    words = struct.unpack("<QQQ", bytes(p.uc.mem_read(a, 24)))
    out = {}
    for fix, w in zip(FIXES, words):
        out[fix] = (CHOICE.get((w >> 48) & 0xFF, "?"), WHY.get((w >> 56) & 0xFF, "?"),
                    w & ((1 << 48) - 1))
    return out


def run_scenario(module, exefs, sdk_ver, refs, patches=(), twin=False, weak=False,
                 expect=None, quiet=False):
    """Один сценарий. Возвращает (список претензий, число проверок)."""
    bad, checks = [], 0
    sdk_path = SDK_DIR / SDKS[sdk_ver]
    p = build_proc(module, exefs, sdk_path, patches, twin)
    w = Watch(p)
    ours = {s: p.addr_of("subsdk9", s) for s in (SYM_HASH, SYM_LDN, SYM_NVRM)}
    if not all(ours.values()) or not p.addr_of("subsdk9", MARKER):
        return ["в модуле нет экспортов автоисправления (сборка без него?)"], 1

    # --- __hash_memory
    w.reset()
    try:
        for payload, ref in zip(hash_cases(), refs):
            p.uc.mem_write(p.data, b"\0" * 0x1000)
            p.uc.mem_write(p.data, payload)
            got = p.call(ours[SYM_HASH], [p.data, len(payload)])
            checks += 1
            if got != ref:
                bad.append("hash %r: %016x, у 22.2.0 %016x" % (payload[:16], got, ref))
                break
    except Exception as e:
        return ["hash: вызов упал: %s" % e], checks + 1
    hash_entered = w.hits.get(SYM_HASH, 0) > 0

    # --- SetProtocol
    w.reset()
    try:
        ldn_rc = p.call(ours[SYM_LDN], [3]) & 0xFFFFFFFF
    except Exception as e:
        return bad + ["ldn: вызов упал: %s" % e], checks + 1
    ldn_entered, ldn_arg = w.hits.get(SYM_LDN, 0) > 0, w.log.get(SYM_LDN)

    # --- NvRmMemQueryHandleParams
    w.reset()
    out = p.data + 0x800
    p.uc.mem_write(out, b"\xAA" * 0x38)
    try:
        nv_rc = p.call(ours[SYM_NVRM], [0x2A, 0x1111, out, 0x38]) & 0xFFFFFFFF
    except Exception as e:
        return bad + ["nvrm: вызов упал: %s" % e], checks + 1
    out_bytes = bytes(p.uc.mem_read(out, 0x18))
    nv_hits = dict(w.hits)
    nv_log = dict(w.log)

    if weak:
        # ОСЛАБЛЕННАЯ проверка: вызовы вернулись, хеш верный — и всё.
        return bad, checks + 1

    state = read_state(p)
    exp = expect if expect is not None else EXPECT[(exefs.name, sdk_ver)]
    for fix in FIXES:
        checks += 1
        got = state[fix][:2] if state else None
        if got != exp[fix]:
            bad.append("%s: решение %s, ждали %s" % (fix, got, exp[fix]))
        elif got[0] == "proxy":
            checks += 1
            want = w.addr[FIX_SYM[fix]]
            if state[fix][2] != want:
                bad.append("%s: цель прокси %#x, а символ в sdk %#x" % (fix, state[fix][2], want))

    # Путь вызова обязан совпасть с решением.
    def path_check(fix, entered, label):
        nonlocal checks
        checks += 1
        want_proxy = exp[fix][0] == "proxy"
        if entered != want_proxy:
            bad.append("%s: вызов %s в sdk, а решение %s" % (label, "ПРИШЁЛ" if entered else "НЕ пришёл",
                                                             exp[fix][0]))
    path_check("hash_memory", hash_entered, "hash")
    path_check("ldn_set_protocol", ldn_entered, "ldn")
    path_check("nvrm_query", nv_hits.get(SYM_NVRM, 0) > 0, "nvrm")

    checks += 1
    if exp["ldn_set_protocol"][0] == "proxy":
        if ldn_arg != 3 or ldn_rc != (SENT_LDN | 3):
            bad.append("ldn: прокси передал %r, вернул %#x" % (ldn_arg, ldn_rc))
    elif ldn_rc != 0:
        bad.append("ldn: своя заглушка вернула %#x, ждали 0" % ldn_rc)

    checks += 1
    nv_exp = exp["nvrm_query"]
    if nv_exp[0] == "proxy" and not w.nvrm_is_stub:
        if nv_log.get(SYM_NVRM) != (0x2A, 0x1111, out, 0x38) or nv_rc != SENT_NVRM:
            bad.append("nvrm: прокси передал %r, вернул %#x" % (nv_log.get(SYM_NVRM), nv_rc))
    elif nv_exp[0] == "proxy":
        if nv_rc != 2 or nv_hits.get(SYM_IOCTL):
            bad.append("nvrm: ждали родную заглушку (код 2, без ioctl), получили %#x, ioctl %r"
                       % (nv_rc, nv_hits.get(SYM_IOCTL)))
    else:
        io = nv_log.get(SYM_IOCTL)
        want_arg = struct.pack("<III", 0x2A, 1, 0)
        want_out = b"\xAA" * 8 + struct.pack("<Q", NVRM_VALUE) + b"\xAA" * 8
        if (nv_rc != 0 or io is None or io[0] != NVRM_FD or io[1] != 0xC00C0109 or
                io[2] != want_arg or io[3] != 12 or out_bytes != want_out):
            bad.append("nvrm: своя дала rc=%#x ioctl=%r out=%s" % (nv_rc, io, out_bytes.hex()))
    if not quiet:
        say("   %-10s + sdk %-7s: hash %-5s ldn %-5s nvrm %-5s | %s"
            % (exefs.name, sdk_ver,
               state["hash_memory"][0] if state else "?",
               state["ldn_set_protocol"][0] if state else "?",
               state["nvrm_query"][0] if state else "?",
               "ЗЕЛЕНО" if not bad else "КРАСНО: " + "; ".join(bad[:3])))
    return bad, checks


def find_sym_in_image(nso_path, data):
    """Смещение строки в образе (rodata/dynstr) — для порчи входа в пробах."""
    n = nso_min.NSO(str(nso_path))
    for seg_off, seg in ((n.TMEM, n.text), (n.RMEM, n.ro), (n.DMEM, n.data)):
        i = seg.find(data)
        if i >= 0:
            return seg_off + i
    return None


def main():
    setup_stdio()
    ap = argparse.ArgumentParser(description="Сверка автоисправления на настоящих sdk")
    ap.add_argument("nso", help="наш собранный subsdk9")
    ap.add_argument("--exefs-root", required=True,
                    help="папка, где лежат WD-patch, SIN-patch, TOTK-patch (распакованные exefs)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--noautofix", help="модуль, собранный с -DNXR_NO_AUTOFIX (затвор нуля)")
    args = ap.parse_args()
    try:
        import unicorn  # noqa: F401
    except ImportError:
        say("нет пакета unicorn (pip install unicorn)")
        sys.exit(2)
    root = Path(args.exefs_root)
    missing = [s for s in SDKS.values() if not (SDK_DIR / s).is_file()]
    games = sorted({g for g, _s in EXPECT})
    absent = [g for g in games if not (root / g / "main").is_file()]
    if missing or absent or not Path(args.nso).is_file():
        say("нечем проверять: нет %s" % (missing + absent + ([args.nso] if not Path(args.nso).is_file() else [])))
        sys.exit(2)

    refs = reference_hashes()
    say("эталон хеша: настоящая __hash_memory nnSdk 22.2.0, входов %d" % len(refs))
    total_bad, total_checks = [], 0
    say("--- сценарии (игра + sdk на консоли) ---")
    for (g, ver) in EXPECT:
        bad, n = run_scenario(Path(args.nso), root / g, ver, refs)
        total_bad += bad
        total_checks += n

    # sdk не опознан (имя модуля испорчено): цель — «как без нас», и для WD
    # на 19.3.13 это родная заглушка NvRm (безопасная сторона), hash — своя.
    sdk19 = SDK_DIR / SDKS["19.3.13"]
    name_off = find_sym_in_image(sdk19, b"nnSdk")
    bad, n = run_scenario(Path(args.nso), root / "WD-patch", "19.3.13", refs,
                          patches=[("sdk", name_off, b"nnSdX")],
                          expect={"hash_memory": ("own", "NoSdkMissing"),
                                  "ldn_set_protocol": ("own", "NoSdkMissing"),
                                  "nvrm_query": ("proxy", "NoSdkDefined")}, quiet=True)
    say("   sdk не опознан (имя «nnSdX»): %s" % ("ЗЕЛЕНО" if not bad else "КРАСНО: " + "; ".join(bad)))
    total_bad += bad
    total_checks += n

    # Два экземпляра nxredirect в процессе (subsdk8 и subsdk9) при НЕопознанном
    # sdk: каждый исключает другой по метке — без взаимного проксирования.
    bad, n = run_scenario(Path(args.nso), root / "WD-patch", "19.3.13", refs,
                          patches=[("sdk", name_off, b"nnSdX")], twin=True,
                          expect={"hash_memory": ("own", "NoSdkMissing"),
                                  "ldn_set_protocol": ("own", "NoSdkMissing"),
                                  "nvrm_query": ("proxy", "NoSdkDefined")}, quiet=True)
    say("   два экземпляра nxredirect: %s" % ("ЗЕЛЕНО" if not bad else "КРАСНО: " + "; ".join(bad)))
    total_bad += bad
    total_checks += n

    ok = not total_bad
    MIN_CHECKS = 150
    if total_checks < MIN_CHECKS:
        say("ЗАТВОР НУЛЯ: проверок %d < %d — прогон ничего не доказал" % (total_checks, MIN_CHECKS))
        ok = False
    say("проверок: %d, претензий: %d" % (total_checks, len(total_bad)))

    if args.self_test:
        ok &= self_test(Path(args.nso), root, refs, name_off, args.noautofix)

    say("ИТОГ: " + ("автоисправление выбирает ровно то, что ждали" if ok else "ЕСТЬ ПРОВАЛЫ"))
    sys.exit(0 if ok else 1)


def self_test(module, root, refs, name_off, noautofix):
    """Порча входа: настоящая проверка — КРАСНАЯ, ослабленная — ЗЕЛЁНАЯ."""
    say("--- негативные пробы (порча входа; ослабленная проверка смотрит только возврат) ---")
    ok = True
    sdk19 = SDK_DIR / SDKS["19.3.13"]
    vk_off = find_sym_in_image(root / "SIN-patch" / "subsdk0", b"vkCreateInstance\0")
    ver_off = find_sym_in_image(sdk19, b"NintendoSdk_nnSdk-19_3_13")
    marker_off = find_sym_in_image(module, MARKER.encode() + b"\0")
    probes = [
        ("Vulkan не виден у SiN (имя vkCreateInstance испорчено) -> своя NvRm поменяла бы "
         "работающий путь SiN", root / "SIN-patch", "19.3.13",
         [("subsdk0", vk_off, b"vkCreateInstanX")], False),
        ("sdk 19.3.13 выдаёт себя за 29.3.13 -> подмена не видна, WD без NvRm",
         root / "WD-patch", "19.3.13",
         [("sdk", ver_off + len("NintendoSdk_nnSdk-"), b"29")], False),
        ("решение hash заранее «своя» на родной 22.2.0 (перекрыли бы рабочую функцию)",
         root / "WD-patch", "22.2.0", "preseed", False),
        ("ДВА экземпляра без меток при неопознанном sdk -> взаимное проксирование",
         root / "WD-patch", "19.3.13",
         [("sdk", name_off, b"nnSdX"), ("subsdk9", marker_off, b"nxr_autofix_stateXv1"),
          ("subsdk8", marker_off, b"nxr_autofix_stateXv1")], True),
    ]
    for title, exefs, ver, patches, twin in probes:
        if patches == "preseed":
            real_bad, weak_bad = preseed_probe(module, exefs, ver, refs)
        else:
            if any(off is None for _m, off, _d in patches):
                say("   проба «%s»: не нашлось места порчи — проба недействительна" % title)
                ok = False
                continue
            exp = EXPECT.get((exefs.name, ver))
            if twin:
                exp = {"hash_memory": ("own", "NoSdkMissing"),
                       "ldn_set_protocol": ("own", "NoSdkMissing"),
                       "nvrm_query": ("proxy", "NoSdkDefined")}
            try:
                real_bad, _ = run_scenario(module, exefs, ver, refs, patches=patches, twin=twin,
                                           expect=exp, quiet=True)
            except Exception as e:
                real_bad = ["упало: %s" % e]
            try:
                weak_bad, _ = run_scenario(module, exefs, ver, refs, patches=patches, twin=twin,
                                           weak=True, quiet=True)
            except Exception as e:
                weak_bad = ["упало: %s" % e]
        good = bool(real_bad)
        say("   %s: настоящая %s, ослабленная %s%s"
            % (title, "КРАСНАЯ" if real_bad else "ЗЕЛЁНАЯ (НЕ ловит!)",
               "ЗЕЛЁНАЯ" if not weak_bad else "красная",
               "" if not real_bad else " — " + real_bad[0][:110]))
        ok &= good
    # Затвор нуля: модуль без автоисправления — настоящая проверка обязана покраснеть.
    if noautofix and Path(noautofix).is_file():
        bad, _ = run_scenario(Path(noautofix), root / "WD-patch", "19.3.13", refs, quiet=True)
        say("   затвор нуля (модуль -DNXR_NO_AUTOFIX): %s" % ("КРАСНЫЙ. Хорошо." if bad else
                                                             "ЗЕЛЁНЫЙ — проверка пустая!"))
        ok &= bool(bad)
    else:
        say("   затвор нуля: модуль без автоисправления не указан (--noautofix) — проба пропущена")
        ok = False
    return ok


def preseed_probe(module, exefs, ver, refs):
    """Слово решения hash заранее «своя/SdkMissing» — как если бы правило ошиблось."""
    sdk_path = SDK_DIR / SDKS[ver]
    res = []
    for weak in (False, True):
        p = build_proc(module, exefs, sdk_path)
        a = p.addr_of("subsdk9", MARKER)
        p.uc.mem_write(a, struct.pack("<Q", (1 << 56) | (1 << 48)))
        # run_scenario строит процесс сам — повторяем его проверки на этом процессе.
        bad = _scenario_on(p, exefs, ver, refs, weak)
        res.append(bad)
    return res[0], res[1]


def _scenario_on(p, exefs, ver, refs, weak):
    w = Watch(p)
    ours = p.addr_of("subsdk9", SYM_HASH)
    bad = []
    for payload, ref in zip(hash_cases(), refs):
        p.uc.mem_write(p.data, b"\0" * 0x1000)
        p.uc.mem_write(p.data, payload)
        if p.call(ours, [p.data, len(payload)]) != ref:
            bad.append("hash разошёлся")
            break
    if weak:
        return bad
    exp = EXPECT[(exefs.name, ver)]["hash_memory"]
    st = read_state(p)["hash_memory"][:2]
    if st != exp:
        bad.append("hash: решение %s, ждали %s" % (st, exp))
    if (w.hits.get(SYM_HASH, 0) > 0) != (exp[0] == "proxy"):
        bad.append("hash: путь вызова не совпал с решением")
    return bad


if __name__ == "__main__":
    main()
