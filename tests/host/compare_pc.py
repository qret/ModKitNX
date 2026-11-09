#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
compare_pc.py — сверка ядра ModKitNX (core_cli) с эталонной реализацией (Python, install.py) на НАСТОЯЩИХ входах.

Входы только читаются. Эталон копируется в рабочую папку (его install.py с --apply пишет
в <ROOT>/out/, а трогать исходный эталон нельзя), там же — все назначения. Ничего не
удаляется: каждый прогон — новая папка run-<время>.

Пути машины в репо не пишутся: всё задаётся переменными среды (или ключами):
  MK_PIPELINE   папка эталона nxredirect (install.py, nxr_build.py, scripts/, src/, hosttest/)
  MK_MODULES    папка modules эталона (manifest.json, autofix/subsdk, autofix-earlyfs/subsdk)
  MK_GAMES      папка с exefs игр: подпапки WD-patch, SIN-patch, TOTK-patch (+ <имя>.cnmt рядом)
  MK_EXTRA      ещё exefs (через ';'), ставятся обычной установкой
  MK_MOD        мод целиком для TOTK-patch (дерево atmosphere/contents/<TID>/…)
  MK_SDK        папка с nnSdk 19.3.13 / 20.5.17 / 22.2.0 (и прочими библиотеками)
  MK_WORK       рабочая папка (временная, вне репо)
  MK_CLI        путь к core_cli ВНУТРИ WSL (по умолчанию /tmp/mkbuild/core_cli)
  MK_WSL        дистрибутив WSL (по умолчанию Ubuntu)

Что сравнивается: деревья раскладки (пути + sha256, .bak-<время> нормализуется),
квитанция ТЕКСТОМ (время подставляется из квитанции эталона, mod.source — путь в разных ОС),
выход C++ прогоняется гейтами эталона (zbic_nso.py --check, npdm_syscalls.py --check), stubdiff
--json против core_cli forecast/detect/profile, classify_nso, NACP против эталонного parse_nacp, поиск sdk по
содержимому (файлы под случайными именами), негативные пробы (испорченный вход — красный).
Коды возврата: 0 — всё сошлось; 1 — есть расхождения; 2 — нет входов / затвор нуля.
"""

import argparse
import ast
import datetime
import hashlib
import importlib.util
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

RESULTS = []          # (ok, имя, подробность)
MIN_CHECKS = 40       # затвор нуля


def check(ok, name, detail=""):
    RESULTS.append((bool(ok), name, detail))
    print(("  ДА  " if ok else "  НЕТ ") + name + (("  — " + detail) if detail and not ok else ""), flush=True)
    return ok


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def _fix_entries(text):
    """Записи каталога — строки с NXR_FIX( (комментарии и лицензия в сверку не входят)."""
    return [ln.strip() for ln in text.replace(b"\r\n", b"\n").split(b"\n") if b"NXR_FIX(" in ln]


def catalog_matches(mine, module_copy, reference):
    """(ok, причина): копия ядра побайтно = копия модуля (nxredirect/src, она поставляется в модуле);
    записи NXR_FIX — те же, что у эталона (комментарии эталона могут отличаться)."""
    if mine != module_copy:
        return False, "копия ядра отличается от nxredirect/src: sha256 %s против %s" % (
            hashlib.sha256(mine).hexdigest()[:12], hashlib.sha256(module_copy).hexdigest()[:12])
    a, b = _fix_entries(mine), _fix_entries(reference)
    if not a or a != b:
        return False, "записи NXR_FIX расходятся с эталоном: %d против %d" % (len(a), len(b))
    return True, ""


def catalog_negatives(mine, module_copy, reference):
    """Сверка обязана краснеть: лишний байт, изменённая буква записи, лишняя запись у эталона."""
    check(not catalog_matches(mine + b"\n", module_copy, reference)[0], "негатив: каталог с лишним байтом — отказ")
    i = mine.find(b"NXR_FIX(")
    bad = mine[:i] + b"X" + mine[i + 1:] if i >= 0 else mine + b"x"
    check(not catalog_matches(bad, bad, reference)[0], "негатив: изменённая запись — отказ")
    check(not catalog_matches(mine, module_copy, reference + b"\nNXR_FIX(x)\n")[0],
          "негатив: у эталона лишняя запись — отказ")


STAMP_RE = re.compile(r"\.(bak|removed)-\d{8}-\d{6}")


def lf_sha(p):
    """sha256 текста квитанции с LF: эталон на Windows пишет её write_text() — с CRLF."""
    return hashlib.sha256(Path(p).read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def tree(root):
    """{путь от корня: sha256 | '<dir>'} — .bak-/.removed-<время> нормализуются;
    квитанции (nxredirect.json и её .bak-) сравниваются как текст с LF."""
    out = {}
    root = Path(root)
    if not root.exists():
        return out
    for p in sorted(root.rglob("*")):
        rel = STAMP_RE.sub(lambda m: "." + m.group(1) + "-STAMP", p.relative_to(root).as_posix())
        out[rel] = "<dir>" if p.is_dir() else (lf_sha(p) if p.name.startswith("nxredirect.json") else sha(p))
    return out


def tree_diff(a, b):
    d = []
    for k in sorted(set(a) | set(b)):
        if a.get(k) != b.get(k):
            d.append("%s: эталон=%s ядро=%s" % (k, (a.get(k) or "нет")[:12], (b.get(k) or "нет")[:12]))
    return d


class Env:
    def __init__(self, a):
        self.pipe_src = Path(a.pipeline)
        self.modules = Path(a.modules)
        self.games = Path(a.games)
        self.extra = [Path(x) for x in (a.extra or "").split(";") if x]
        self.mod = Path(a.mod) if a.mod else None
        self.sdk = Path(a.sdk) if a.sdk else None
        self.run = Path(a.work) / ("run-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
        self.cli = a.cli
        self.wsl = a.wsl
        self.run.mkdir(parents=True)
        self.pipe = self.run / "pipeline"
        ign = shutil.ignore_patterns("dist", "out", "out-diag", "game-exefs*", "__pycache__", "*.removed-*")
        shutil.copytree(self.pipe_src, self.pipe, ignore=ign)
        self.py = sys.executable

    @staticmethod
    def wslpath(p):
        p = str(Path(p).resolve()).replace("\\", "/")
        return "/mnt/" + p[0].lower() + p[2:]

    def cli_run(self, *args):
        conv = []
        for x in args:
            x = str(x)
            conv.append(self.wslpath(x) if re.match(r"^[A-Za-z]:[\\/]", x) else x)
        t0 = time.time()
        r = subprocess.run(["wsl", "-d", self.wsl, "--", self.cli] + conv, capture_output=True)
        out = r.stdout.decode("utf-8", "replace")
        try:
            j = json.loads(out) if out.strip() else None
        except Exception:
            j = {"error": "NOT_JSON", "message": out[-2000:] + r.stderr.decode("utf-8", "replace")[-2000:]}
        return r.returncode, j, time.time() - t0

    def pc(self, script, *args):
        env = dict(os.environ, PYTHONIOENCODING="utf-8")
        r = subprocess.run([self.py, str(self.pipe / script)] + [str(x) for x in args],
                           capture_output=True, cwd=str(self.pipe), env=env)
        return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace")


def load_pipeline_module(env, name, rel):
    sys.path.insert(0, str(env.pipe))
    spec = importlib.util.spec_from_file_location(name, env.pipe / rel)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


# ------------------------------------------------------------------ установка: эталон против ядра

def install_case(env, name, exefs, flags, core_flags=None, pre=None, seq=None, module="autofix"):
    """Один случай: те же шаги в эталоне и в ядре в разные назначения, затем сверка."""
    print("== " + name, flush=True)
    base = env.run / "cases" / name
    pc_dest, core_dest, cons_dest = base / "pc", base / "core", base / "console"
    for d in (pc_dest, core_dest, cons_dest):
        d.mkdir(parents=True)
        if pre:
            pre(d)
    steps = seq or [flags]
    stamps_seen = set()
    for i, st in enumerate(steps):
        # вид сборки шага: --diag / --diag-gfx — отладочные варианты поставки (гейт эталона сверяет модуль с ключами)
        kind = ("-diag" if "--diag" in st else "") + ("-gfx" if "--diag-gfx" in st else "")
        mod_main = env.modules / (module + kind if kind else module) / "subsdk"
        mod_early = env.modules / ("autofix" + kind + "-earlyfs") / "subsdk"
        modargs = [] if "--zbic-only" in st else ["--module", mod_early if "--mod" in st else mod_main, "--no-build"]
        rc, out = env.pc("install.py", "--exefs", exefs, "--dest", pc_dest, *modargs, "--apply", *st)
        if not check(rc == 0, "%s: эталон install.py шаг %d отработал" % (name, i + 1), out[-800:]):
            return None
        rec = json.loads((next(pc_dest.glob("atmosphere/contents/*/nxredirect.json"))).read_text(encoding="utf-8"))
        now = rec["installed"]
        # STAMP эталона (одно на прогон): по новым .bak-<время>; ядру — то же, чтобы имена совпали
        stamps = set()
        for q in pc_dest.rglob("*"):
            m = re.search(r"\.bak-(\d{8}-\d{6})", q.name)
            if m:
                stamps.add(m.group(1))
        new = sorted(stamps - stamps_seen)
        stamps_seen |= stamps
        stamp = new[0] if new else "20000101-000000"
        cst = [x for x in st if x != "--no-build"]
        for dest, ib in ((core_dest, "none"), (cons_dest, "console")):
            rc2, j, dt = env.cli_run("install", "--exefs", exefs, "--dest", dest, "--module", mod_main,
                                     "--module-early", mod_early, "--installed-by", ib, "--now", now,
                                     "--stamp", stamp, "--apply", *(core_flags or cst))
            if not check(rc2 == 0 and j and "error" not in j, "%s: ядро шаг %d (%s) отработало за %.1f с"
                         % (name, i + 1, ib, dt), json.dumps(j, ensure_ascii=False)[:800]):
                return None
    ta, tb = tree(pc_dest), tree(core_dest)
    # сама квитанция сверяется ниже ТЕКСТОМ (время, STAMP, mod.source, CRLF эталона); здесь — только что она есть
    for t in (ta, tb):
        for k in [k for k in t if k.endswith("/nxredirect.json")]:
            t[k] = "<квитанция>"
    check(ta == tb, "%s: дерево раскладки эталона == ядро (%d путей)" % (name, len(ta)), "; ".join(tree_diff(ta, tb))[:1500])
    ra = next(pc_dest.glob("atmosphere/contents/*/nxredirect.json"))
    rb = next(core_dest.glob("atmosphere/contents/*/nxredirect.json"))
    ja, jb = json.loads(ra.read_text(encoding="utf-8")), json.loads(rb.read_text(encoding="utf-8"))
    # sha256 копии прежней квитанции у эталона — от файла с CRLF; приводим к LF и пишем тем же json.dumps эталона
    for k in list((ja.get("own_backups") or {})):
        if "nxredirect.json.bak-" in k:
            ja["own_backups"][k] = lf_sha(pc_dest / k)
    ta_text = json.dumps(ja, ensure_ascii=False, indent=2) + "\n"
    tb_text = rb.read_bytes().decode("utf-8")
    if isinstance(ja.get("sdk_swap"), dict) and isinstance(jb.get("sdk_swap"), dict):
        # время подмены sdk эталон берёт своим datetime.now() — секунда может разойтись с "installed"
        tail = ',\n    "verified_on_console"'
        tb_text = tb_text.replace('"installed": "%s"%s' % (jb["sdk_swap"]["installed"], tail),
                                  '"installed": "%s"%s' % (ja["sdk_swap"]["installed"], tail))
    if isinstance(ja.get("mod"), dict) and isinstance(jb.get("mod"), dict):
        # mod.source — путь к моду: у эталона Windows-путь, у ядра — путь WSL; подставляем вариант эталона
        tb_text = tb_text.replace(json.dumps(jb["mod"]["source"], ensure_ascii=False),
                                  json.dumps(ja["mod"]["source"], ensure_ascii=False))
    check(ta_text == tb_text, "%s: квитанция побайтно == эталон (время и STAMP — из эталона; CRLF эталона -> LF)" % name,
          "\nэталон:\n%s\nядро:\n%s" % (ta_text[-900:], tb_text[-900:]))
    rc_ = next(cons_dest.glob("atmosphere/contents/*/nxredirect.json"))
    jc = json.loads(rc_.read_text(encoding="utf-8"))
    extra = [k for k in jc if k not in jb]
    same = all(jc[k] == jb[k] or k in ("mod", "own_backups") for k in jb)
    check(same and jc.get("installed_by") == "console" and extra == ["installed_by"] and list(jc)[-1] == "installed_by",
          "%s: квитанция консоли = квитанция эталона + installed_by в конце" % name, str(extra))
    # гейты эталона на выходе ядра
    tid = jb["game_tid"]
    ex = core_dest / "atmosphere" / "contents" / tid / "exefs"
    for z in jb.get("zbic_files") or []:
        rc, out = env.pc("scripts/zbic_nso.py", "--check", ex / z, "--orig", Path(exefs) / z)
        check(rc == 0 and "13 of 13" in out, "%s: гейт эталона zbic_nso.py --check на %s ядра" % (name, z), out[-400:])
    if (ex / "main.npdm").is_file() and jb.get("npdm_sha256"):
        rc, out = env.pc("scripts/npdm_syscalls.py", ex / "main.npdm", "--check", "--program-id", tid)
        check(rc == 0, "%s: гейт эталона npdm_syscalls.py --check на main.npdm ядра" % name, out[-400:])
    return cons_dest, tid


def uninstall_case(env, name, cons_dest, tid, expect_tree, modify=None):
    """Снятие ядром по квитанции (настоящее удаление): дерево вернулось к expect_tree."""
    if modify:
        modify(cons_dest)
    rc, j, _ = env.cli_run("uninstall", "--dest", cons_dest, "--tid", tid)
    check(rc == 0 and j and j.get("actions"), "%s: план снятия построен (%d действий)"
          % (name, len((j or {}).get("actions") or [])), json.dumps(j, ensure_ascii=False)[:600])
    rc, j, _ = env.cli_run("uninstall", "--dest", cons_dest, "--tid", tid, "--apply")
    ok = rc == 0 and j and not (j.get("applied") or {}).get("problems")
    check(ok, "%s: снятие выполнено" % name, json.dumps(j, ensure_ascii=False)[:600])
    got = tree(cons_dest)
    check(got == expect_tree, "%s: после снятия дерево = как до установки" % name,
          "; ".join(tree_diff(expect_tree, got))[:1200])
    return j


# ------------------------------------------------------------------ stubdiff

def strip_keys(d, keys):
    return {k: v for k, v in d.items() if k not in keys}


def stubdiff_cases(env):
    print("== stubdiff: прогноз / детектор / профиль", flush=True)
    sdks = {}
    for f in sorted(env.sdk.iterdir()):
        m = re.search(r"nnSdk-(\d+)_(\d+)_(\d+)", f.name)
        if f.is_file() and m:
            sdks["%s.%s.%s" % m.groups()] = f
    check(len(sdks) == 3, "найдены три nnSdk для прогноза: " + ", ".join(sorted(sdks)))
    for g in ("WD-patch", "SIN-patch", "TOTK-patch"):
        ex = env.games / g
        for v, f in sorted(sdks.items()):
            rc, out = env.pc("scripts/stubdiff.py", "--forecast", "--sdk", f, "--game-exefs", ex, "--json")
            pa = json.loads(out) if rc == 0 else None
            rc2, pb, _ = env.cli_run("forecast", "--sdk", f, "--game-exefs", ex)
            check(pa is not None and pb and strip_keys(pa, {"sdk"}) == strip_keys(pb, {"sdk"}),
                  "прогноз %s + %s: ядро == stubdiff.py" % (g, v), "эталон %s / ядро %s" % (pa, pb))
        cn = env.games / (g + ".cnmt")
        args = ["--cnmt", cn] if cn.is_file() else []
        rc, out = env.pc("scripts/stubdiff.py", "--profile", "--game-exefs", ex, "--json", *args)
        pa = json.loads(out) if rc == 0 else None
        rc2, pb, _ = env.cli_run("profile", "--game-exefs", ex, *args)
        check(pa is not None and pb and strip_keys(pa, {"exefs"}) == strip_keys(pb, {"exefs"}),
              "профиль %s: ядро == stubdiff.py" % g, "эталон %s / ядро %s" % (pa, pb))
    old = sdks.get("19.3.13")
    for g in ("WD-patch", "SIN-patch"):
        ex = env.games / g
        rc, out = env.pc("scripts/stubdiff.py", "--old", old, "--new", ex / "sdk", "--game-exefs", ex, "--json")
        pa = json.loads(out) if out.strip().startswith("{") else {}
        rc2, pb, _ = env.cli_run("detect", "--old", old, "--new", ex / "sdk", "--game-exefs", ex)
        keys = {"old", "new", "exefs"}
        check(pb is not None and strip_keys(pa, keys) == strip_keys(pb, keys) and rc == rc2,
              "детектор %s 19.3.13 -> родная: ядро == stubdiff.py (код %d)" % (g, rc), "эталон %s / ядро %s" % (pa, pb))
    # затвор нуля детектора: та же библиотека против себя — находок нет
    rc2, pb, _ = env.cli_run("detect", "--old", sdks["22.2.0"], "--new", sdks["22.2.0"], "--game-exefs", env.games / "WD-patch")
    check(pb and not pb["found"], "детектор: 22.2.0 против самой себя — находок нет")


# ------------------------------------------------------------------ classify

def classify_cases(env):
    print("== classify_nso: ядро против эталона", flush=True)
    nb = load_pipeline_module(env, "nxr_build_cmp", "nxr_build.py")
    files = [p for p in sorted(env.modules.rglob("subsdk")) if p.is_file()]
    for g in ("WD-patch", "SIN-patch", "TOTK-patch"):
        files += [p for p in sorted((env.games / g).iterdir()) if p.is_file() and not p.name.endswith(".cnmt")]
    n = 0
    for p in files:
        a = nb.classify_nso(p)[0]
        rc, j, _ = env.cli_run("classify", p)
        n += 1
        check(j and j.get("class") == a, "classify %s/%s: %s" % (p.parent.name, p.name, a), str(j))
    return n


# ------------------------------------------------------------------ sdk по содержимому

def sdk_scan_case(env):
    print("== поиск sdk по содержимому (случайные имена, мусор, дубль)", flush=True)
    rnd = random.Random(20261108)
    a, b = env.run / "sdkscan" / "modkit", env.run / "sdkscan" / "dbi" / "deep" / "er"
    a.mkdir(parents=True)
    b.mkdir(parents=True)
    libs = [f for f in sorted(env.sdk.iterdir()) if f.is_file()]
    nn = [f for f in libs if "nnSdk" in f.name]
    for f in libs:
        shutil.copy2(f, a / ("x%08x.bin" % rnd.getrandbits(32)))
    (a / "garbage.dat").write_bytes(bytes(rnd.getrandbits(8) for _ in range(5000)))
    (a / "fake-nso").write_bytes(b"NSO0" + bytes(0x200))
    shutil.copy2(nn[0], b / ("копия_%d" % rnd.getrandbits(16)))
    (b / "junk").write_bytes(b"\0" * 100)
    rc, j, _ = env.cli_run("sdk-scan", "%s=modkit" % a, "%s=dbi" % (env.run / "sdkscan" / "dbi"))
    vers = sorted(e["version"] for e in (j or []))
    check(vers == ["19.3.13", "20.5.17", "22.2.0"], "найдено ровно три nnSdk: " + ", ".join(map(str, vers)), str(j)[:600])
    dup = [e for e in (j or []) if len(e["sources"]) == 2]
    check(len(dup) == 1 and {s["label"] for s in dup[0]["sources"]} == {"modkit", "dbi"},
          "дубль из второй папки посчитан один раз (два источника у одной записи)", str(dup)[:400])


# ------------------------------------------------------------------ NACP

def web_parse_nacp(env_web_nca):
    src = Path(env_web_nca).read_text(encoding="utf-8")
    tree_ = ast.parse(src)
    keep = [n for n in tree_.body if (isinstance(n, ast.Assign) and any(
        isinstance(t, ast.Name) and t.id.startswith("NACP_") for t in n.targets)) or
        (isinstance(n, ast.FunctionDef) and n.name in ("_cstr", "parse_nacp"))]
    ns = {"DumpProblem": ValueError}
    exec(compile(ast.Module(body=keep, type_ignores=[]), "nca_nacp", "exec"), ns)
    return ns["parse_nacp"]


def nacp_cases(env, web_nca):
    print("== NACP: ядро против эталона (синтетика, сжатая таблица SDK 20+)", flush=True)
    parse = web_parse_nacp(web_nca)
    d = env.run / "nacp"
    d.mkdir()

    def make(titles, ver):
        n = bytearray(0x4000)
        for i, (name, pub) in titles.items():
            nb, pb = name.encode(), pub.encode()
            n[i * 0x300:i * 0x300 + len(nb)] = nb
            n[i * 0x300 + 0x200:i * 0x300 + 0x200 + len(pb)] = pb
        n[0x3060:0x3060 + len(ver)] = ver.encode()
        return n

    cases = {
        "en": ({0: ("Well Dweller", "Pub"), 11: ("Колодезник", "Изд")}, "1.0.3"),
        "ru-only": ({11: ("  Тени ", "")}, " 2.1 "),
        "brit": ({1: ("Colour", "UK"), 2: ("色", "JP")}, "v9"),
        "empty": ({}, ""),
    }
    for nm, (titles, ver) in cases.items():
        plain = make(titles, ver)
        (d / (nm + ".nacp")).write_bytes(plain)
        want = parse(bytes(plain))
        rc, j, _ = env.cli_run("nacp", d / (nm + ".nacp"))
        got = {k: j.get(k) for k in want} if j else None
        check(got == want, "NACP %s: ядро == эталонный parse_nacp" % nm, "эталон %s / ядро %s" % (want, j))
        if titles:
            co = zlib.compressobj(9, zlib.DEFLATED, -15)
            raw = co.compress(bytes(plain[:0x300 * 16])) + co.flush()
            comp = bytearray(plain)
            comp[:0x3000] = bytes(0x3000)
            comp[0:2] = struct.pack("<H", len(raw))
            comp[2:2 + len(raw)] = raw
            (d / (nm + "-z.nacp")).write_bytes(comp)
            rc, j, _ = env.cli_run("nacp", d / (nm + "-z.nacp"))
            got = {k: j.get(k) for k in want} if j else None
            check(got == want and j.get("compressed") is True, "NACP %s (сжатая таблица): распакована и == эталон" % nm,
                  "эталон %s / ядро %s" % (want, j))


# ------------------------------------------------------------------ ZBIC: замер и негативные пробы

def zbic_cases(env):
    print("== ZBIC: побайтно с zbic_nso.py, замер, негативные пробы", flush=True)
    d = env.run / "zbic"
    d.mkdir()
    for g in ("WD-patch", "SIN-patch"):
        src = env.games / g / "main"
        rc, out = env.pc("scripts/zbic_nso.py", "--rebuild", src, "-o", d / (g + "-pc"))
        r = subprocess.run(["wsl", "-d", env.wsl, "--", "/usr/bin/time", "-f", "%M %e", env.cli, "zbic-rebuild",
                            env.wslpath(src), "-o", env.wslpath(d / (g + "-core"))], capture_output=True)
        tm = r.stderr.decode().strip().splitlines()[-1].split()
        check(rc == 0 and r.returncode == 0 and sha(d / (g + "-pc")) == sha(d / (g + "-core")),
              "%s main: несжатый NSO ядра == zbic_nso.py побайтно (%d Б, ядро %s с, пик %d МБ)"
              % (g, (d / (g + "-core")).stat().st_size, tm[1], int(tm[0]) // 1024), out[-300:])
    rc, j, _ = env.cli_run("zbic-scan", env.games / "TOTK-patch")
    check(j and j["ok"] and not j["zbic"], "TOTK-patch: сжатия ZBIC нет (распознано по содержимому)", str(j))
    rc, j, _ = env.cli_run("install", "--exefs", env.games / "TOTK-patch", "--dest", d / "totk-zo", "--zbic-only")
    check(rc != 0 and j.get("error") == "FAIL_ZBIC_NOT_FOUND", "негатив: --zbic-only на TOTK — FAIL_ZBIC_NOT_FOUND", str(j))
    raw = bytearray((env.games / "SIN-patch" / "main").read_bytes())
    fo, csz = struct.unpack_from("<I", raw, 0x10)[0], struct.unpack_from("<I", raw, 0x60)[0]
    bad = bytearray(raw)
    bad[fo + csz // 2] ^= 0x5A
    (d / "bad-seg").write_bytes(bad)
    rc, j, _ = env.cli_run("zbic-rebuild", d / "bad-seg", "-o", d / "bad-seg.out")
    check(rc != 0 and j.get("error") in ("ZBIC_DECODE", "ZBIC_HASH"), "негатив: порча сжатого сегмента — отказ (%s)"
          % (j or {}).get("error"), str(j))
    bad = bytearray(raw)
    bad[fo:fo + 4] = b"ZBIX"
    (d / "bad-magic").write_bytes(bad)
    rc, j, _ = env.cli_run("zbic-rebuild", d / "bad-magic", "-o", d / "bad-magic.out")
    check(rc != 0, "негатив: испорченная магия кадра — отказ", str(j))
    good = (d / "SIN-patch-core").read_bytes()
    for what, off in (("байт в .text", 0x100 + 0x40 + 100), ("флаги", 0x0C)):
        bad = bytearray(good)
        bad[off] ^= 0x01
        (d / "bad-rb").write_bytes(bad)
        rc, j, _ = env.cli_run("zbic-check", d / "bad-rb", "--orig", env.games / "SIN-patch" / "main")
        check(rc != 0 and j["problems"], "негатив: гейт 13 проверок краснеет (%s)" % what, str(j))
    rc, j, _ = env.cli_run("zbic-check", d / "SIN-patch-core", "--orig", env.games / "SIN-patch" / "main")
    check(rc == 0 and j["checks"] == 13 and not j["problems"], "гейт ядра на годном: 13 из 13", str(j))
    # npdm: бит SdCard уже стоит — эталон отказывает (ровно 2 байта), ядро тоже
    npdm = env.run / "cases"
    pcn = next(npdm.glob("*/pc/atmosphere/contents/*/exefs/main.npdm"), None)
    if pcn:
        rc, j, _ = env.cli_run("npdm-game", pcn, "-o", d / "npdm-twice")
        rc2, out = env.pc("scripts/npdm_sdcard.py", pcn, "-o", d / "npdm-twice-pc")
        check(rc != 0 and rc2 != 0, "негатив: npdm с уже стоящим SdCard — отказ и у эталона (код %d), и у ядра" % rc2, str(j))
    bad = bytearray((env.games / "WD-patch" / "main.npdm").read_bytes())
    bad[0:4] = b"MATE"
    (d / "bad.npdm").write_bytes(bad)
    rc, j, _ = env.cli_run("npdm-game", d / "bad.npdm", "-o", d / "bad.npdm.out")
    check(rc != 0, "негатив: npdm без META — отказ", str(j))


def main():
    ap = argparse.ArgumentParser(description="сверка ядра ModKitNX с эталонной реализацией")
    g = os.environ.get
    ap.add_argument("--pipeline", default=g("MK_PIPELINE"))
    ap.add_argument("--modules", default=g("MK_MODULES"))
    ap.add_argument("--games", default=g("MK_GAMES"))
    ap.add_argument("--extra", default=g("MK_EXTRA"))
    ap.add_argument("--mod", default=g("MK_MOD"))
    ap.add_argument("--sdk", default=g("MK_SDK"))
    ap.add_argument("--web-nca", default=g("MK_WEB_NCA"), help="эталонный dump/nca.py (parse_nacp)")
    ap.add_argument("--work", default=g("MK_WORK"))
    ap.add_argument("--cli", default=g("MK_CLI", "/tmp/mkbuild/core_cli"))
    ap.add_argument("--wsl", default=g("MK_WSL", "Ubuntu"))
    a = ap.parse_args()
    need = [k for k in ("pipeline", "modules", "games", "work") if not getattr(a, k)]
    if need:
        print("нет входов: " + ", ".join("--" + k for k in need) + " (или MK_* в среде)")
        return 2
    env = Env(a)
    print("рабочая папка: " + str(env.run))
    G = env.games
    repo = Path(__file__).resolve().parents[2]
    mine = (repo / "core" / "src" / "nxr_autofix_catalog.h").read_bytes()
    module_copy = (repo / "nxredirect" / "src" / "nxr_autofix_catalog.h").read_bytes()
    reference = (env.pipe_src / "src" / "nxr_autofix_catalog.h").read_bytes()
    ok, why = catalog_matches(mine, module_copy, reference)
    check(ok, "каталог автоисправления: ядро = модуль побайтно, записи = эталон", why)
    catalog_negatives(mine, module_copy, reference)

    # 1. только снять ZBIC
    install_case(env, "WD-zbic-only", G / "WD-patch", ["--zbic-only"])
    install_case(env, "SIN-zbic-only", G / "SIN-patch", ["--zbic-only"])
    # 2. установка модуля + npdm + снятие ZBIC
    r_wd = install_case(env, "WD-install-zbic", G / "WD-patch", ["--zbic-fix"])
    r_sin = install_case(env, "SIN-install-zbic", G / "SIN-patch", ["--zbic-fix"])
    r_totk = install_case(env, "TOTK-install", G / "TOTK-patch", [])
    # 3. порядок «сначала ZBIC, потом мод» и повторная установка (обновление, .bak квитанции)
    install_case(env, "WD-zbic-then-install", G / "WD-patch", None, seq=[["--zbic-only"], []])
    install_case(env, "SIN-install-twice", G / "SIN-patch", None, seq=[["--zbic-fix"], ["--zbic-fix"]])
    # 3а. вид сборки: обычная -> отладочная -> обычная — свой модуль перезаписывается начисто,
    # без .bak; квитанция называет diag, diag_gfx, module_variant; own_backups пуст
    r_kind = install_case(env, "WD-plain-diag-plain", G / "WD-patch", None,
                          seq=[["--zbic-fix"], ["--zbic-fix", "--diag"], ["--zbic-fix"]])
    install_case(env, "SIN-diag-gfx-then-plain", G / "SIN-patch", None,
                 seq=[["--zbic-fix", "--diag", "--diag-gfx"], ["--zbic-fix"]])
    if r_kind:
        cons, tid = r_kind
        rj = json.loads((cons / "atmosphere" / "contents" / tid / "nxredirect.json").read_text(encoding="utf-8"))
        baks = [q.name for q in cons.rglob("*") if ".bak-" in q.name]
        check(not baks and rj.get("own_backups") == {} and rj.get("module_variant") == "autofix"
              and rj.get("diag") is False and rj.get("diag_gfx") is False,
              "обычная -> отладочная -> обычная: ни одного .bak, own_backups пуст, module_variant=autofix",
              "bak=%s own_backups=%s variant=%s" % (baks, rj.get("own_backups"), rj.get("module_variant")))

    # 4. чужой main в назначении: заменяется с копией .bak, попадает в neighbours
    def foreign_main(dest):
        ex = dest / "atmosphere" / "contents" / "01007A2027548000" / "exefs"
        ex.mkdir(parents=True)
        (ex / "main").write_bytes(b"foreign main, not ours")
        (ex / "subsdk7").write_bytes(b"not an nso at all")
    tid_wd = None
    if r_wd:
        tid_wd = r_wd[1]

    def foreign_main_wd(dest):
        ex = dest / "atmosphere" / "contents" / tid_wd / "exefs"
        ex.mkdir(parents=True)
        (ex / "main").write_bytes(b"foreign main, not ours")
        (ex / "readme.txt").write_text("чужой файл", encoding="utf-8")
    r_wdf = install_case(env, "WD-foreign-main", G / "WD-patch", ["--zbic-fix"], pre=foreign_main_wd) if tid_wd else None
    # 5. мод целиком (TotK + UltraCam)
    r_mod = None
    if env.mod:
        r_mod = install_case(env, "TOTK-mod-ultracam", G / "TOTK-patch", ["--mod", env.mod])
    # подмена sdk (--swap-sdk эталона) — формат sdk_swap 1:1, чужой sdk в назначении откладывается
    r_swap = None
    if env.sdk and tid_wd:
        nn19 = next((f for f in sorted(env.sdk.iterdir()) if "nnSdk-19_3_13" in f.name), None)

        def foreign_sdk_wd(dest):
            ex = dest / "atmosphere" / "contents" / tid_wd / "exefs"
            ex.mkdir(parents=True)
            (ex / "sdk").write_bytes(b"someone else's sdk")
        if nn19:
            r_swap = install_case(env, "WD-swap-sdk-foreign", G / "WD-patch", ["--zbic-fix", "--swap-sdk", nn19],
                                  pre=foreign_sdk_wd)
    for ex in env.extra:
        install_case(env, "extra-" + ex.parent.name + "-" + ex.name, ex, [])

    # игра обновилась после установки — ModuleId нашего main против exefs игры
    if r_wd:
        rc, j, _ = env.cli_run("status", "--dest", r_wd[0], "--tid", r_wd[1], "--game-exefs", G / "WD-patch")
        check(j and j.get("matches_game") is True, "устаревание: установка WD совпадает с exefs WD", str(j)[:300])
        rc, j, _ = env.cli_run("status", "--dest", r_wd[0], "--tid", r_wd[1], "--game-exefs", G / "SIN-patch")
        check(j and j.get("matches_game") is False and j.get("stale") == ["main"],
              "устаревание, негатив: «другая версия игры» (main SiN) — устарело, main", str(j)[:300])

    # 6. снятие ядром по квитанции (настоящее удаление)
    print("== снятие по квитанции (ядро)", flush=True)
    for nm, r in (("WD-install-zbic", r_wd), ("TOTK-mod-ultracam", r_mod), ("WD-plain-diag-plain", r_kind)):
        if r:
            uninstall_case(env, "снятие " + nm, r[0], r[1], {})
    if r_swap:
        tmp = env.run / "pre-swap"
        tmp.mkdir()
        foreign_sdk_wd(tmp)
        uninstall_case(env, "снятие WD-swap-sdk-foreign (чужой sdk возвращается)", r_swap[0], r_swap[1], tree(tmp))
    if r_wdf:
        pre_tree = {}
        tmp = env.run / "pre-foreign"
        tmp.mkdir()
        foreign_main_wd(tmp)
        pre_tree = tree(tmp)
        uninstall_case(env, "снятие WD-foreign-main (чужой main и readme возвращаются)", r_wdf[0], r_wdf[1], pre_tree)
    if r_sin:
        cons, tid = r_sin
        ex = cons / "atmosphere" / "contents" / tid / "exefs"

        def modify(dest):
            (ex / "main").write_bytes(b"changed after install")
        j = uninstall_case(env, "снятие SIN с изменённым main", cons, tid,
                           {"atmosphere": "<dir>", "atmosphere/contents": "<dir>",
                            "atmosphere/contents/" + tid: "<dir>", "atmosphere/contents/" + tid + "/exefs": "<dir>",
                            "atmosphere/contents/" + tid + "/exefs/main": hashlib.sha256(b"changed after install").hexdigest()},
                           modify=modify)
        check(any("изменён" in w for w in (j or {}).get("warnings", [])),
              "изменённый после установки main не удалён, в плане — предупреждение")

    if env.sdk:
        stubdiff_cases(env)
        sdk_scan_case(env)
    classify_cases(env)
    if a.web_nca:
        nacp_cases(env, a.web_nca)
    zbic_cases(env)

    ok = sum(1 for r in RESULTS if r[0])
    bad = [r for r in RESULTS if not r[0]]
    print("\nИТОГ: проверок %d, сошлось %d, расхождений %d" % (len(RESULTS), ok, len(bad)))
    for r in bad:
        print("  НЕТ " + r[1])
    if len(RESULTS) < MIN_CHECKS:
        print("ЗАТВОР НУЛЯ: проверок меньше %d" % MIN_CHECKS)
        return 2
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
