# -*- coding: utf-8 -*-
"""dkcal - общие помощники для скриптов-провайдеров календаря DKeys (договор: README.md рядом).
Только стандартная библиотека. Работа с ICS - на уровне строк, чтобы ничего лишнего в событиях не терять."""
import json
import os
import sys
import time

CRLF = "\r\n"


def io_utf8():
    """stdout/stdin/stderr - UTF-8 независимо от кодовой страницы консоли."""
    for s in (sys.stdin, sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8")
        except Exception:
            pass


def now_ics():
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())


def iso_to_ics(iso):
    """2026-11-16T10:00:00Z | 20261116T100000Z -> 20261116T100000Z (пусто/мусор -> '')."""
    d = "".join(c for c in (iso or "") if c.isdigit() or c == "T")
    return d if len(d) >= 15 and "T" in d else ""


# ---------- ICS на уровне строк ----------
def unfold(text):
    lines = []
    for raw in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if raw[:1] in (" ", "\t") and lines:
            lines[-1] += raw[1:]
        elif raw != "":
            lines.append(raw)
    return lines


def fold(line):
    """Складывает строку до 75 байт UTF-8 (продолжение с пробела)."""
    out, cur, n = [], "", 0
    for ch in line:
        b = len(ch.encode("utf-8"))
        if n + b > (75 if not out else 74):
            out.append(cur)
            cur, n = "", 0
        cur += ch
        n += b
    out.append(cur)
    return CRLF.join(out[0:1] + [" " + x for x in out[1:]])


def split_vevents(text):
    """Все VEVENT текста ICS: список событий, событие = список развёрнутых строк от BEGIN:VEVENT до END:VEVENT."""
    events, cur, depth = [], None, 0
    for ln in unfold(text):
        up = ln.upper()
        if cur is None:
            if up == "BEGIN:VEVENT":
                cur, depth = [ln], 0
            continue
        cur.append(ln)
        if up.startswith("BEGIN:"):
            depth += 1
        elif up.startswith("END:"):
            if depth == 0 and up == "END:VEVENT":
                events.append(cur)
                cur = None
            else:
                depth -= 1
    return events


def pname(line):
    """Имя свойства строки (в верхнем регистре, без параметров)."""
    i = 0
    while i < len(line) and line[i] not in ";:":
        i += 1
    return line[:i].upper()


def pvalue(line):
    q = False
    for i, ch in enumerate(line):
        if ch == '"':
            q = not q
        elif ch == ":" and not q:
            return line[i + 1:]
    return ""


def get(ev, name, depth0=True):
    """Значение свойства верхнего уровня события (без вложенных VALARM)."""
    depth = 0
    for ln in ev[1:-1]:
        up = ln.upper()
        if up.startswith("BEGIN:"):
            depth += 1
        elif up.startswith("END:"):
            depth -= 1
        elif depth == 0 and pname(ln) == name.upper():
            return pvalue(ln)
    return None


def drop(ev, *names):
    names = {n.upper() for n in names}
    out, depth = [ev[0]], 0
    for ln in ev[1:-1]:
        up = ln.upper()
        if up.startswith("BEGIN:"):
            depth += 1
        elif up.startswith("END:"):
            depth -= 1
        elif depth == 0 and pname(ln) in names:
            continue
        out.append(ln)
    out.append(ev[-1])
    return out


def put(ev, name, value):
    """Заменяет (или добавляет) свойство верхнего уровня."""
    ev = drop(ev, name)
    return ev[:-1] + ["%s:%s" % (name, value), ev[-1]]


def uid_of(ev):
    return get(ev, "UID") or ""


def rec_of(ev):
    return get(ev, "RECURRENCE-ID")


def vcalendar(events, extra=None):
    """Текст VCALENDAR (CRLF) из списка событий и дополнительных свойств календаря."""
    out = ["BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//DKeys//Provider//EN"] + list(extra or [])
    for ev in events:
        out.extend(ev)
    out.append("END:VCALENDAR")
    return CRLF.join(fold(x) for x in out) + CRLF


def group_by_uid(events):
    """События с одинаковым UID (серия: главное + замены) вместе, порядок первого появления сохраняется."""
    order, groups = [], {}
    for ev in events:
        u = uid_of(ev)
        if u not in groups:
            groups[u] = []
            order.append(u)
        groups[u].append(ev)
    return [(u, groups[u]) for u in order]


def deleted_marker(uid, etag="", recid=None):
    ev = ["BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:" + now_ics(), "X-DKEYS-DELETED:1"]
    if etag:
        ev.append("X-DKEYS-ETAG:" + etag)
    if recid:
        ev.append("RECURRENCE-ID:" + recid)
    ev.append("END:VEVENT")
    return ev


# ---------- аргументы и вывод ----------
def parse_args(argv):
    """argv без имени скрипта: глагол и параметры --since, --profile, --dir (значения могут быть пустыми)."""
    verb, opts, rest, i = "", {"since": "", "profile": "", "dir": ""}, [], 0
    while i < len(argv):
        a = argv[i]
        if a.startswith("--") and a[2:] in opts:
            opts[a[2:]] = argv[i + 1] if i + 1 < len(argv) else ""
            i += 2
            continue
        if not verb:
            verb = a
        else:
            rest.append(a)
        i += 1
    return verb, opts, rest


def out(text):
    sys.stdout.write(text)
    sys.stdout.flush()


def fail(msg, code=1):
    sys.stderr.write(str(msg).rstrip() + "\n")
    sys.stderr.flush()
    sys.exit(code)


def info_json(name, **caps):
    d = {"name": name, "contract": 1, "create": True, "update": True, "delete": True, "calendars": False}
    d.update(caps)
    return json.dumps(d, ensure_ascii=False)


def state_dir(script_file, profile=""):
    """calendars/<имя>.state/<профиль> - токены и кэш источника (не в git)."""
    base = os.path.splitext(os.path.abspath(script_file))[0] + ".state"
    p = os.path.join(base, profile or "default")
    os.makedirs(p, exist_ok=True)
    return p


def write_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)
    os.replace(tmp, path)


def read_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default
