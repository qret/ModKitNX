#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
stubdiff.py — ДЕТЕКТОР ЗАГЛУШЕК и ПРОГНОЗ АВТОИСПРАВЛЕНИЯ на ПК.

Три режима, один каталог исправлений (src/nxr_autofix_catalog.h — тот же, что
компилируется в модуль):

1. ДЕТЕКТОР (--old <sdk консоли> --new <родная sdk игры> --game-exefs <папка>):
   какие функции, которые игра ИМПОРТИРУЕТ, в подменяемой (старой) sdk
     - отсутствуют совсем (без исправления rtld оставит ноль -> падение), или
     - являются заглушкой «вернуть константу» (`mov w0,#N; ret`), а в новой —
       настоящие.
   Импорты берутся у main и subsdk* игры (то, что разрешают сами модули игры,
   например malloc из main, — не в счёт). Итог делится на
     «известные — модуль исправит сам» (есть в каталоге) и
     «НОВЫЕ — найдено, исправления нет».
   ЧЕСТНАЯ ГРАНИЦА: исправление для новой функции детектор НЕ пишет — он только
   показывает, где искать. Своя реализация пишется руками, сверяется с оригиналом
   и вносится в каталог.

2. ПРОГНОЗ (--forecast --sdk <sdk консоли> --game-exefs <папка>): что выберет
   модуль на консоли с этой sdk для каждого исправления каталога — СВОЯ или
   ПРОКСИ и почему. Тот же алгоритм, что nxr::autofix::Decide в модуле, но по
   файлам, а не по памяти процесса; сверка двух способов — --self-test (с
   таблицей hosttest/run_autofix.py, полученной ЭМУЛЯЦИЕЙ готового модуля).

3. ПРОФИЛЬ ИГРЫ (--profile --game-exefs <папка> [--cnmt <файл>]): чем игра
   рисует (Vulkan / EGL-OpenGL / NVN — по импортам main и по модулям exefs),
   версия её родной sdk (строки SDK MW), сжатие ZBIC, и — если дан CNMT —
   required_system_version (какую прошивку игра требует). CNMT в exefs не
   лежит: его читает из NSP веб-версия (nxrweb.dump) или даёт человек файлом
   (.cnmt — двоичный, или .cnmt.xml).

    python stubdiff.py --old SDK19 --new <exefs>/sdk --game-exefs <exefs> [--json]
    python stubdiff.py --forecast --sdk SDK19 --game-exefs <exefs> [--json]
    python stubdiff.py --profile --game-exefs <exefs> [--cnmt X.cnmt] [--json]
    python stubdiff.py --self-test --exefs-root <папка с WD-patch, SIN-patch, TOTK-patch>
Коды возврата: детектор — 0 нет новых, 1 найдены НОВЫЕ (исправления нет),
2 — ошибка входа; прогноз и профиль — 0 / 2; самопроверка — 0 / 1 / 2.
Ничего не пишет и не удаляет.
"""

import argparse
import importlib.util
import json
import os
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CATALOG = ROOT / "src" / "nxr_autofix_catalog.h"
EXPORTS_HDR = ROOT / "src" / "nxr_compat_exports.hpp"
NSO_MIN = ROOT / "hosttest" / "nso_min.py"
# Папка с библиотеками NintendoSDK (только чтение) — переменная окружения NXR_SDK_DIR;
# без неё самопроверка скажет «нечем проверять» и вернёт 2 (пропуск, не провал).
SDK_DIR = Path(os.environ.get("NXR_SDK_DIR") or "NXR_SDK_DIR-не-задана")

FIX_RE = re.compile(r'NXR_FIX\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*(\w+)\s*,'
                    r'\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)', re.S)
VULKAN_SYM = "vkCreateInstance"
SDK_NAME = "nnSdk"


def setup_stdio():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def say(msg=""):
    print(msg, flush=True)


def _nso_mod():
    spec = importlib.util.spec_from_file_location("nxr_nso_min_sd", NSO_MIN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_NSO = None


def NSO(path):
    global _NSO
    if _NSO is None:
        _NSO = _nso_mod()
    return _NSO.NSO(str(path))


# ------------------------------------------------------------------ каталог

def load_catalog(path=CATALOG, text=None):
    """Записи NXR_FIX(...) по тексту файла: тот же список, что видит модуль."""
    text = text if text is not None else Path(path).read_text(encoding="utf-8")
    # Комментарии вида /* ... */ не мешают: в них NXR_FIX( с шестью аргументами нет.
    out = []
    for m in FIX_RE.finditer(text):
        out.append({"id": m.group(1), "symbol": m.group(2), "kind": m.group(3),
                    "scope": m.group(4), "history": m.group(5), "own": m.group(6)})
    return out


# ------------------------------------------------------------- разбор модулей

def is_return_const_stub(i0, i1):
    """Тот же признак, что nxr::autofix::IsReturnConstStub."""
    if i1 != 0xD65F03C0:
        return False
    op = i0 & 0xFF80001F
    return op in (0x52800000, 0xD2800000, 0x12800000, 0x92800000) or i0 in (0x2A1F03E0, 0xAA1F03E0)


def mw_version(n):
    """Наибольшая версия «SDK MW+Nintendo+NintendoSDK_/Sdk_<lib>-X_Y_Z» в хвосте rodata."""
    tail = n.ro[-0x4000:]
    best = None
    for m in re.finditer(rb"SDK MW\+Nintendo\+Nintendo(?:SDK|Sdk|SDk|SdK)_[^-\x00]{1,64}-(\d+)_(\d+)_(\d+)", tail):
        v = tuple(int(x) for x in m.groups())
        if best is None or v > best:
            best = v
    return best


def module_name(n):
    ro = n.ro
    if len(ro) < 8:
        return None
    w0, w1 = struct.unpack_from("<II", ro, 0)
    if w0 == 0:
        raw = ro[8:8 + w1]
    elif w0 == 1 and len(ro) >= 0x14:
        ln = struct.unpack_from("<I", ro, 0x10)[0]
        raw = ro[0x14:0x14 + ln]
    else:
        return None
    s = raw.rstrip(b"\0").decode("utf-8", "replace")
    s = re.split(r"[\\/]", s)[-1]
    return s.split(".")[0] or None


def body_is_stub(n, addr):
    b = n.mem(addr, 8)
    if len(b) < 8:
        return False
    i0, i1 = struct.unpack("<II", b)
    return is_return_const_stub(i0, i1)


def is_zbic(path):
    try:
        raw = Path(path).read_bytes()[:0x10]
        return raw[:4] == b"NSO0" and bool(struct.unpack_from("<I", raw, 0xC)[0] & 0x80)
    except OSError:
        return False


def game_files(exefs):
    exefs = Path(exefs)
    names = ["main"] + ["subsdk%d" % i for i in range(10)]
    return [(nm, exefs / nm) for nm in names if (exefs / nm).is_file()]


def game_imports(exefs):
    """Импорты модулей игры, которых модули игры сами не разрешают: {имя: (bind, [кто])}."""
    imports, exports, mods = {}, set(), {}
    for nm, p in game_files(exefs):
        n = NSO(p)
        mods[nm] = n
        for s, (info,) in n.imports().items():
            bind = info >> 4
            prev = imports.get(s)
            who = (prev[1] if prev else []) + [nm]
            # Хоть один сильный импорт — символ обязателен.
            imports[s] = (1 if (prev and prev[0] == 1) or bind != 2 else 2, who)
        exports |= set(n.exports())
    return {s: v for s, v in imports.items() if s not in exports}, mods


# --------------------------------------------------------------- 1. детектор

def detect(old_sdk, new_sdk, exefs, catalog=None):
    catalog = catalog if catalog is not None else load_catalog()
    known = {c["symbol"]: c for c in catalog}
    old, new = NSO(old_sdk), NSO(new_sdk)
    oex, nex = old.exports(), new.exports()
    imps, _mods = game_imports(exefs)
    found = []
    for sym, (bind, who) in sorted(imps.items()):
        o, nw = oex.get(sym), nex.get(sym)
        if nw is None:
            continue                      # нет и в новой: не дело sdk (например, WEAK ZSTD_trace_*)
        if o is None:
            kind = "MISSING"
        elif body_is_stub(old, o[0]) and not body_is_stub(new, nw[0]):
            kind = "STUB"
        else:
            continue
        c = known.get(sym)
        found.append({"symbol": sym, "kind": kind, "weak": bind == 2, "importers": who,
                      "known": c is not None, "fix": c["id"] if c else None,
                      "catalog_kind": c["kind"] if c else None})
    return {"old": str(old_sdk), "new": str(new_sdk), "exefs": str(exefs),
            "old_version": _v(mw_version(old)), "new_version": _v(mw_version(new)),
            "found": found,
            "known": [f for f in found if f["known"]],
            "new_unfixed": [f for f in found if not f["known"]]}


def _v(t):
    return "%d.%d.%d" % t if t else None


# ---------------------------------------------------------------- 2. прогноз

def decide(fix, sdk_known, defined, def_is_stub, sdk_ver, game_ver, vulkan):
    """Зеркало nxr::autofix::Decide (src/nxr_autofix.hpp). (choice, why)."""
    if not defined:
        return ("own", "SdkMissing" if sdk_known else "NoSdkMissing")
    if not sdk_known:
        return ("proxy", "NoSdkDefined")
    if fix["kind"] == "MISSING":
        return ("proxy", "SdkDefined")
    if not def_is_stub:
        return ("proxy", "SdkReal")
    if fix["scope"] == "NO_VULKAN" and vulkan:
        return ("proxy", "StubVulkan")
    if not sdk_ver or not game_ver:
        return ("proxy", "StubNoVersion")
    if sdk_ver >= game_ver:
        return ("proxy", "StubNative")
    return ("own", "StubOlderSdk")


def forecast(console_sdk, exefs, catalog=None):
    catalog = catalog if catalog is not None else load_catalog()
    sdk = NSO(console_sdk)
    sdk_known = module_name(sdk) == SDK_NAME
    sdk_ver = mw_version(sdk)
    game_ver, vulkan, vk_mod = None, False, None
    for nm, p in game_files(exefs):
        n = NSO(p)
        v = mw_version(n)
        if v and (game_ver is None or v > game_ver):
            game_ver = v
        if VULKAN_SYM in n.exports() and not vulkan:
            vulkan, vk_mod = True, nm
    ex = sdk.exports()
    res = {}
    for fix in catalog:
        d = ex.get(fix["symbol"])
        stub = bool(d) and body_is_stub(sdk, d[0])
        choice, why = decide(fix, sdk_known, bool(d), stub, sdk_ver, game_ver, vulkan)
        res[fix["id"]] = {"choice": choice, "why": why, "symbol": fix["symbol"],
                          "sdk_has": bool(d), "sdk_stub": stub}
    return {"sdk": str(console_sdk), "sdk_name": module_name(sdk), "sdk_known": sdk_known,
            "sdk_version": _v(sdk_ver), "game_version": _v(game_ver),
            "swapped": bool(sdk_ver and game_ver and sdk_ver < game_ver),
            "vulkan": vulkan, "vulkan_module": vk_mod, "fixes": res}


# ------------------------------------------------------------------ 3. профиль

def hos_version_text(v):
    return "%d.%d.%d" % ((v >> 26) & 0x3F, (v >> 20) & 0x3F, (v >> 16) & 0xF)


def read_cnmt(path):
    """required_system_version из .cnmt (двоичный) или .cnmt.xml. None — не прочли."""
    p = Path(path)
    data = p.read_bytes()
    if p.suffix.lower() == ".xml" or data.lstrip()[:1] == b"<":
        m = re.search(rb"<RequiredSystemVersion>\s*(\d+)\s*</RequiredSystemVersion>", data)
        if not m:
            return None
        v = int(m.group(1))
        return {"required_system_version": v, "text": hos_version_text(v), "source": "xml"}
    if len(data) < 0x28:
        return None
    tid, ver, ttype = struct.unpack_from("<QIB", data, 0)
    ext = struct.unpack_from("<H", data, 0x0E)[0]
    if ttype not in (0x80, 0x81) or ext < 0x10:
        return {"title_id": "%016X" % tid, "type": ttype, "required_system_version": None,
                "text": None, "source": "cnmt"}
    _id, rsv, _x = struct.unpack_from("<QII", data, 0x20)
    return {"title_id": "%016X" % tid, "version": ver, "type": ttype,
            "required_system_version": rsv, "text": hos_version_text(rsv), "source": "cnmt"}


def profile(exefs, cnmt=None):
    exefs = Path(exefs)
    mods = []
    gfx = {"vulkan": False, "opengl": False, "nvn": False}
    main_imports = set()
    for nm, p in game_files(exefs):
        n = NSO(p)
        ex = n.exports()
        if nm == "main":
            main_imports = set(n.imports())
        mods.append({"file": nm, "name": module_name(n), "mw_version": _v(mw_version(n)),
                     "zbic": is_zbic(p)})
        if VULKAN_SYM in ex:
            gfx["vulkan"] = True
        if "eglCreateWindowSurface" in ex:
            gfx["opengl"] = True
    uses = {"vulkan": any(s.startswith("vk") for s in main_imports),
            "opengl": any(s.startswith("egl") for s in main_imports),
            "nvn": any(s.startswith("nvn") for s in main_imports)}
    sdk_p = exefs / "sdk"
    sdk = NSO(sdk_p) if sdk_p.is_file() else None
    out = {"exefs": str(exefs), "modules": mods,
           "graphics_libraries": gfx, "main_imports_graphics": uses,
           "sdk_version": _v(mw_version(sdk)) if sdk else None,
           "sdk_buildid": sdk.buildid[:40] if sdk else None,
           "zbic_files": [m["file"] for m in mods if m["zbic"]] + (["sdk"] if is_zbic(sdk_p) else []),
           "cnmt": read_cnmt(cnmt) if cnmt else None}
    return out


def profile_lines(pr):
    gl = pr["graphics_libraries"]
    us = pr["main_imports_graphics"]
    lines = ["родная sdk игры: %s (BuildId %s)" % (pr["sdk_version"] or "не прочитана",
                                                   pr["sdk_buildid"] or "-"),
             "графика в exefs: Vulkan %s, OpenGL/EGL %s; main импортирует: vk* %s, egl* %s, nvn* %s"
             % tuple("да" if x else "нет" for x in (gl["vulkan"], gl["opengl"], us["vulkan"],
                                                     us["opengl"], us["nvn"])),
             "сжатие ZBIC: " + (", ".join(pr["zbic_files"]) if pr["zbic_files"] else "нет")]
    c = pr.get("cnmt")
    if c is None:
        lines.append("required_system_version: НЕ ИЗВЕСТНА — нужен CNMT (--cnmt; веб-версия "
                     "читает его из NSP)")
    elif c.get("required_system_version") is None:
        lines.append("required_system_version: в этом CNMT нет (тип %s)" % c.get("type"))
    else:
        lines.append("required_system_version: %s (0x%08X) — прошивка ниже требует подмены sdk "
                     "средствами консоли; модуль тогда включит нужные исправления сам"
                     % (c["text"], c["required_system_version"]))
    return lines


# --------------------------------------------------------------- самопроверка

def _self_test_cnmt(tmp, check):
    """CNMT двоичный и xml во временной папке tmp (часть самопроверки)."""
    blob = bytearray(0x40)
    struct.pack_into("<QIB", blob, 0, 0x0100217023F6C000, 0, 0x80)
    struct.pack_into("<H", blob, 0x0E, 0x10)
    struct.pack_into("<QII", blob, 0x20, 0x0100217023F6C800, 0x58500000, 0)
    (tmp / "t.cnmt").write_bytes(bytes(blob))
    (tmp / "t.cnmt.xml").write_text("<ContentMeta><RequiredSystemVersion>1481637888"
                                    "</RequiredSystemVersion></ContentMeta>", encoding="utf-8")
    c1, c2 = read_cnmt(tmp / "t.cnmt"), read_cnmt(tmp / "t.cnmt.xml")
    check(c1 and c1["text"] == "22.5.0" and c2 and c2["text"] == "22.5.0",
          "CNMT: required_system_version 0x58500000 -> 22.5.0 (двоичный и xml)")
    blob2 = bytearray(blob)
    blob2[0x0C] = 0x82    # тип AddOnContent: поля нет
    (tmp / "t2.cnmt").write_bytes(bytes(blob2))
    check(read_cnmt(tmp / "t2.cnmt")["required_system_version"] is None,
          "CNMT другого типа: required_system_version не выдумывается")


def self_test(root):
    ok = True
    n_checks = 0

    def check(cond, what):
        nonlocal ok, n_checks
        n_checks += 1
        say("   %s %s" % ("ДА " if cond else "НЕТ", what))
        ok &= bool(cond)

    cat = load_catalog()
    check(len(cat) == 3, "каталог разобран: %d записи" % len(cat))
    hdr = EXPORTS_HDR.read_text(encoding="utf-8")
    check(all(('asm("%s")' % c["symbol"]) in hdr for c in cat),
          "у каждой записи каталога есть экспорт в nxr_compat_exports.hpp")
    auto = (ROOT / "src" / "nxr_autofix.hpp").read_text(encoding="utf-8")
    check(all(c["symbol"] in auto for c in cat), "порядок/имена каталога закреплены static_assert")

    sdk = {v: SDK_DIR / f for v, f in (("19.3.13", "NintendoSdk_nnSdk-19_3_13-Release"),
                                       ("20.5.17", "NintendoSdk_nnSdk-20_5_17-Release"),
                                       ("22.2.0", "NintendoSdk_nnSdk-22_2_0-Release"))}
    if not all(p.is_file() for p in sdk.values()):
        say("нечем проверять: нет библиотек в %s" % SDK_DIR)
        return 2
    # Второй способ против первого: таблица run_autofix.py получена ЭМУЛЯЦИЕЙ
    # готового модуля; здесь то же самое считается по файлам.
    spec = importlib.util.spec_from_file_location("nxr_run_autofix", ROOT / "hosttest" / "run_autofix.py")
    ra = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ra)
    for (game, ver), exp in ra.EXPECT.items():
        ex = Path(root) / game
        if not (ex / "main").is_file():
            say("нечем проверять: нет %s" % ex)
            return 2
        fc = forecast(sdk[ver], ex, cat)
        got = {k: (v["choice"], v["why"]) for k, v in fc["fixes"].items()}
        check(got == exp, "прогноз по файлам = эмуляция модуля: %s + %s" % (game, ver))

    wd, sin = Path(root) / "WD-patch", Path(root) / "SIN-patch"
    d = detect(sdk["19.3.13"], wd / "sdk", wd, cat)
    check({f["fix"] for f in d["known"]} == {"hash_memory", "nvrm_query"},
          "WD 19.3.13 -> 22.2.0: известные = hash_memory, nvrm_query (%s)"
          % sorted(f["symbol"] for f in d["known"]))
    say("      новых (без исправления): %s" % ([f["symbol"] for f in d["new_unfixed"]] or "нет"))
    d2 = detect(sdk["19.3.13"], sin / "sdk", sin, cat)
    check({f["fix"] for f in d2["known"]} == {"hash_memory", "ldn_set_protocol", "nvrm_query"},
          "SiN 19.3.13 -> 22.2.5: известные = все три (%s)"
          % sorted(f["symbol"] for f in d2["known"]))
    say("      новых (без исправления): %s" % ([f["symbol"] for f in d2["new_unfixed"]] or "нет"))
    # Затвор нуля: одна и та же библиотека — находок нет.
    d3 = detect(sdk["22.2.0"], sdk["22.2.0"], wd, cat)
    check(not d3["found"], "затвор нуля: 22.2.0 против самой себя — находок нет")

    say("   --- негативные пробы ---")
    # 1) Каталог без nvrm_query: та же находка обязана стать НОВОЙ.
    short = [c for c in cat if c["id"] != "nvrm_query"]
    d4 = detect(sdk["19.3.13"], wd / "sdk", wd, short)
    check(any(f["symbol"] == "NvRmMemQueryHandleParams" for f in d4["new_unfixed"]),
          "без записи в каталоге NvRm уходит в «НОВЫЕ — исправления нет»")
    # 2) Ослабленный детектор (заглушку не распознаёт) NvRm не видит — настоящий видит.
    global body_is_stub
    real = body_is_stub
    body_is_stub = lambda n, a: False  # noqa: E731
    try:
        d5 = detect(sdk["19.3.13"], wd / "sdk", wd, cat)
    finally:
        body_is_stub = real
    check(not any(f["symbol"] == "NvRmMemQueryHandleParams" for f in d5["found"]),
          "ослабленный детектор (без распознавания заглушек) NvRm НЕ видит — значит ловит именно признак")
    # 3) Прогноз с выключенной защитой Vulkan дал бы SiN свою NvRm — настоящий нет.
    weak_cat = [dict(c, scope="ANY") for c in cat]
    fc = forecast(sdk["19.3.13"], sin, weak_cat)
    check(fc["fixes"]["nvrm_query"]["choice"] == "own",
          "без scope NO_VULKAN прогноз для SiN — «своя» (то, от чего защищает каталог)")
    # CNMT: значение из разбора дампов (FW-1.md): у Well Dweller и SiN — 22.5.0 = 0x58500000.
    # Своя свежая временная папка, убирается сразу после проверки CNMT
    # (прежде — общая %TEMP%\nxr-stubdiff-cnmt, оставалась навсегда).
    # NXR_KEEP_TEMP=1 — оставить для разбора.
    import os
    import shutil
    import tempfile
    tmp = Path(tempfile.mkdtemp(prefix="nxr-stubdiff-cnmt-"))
    try:
        _self_test_cnmt(tmp, check)
    finally:
        if os.environ.get("NXR_KEEP_TEMP", "").strip() not in ("", "0"):
            say("   NXR_KEEP_TEMP: папка проверки CNMT оставлена: %s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    # 4) Каталог из текста без записей — ноль.
    check(load_catalog(text="/* NXR_FIX(a, \"b\", MISSING, ANY) */") == [],
          "неполная запись (4 аргумента) каталогом не считается")
    if n_checks < 12:
        say("ЗАТВОР НУЛЯ: проверок %d" % n_checks)
        ok = False
    say("ИТОГ: " + ("как надо" if ok else "ЕСТЬ ПРОВАЛЫ"))
    return 0 if ok else 1


# ----------------------------------------------------------------------- main

def main():
    setup_stdio()
    ap = argparse.ArgumentParser(description="детектор заглушек, прогноз автоисправления, профиль игры")
    ap.add_argument("--old", help="sdk КОНСОЛИ (та, что подменяет родную)")
    ap.add_argument("--new", help="родная sdk игры (обычно <exefs>/sdk)")
    ap.add_argument("--game-exefs")
    ap.add_argument("--forecast", action="store_true")
    ap.add_argument("--sdk", help="для --forecast: sdk консоли")
    ap.add_argument("--profile", action="store_true")
    ap.add_argument("--cnmt", help="для --profile: файл .cnmt или .cnmt.xml игры")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--exefs-root")
    a = ap.parse_args()
    if a.self_test:
        if not a.exefs_root:
            say("--self-test требует --exefs-root")
            sys.exit(2)
        sys.exit(self_test(a.exefs_root))
    if not a.game_exefs or not Path(a.game_exefs).is_dir():
        say("нужна --game-exefs <папка>")
        sys.exit(2)
    if a.profile:
        pr = profile(a.game_exefs, a.cnmt)
        if a.json:
            print(json.dumps(pr, ensure_ascii=False, indent=1))
        else:
            for line in profile_lines(pr):
                say(line)
        sys.exit(0)
    if a.forecast:
        if not a.sdk or not Path(a.sdk).is_file():
            say("--forecast требует --sdk <файл sdk консоли>")
            sys.exit(2)
        fc = forecast(a.sdk, a.game_exefs)
        if a.json:
            print(json.dumps(fc, ensure_ascii=False, indent=1))
        else:
            say("sdk консоли: %s, версия %s; библиотеки игры %s; подмена: %s; Vulkan: %s"
                % (fc["sdk_name"], fc["sdk_version"], fc["game_version"],
                   "ДА" if fc["swapped"] else "нет", "да" if fc["vulkan"] else "нет"))
            for k, v in fc["fixes"].items():
                say("  %-17s %-6s %s" % (k, v["choice"].upper(), v["why"]))
        sys.exit(0)
    if not a.old or not a.new:
        say("детектор: нужны --old и --new (или --forecast / --profile)")
        sys.exit(2)
    d = detect(a.old, a.new, a.game_exefs)
    if a.json:
        print(json.dumps(d, ensure_ascii=False, indent=1))
    else:
        say("старая sdk %s -> новая %s, игра %s" % (d["old_version"], d["new_version"], d["exefs"]))
        for f in d["known"]:
            say("  ИЗВЕСТНАЯ (в каталоге; своя или прокси — решит модуль, см. --forecast): %-45s %s, импортируют %s"
                % (f["symbol"], f["kind"], ",".join(f["importers"])))
        for f in d["new_unfixed"]:
            say("  НОВАЯ — найдено, исправления НЕТ: %-45s %s%s, импортируют %s"
                % (f["symbol"], f["kind"], " (WEAK)" if f["weak"] else "", ",".join(f["importers"])))
        if not d["found"]:
            say("  расхождений по импортам игры нет")
    sys.exit(1 if d["new_unfixed"] else 0)


if __name__ == "__main__":
    main()
