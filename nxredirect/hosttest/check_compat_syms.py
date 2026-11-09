#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_compat_syms — проверка собранного модуля (NSO) на экспорт совместимости.

Что проверяется по разобранному NSO (а не по логам сборки):
  1. символ _ZNSt3__113__hash_memoryEPKvm присутствует ровно тогда, когда ждём;
  2. символ _ZN2nn3ldn11SetProtocolENS0_8ProtocolE — то же самое;
  3. присутствующий символ — GLOBAL (bind 1), FUNC (type 2), видимость DEFAULT
     (st_other = 0), индекс секции не SHN_UNDEF;
  4. у модуля по-прежнему НЕТ импортов (ни одного символа с shndx = 0):
     импорт означал бы, что модуль сам чего-то ждёт от других, а он не должен.

Зачем п.1 в обе стороны. Наш subsdkN ищется раньше `sdk`, поэтому экспорт
перекрывает одноимённый символ у всех, включая тех, у кого грузится родная `sdk`
и всё работает. Поэтому «символа нет в обычной сборке» — такое же требование,
как «символ есть в специальной».

  5. символ NvRmMemQueryHandleParams — исправление для подменённой
     sdk (-DNXR_COMPAT_NVRM_QUERY) — ровно тогда, когда ждём (--nvrm yes|no, по
     умолчанию no). Этот символ, в отличие от двух выше, ЕСТЬ в любой sdk: у
     родной 22.2 он настоящий, и лишний экспорт перекрыл бы его.

Запуск:
    python check_compat_syms.py <nso> --expect none|hash|ldn|both [--nvrm yes|no]
    python check_compat_syms.py --self-test <nso-none> <nso-hash> <nso-ldn> <nso-both>
    python check_compat_syms.py --self-test-nvrm <nso-с-исправлением> <nso-без>
"""

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import nso_min

SYM_HASH = "_ZNSt3__113__hash_memoryEPKvm"
SYM_LDN = "_ZN2nn3ldn11SetProtocolENS0_8ProtocolE"
SYM_NVRM = "NvRmMemQueryHandleParams"

EXPECT = {
    "none": (False, False),
    "hash": (True, False),
    "ldn": (False, True),
    "both": (True, True),
}


def say(msg=""):
    print(msg, flush=True)


def setup_stdio():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def inspect(path):
    """Вернуть словарь: что за символы и в каком они виде."""
    n = nso_min.NSO(str(path))
    found = {}
    for i, nm, info, other, shndx, val, size in n.syms():
        if nm in (SYM_HASH, SYM_LDN, SYM_NVRM):
            found[nm] = {"bind": info >> 4, "type": info & 0xF, "other": other,
                         "shndx": shndx, "value": val, "size": size}
    return {"found": found, "imports": sorted(n.imports().keys()),
            "exports": len(n.exports()), "buildid": n.buildid[:40]}


def check(path, expect, quiet=False, nvrm=False):
    """Вернуть список претензий (пустой = зелено)."""
    want_hash, want_ldn = EXPECT[expect]
    info = inspect(path)
    bad = []
    for sym, want in ((SYM_HASH, want_hash), (SYM_LDN, want_ldn), (SYM_NVRM, bool(nvrm))):
        got = info["found"].get(sym)
        if want and got is None:
            bad.append("нет символа %s, а он должен быть" % sym)
            continue
        if not want and got is not None:
            bad.append("символ %s есть, а его быть НЕ должно (перекрыл бы родной)" % sym)
            continue
        if got is None:
            continue
        if got["bind"] != 1:
            bad.append("%s: связывание %d, а нужно GLOBAL(1)" % (sym, got["bind"]))
        if got["type"] != 2:
            bad.append("%s: тип %d, а нужно FUNC(2)" % (sym, got["type"]))
        if got["other"] != 0:
            bad.append("%s: видимость %d, а нужно DEFAULT(0)" % (sym, got["other"]))
        if got["shndx"] == 0:
            bad.append("%s: SHN_UNDEF — это импорт, а не экспорт" % sym)
        if got["size"] == 0:
            bad.append("%s: нулевой размер — тело выброшено сборкой" % sym)
    if info["imports"]:
        bad.append("у модуля появились импорты: %s" % ", ".join(info["imports"][:5]))
    if not quiet:
        say("%s: экспортов %d, импортов %d" % (Path(path).name, info["exports"],
                                               len(info["imports"])))
        for sym, got in sorted(info["found"].items()):
            say("   %s @ %#x размер %d bind=%d type=%d vis=%d shndx=%d"
                % (sym, got["value"], got["size"], got["bind"], got["type"],
                   got["other"], got["shndx"]))
        for b in bad:
            say("   ПРЕТЕНЗИЯ: " + b)
    return bad


def self_test(paths):
    """
    Негативные пробы: каждый вариант сборки проверяется НЕ ТОЛЬКО своим ожиданием,
    но и чужими — на чужом ожидании проверка обязана стать красной. Это и есть
    доказательство, что проверка вообще что-то ловит (затвор нуля: если бы она
    всегда возвращала «зелено», ни одна чужая пара не покраснела бы).
    """
    ok = True
    for mode, path in paths.items():
        bad = check(path, mode)
        if bad:
            say("ПРОВАЛ: вариант %s не соответствует собственному ожиданию" % mode)
            ok = False
        else:
            say("   вариант %s: своё ожидание ЗЕЛЁНОЕ" % mode)
    say("--- негативные пробы (чужое ожидание обязано быть КРАСНЫМ) ---")
    for mode, path in paths.items():
        for other in EXPECT:
            if other == mode:
                continue
            bad = check(path, other, quiet=True)
            if bad:
                say("   %s под ожиданием %s: КРАСНОЕ (%s). Хорошо." % (mode, other, bad[0]))
            else:
                say("   %s под ожиданием %s: осталось ЗЕЛЁНЫМ — проверка ничего не ловит!"
                    % (mode, other))
                ok = False
    return ok


def self_test_nvrm(yes, no):
    """
    Негативные пробы для NvRmMemQueryHandleParams: сборка с исправлением под
    своим ожиданием зелёная, под чужим — красная; без исправления — наоборот.
    """
    ok = True
    for path, want in ((yes, True), (no, False)):
        mine = check(path, "both" if want else "none", quiet=True, nvrm=want)
        mine = [b for b in mine if SYM_HASH not in b and SYM_LDN not in b]
        other = check(path, "both" if want else "none", quiet=True, nvrm=not want)
        other = [b for b in other if SYM_NVRM in b]
        say("   %s: своё ожидание (nvrm=%s) %s, чужое %s"
            % (Path(path).name, want, "ЗЕЛЁНОЕ" if not mine else "КРАСНОЕ " + str(mine),
               "КРАСНОЕ (хорошо)" if other else "ЗЕЛЁНОЕ — проверка ничего не ловит!"))
        ok = ok and not mine and bool(other)
    return ok


def main():
    setup_stdio()
    ap = argparse.ArgumentParser(description="Проверка экспортов совместимости в NSO")
    ap.add_argument("nso", nargs="*")
    ap.add_argument("--expect", choices=sorted(EXPECT), default="none")
    ap.add_argument("--nvrm", choices=("yes", "no"), default="no",
                    help="ждать ли экспорт NvRmMemQueryHandleParams (-DNXR_COMPAT_NVRM_QUERY)")
    ap.add_argument("--self-test-nvrm", action="store_true",
                    help="принять 2 файла: с исправлением NvRm и без");
    ap.add_argument("--self-test", action="store_true",
                    help="принять 4 файла: none hash ldn both")
    args = ap.parse_args()

    if args.self_test_nvrm:
        if len(args.nso) != 2:
            raise SystemExit("для --self-test-nvrm нужно 2 файла: с исправлением и без")
        ok = self_test_nvrm(args.nso[0], args.nso[1])
        say("ИТОГ: " + ("все пробы сработали" if ok else "ЕСТЬ ПРОВАЛЫ"))
        sys.exit(0 if ok else 1)

    if args.self_test:
        if len(args.nso) != 4:
            raise SystemExit("для --self-test нужно 4 файла: none hash ldn both")
        paths = dict(zip(["none", "hash", "ldn", "both"], args.nso))
        ok = self_test(paths)
        say("ИТОГ: " + ("все пробы сработали" if ok else "ЕСТЬ ПРОВАЛЫ"))
        sys.exit(0 if ok else 1)

    if len(args.nso) != 1:
        raise SystemExit("нужен ровно один файл NSO")
    bad = check(args.nso[0], args.expect, nvrm=(args.nvrm == "yes"))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
