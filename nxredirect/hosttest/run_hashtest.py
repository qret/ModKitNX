#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_hashtest — хост-проверка недостающих символов (src/nxr_compat.hpp) на ПК.

Что сверяется и с чем
---------------------
Эталон — НЕ вторая переписанная реализация, а настоящий код
`_ZNSt3__113__hash_memoryEPKvm` из NintendoSdk_nnSdk-22_2_0-Release: сегменты NSO
кладутся в эмулятор ARM64 (Unicorn) как есть, вызывается адрес экспорта, читается
x0. Тело функции самодостаточно (928 байт, ни одного внешнего вызова и ни одного
системного вызова — проверено по дизассемблеру), поэтому эмулировать можно.

Наша сторона — hosttest/hash_test.cpp, собранный обычным g++ (в WSL), который
включает тот же самый src/nxr_compat.hpp, что идёт в модуль. Копий кода нет.

Негативные пробы (--self-test)
------------------------------
Каждая проба — это порча ОДНОГО места в копии исходника (сам проект не трогается):
ослабленная сборка обязана дать КРАСНЫЙ результат на тех же входах, на которых
настоящая даёт зелёный. Плюс «затвор нуля»: проверка, что сверка ловит вариант,
который всегда возвращает 0.

Запуск:
    python run_hashtest.py                # сверка
    python run_hashtest.py --self-test    # негативные пробы
    python run_hashtest.py --sdk <путь к NintendoSdk_nnSdk-22_2_0-Release>
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"
# Эталон nnSdk 22.2.0: --sdk, NXR_REF_SDK или папка $NXR_SDK_DIR (путь не зашит).
DEFAULT_SDK = Path(os.environ.get("NXR_SDK_DIR") or "NXR_SDK_DIR-не-задана") / "NintendoSdk_nnSdk-22_2_0-Release"

WSL_DISTRO = "Ubuntu"


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
    drive, rest = p[0], p[2:].replace("\\", "/")
    return "/mnt/%s%s" % (drive.lower(), rest)


def run_wsl(script, timeout=600):
    """
    Скрипт передаётся В БАЙТАХ. С text=True Python на Windows переводит \\n в
    \\r\\n, и bash получает строки с \\r на конце — проверено: `set -e\\r` даёт
    «invalid option», а пути обрастают \\r. Поэтому только bytes.
    """
    res = subprocess.run(["wsl", "-d", WSL_DISTRO, "--", "bash", "-l", "-s"],
                         input=script.encode("utf-8"), capture_output=True,
                         timeout=timeout)
    return subprocess.CompletedProcess(
        res.args, res.returncode,
        res.stdout.decode("utf-8", "replace"),
        res.stderr.decode("utf-8", "replace"))


# ------------------------------------------------------------------ наша сторона


def build_and_run(src_dir, test_cpp, tag):
    """Собрать hash_test.cpp (с nxr_compat.hpp из src_dir) и выполнить его."""
    work = "/tmp/nxr-hashtest-" + tag
    script = (
        "set -e\n"
        f'WORK="{work}"\n'
        'rm -rf "$WORK"; mkdir -p "$WORK/src" "$WORK/hosttest"\n'
        f'cp "{wsl_path(src_dir)}/nxr_compat.hpp" "$WORK/src/"\n'
        f'cp "{wsl_path(test_cpp)}" "$WORK/hosttest/hash_test.cpp"\n'
        'cd "$WORK/hosttest"\n'
        "g++ -O2 -std=gnu++17 -Wall -o hash_test hash_test.cpp\n"
        "./hash_test\n"
    )
    res = run_wsl(script)
    if res.returncode != 0:
        say(res.stdout[-3000:])
        say(res.stderr[-3000:])
        raise SystemExit("сборка или запуск хост-теста не удались (rc=%d)" % res.returncode)
    return res.stdout


def parse(out):
    """Разобрать вывод теста в список случаев и коды заглушки."""
    cases = []       # (метка, смещение, длина, значение)
    ldn = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("ldn "):
            ldn.append(int(line.split()[1]))
            continue
        if line.startswith("camera_create "):
            _, ln, val = line.split()
            cases.append(("camera_create", None, int(ln), int(val, 16)))
            continue
        m = re.match(r"^(\d+) (\d+) ([0-9a-f]{16})$", line)
        if not m:
            raise SystemExit("непонятная строка теста: %r" % line)
        cases.append(("buf", int(m.group(1)), int(m.group(2)), int(m.group(3), 16)))
    return cases, ldn


# ------------------------------------------------------------------ эталон


def load_reference(sdk_path):
    """Подготовить вызов настоящего кода SDK в эмуляторе."""
    try:
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
        from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                         UC_ARM64_REG_SP, UC_ARM64_REG_LR)
    except ImportError:
        raise SystemExit("нет пакета unicorn: установить `pip install unicorn` "
                         "(нужен, чтобы выполнить настоящий код SDK на ПК)")
    sys.path.insert(0, str(HERE))
    import nso_min

    m = nso_min.NSO(sdk_path)
    base, stack, data, ret = 0x100000000, 0x200000000, 0x300000000, 0x400000000
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    size = ((m.DMEM + m.DSZ + m.bss + 0xFFFF) // 0x10000) * 0x10000
    uc.mem_map(base, size)
    uc.mem_write(base + m.TMEM, m.text)
    uc.mem_write(base + m.RMEM, m.ro)
    uc.mem_write(base + m.DMEM, m.data)
    uc.mem_map(stack, 0x100000)
    uc.mem_map(data, 0x8000)
    uc.mem_map(ret, 0x1000)
    exports = m.exports()
    name = "_ZNSt3__113__hash_memoryEPKvm"
    if name not in exports:
        raise SystemExit("в %s нет символа %s" % (sdk_path, name))
    addr, _sz, _info = exports[name]

    def call(payload):
        uc.mem_write(data, b"\x00" * 0x8000)
        uc.mem_write(data, payload)
        uc.reg_write(UC_ARM64_REG_X0, data)
        uc.reg_write(UC_ARM64_REG_X1, len(payload))
        uc.reg_write(UC_ARM64_REG_SP, stack + 0x80000)
        uc.reg_write(UC_ARM64_REG_LR, ret)
        uc.emu_start(base + addr, ret)
        return uc.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFFFFFFFFFF

    return call, {"buildid": m.buildid[:40], "addr": addr, "text_size": m.TSZ}


def make_buf(n=8192):
    """Тот же ЛКГ, что в hash_test.cpp."""
    out = bytearray(n)
    s = 123456789
    for i in range(n):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (s >> 16) & 0xFF
    return bytes(out)


# ------------------------------------------------------------------ сверка


def compare(cases, ldn, call, quiet=False):
    """Вернуть (число сошедшихся, список расхождений)."""
    buf = make_buf()
    bad = []
    for label, off, ln, val in cases:
        payload = b"camera_create" if label == "camera_create" else buf[off:off + ln]
        if len(payload) != ln:
            bad.append((label, off, ln, val, None, "короткий буфер"))
            continue
        ref = call(payload)
        if ref != val:
            bad.append((label, off, ln, val, ref, "значения разошлись"))
    for code in ldn:
        if code != 0:
            bad.append(("ldn", None, None, code, 0, "заглушка вернула не успех"))
    if not quiet:
        for b in bad[:10]:
            say("   РАСХОЖДЕНИЕ %s off=%s len=%s наш=%s оригинал=%s (%s)"
                % (b[0], b[1], b[2],
                   ("%016x" % b[3]) if b[3] is not None else "-",
                   ("%016x" % b[4]) if b[4] is not None else "-", b[5]))
        if len(bad) > 10:
            say("   ... и ещё %d" % (len(bad) - 10))
    return len(cases) + len(ldn) - len(bad), bad


# ------------------------------------------------------------------ негативные пробы

WEAKENINGS = [
    ("константа k_mul сдвинута на 1",
     "0x9DDFEA08EB382D69ULL", "0x9DDFEA08EB382D68ULL"),
    ("граница ветки len>=4 сдвинута",
     "if (len >= 4) {", "if (len >= 5) {"),
    ("поворот в ветке 33..64 на 38 вместо 37",
     "std::uint64_t c = Rotate(a, 37);", "std::uint64_t c = Rotate(a, 38);"),
    ("в длинной ветке убран xor w.second",
     "x ^= w.second;", "x ^= 0;"),
    ("ЗАТВОР НУЛЯ: хеш всегда 0",
     "    const char* s = static_cast<const char*>(key);",
     "    const char* s = static_cast<const char*>(key); if (s || !s) return 0;"),
    ("заглушка LDN возвращает ошибку",
     "    return 0;  /* nn::ResultSuccess */", "    return 0x2C8;"),
]


def self_test(sdk_path):
    call, info = load_reference(sdk_path)
    say("эталон: BuildId %s, .text %#x, символ по адресу %#x"
        % (info["buildid"], info["text_size"], info["addr"]))
    src_text = (SRC / "nxr_compat.hpp").read_text(encoding="utf-8")

    say("--- настоящая реализация (должна быть ЗЕЛЁНОЙ) ---")
    out = build_and_run(SRC, HERE / "hash_test.cpp", "real")
    cases, ldn = parse(out)
    ok, bad = compare(cases, ldn, call)
    say("   сошлось %d из %d" % (ok, len(cases) + len(ldn)))
    if bad:
        say("ПРОВАЛ: настоящая реализация расходится с оригиналом")
        return False

    all_ok = True
    for idx, (title, frm, to) in enumerate(WEAKENINGS):
        if frm not in src_text:
            say("   проба %d (%s): НЕ НАЙДЕН фрагмент для порчи — проба недействительна"
                % (idx + 1, title))
            all_ok = False
            continue
        tmp = Path(tempfile.mkdtemp(prefix="nxr-weak-"))
        (tmp / "src").mkdir()
        (tmp / "src" / "nxr_compat.hpp").write_text(src_text.replace(frm, to, 1),
                                                    encoding="utf-8")
        try:
            out = build_and_run(tmp / "src", HERE / "hash_test.cpp", "weak%d" % idx)
            wcases, wldn = parse(out)
            wok, wbad = compare(wcases, wldn, call, quiet=True)
            if wbad:
                say("   проба %d (%s): ослабленная КРАСНАЯ — расхождений %d. Хорошо."
                    % (idx + 1, title, len(wbad)))
            else:
                say("   проба %d (%s): ослабленная осталась ЗЕЛЁНОЙ — проверка ничего не ловит!"
                    % (idx + 1, title))
                all_ok = False
        except SystemExit as e:
            say("   проба %d (%s): сборка ослабленной не удалась (%s)" % (idx + 1, title, e))
            all_ok = False
        finally:
            # Своя временная папка этой пробы; NXR_KEEP_TEMP=1 — оставить для разбора.
            if os.environ.get("NXR_KEEP_TEMP", "").strip() not in ("", "0"):
                say("   NXR_KEEP_TEMP: копия с порчей оставлена: %s" % tmp)
            else:
                shutil.rmtree(tmp, ignore_errors=True)
    return all_ok


def main():
    setup_stdio()
    ap = argparse.ArgumentParser(description="Хост-проверка nxr_compat.hpp")
    ap.add_argument("--sdk", default=os.environ.get("NXR_REF_SDK", str(DEFAULT_SDK)),
                    help="путь к NintendoSdk_nnSdk-22_2_0-Release (эталон)")
    ap.add_argument("--self-test", action="store_true", help="негативные пробы")
    args = ap.parse_args()

    if not Path(args.sdk).is_file():
        raise SystemExit("нет файла эталона: %s" % args.sdk)

    if args.self_test:
        ok = self_test(args.sdk)
        say("ИТОГ: " + ("все негативные пробы сработали" if ok else "ЕСТЬ ПРОВАЛЫ"))
        sys.exit(0 if ok else 1)

    call, info = load_reference(args.sdk)
    say("эталон: BuildId %s, символ по адресу %#x" % (info["buildid"], info["addr"]))
    out = build_and_run(SRC, HERE / "hash_test.cpp", "real")
    cases, ldn = parse(out)
    ok, bad = compare(cases, ldn, call)
    say("сошлось %d из %d" % (ok, len(cases) + len(ldn)))
    sys.exit(0 if not bad else 1)


if __name__ == "__main__":
    main()
