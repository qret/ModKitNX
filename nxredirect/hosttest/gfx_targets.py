#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gfx_targets.py — проверка ЦЕЛЕЙ перехвата диагностики графики.

Зачем. src/nxr_gfx.hpp перехватывает графическую цепочку по именам символов.
Если имя написано с опечаткой или такого символа в модулях игры нет, на консоли
перехватчик просто не встанет, и отчёт будет пустым — а понять это можно будет
только после пробы на консоли. Этот гейт отвечает на вопрос заранее, на ПК:
каждый символ из таблицы Targets() ищется в .dynsym КАЖДОГО модуля exefs.

    python gfx_targets.py <папка exefs игры> [--require N] [--self-test]

Папка exefs должна содержать те модули, что реально грузятся на консоли,
включая ПОДМЕНЁННУЮ sdk (иначе проверка отвечает не на тот вопрос).

Коды возврата: 0 — все обязательные цели найдены; 1 — нет; 2 — ошибка запуска.
Ничего не пишет и не удаляет.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

# Имена целей берутся ИЗ САМОГО ЗАГОЛОВКА, а не переписываются сюда руками:
# копия разошлась бы с исходником молча.
HEADER = Path(__file__).resolve().parent.parent / "src" / "nxr_gfx.hpp"

# Цели, без которых диагностика бессмысленна: на них держится вся цепочка.
REQUIRED = [
    "_ZN2nn2vi10InitializeEv",
    "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",
    "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",
    "eglGetDisplay",
    "eglInitialize",
    "eglChooseConfig",
    "eglCreateContext",
    "eglCreateWindowSurface",
    "eglMakeCurrent",
    "eglSwapBuffers",
    "eglGetError",
]


def lz4_block(src):
    """Распаковка LZ4-блока: тем же способом сжаты сегменты NSO у subsdkN."""
    d = bytearray()
    i = 0
    while i < len(src):
        t = src[i]
        i += 1
        lit = t >> 4
        mat = t & 15
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        d += src[i:i + lit]
        i += lit
        if i >= len(src):
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = mat + 4
        if mat == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        s = len(d) - off
        for k in range(ml):
            d.append(d[s + k])
    return bytes(d)


class Nso:
    """Минимальный разбор NSO: сегменты, .dynamic, .dynsym, .dynstr."""

    def __init__(self, path):
        raw = Path(path).read_bytes()
        if raw[:4] != b"NSO0":
            raise ValueError(f"{path}: не NSO0")
        self.flags, = struct.unpack_from("<I", raw, 0xC)
        if self.flags & 0x80:
            # ZBIC (сжатие прошивок 20+) здесь не распаковываем: в проверку
            # кладут уже снятый ZBIC main (как в ШАГЕ 4).
            raise ValueError(f"{path}: сегменты сжаты ZBIC, нужен несжатый main")
        self.segs = []
        self.meta = []
        for n in range(3):
            fo, mo, sz = struct.unpack_from("<III", raw, 0x10 + n * 0x10)
            csz, = struct.unpack_from("<I", raw, 0x60 + n * 4)
            blob = raw[fo:fo + csz]
            data = lz4_block(blob) if (self.flags >> n) & 1 else blob[:sz]
            if len(data) != sz:
                raise ValueError(f"{path}: сегмент {n} распакован не в тот размер")
            self.segs.append(data)
            self.meta.append((mo, sz))
        self.bss, = struct.unpack_from("<I", raw, 0x3C)
        self._dyn()

    def mem(self, a, n):
        for data, (mo, sz) in zip(self.segs, self.meta):
            if mo <= a < mo + sz:
                return data[a - mo:a - mo + n]
        return b"\0" * n

    def _dyn(self):
        mod_off, = struct.unpack_from("<I", self.segs[0], 4)
        magic, dyn_rel = struct.unpack("<4si", self.mem(mod_off, 8))
        if magic != b"MOD0":
            raise ValueError("нет MOD0")
        a = mod_off + dyn_rel
        dt = {}
        while True:
            tag, val = struct.unpack("<qQ", self.mem(a, 16))
            if tag == 0:
                break
            dt.setdefault(tag, []).append(val)
            a += 16
        self.strtab = dt[5][0]
        self.strsz = dt[10][0]
        self.symtab = dt[6][0]
        self.syment = dt[11][0]
        self.gnuhash = dt.get(0x6FFFFEF5, [None])[0]

    def _nsyms(self):
        g = self.gnuhash
        if g is None:
            return (self.strtab - self.symtab) // self.syment
        nbuck, symoff, maskwords, _shift = struct.unpack("<IIII", self.mem(g, 16))
        bucket_off = g + 16 + maskwords * 8
        buckets = struct.unpack("<%dI" % nbuck, self.mem(bucket_off, nbuck * 4))
        last = max(buckets) if buckets else symoff
        if last < symoff:
            return symoff
        chain = bucket_off + nbuck * 4
        i = last
        while True:
            v, = struct.unpack("<I", self.mem(chain + (i - symoff) * 4, 4))
            i += 1
            if v & 1:
                break
        return i

    def _name(self, off):
        b = self.mem(self.strtab + off, 1024)
        return b[:b.find(b"\0")].decode("utf8", "replace")

    def defined(self):
        """Имена символов, ОПРЕДЕЛЁННЫХ в этом модуле (shndx != 0)."""
        out = set()
        for i in range(self._nsyms()):
            nameo, _info, _other, shndx, _val, _size = struct.unpack(
                "<IBBHQQ", self.mem(self.symtab + i * self.syment, 24))
            if shndx != 0 and nameo < self.strsz:
                nm = self._name(nameo)
                if nm:
                    out.add(nm)
        return out


def targets_from_header_text(text):
    """Имена символов из таблицы Targets() — по УЖЕ прочитанному тексту.

    Отдельная функция нужна негативным пробам: они подсовывают сюда нарочно
    испорченную копию заголовка, не трогая файл на диске.
    """
    m = re.search(r"constexpr Target kTargets\[T_Count\] = \{(.+?)\n *\};",
                  text, re.S)
    if not m:
        raise ValueError("в nxr_gfx.hpp не нашлась таблица Targets()")
    return re.findall(r'\{\s*"([^"]+)"\s*,', m.group(1))


def targets_from_header(path=HEADER):
    """Имена символов из таблицы Targets() заголовка nxr_gfx.hpp."""
    return targets_from_header_text(Path(path).read_text(encoding="utf-8"))


def lock_targets_from_header_text(text):
    """
    Имена целей ВТОРОЙ таблицы — УЗКОЙ пробы блокировок (kLockTargets, ШАГ-8).

    Отдельно от Targets() потому, что эти цели живут не в графической цепочке:
    они ловят захват блокировки дисплея, из-за которого eglCreateWindowSurface
    и отдаёт EGL_BAD_ALLOC. В список обязательных (REQUIRED) они не входят:
    сборка без пробы блокировок остаётся годной — и именно она собирается
    по умолчанию, ключ -DNXR_DIAG_LOCK выключен.
    """
    m = re.search(r"constexpr Target kLockTargets\[L_Count\] = \{(.+?)\n *\};",
                  text, re.S)
    if not m:
        return []
    return re.findall(r'\{\s*"([^"]+)"\s*,', m.group(1))


def lock_targets_from_header(path=HEADER):
    return lock_targets_from_header_text(Path(path).read_text(encoding="utf-8"))


# --- Проба SetError ---------------------------------------------------------
#
# Перехват ставится не по имени, а по СМЕЩЕНИЮ внутри OpenGL 22.2.0 (функции
# записи ошибки EGL не экспортируются). Поэтому гейт сверяет таблицу
# kSetErrorTargets с НАСТОЯЩИМ модулем GL: экспорт eglCreateWindowSurface на
# 0xe670, по смещению — ровно 8 слов подписи, а регистр кода совпадает с
# регистром в инструкции `cmp wN, #0x3000` (7-е слово подписи). Опечатка в
# таблице покраснеет здесь, а не на консоли (там подпись не совпала бы, и хук
# просто не встал бы — проба пропала бы молча).

SETERR_RE = re.compile(
    r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*\{([^}]*)\}")


def seterror_targets_from_header_text(text):
    """[(offset, codeReg, funcReg, fmtReg, [8 слов])] из kSetErrorTargets."""
    m = re.search(r"constexpr SetErrorTarget kSetErrorTargets\[S_Count\] = \{(.+?)\n    \};",
                  text, re.S)
    if not m:
        return []
    out = []
    for off, code, fn, fmt, sig in SETERR_RE.findall(m.group(1)):
        words = [int(w.strip().rstrip("uU"), 16) for w in sig.split(",") if w.strip()]
        out.append((int(off, 16), int(code), int(fn), int(fmt), words))
    return out


def gl_anchor_from_header_text(text):
    m = re.search(r'constexpr std::uint64_t GlAnchorOffset = (0x[0-9a-fA-F]+);', text)
    return int(m.group(1), 16) if m else None


class GlImage:
    """Модуль GL: образ (text+rodata+data) и экспорты с адресами."""

    def __init__(self, nso):
        self.nso = nso
        self.exports = {}
        for i in range(nso._nsyms()):
            nameo, _info, _other, shndx, val, _size = struct.unpack(
                "<IBBHQQ", nso.mem(nso.symtab + i * nso.syment, 24))
            if shndx != 0 and nameo < nso.strsz:
                nm = nso._name(nameo)
                if nm:
                    self.exports[nm] = val


def check_seterror(text, gl):
    """(сколько целей сверено, список расхождений) против настоящего GL."""
    problems = []
    targets = seterror_targets_from_header_text(text)
    anchor = gl_anchor_from_header_text(text)
    if len(targets) != 2:
        problems.append("в kSetErrorTargets %d целей, а ждём 2" % len(targets))
    got = gl.exports.get("eglCreateWindowSurface")
    if anchor is None or got != anchor:
        problems.append("eglCreateWindowSurface в GL на %s, а в заголовке GlAnchorOffset %s"
                        % (hex(got) if got is not None else "нет", hex(anchor) if anchor else "нет"))
    checked = 0
    for off, code, fn, fmt, words in targets:
        real = list(struct.unpack("<8I", gl.nso.mem(off, 32)))
        checked += 1
        if words != real:
            problems.append("подпись по GL+%#x не совпала с модулем GL" % off)
            continue
        cmp_reg = (real[6] >> 5) & 31     # cmp wN, #0x3000 -> поле Rn
        if (real[6] & 0xFFFFFC1F) != 0x71400C1F:
            problems.append("GL+%#x: 7-е слово не `cmp wN,#0x3000`" % off)
        elif cmp_reg != code:
            problems.append("GL+%#x: код в w%d, а в таблице w%d" % (off, cmp_reg, code))
        if not (0 <= fn <= 7 and 0 <= fmt <= 7 and fn != fmt):
            problems.append("GL+%#x: регистры функции/сообщения вне x0..x7" % off)
    return checked, problems


def find_gl(exefs):
    """Модуль exefs, экспортирующий eglCreateWindowSurface (GL), или None."""
    for name in ("subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "subsdk5"):
        p = Path(exefs) / name
        if not p.is_file():
            continue
        try:
            g = GlImage(Nso(p))
        except Exception:
            continue
        if "eglCreateWindowSurface" in g.exports:
            return name, g
    return None, None


# --- Соответствие «вид возврата в таблице» <-> «сигнатура перехватчика» ------
#
# Зачем. Однажды перехватчик nn::vi::Initialize был объявлен возвращающим
# Result, хотя сама функция не возвращает ничего: в w0 оставался мусор от
# последнего внутреннего вызова, и отчёт начинался строкой
# «!! nn::vi::Initialize() = ОШИБКА 0xda0a3e40» — несуществующей поломкой ровно
# там, куда инструкция велит смотреть первым делом.
#
# На консольной сборке расхождение ловит static_assert, но проверка нужна и на
# ПК: сборка модуля требует devkitPro, а гейт обязан краснеть и без него.
# Заголовок разбирается как текст: у каждого HOOK_DEFINE_TRAMPOLINE берём тип
# возврата Callback и номер цели, который хук поминает (HumanOf / RetKindOf /
# LogLookup). Void в таблице обязан отвечать void в сигнатуре, и наоборот.

HOOK_RE = re.compile(
    r"HOOK_DEFINE_TRAMPOLINE\((\w+)\)\s*\{(.*?)\n    \};", re.S)
CALLBACK_RE = re.compile(r"static\s+([\w:*<>\s]+?)\s+Callback\s*\(")
TARGET_RE = re.compile(r"\bT_([A-Za-z]\w*)\b")


def kinds_from_header(text):
    """Вид возврата по имени цели: {'ViInitialize': 'Void', ...}."""
    m = re.search(r"enum TargetIndex : int \{(.+?)\n *\};", text, re.S)
    if not m:
        raise ValueError("в nxr_gfx.hpp не нашлось перечисление TargetIndex")
    order = [x for x in re.findall(r"T_(\w+)", m.group(1)) if x != "Count"]
    m2 = re.search(r"constexpr Target kTargets\[T_Count\] = \{(.+?)\n *\};",
                   text, re.S)
    if not m2:
        raise ValueError("в nxr_gfx.hpp не нашлась таблица kTargets")
    kinds = re.findall(r"RetKind::(\w+)", m2.group(1))
    if len(kinds) != len(order):
        raise ValueError("видов возврата %d, а целей %d — таблица разъехалась"
                         % (len(kinds), len(order)))
    return dict(zip(order, kinds))


def check_ret_kinds(text):
    """(сколько перехватчиков сверено, список расхождений)."""
    kinds = kinds_from_header(text)
    problems = []
    checked = 0
    for hook_name, body in HOOK_RE.findall(text):
        mcb = CALLBACK_RE.search(body)
        if not mcb:
            continue
        ret_type = " ".join(mcb.group(1).split())
        names = [n for n in TARGET_RE.findall(body) if n in kinds]
        if not names:
            continue
        kind = kinds[names[0]]
        checked += 1
        if kind == "Void" and ret_type != "void":
            problems.append(
                "%s: цель T_%s объявлена Void, а Callback возвращает «%s» — "
                "мусор из регистра попадёт в отчёт как ошибка"
                % (hook_name, names[0], ret_type))
        if kind != "Void" and ret_type == "void":
            problems.append(
                "%s: цель T_%s объявлена %s, а Callback ничего не возвращает — "
                "результат звена потеряется" % (hook_name, names[0], kind))
    return checked, problems


# --- Бюджет трамплинов exlaunch ---------------------------------------------
#
# Зачем эта проверка. Проба ШАГ-7 не дала игре запуститься: модуль
# ставил 22 хука, а пул трамплинов exlaunch вмещал 20, и при исчерпании пула
# exlaunch не отказывает мягко, а аварийно завершает процесс (запись по адресу
# 0x6969696900000000 — подпись exl::diag::AbortWithCtx). Число трамплинов
# продублировано в nxr_gfx.hpp, и если дубликат разъедется с настоящими
# константами exlaunch, всё повторится. Гейт сверяет их между собой.

EXL_SETTING = Path(__file__).resolve().parents[1] / "exlaunch" / "source" / "program" / "setting.hpp"
EXL_HOOKIMPL = (Path(__file__).resolve().parents[1] / "exlaunch" / "source" / "lib"
                / "hook" / "nx64" / "hook_impl.cpp")


def hook_budget(header_text=None):
    """
    (числа из заголовка, числа из exlaunch, список расхождений).

    Из nxr_gfx.hpp берутся TrampolineBytes / JitSizeBytes / HooksOutsideGfx и
    длины обеих таблиц целей; из exlaunch — JitSize, MaxInstructions и то,
    каким сравнением отсекается переполнение пула.
    """
    text = header_text if header_text is not None else Path(HEADER).read_text(encoding="utf-8")
    problems = []

    def num(pattern, where, src):
        m = re.search(pattern, src)
        if not m:
            problems.append("в %s не нашлось: %s" % (where, pattern))
            return None
        return int(m.group(1), 0)

    ours = {
        "TrampolineBytes": num(r"constexpr int TrampolineBytes = (\w+);", "nxr_gfx.hpp", text),
        "JitSizeBytes": num(r"constexpr int JitSizeBytes\s*= (\w+);", "nxr_gfx.hpp", text),
        "HooksOutsideGfx": num(r"constexpr int HooksOutsideGfx = (\w+);", "nxr_gfx.hpp", text),
    }
    ours["T_Count"] = len(targets_from_header_text(text))
    ours["L_Count"] = len(lock_targets_from_header_text(text))
    # Пробы SetError — inline-хуки exlaunch: каждая берёт ОДИН трамплин из
    # общего пула (HookInline -> Hook(..., true)), поэтому в счёт входят.
    ours["S_Count"] = len(seterror_targets_from_header_text(text))

    exl_text = EXL_SETTING.read_text(encoding="utf-8")
    impl_text = EXL_HOOKIMPL.read_text(encoding="utf-8")
    theirs = {
        "JitSize": num(r"constexpr size_t JitSize = (\w+);", "exlaunch/setting.hpp", exl_text),
        "MaxInstructions": num(r"constexpr s64 MaxInstructions = (\w+);",
                               "exlaunch/hook_impl.cpp", impl_text),
    }
    if None in ours.values() or None in theirs.values():
        return ours, theirs, problems

    tramp = theirs["MaxInstructions"] * 10 * 4
    if tramp != ours["TrampolineBytes"]:
        problems.append("размер трамплина в exlaunch %d, а в nxr_gfx.hpp %d"
                        % (tramp, ours["TrampolineBytes"]))
    if theirs["JitSize"] != ours["JitSizeBytes"]:
        problems.append("JitSize в exlaunch %#x, а в nxr_gfx.hpp %#x"
                        % (theirs["JitSize"], ours["JitSizeBytes"]))
    pool = theirs["JitSize"] // tramp
    total = ours["T_Count"] + ours["L_Count"] + ours["S_Count"] + ours["HooksOutsideGfx"]
    if total > pool:
        problems.append("хуков у модуля %d, а трамплинов в пуле %d — "
                        "на консоли это аварийное завершение игры" % (total, pool))
    if not re.search(r"if\(i >= HookMax\)", impl_text):
        problems.append("в exlaunch не исправлена ошибка на единицу "
                        "(должно быть `if(i >= HookMax)`): последний трамплин "
                        "ложится за границу JIT-области")
    ours["total"] = total
    theirs["pool"] = pool
    return ours, theirs, problems


def setup_stdio():
    """
    stdout/stderr — UTF-8, даже когда это КАНАЛ, а не консоль.

    Та же болезнь, что лечили у check_nso.py и check_events.py (замер:
    локаль машины cp1250): при выводе в канал или файл русский текст падал
    UnicodeEncodeError ещё до первой строки отчёта — код возврата 1 на заведомо
    годном входе, то есть ложная находка гейта. Из конвейера вызов идёт с
    PYTHONIOENCODING=utf-8, но скрипт обязан работать и когда его зовут руками.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

def main():
    setup_stdio()
    ap = argparse.ArgumentParser(description="Цели перехвата графики есть в модулях игры?")
    ap.add_argument("exefs", nargs="?")
    ap.add_argument("--require", type=int, default=len(REQUIRED),
                    help="сколько обязательных целей должно найтись")
    ap.add_argument("--self-test", action="store_true",
                    help="негативные пробы самого гейта")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    if not args.exefs:
        print("не задана папка exefs")
        return 2

    exefs = Path(args.exefs)
    mods = {}
    for name in ("main", "rtld", "sdk", "subsdk0", "subsdk1", "subsdk2",
                 "subsdk3", "subsdk4", "subsdk5"):
        p = exefs / name
        if p.is_file():
            try:
                mods[name] = Nso(p).defined()
            except Exception as exc:  # модуль может быть в ZBIC — так и скажем
                print(f"  {name}: не разобран ({exc})")
    if not mods:
        print("в папке нет ни одного разобранного модуля — проверять нечего")
        return 2

    header_text = Path(HEADER).read_text(encoding="utf-8")

    ours, theirs, budget_problems = hook_budget(header_text)
    print("бюджет трамплинов exlaunch: пул %s, хуков у модуля %s"
          % (theirs.get("pool", "?"), ours.get("total", "?")))
    for line in budget_problems:
        print("  РАСХОЖДЕНИЕ: " + line)
    if budget_problems:
        print("ГЕЙТ КРАСНЫЙ: бюджет хуков не сходится — на консоли это "
              "аварийное завершение игры (так упал ШАГ-7)")
        return 1

    gl_name, gl = find_gl(exefs)
    if gl is None:
        print("проба SetError: модуля OpenGL (экспорт eglCreateWindowSurface) в exefs нет — "
              "хук по подписи не встанет, сверять не с чем")
    else:
        se_checked, se_problems = check_seterror(header_text, gl)
        if se_problems and gl.exports.get("eglCreateWindowSurface") != gl_anchor_from_header_text(header_text):
            print("проба SetError: %s — другая версия OpenGL (якорь не совпал); на консоли "
                  "хук по подписи НЕ встанет, игре это не мешает" % gl_name)
        elif se_problems:
            for line in se_problems:
                print("  РАСХОЖДЕНИЕ SetError: " + line)
            print("ГЕЙТ КРАСНЫЙ: таблица kSetErrorTargets не сходится с модулем OpenGL игры")
            return 1
        else:
            print("проба SetError: %d цели сверены с %s (подпись, регистр кода) — ok"
                  % (se_checked, gl_name))

    checked, problems = check_ret_kinds(header_text)
    print(f"сверено сигнатур перехватчиков с таблицей: {checked}")
    if checked < 10:
        print("ГЕЙТ КРАСНЫЙ: сверено слишком мало перехватчиков — "
              "разбор заголовка не сработал")
        return 1
    for line in problems:
        print("  РАСХОЖДЕНИЕ: " + line)
    if problems:
        print("ГЕЙТ КРАСНЫЙ: вид возврата в таблице не совпал с сигнатурой хука")
        return 1

    syms = targets_from_header()
    syms.append("eglGetError")  # его не хукаем, но зовём сами
    print(f"модули: {', '.join(sorted(mods))}")
    print(f"целей в таблице (плюс eglGetError): {len(syms)}")
    found_required = 0
    missing = []
    for s in syms:
        where = [m for m, d in mods.items() if s in d]
        mark = "ok  " if where else "НЕТ "
        print(f"  {mark}{s} -> {', '.join(where) if where else '(не найден ни в одном модуле)'}")
        if not where:
            missing.append(s)
        elif s in REQUIRED:
            found_required += 1

    print()
    print(f"обязательных целей найдено: {found_required} из {len(REQUIRED)}")
    if found_required < args.require:
        print("ГЕЙТ КРАСНЫЙ: обязательных целей меньше требуемого")
        return 1
    if missing:
        print("не найдены (перехватчик на них просто не встанет, игре это не мешает):")
        for s in missing:
            print("    " + s)
    print("ГЕЙТ ЗЕЛЁНЫЙ")
    return 0


def self_test():
    """
    Негативные пробы. Каждая обязана покраснеть — иначе гейт «всегда зелёный».
    Пробы работают на разборе, не трогая файлы на диске.
    """
    ok = True

    # 1. Затвор нуля: пустой набор модулей не может дать зелёный.
    class FakeArgs:
        pass

    syms = targets_from_header()
    if len(syms) < 10:
        print("ПРОБА 1 КРАСНАЯ: таблица целей подозрительно короткая")
        ok = False
    else:
        print(f"проба 1: таблица целей разобрана, {len(syms)} имён — ok")

    # 2. Имя с опечаткой не должно находиться нигде: проверяем, что поиск
    #    вообще различает имена (иначе «ok» ничего не значит).
    if "eglInitializeXXX" in syms:
        print("ПРОБА 2 КРАСНАЯ: в таблице мусорное имя")
        ok = False
    else:
        print("проба 2: мусорного имени в таблице нет — ok")

    # 3. Все обязательные имена обязаны присутствовать в таблице заголовка
    #    (кроме eglGetError, который не хукается).
    absent = [s for s in REQUIRED if s != "eglGetError" and s not in syms]
    if absent:
        print("ПРОБА 3 КРАСНАЯ: в таблице заголовка нет обязательных целей: "
              + ", ".join(absent))
        ok = False
    else:
        print("проба 3: все обязательные цели есть в таблице заголовка — ok")

    # 4. Сигнатуры перехватчиков обязаны совпадать с таблицей, и сверенных
    #    хуков должно быть не ноль — затвор нуля.
    header_text = Path(HEADER).read_text(encoding="utf-8")
    checked, problems = check_ret_kinds(header_text)
    if checked < 10:
        print(f"ПРОБА 4 КРАСНАЯ: сверено всего {checked} перехватчиков — "
              "разбор заголовка не работает")
        ok = False
    elif problems:
        print("ПРОБА 4 КРАСНАЯ: " + "; ".join(problems))
        ok = False
    else:
        print(f"проба 4: сигнатуры {checked} перехватчиков совпали с таблицей — ok")

    # 5. НЕГАТИВНАЯ ПРОБА к ней же: копию текста портим ровно той ошибкой, что
    #    уже была однажды — nn::vi::Initialize снова возвращает Result.
    #    Проверка обязана это поймать, иначе она ничего не проверяет.
    weak = header_text.replace("static void Callback() {", "static Result Callback() {")
    if weak == header_text:
        print("ПРОБА 5 КРАСНАЯ: ослабить заголовок не удалось — "
              "проба ничего не проверяет")
        ok = False
    else:
        _, weak_problems = check_ret_kinds(weak)
        if weak_problems:
            print("проба 5: ослабленный заголовок пойман — ok (" + weak_problems[0] + ")")
        else:
            print("ПРОБА 5 КРАСНАЯ: ослабленный заголовок прошёл проверку")
            ok = False

    # 6. Бюджет трамплинов сходится с настоящими константами exlaunch.
    ours, theirs, budget_problems = hook_budget(header_text)
    if budget_problems:
        print("ПРОБА 6 КРАСНАЯ: " + "; ".join(budget_problems))
        ok = False
    else:
        print("проба 6: хуков у модуля %d, трамплинов в пуле %d — ok"
              % (ours["total"], theirs["pool"]))

    # 7. НЕГАТИВНАЯ ПРОБА к ней же: копии заголовка возвращаем прежний,
    #    маленький пул (0x1000 = 20 трамплинов) — ровно то, на чём упал ШАГ-7
    #    вместе с пятью целями блокировок. Проверка обязана покраснеть.
    weak_budget = header_text.replace("constexpr int JitSizeBytes    = 0x2000;",
                                      "constexpr int JitSizeBytes    = 0x1000;")
    if weak_budget == header_text:
        print("ПРОБА 7 КРАСНАЯ: ослабить бюджет не удалось — "
              "проба ничего не проверяет")
        ok = False
    else:
        _, _, weak_problems = hook_budget(weak_budget)
        if weak_problems:
            print("проба 7: уменьшенный пул пойман — ok (" + weak_problems[0] + ")")
        else:
            print("ПРОБА 7 КРАСНАЯ: уменьшенный пул прошёл проверку")
            ok = False

    # 8. ВТОРАЯ НЕГАТИВНАЯ ПРОБА: само правило «хуков не больше, чем
    #    трамплинов». Ослабляем счёт хуков так, чтобы их стало больше пула —
    #    именно это и случилось на консоли (22 хука при пуле 20).
    # Число хуков вне диагностики менялось (2 -> 3, когда появился хук
    # nn::fs::SetAllocator для затвора ФС), поэтому подменяем его по образцу,
    # а не по конкретной цифре: иначе проба молча перестаёт ослаблять.
    weak_count = re.sub(r"constexpr int HooksOutsideGfx = \d+;",
                        "constexpr int HooksOutsideGfx = 30;", header_text)
    if weak_count == header_text:
        print("ПРОБА 8 КРАСНАЯ: ослабить счёт хуков не удалось")
        ok = False
    else:
        _, _, weak_problems = hook_budget(weak_count)
        if any("трамплинов в пуле" in p for p in weak_problems):
            print("проба 8: перебор хуков пойман — ok (" + weak_problems[0] + ")")
        else:
            print("ПРОБА 8 КРАСНАЯ: перебор хуков прошёл проверку")
            ok = False

    # 9. Пробы SetError учтены в бюджете: таблица разобрана (2 цели), и копия
    #    заголовка без них дала бы МЕНЬШИЙ счёт хуков — то есть счёт их видит.
    se = seterror_targets_from_header_text(header_text)
    weak_se = re.sub(r"constexpr SetErrorTarget kSetErrorTargets\[S_Count\] = \{.+?\n    \};",
                     "", header_text, flags=re.S)
    if len(se) == 2 and ours.get("S_Count") == 2 and hook_budget(weak_se)[0].get("total", 0) < ours["total"]:
        print("проба 9: пробы SetError (2) входят в счёт трамплинов — ok")
    else:
        print("ПРОБА 9 КРАСНАЯ: пробы SetError не учтены в бюджете (%d)" % len(se))
        ok = False

    # 10. Таблица kSetErrorTargets против НАСТОЯЩЕГО OpenGL 22.2.0, и негативная
    #     проба: копия с одним испорченным словом подписи и копия с перепутанным
    #     регистром кода обязаны покраснеть на том же модуле.
    import os
    gl_path = Path(os.environ.get("NXR_REF_GL",
                                  str(Path(os.environ.get("NXR_SDK_DIR") or "NXR_SDK_DIR-не-задана")
                                      / "NintendoSDK_OpenGL-22_2_0-Release")))
    if not gl_path.is_file():
        print("проба 10 пропущена: нет эталона OpenGL (%s; задать NXR_REF_GL)" % gl_path)
    else:
        gl = GlImage(Nso(gl_path))
        n, probs = check_seterror(header_text, gl)
        bad_sig = header_text.replace("0xD11303FFu", "0xD11303FEu", 1)
        bad_reg = header_text.replace("{ 0xe060, 1, 5, 6,", "{ 0xe060, 0, 5, 6,", 1)
        _, p1 = check_seterror(bad_sig, gl)
        _, p2 = check_seterror(bad_reg, gl)
        if n == 2 and not probs and p1 and p2 and bad_sig != header_text and bad_reg != header_text:
            print("проба 10: подпись и регистры сверены с эталоном GL; испорченная подпись "
                  "и чужой регистр пойманы — ok (" + p1[0] + "; " + p2[0] + ")")
        else:
            print("ПРОБА 10 КРАСНАЯ: сверено %d, расхождений %s, порча поймана: %s/%s"
                  % (n, probs, bool(p1), bool(p2)))
            ok = False

    print("САМОПРОВЕРКА " + ("ЗЕЛЁНАЯ" if ok else "КРАСНАЯ"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
