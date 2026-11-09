#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_autofixtest — сборка и прогон hosttest/autofix_test.cpp (g++ в WSL) с тем же
src/nxr_autofix.hpp, что идёт в модуль; с --self-test — негативные пробы:
порча ОДНОГО места в КОПИИ заголовка обязана сделать тест красным.

    python run_autofixtest.py              # настоящая логика
    python run_autofixtest.py --self-test  # плюс порчи
Код возврата: 0 — как надо, 1 — провал, 2 — нечем собрать.
Ничего не удаляет: копии — в своей папке под %TEMP% и в /tmp WSL.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"
FILES = ["nxr_autofix.hpp", "nxr_autofix_catalog.h", "nxr_symfind.hpp", "nxr_fsgate.hpp"]
WSL_DISTRO = "Ubuntu"

WEAKENINGS = [
    ("публикация решения простой записью вместо compare_exchange",
     "        if (__atomic_compare_exchange_n(word, &expected, mine, false,\n"
     "                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {",
     "        __atomic_store_n(word, mine, __ATOMIC_RELEASE);\n        if (true) {"),
    ("проигравший поток берёт СВОЁ решение, а не опубликованное",
     "            w = expected;   /* опубликовано другим потоком раньше — берём ЕГО */",
     "            w = mine;"),
    ("паспорт пишется и при закрытом затворе ФС",
     "    if (!gate.IsOpen()) {\n        return false;\n    }\n    if (path == nullptr",
     "    if (path == nullptr"),
    ("нет защиты Vulkan (SiN получил бы свою NvRm)",
     "    if (fix.scope == Scope::NO_VULKAN && f.vulkan) {",
     "    if (false) {"),
    ("нет сравнения версий (родная старая игра получила бы свою NvRm)",
     "    if (f.sdkVer >= f.gameVer) {",
     "    if (false) {"),
    ("любой код считается заглушкой",
     "    if (i1 != 0xD65F03C0u) {\n        return false;\n    }",
     "    if (i1 != 0xD65F03C0u) {\n        return true;\n    }"),
    ("MISSING: символ в sdk есть, а берём свою (перекрыли бы родную функцию)",
     "    if (fix.kind == Kind::MISSING) {\n        r.why = Why::SdkDefined;\n        return r;",
     "    if (fix.kind == Kind::MISSING) {\n        r.why = Why::SdkDefined;\n        r.choice = Choice::Own; r.target = 0; return r;"),
    ("ЗАТВОР НУЛЯ: Decide всегда «своя»",
     "    Resolution r {};\n    if (f.def == 0) {",
     "    Resolution r {};\n    r.choice = Choice::Own; return r;\n    if (f.def == 0) {"),
]


def say(msg=""):
    print(msg, flush=True)


def setup_stdio():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def wsl_path(p):
    p = str(Path(p).resolve())
    return "/mnt/%s%s" % (p[0].lower(), p[2:].replace("\\", "/"))


# Метка «дошли до компилятора»: печатается скриптом прямо перед g++. Нет её в
# выводе — прогон НЕ ВЫПОЛНЯЛСЯ (WSL не запустилась, нет g++, не скопировались
# файлы), и ненулевой код возврата тогда не значит «тест красный» (так было:
# при занятом ext4.vhdx все негативные пробы печатали «КРАСНАЯ. Хорошо.»).
RAN_MARK = "NXR_HOSTTEST_COMPILE_START"


def ran(out):
    """Дошёл ли прогон до компилятора (иначе не проверено ничего)."""
    return RAN_MARK in out


def build_and_run(src_dir, tag):
    work = "/tmp/nxr-autofixtest-" + tag
    cps = "".join('cp "%s/%s" "$WORK/src/"\n' % (wsl_path(src_dir), f) for f in FILES)
    script = (
        "set -e\n"
        f'WORK="{work}"\n'
        'mkdir -p "$WORK/src" "$WORK/hosttest"\n'
        + cps +
        f'cp "{wsl_path(HERE / "autofix_test.cpp")}" "$WORK/hosttest/"\n'
        'cd "$WORK/hosttest"\n'
        'command -v g++ >/dev/null || { echo "нет g++ в WSL"; exit 97; }\n'
        f'echo "{RAN_MARK}"\n'
        "g++ -O2 -std=c++17 -Wall -pthread -o autofix_test autofix_test.cpp -I../src\n"
        "./autofix_test\n"
    )
    try:
        res = subprocess.run(["wsl", "-d", WSL_DISTRO, "--", "bash", "-l", "-s"],
                             input=script.encode("utf-8"), capture_output=True, timeout=900)
    except (OSError, subprocess.TimeoutExpired) as e:
        return 1, "wsl не запустилась: %s" % e
    out = res.stdout.decode("utf-8", "replace") + res.stderr.decode("utf-8", "replace")
    # wsl.exe пишет свои ошибки в UTF-16; убираем нули, чтобы текст был читаем.
    return res.returncode, out.replace("\x00", "")


def main():
    setup_stdio()
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    rc, out = build_and_run(SRC, "real")
    for line in out.splitlines():
        if "ПРОВАЛ" in line or "ИТОГ" in line or "проверок" in line or "error" in line:
            say("   " + line)
    ok = rc == 0 and "ВСЕ ПРОВЕРКИ ЗЕЛЁНЫЕ" in out
    if not ran(out):
        say("   настоящая логика: НЕ ВЫПОЛНЕНО (WSL или g++ недоступны) — ничего не проверено")
        ok = False
    if not ok:
        say(out[-3000:])
    if args.self_test:
        text = {f: (SRC / f).read_text(encoding="utf-8") for f in FILES}
        # Своя свежая временная папка на прогон, убирается в конце
        # (прежде — общая %TEMP%\nxr-autofix-weak, оставалась навсегда).
        # NXR_KEEP_TEMP=1 — оставить для разбора.
        root = Path(tempfile.mkdtemp(prefix="nxr-autofix-weak-"))
        try:
            ok = _weak_probes(text, root, ok)
        finally:
            if os.environ.get("NXR_KEEP_TEMP", "").strip() not in ("", "0"):
                say("   NXR_KEEP_TEMP: копии с порчей оставлены: %s" % root)
            else:
                shutil.rmtree(root, ignore_errors=True)
    say("ИТОГ: " + ("как надо" if ok else "ЕСТЬ ПРОВАЛЫ"))
    sys.exit(0 if ok else 1)


def _weak_probes(text, root, ok):
    """Порченые копии заголовка в root; каждая обязана сделать тест красным."""
    for i, (title, frm, to) in enumerate(WEAKENINGS):
        hdr = text["nxr_autofix.hpp"]
        if frm not in hdr:
            say("   проба %d (%s): фрагмент не найден — проба недействительна" % (i + 1, title))
            ok = False
            continue
        d = root / ("w%d" % i)
        d.mkdir(parents=True, exist_ok=True)
        for f in FILES:
            (d / f).write_text(text[f] if f != "nxr_autofix.hpp" else hdr.replace(frm, to, 1),
                               encoding="utf-8")
        wrc, wout = build_and_run(d, "w%d" % i)
        if not ran(wout):
            say("   проба %d (%s): НЕ ВЫПОЛНЕНО (WSL или g++ недоступны) — ничего не проверено"
                % (i + 1, title))
            ok = False
            continue
        red = wrc != 0
        first = next((l.strip() for l in wout.splitlines() if "ПРОВАЛ" in l or "ЗАТВОР" in l), "")
        say("   проба %d (%s): %s%s" % (i + 1, title, "КРАСНАЯ. Хорошо." if red else
                                        "осталась ЗЕЛЁНОЙ — тест ничего не ловит!",
                                        (" — " + first[:100]) if red and first else ""))
        ok &= red
    return ok


if __name__ == "__main__":
    main()
