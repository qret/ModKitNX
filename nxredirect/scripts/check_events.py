#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_events — гейт машинного протокола событий (--events, NDJSON).

Зачем. Внешний интерфейс (веб-оболочка) читает не русский лог, а поток событий:
одна строка — одно событие с СТАБИЛЬНЫМ кодом. Если хоть одна строка окажется не
JSON'ом или без кода, интерфейс покажет неправду молча. Поэтому поток проверяется
гейтом, а у гейта есть постоянные негативные пробы.

Запуск:
    python scripts/check_events.py <файл.ndjson>     проверить поток событий
    python scripts/check_events.py --self-test       негативные пробы самого гейта
    python scripts/check_events.py --check-docs      сверить описание событий (DOCS_EVENTS) с реестром
    python scripts/check_events.py --check-source    сверить коды в коде с реестром
    python scripts/check_events.py --registry-md     напечатать блок реестра для документа

Коды возврата (их НЕ менять — на них опираются self-test конвейера и веб):
    0  всё сошлось
    1  находки: битая строка, событие без кода, код вне реестра и т.п.
    2  нет входа или проверено НОЛЬ событий («затвор нуля»: пустой поток — красный)
    3  проба не поймана — гейт лжёт о своей работе

Ничего не пишет, кроме временных файлов своих проб (в системной temp-папке).
Ничего не удаляет из проекта.
"""

import argparse
import datetime
import importlib.util
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SELF = Path(__file__).resolve()
DOCS_EVENTS = ROOT.parent / "docs" / "события.md"

MARK_BEGIN = "<!-- РЕЕСТР-КОДОВ:начало (порождается scripts/check_events.py --registry-md) -->"
MARK_END = "<!-- РЕЕСТР-КОДОВ:конец -->"

LEVELS = ("debug", "info", "warn", "error")
REQUIRED = ("ts", "level", "code", "msg", "data")


def load_nxr_build():
    """Реестр кодов живёт в nxr_build.py — одно число в одном месте."""
    spec = importlib.util.spec_from_file_location("nxr_build_for_events",
                                                 ROOT / "nxr_build.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


NXR = load_nxr_build()
EVENT_CODES = NXR.EVENT_CODES
KNOWN_CODES = NXR.event_codes()
EVENTS_SCHEMA = NXR.EVENTS_SCHEMA


def setup_stdio():
    """
    stdout/stderr — UTF-8, даже когда это КАНАЛ, а не консоль.

    Замер: без этого `--help` падал на UnicodeEncodeError (локаль
    машины cp1250), а `--self-test` возвращал 3 («проба не получилась: 11 из 12»)
    — потому что русский текст приговора подпроцесса превращался в «?????» и
    сверка `want_text in out` не находила ожидаемую строку. Гейт лгал о себе из-за
    среды, а не из-за дефекта проверок: с PYTHONIOENCODING=utf-8 тот же прогон
    давал 0.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def say(msg):
    enc = sys.stdout.encoding or "utf-8"
    sys.stdout.write(str(msg).encode(enc, errors="replace").decode(enc) + "\n")
    sys.stdout.flush()


# ------------------------------------------------------------------ реестр -> документ


def registry_md():
    rows = [MARK_BEGIN,
            "",
            "| код | уровень | когда | поля `data` | что значит для пользователя |",
            "|---|---|---|---|---|"]
    for code, level, when, data, meaning in EVENT_CODES:
        rows.append("| `%s` | %s | %s | %s | %s |"
                    % (code, level, when, data, meaning))
    rows.append("")
    rows.append("Живых кодов: **%d**. Выведенных из употребления: **%d**."
                % (len(EVENT_CODES), len(NXR.EVENT_CODES_RETIRED)))
    rows.append("")
    rows.append(MARK_END)
    return "\n".join(rows)


def check_docs():
    """Блок реестра в документе обязан совпадать с реестром в коде посимвольно."""
    if not DOCS_EVENTS.is_file():
        say("нет входа: не найден " + str(DOCS_EVENTS))
        return 2
    text = DOCS_EVENTS.read_text(encoding="utf-8").replace("\r\n", "\n")
    if MARK_BEGIN not in text or MARK_END not in text:
        say("X в %s нет маркеров блока реестра" % DOCS_EVENTS.name)
        return 1
    block = text[text.index(MARK_BEGIN):text.index(MARK_END) + len(MARK_END)]
    want = registry_md()
    if block != want:
        say("X блок реестра в %s разошёлся с EVENT_CODES в nxr_build.py" % DOCS_EVENTS.name)
        say("  перегенерировать: python scripts/check_events.py --registry-md")
        b, w = block.splitlines(), want.splitlines()
        for n in range(max(len(b), len(w))):
            lb = b[n] if n < len(b) else "(нет строки)"
            lw = w[n] if n < len(w) else "(нет строки)"
            if lb != lw:
                say("  первая разница, строка %d:" % (n + 1))
                say("    в документе: " + lb)
                say("    в реестре:   " + lw)
                break
        return 1
    say("checks passed: 1 of 1 · critical: 0 · new warnings: 0")
    say("  ok блок реестра в документе совпадает с EVENT_CODES (строк таблицы: %d)"
        % len(EVENT_CODES))
    return 0


def check_source():
    """
    Каждый код, встречающийся в коде конвейера, обязан быть в реестре, и наоборот.

    Сторож соответствия: линтер отсутствующего не видит, а забытая строка реестра
    означает, что документ обещает больше, чем конвейер выпускает.
    """
    import re

    files = [ROOT / n for n in ("nxr_build.py", "install.py", "build.py", "build-diag.py")]
    missing_files = [str(p) for p in files if not p.is_file()]
    if missing_files:
        say("нет входа: " + ", ".join(missing_files))
        return 2
    used = set()
    for p in files:
        s = p.read_text(encoding="utf-8")
        used |= set(re.findall(r'code=\s*"([A-Z_0-9]+)"', s))
        used |= set(re.findall(r'emit\(\s*"([A-Z_0-9]+)"', s))
        used |= set(re.findall(r'code or "([A-Z_0-9]+)"', s))
        used |= set(re.findall(r'"(PROBE_[A-Z_]+)"', s))
    if not used:
        say("ничего не проверив: в исходниках не нашлось ни одного кода события")
        return 2
    unknown = sorted(used - KNOWN_CODES)
    unused = sorted(KNOWN_CODES - used)
    problems = []
    if unknown:
        problems.append("коды есть в коде, но не в реестре: " + ", ".join(unknown))
    if unused:
        problems.append("коды есть в реестре, но не выпускаются кодом: " + ", ".join(unused))
    if problems:
        say("checks passed: 0 of 1 · critical: %d · new warnings: 0" % len(problems))
        for p in problems:
            say("X " + p)
        return 1
    say("checks passed: 1 of 1 · critical: 0 · new warnings: 0")
    say("  ok коды в исходниках и в реестре совпадают (кодов %d, файлов %d)"
        % (len(used), len(files)))
    return 0


# ------------------------------------------------------------------ гейт потока


def check_stream(path):
    """
    Проверки потока событий. Возвращает код возврата.

    Нумерация проверок — постоянный адрес (на неё ссылается описание событий, DOCS_EVENTS):
      1. строка разбирается как объект JSON;
      2. есть все обязательные поля;
      3. code — непустая строка;
      4. code есть в реестре;
      5. level из известных;
      6. ts разбирается как время ISO 8601;
      7. data — объект, msg — строка;
      8. schema совпадает с EVENTS_SCHEMA;
      9. у события об отказе (level=error, код FAIL_*) в data.rc стоит 1 —
         коды возврата конвейера не менялись, и протокол обязан это подтверждать.
    """
    p = Path(path)
    if not p.is_file():
        say("нет входа: нет файла событий " + str(p))
        return 2
    raw = p.read_text(encoding="utf-8")
    lines = [l for l in raw.replace("\r\n", "\n").split("\n") if l.strip()]
    problems = []
    checked = 0
    codes_seen = {}

    for n, line in enumerate(lines, 1):
        # ---- 1. объект JSON
        try:
            ev = json.loads(line)
        except Exception as e:
            problems.append("строка %d: не разбирается как JSON (%s): %s"
                            % (n, e, line[:120]))
            continue
        if not isinstance(ev, dict):
            problems.append("строка %d: JSON есть, но это не объект (%s)"
                            % (n, type(ev).__name__))
            continue
        checked += 1
        # ---- 2. обязательные поля
        lack = [f for f in REQUIRED if f not in ev]
        if lack:
            problems.append("строка %d: нет обязательных полей: %s"
                            % (n, ", ".join(lack)))
        # ---- 3. code непустой
        code = ev.get("code")
        if not isinstance(code, str) or not code.strip():
            problems.append("строка %d: событие без кода (code=%r) — такого быть не может"
                            % (n, code))
            continue
        codes_seen[code] = codes_seen.get(code, 0) + 1
        # ---- 4. код в реестре
        if code not in KNOWN_CODES:
            problems.append("строка %d: код %s отсутствует в реестре EVENT_CODES "
                            "(заведите его или исправьте опечатку)" % (n, code))
        # ---- 5. уровень
        if ev.get("level") not in LEVELS:
            problems.append("строка %d (%s): неизвестный уровень %r, ожидался один из %s"
                            % (n, code, ev.get("level"), "/".join(LEVELS)))
        # ---- 6. время
        ts = ev.get("ts")
        if not isinstance(ts, str) or not ts.strip():
            problems.append("строка %d (%s): нет времени ts" % (n, code))
        else:
            try:
                datetime.datetime.fromisoformat(ts)
            except Exception:
                problems.append("строка %d (%s): ts не разбирается как ISO 8601: %r"
                                % (n, code, ts))
        # ---- 7. msg и data
        if not isinstance(ev.get("msg"), str):
            problems.append("строка %d (%s): msg должен быть строкой, а не %s"
                            % (n, code, type(ev.get("msg")).__name__))
        if not isinstance(ev.get("data"), dict):
            problems.append("строка %d (%s): data должен быть объектом, а не %s"
                            % (n, code, type(ev.get("data")).__name__))
        # ---- 8. схема
        if "schema" in ev and ev["schema"] != EVENTS_SCHEMA:
            problems.append("строка %d (%s): schema %r, а инструмент пишет %r"
                            % (n, code, ev["schema"], EVENTS_SCHEMA))
        # ---- 9. отказ несёт прежний код возврата
        if code.startswith("FAIL_"):
            rc = (ev.get("data") or {}).get("rc") if isinstance(ev.get("data"), dict) else None
            if rc != 1:
                problems.append("строка %d (%s): у отказа data.rc=%r, а конвейер выходит с 1 "
                                "(коды возврата не менялись)" % (n, code, rc))

    # «Затвор нуля»: зелёный отчёт о нуле проверенных событий ничего не значит.
    if checked == 0:
        say("ничего не проверив: событий в потоке %d — это красный, а не зелёный" % checked)
        say("  файл: %s (непустых строк: %d)" % (p, len(lines)))
        for problem in problems[:10]:
            say("X " + problem)
        return 2

    say("checks passed: %d of %d · critical: %d · new warnings: 0"
        % (0 if problems else 4, 4, len(problems)))
    for problem in problems:
        say("X " + problem)
    if problems:
        return 1
    say("  ok каждая строка — объект JSON (событий: %d, строк: %d)" % (checked, len(lines)))
    say("  ok у каждого события непустой код из реестра (разных кодов: %d)"
        % len(codes_seen))
    say("  ok у каждого события есть ts, level, msg и data (событий: %d)" % checked)
    fails = sum(v for k, v in codes_seen.items() if k.startswith("FAIL_"))
    # Зелёная строка со счётчиком ноль ничего не значит (метод, §31.4): при нулевом
    # числе отказов проверка 9 просто не о чём, и так и пишется — без счётчика-нуля.
    if fails:
        say("  ok отказы несут data.rc=1 (отказов в потоке: %d)" % fails)
    else:
        say("  — проверка 9 (отказ несёт прежний код возврата) не о чём: "
            "отказов в этом потоке нет, прогон зелёный")
    say("  частота кодов: " + ", ".join("%s=%d" % kv for kv in sorted(codes_seen.items())))
    return 0


# ------------------------------------------------------------------ негативные пробы


def _good_stream():
    def ev(code, level="info", **data):
        return {"schema": EVENTS_SCHEMA,
                "ts": datetime.datetime.now().astimezone().isoformat(timespec="milliseconds"),
                "level": level, "code": code, "msg": "человеческая строка", "data": data}
    return [ev("RUN_START", tool="build.py"),
            ev("STEP_START", n=1, of=7, name="build"),
            ev("LOG"),
            ev("GATE_PASS", gate="check_nso"),
            ev("FAIL_GATE_ZIP", level="error", rc=1, gate="zip", problems=["x"])]


def _write(path, events):
    path.write_text("\n".join(json.dumps(e, ensure_ascii=False) for e in events) + "\n",
                    encoding="utf-8", newline="\n")


def self_test():
    """
    Постоянные пробы гейта. Каждая обязана дать ожидаемый код возврата.

    Пробы бьют по классам: порча формата, исчезновение поля, код вне реестра,
    ноль событий (общий затвор), подмена кода возврата у отказа.
    """
    import subprocess
    import tempfile

    def run(p):
        # PYTHONIOENCODING обязателен: приговор подпроцесса читается по-русски и
        # сверяется с ожидаемой строкой. Без него подпроцесс пишет в канал в
        # кодировке локали (cp1250 на этой машине), кириллица становится «?», и
        # проба выглядит не сработавшей — дефект среды, а не проверки.
        env = dict(os.environ)
        env["PYTHONIOENCODING"] = "utf-8"
        res = subprocess.run([sys.executable, str(SELF), str(p)],
                             capture_output=True, text=True, encoding="utf-8",
                             errors="replace", env=env)
        return res.returncode, res.stdout + res.stderr

    probes = [
        # (имя пробы, ожидаемый код возврата, что обязано быть в приговоре)
        ("годный поток (положительная проба)", 0, "ok каждая строка"),
        ("строка не JSON", 1, "не разбирается как JSON"),
        ("JSON не объект", 1, "это не объект"),
        ("событие без поля code", 1, "событие без кода"),
        ("пустой code", 1, "событие без кода"),
        ("код вне реестра", 1, "отсутствует в реестре"),
        ("неизвестный уровень", 1, "неизвестный уровень"),
        ("битое время ts", 1, "не разбирается как ISO"),
        ("data не объект", 1, "data должен быть объектом"),
        ("чужая schema", 1, "schema"),
        ("у отказа подменён код возврата", 1, "коды возврата не менялись"),
        ("ноль событий — затвор нуля", 2, "ничего не проверив"),
    ]
    missed = 0
    ran = 0
    with tempfile.TemporaryDirectory(prefix="nxr-check-events-selftest-") as td:
        td = Path(td)
        for name, want_rc, want_text in probes:
            evs = _good_stream()
            p = td / ("probe-%d.ndjson" % (ran + 1))
            if name.startswith("годный"):
                _write(p, evs)
            elif name == "строка не JSON":
                _write(p, evs)
                p.write_text(p.read_text(encoding="utf-8") + '{"ts": "нет закрывающей\n',
                             encoding="utf-8", newline="\n")
            elif name == "JSON не объект":
                _write(p, evs)
                p.write_text(p.read_text(encoding="utf-8") + '[1, 2, 3]\n',
                             encoding="utf-8", newline="\n")
            elif name == "событие без поля code":
                evs[1].pop("code")
                _write(p, evs)
            elif name == "пустой code":
                evs[1]["code"] = "   "
                _write(p, evs)
            elif name == "код вне реестра":
                evs[1]["code"] = "ВЫДУМАННЫЙ_КОД"
                _write(p, evs)
            elif name == "неизвестный уровень":
                evs[1]["level"] = "критично"
                _write(p, evs)
            elif name == "битое время ts":
                evs[1]["ts"] = "вчера вечером"
                _write(p, evs)
            elif name == "data не объект":
                evs[1]["data"] = ["n", 1]
                _write(p, evs)
            elif name == "чужая schema":
                evs[1]["schema"] = EVENTS_SCHEMA + 7
                _write(p, evs)
            elif name == "у отказа подменён код возврата":
                evs[-1]["data"]["rc"] = 0
                _write(p, evs)
            elif name.startswith("ноль событий"):
                p.write_text("", encoding="utf-8", newline="\n")
            else:
                raise AssertionError("проба без сценария: " + name)
            rc, out = run(p)
            ran += 1
            good = rc == want_rc and want_text in out
            if not good:
                missed += 1
            say("  %s %s (код %d, ждали %d)"
                % ("поймана  " if good else "ПРОПУЩЕНА", name, rc, want_rc))
    if not ran:
        say("ни одна проба не запущена — проверять гейт нечем")
        return 3
    if missed:
        say("проба не получилась: %d из %d" % (missed, ran))
        return 3
    say("все %d проб пойманы" % ran)
    return 0


def main():
    setup_stdio()
    ap = argparse.ArgumentParser(
        description="Гейт машинного протокола событий (--events, NDJSON)")
    ap.add_argument("file", nargs="?", help="файл событий NDJSON")
    ap.add_argument("--self-test", action="store_true",
                    help="негативные пробы самого гейта (свой код возврата 3)")
    ap.add_argument("--check-docs", action="store_true",
                    help="сверить блок реестра в docs/события.md с EVENT_CODES")
    ap.add_argument("--check-source", action="store_true",
                    help="сверить коды, встречающиеся в исходниках, с реестром")
    ap.add_argument("--registry-md", action="store_true",
                    help="напечатать блок реестра в markdown (для вставки в документ)")
    args = ap.parse_args()

    if args.registry_md:
        say(registry_md())
        return 0
    if args.self_test:
        return self_test()
    if args.check_docs:
        return check_docs()
    if args.check_source:
        return check_source()
    if not args.file:
        say("нет входа: укажите файл событий или один из ключей "
            "(--self-test, --check-docs, --check-source, --registry-md)")
        return 2
    return check_stream(args.file)


if __name__ == "__main__":
    sys.exit(main())
