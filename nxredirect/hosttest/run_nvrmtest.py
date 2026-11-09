#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_nvrmtest — сверка исправления NvRmMemQueryHandleParams (src/nxr_compat.hpp)
с НАСТОЯЩИМ кодом nnSdk 22.2.0.

Эталон — не вторая переписанная реализация, а сама функция
NvRmMemQueryHandleParams из NintendoSdk_nnSdk-22_2_0-Release (@0x468b90),
выполненная в эмуляторе ARM64 (Unicorn). Её внешние вызовы (LockMutex,
UnlockMutex, NvOsDrvOpen, NvOsDrvIoctl) идут через PLT: в ячейки GOT кладутся
адреса заглушек, а заглушки делают ровно то же, что подделка в nvrm_test.cpp
(открытие отдаёт fd 7, ioctl кладёт value в [arg+8] и отвечает заданным кодом).
Глобальная ячейка fd nvmap ставится в нужное состояние перед каждым вызовом.

Наша сторона — hosttest/nvrm_test.cpp (g++ в WSL) с тем же src/nxr_compat.hpp,
что идёт в модуль. Сравнивается ВСЁ наблюдаемое: ответ, сколько раз и с каким
fd/запросом/размером позван ioctl, 12 байт его аргумента и 24 байта out.

Негативные пробы (--self-test): порча ОДНОГО места в копии исходника обязана
дать расхождение на тех же входах; плюс «затвор нуля» (функция всегда 0).

Запуск:
    python run_nvrmtest.py              # сверка
    python run_nvrmtest.py --self-test  # негативные пробы
Ничего не удаляет: временные копии — в своей папке под %TEMP%.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"
# Эталон nnSdk 22.2.0: --sdk, NXR_REF_SDK или папка $NXR_SDK_DIR (путь не зашит).
DEFAULT_SDK = Path(os.environ.get("NXR_SDK_DIR") or "NXR_SDK_DIR-не-задана") / "NintendoSdk_nnSdk-22_2_0-Release"
WSL_DISTRO = "Ubuntu"

# Те же входы, что kCases в nvrm_test.cpp: (fdState, handle, rc, value, fill).
CASES = [
    (-1, 0x1, 0, 0x3fc000, 0xAA),
    (-1, 0x2a, 0, 0x0, 0x55),
    (5, 0x7fffffff, 0, 0xffffffff, 0xCC),
    (5, 0x10, 4, 0x1234, 0xAA),
    (-1, 0x10, 0x80000001, 0x1, 0x11),
    (3, 0xdeadbeef, 0, 0x80000000, 0x00),
    (-1, 0, 0x7, 0x0, 0xFF),
]

# Адреса в nnSdk 22.2.0 (по дизассемблеру).
FN_ADDR = 0x468b90             # NvRmMemQueryHandleParams
FD_CELL = 0xba72e0             # глобальный fd nvmap (adrp 0xba7000 + 0x2e0)


def setup_stdio():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def say(msg=""):
    print(msg, flush=True)


def wsl_path(p):
    p = str(Path(p).resolve())
    return "/mnt/%s%s" % (p[0].lower(), p[2:].replace("\\", "/"))


def run_wsl(script, timeout=600):
    try:
        res = subprocess.run(["wsl", "-d", WSL_DISTRO, "--", "bash", "-l", "-s"],
                             input=script.encode("utf-8"), capture_output=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as e:
        return 1, "", "wsl не запустилась: %s" % e
    # wsl.exe пишет свои ошибки в UTF-16; убираем нули, чтобы текст был читаем.
    return (res.returncode, res.stdout.decode("utf-8", "replace").replace("\x00", ""),
            res.stderr.decode("utf-8", "replace").replace("\x00", ""))


# Метка «дошли до компилятора»: печатается скриптом прямо перед g++. Нет её в
# выводе — прогон НЕ ВЫПОЛНЯЛСЯ (WSL не запустилась, нет g++, не скопировались
# файлы), и пустой вывод тогда не значит «расхождение» (так было: при занятом
# ext4.vhdx все негативные пробы печатали «КРАСНАЯ … Хорошо.»).
RAN_MARK = "NXR_HOSTTEST_COMPILE_START"


def ran(out):
    """Дошёл ли прогон до компилятора (иначе не проверено ничего)."""
    return RAN_MARK in out


def build_and_run(src_dir, tag):
    work = "/tmp/nxr-nvrmtest-" + tag
    script = (
        "set -e\n"
        f'WORK="{work}"\n'
        'mkdir -p "$WORK/src" "$WORK/hosttest"\n'
        f'cp "{wsl_path(src_dir)}/nxr_compat.hpp" "$WORK/src/"\n'
        f'cp "{wsl_path(HERE / "nvrm_test.cpp")}" "$WORK/hosttest/nvrm_test.cpp"\n'
        'cd "$WORK/hosttest"\n'
        'command -v g++ >/dev/null || { echo "нет g++ в WSL"; exit 97; }\n'
        f'echo "{RAN_MARK}"\n'
        "g++ -O2 -std=c++17 -Wall -o nvrm_test nvrm_test.cpp -I../src\n"
        "./nvrm_test\n"
    )
    rc, out, err = run_wsl(script)
    return rc, out + err


LINE_RE = re.compile(r"^case (\d+) ret=([0-9a-f]{8}) calls=(\d+) fd=(-?\d+) req=([0-9a-f]{8}) "
                     r"size=(\d+) arg=([0-9a-f]{24}) out=([0-9a-f]{48})$")


def parse(out):
    got = {}
    for line in out.splitlines():
        m = LINE_RE.match(line.strip())
        if m:
            i = int(m.group(1))
            got[i] = (int(m.group(2), 16), int(m.group(3)), int(m.group(4)),
                      int(m.group(5), 16), int(m.group(6)), m.group(7), m.group(8))
    return got


# ------------------------------------------------------------------ эталон

def load_reference(sdk_path):
    try:
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
        from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                         UC_ARM64_REG_X3, UC_ARM64_REG_SP, UC_ARM64_REG_LR,
                                         UC_ARM64_REG_PC)
    except ImportError:
        raise SystemExit("нет пакета unicorn (pip install unicorn): без него эталон не выполнить")
    sys.path.insert(0, str(HERE))
    import nso_min

    m = nso_min.NSO(str(sdk_path))
    exports = m.exports()
    if exports.get("NvRmMemQueryHandleParams", (None,))[0] != FN_ADDR:
        raise SystemExit("в %s NvRmMemQueryHandleParams не по %#x — не тот эталон" % (sdk_path, FN_ADDR))

    # GOT-ячейки нужных импортов (JUMP_SLOT/GLOB_DAT) — по таблицам релокаций.
    want = {"_ZN2nn2os9LockMutexEPNS0_9MutexTypeE": "lock",
            "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE": "unlock",
            "NvOsDrvOpen": "open", "NvOsDrvIoctl": "ioctl"}
    got_cells = {}
    syms = {i: nm for i, nm, *_ in m.syms()}
    for tag_a, tag_s in ((23, 2), (7, 8)):
        if tag_a not in m.DT:
            continue
        at, sz = m.DT[tag_a][0], m.DT[tag_s][0]
        for k in range(0, sz, 24):
            off, info, _add = struct.unpack("<QQq", m.mem(at + k, 24))
            if (info & 0xffffffff) in (1025, 1026) and syms.get(info >> 32) in want:
                got_cells[want[syms[info >> 32]]] = off
    if set(got_cells) != set(want.values()):
        raise SystemExit("не нашлись ячейки GOT: %s" % sorted(set(want.values()) - set(got_cells)))

    base, stack, data, stubs = 0x100000000, 0x200000000, 0x300000000, 0x400000000
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    size = ((m.DMEM + m.DSZ + m.bss + 0xFFFF) // 0x10000) * 0x10000
    uc.mem_map(base, size)
    uc.mem_write(base + m.TMEM, m.text)
    uc.mem_write(base + m.RMEM, m.ro)
    uc.mem_write(base + m.DMEM, m.data)
    uc.mem_map(stack, 0x100000)
    uc.mem_map(data, 0x10000)
    uc.mem_map(stubs, 0x1000)
    ret_addr = stubs + 0x800
    stub_addr = {"lock": stubs + 0x0, "unlock": stubs + 0x10, "open": stubs + 0x20,
                 "ioctl": stubs + 0x30}
    for k, a in stub_addr.items():
        uc.mem_write(base + got_cells[k], struct.pack("<Q", a))
        uc.mem_write(a, b"\xc0\x03\x5f\xd6")   # ret
    uc.mem_write(ret_addr, b"\xc0\x03\x5f\xd6")

    state = {}

    def on_code(u, addr, _size, _ud):
        if addr == stub_addr["open"]:
            state["opens"] += 1
            u.reg_write(UC_ARM64_REG_X0, 7)
        elif addr == stub_addr["ioctl"]:
            fd = u.reg_read(UC_ARM64_REG_X0) & 0xffffffff
            req = u.reg_read(UC_ARM64_REG_X1) & 0xffffffff
            arg = u.reg_read(UC_ARM64_REG_X2)
            sz = u.reg_read(UC_ARM64_REG_X3) & 0xffffffff
            state["calls"] += 1
            state["fd"] = fd - (1 << 32) if fd & 0x80000000 else fd
            state["req"] = req
            state["size"] = sz
            state["arg"] = bytes(u.mem_read(arg, 12)).hex()
            u.mem_write(arg + 8, struct.pack("<I", state["value"]))
            u.reg_write(UC_ARM64_REG_X0, state["rc"])
    uc.hook_add(UC_HOOK_CODE, on_code, begin=stubs, end=stubs + 0x40)

    def call(case):
        fd_state, handle, rc, value, fill = case
        state.update(opens=0, calls=0, fd=0, req=0, size=0, arg="00" * 12, rc=rc, value=value)
        uc.mem_write(base + FD_CELL, struct.pack("<i", fd_state))
        uc.mem_write(data, bytes([fill]) * 0x38)
        uc.reg_write(UC_ARM64_REG_X0, handle)
        uc.reg_write(UC_ARM64_REG_X1, 0)
        uc.reg_write(UC_ARM64_REG_X2, data)
        uc.reg_write(UC_ARM64_REG_X3, 0x38)
        uc.reg_write(UC_ARM64_REG_SP, stack + 0x80000)
        uc.reg_write(UC_ARM64_REG_LR, ret_addr)
        uc.emu_start(base + FN_ADDR, ret_addr)
        ret = uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff
        out = bytes(uc.mem_read(data, 0x18)).hex()
        return (ret, state["calls"], state["fd"], state["req"], state["size"], state["arg"], out)

    return call, {"buildid": m.buildid[:40], "got": {k: hex(v) for k, v in got_cells.items()}}


def compare(ours, call, quiet=False):
    bad = []
    for i, case in enumerate(CASES):
        ref = call(case)
        mine = ours.get(i)
        if mine != ref:
            bad.append((i, mine, ref))
    if not quiet:
        for i, mine, ref in bad[:6]:
            say("   РАСХОЖДЕНИЕ case %d" ":\n      наш      %s\n      оригинал %s" % (i, mine, ref))
    return len(CASES) - len(bad), bad


WEAKENINGS = [
    ("param = 0 вместо 1 (не тот параметр nvmap)",
     "    a.param  = NvmapParamSize;", "    a.param  = 0;"),
    ("запрос ioctl 0xC00C0108",
     "inline constexpr std::uint32_t NvmapIocParam      = 0xC00C0109u;",
     "inline constexpr std::uint32_t NvmapIocParam      = 0xC00C0108u;"),
    ("размер в out пишется и при ошибке ioctl",
     "    if (rc != 0) {\n        return rc;\n    }\n    const std::uint64_t size",
     "    const std::uint64_t size"),
    ("размер пишется 32 битами (старшие байты out остаются мусором)",
     "    const std::uint64_t size = a.value;",
     "    const std::uint32_t size = a.value;"),
    ("размер кладётся в [out+0x10]",
     "inline constexpr std::uint64_t QueryOutSizeOffset = 8;",
     "inline constexpr std::uint64_t QueryOutSizeOffset = 0x10;"),
    ("при ошибке ioctl возвращается 2 (как заглушка), а не код ioctl",
     "    if (rc != 0) {\n        return rc;\n    }",
     "    if (rc != 0) {\n        return 2;\n    }"),
    ("ЗАТВОР НУЛЯ: функция всегда 0, ioctl не зовётся",
     "    const std::int32_t fd = getFd();",
     "    if (handle || !handle) return 0;\n    const std::int32_t fd = getFd();"),
]


def self_test(sdk):
    call, info = load_reference(sdk)
    say("эталон: BuildId %s, GOT %s" % (info["buildid"], info["got"]))
    text = (SRC / "nxr_compat.hpp").read_text(encoding="utf-8")
    rc, out = build_and_run(SRC, "real")
    ours = parse(out)
    n, bad = compare(ours, call)
    say("--- настоящая реализация: сошлось %d из %d (код теста %d) ---" % (n, len(CASES), rc))
    ok = (rc == 0 and not bad and len(ours) == len(CASES))
    if not ran(out):
        say("   настоящая реализация: НЕ ВЫПОЛНЕНО (WSL или g++ недоступны) — ничего не проверено")
        ok = False
    if not ok:
        say(out[-2000:])
    # Своя свежая временная папка на прогон, убирается в конце
    # (прежде — общая %TEMP%\nxr-nvrm-weak, оставалась навсегда).
    # NXR_KEEP_TEMP=1 — оставить для разбора.
    root = Path(tempfile.mkdtemp(prefix="nxr-nvrm-weak-"))
    try:
        return _weak_probes(text, call, root, ok)
    finally:
        if os.environ.get("NXR_KEEP_TEMP", "").strip() not in ("", "0"):
            say("   NXR_KEEP_TEMP: копии с порчей оставлены: %s" % root)
        else:
            shutil.rmtree(root, ignore_errors=True)


def _weak_probes(text, call, root, ok):
    """Порченые копии nxr_compat.hpp в root; каждая обязана разойтись с эталоном."""
    for idx, (title, frm, to) in enumerate(WEAKENINGS):
        if frm not in text:
            say("   проба %d (%s): фрагмент не найден — проба недействительна" % (idx + 1, title))
            ok = False
            continue
        d = root / ("w%d" % idx)
        d.mkdir(parents=True, exist_ok=True)
        (d / "nxr_compat.hpp").write_text(text.replace(frm, to, 1), encoding="utf-8")
        wrc, wout = build_and_run(d, "weak%d" % idx)
        if not ran(wout):
            say("   проба %d (%s): НЕ ВЫПОЛНЕНО (WSL или g++ недоступны) — ничего не проверено"
                % (idx + 1, title))
            ok = False
            continue
        wours = parse(wout)
        _wn, wbad = compare(wours, call, quiet=True)
        if wbad or len(wours) != len(CASES):
            say("   проба %d (%s): КРАСНАЯ — расхождений %d. Хорошо." % (idx + 1, title, len(wbad)))
        else:
            say("   проба %d (%s): осталась ЗЕЛЁНОЙ — сверка ничего не ловит!" % (idx + 1, title))
            ok = False
    return ok


def main():
    setup_stdio()
    ap = argparse.ArgumentParser(description="Сверка NvRmMemQueryHandleParams с nnSdk 22.2.0")
    ap.add_argument("--sdk", default=os.environ.get("NXR_REF_SDK", str(DEFAULT_SDK)))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if not Path(args.sdk).is_file():
        say("нет файла эталона: %s" % args.sdk)
        sys.exit(2)
    if args.self_test:
        ok = self_test(args.sdk)
        say("ИТОГ: " + ("все негативные пробы сработали" if ok else "ЕСТЬ ПРОВАЛЫ"))
        sys.exit(0 if ok else 1)
    call, info = load_reference(args.sdk)
    say("эталон: BuildId %s, GOT %s" % (info["buildid"], info["got"]))
    rc, out = build_and_run(SRC, "real")
    if not ran(out):
        say("НЕ ВЫПОЛНЕНО (WSL или g++ недоступны) — ничего не проверено")
        say(out[-2000:])
        sys.exit(1)
    for line in out.splitlines():
        if line.startswith("  ") or "ТЕСТ" in line or "проверок" in line:
            say(line)
    ours = parse(out)
    n, bad = compare(ours, call)
    say("сверка с оригиналом 22.2.0: сошлось %d из %d" % (n, len(CASES)))
    sys.exit(0 if (rc == 0 and not bad and len(ours) == len(CASES)) else 1)


if __name__ == "__main__":
    main()
