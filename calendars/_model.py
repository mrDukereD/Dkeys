# -*- coding: utf-8 -*-
"""_model - общее для google.py и outlook.py (не источник): время события с часовым поясом, чтение и запись VEVENT, напоминания, RRULE,
сравнение с серверной версией (в сервер уходит только изменившееся). Только стандартная библиотека; zoneinfo на Windows требует пакета tzdata."""
import datetime as dt
import hashlib
import os
import re
import sys
from collections import namedtuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dkcal as D  # noqa: E402

UTC = dt.timezone.utc


class TzError(Exception):
    pass


# ---------- часовые пояса ----------
_ZONES = [
    ("UTC", "UTC"), ("Russian Standard Time", "Europe/Moscow", "Europe/Kirov", "Europe/Simferopol"), ("Kaliningrad Standard Time", "Europe/Kaliningrad"),
    ("Russia Time Zone 3", "Europe/Samara"), ("Volgograd Standard Time", "Europe/Volgograd"), ("Saratov Standard Time", "Europe/Saratov"),
    ("Astrakhan Standard Time", "Europe/Astrakhan", "Europe/Ulyanovsk"), ("Belarus Standard Time", "Europe/Minsk"),
    ("FLE Standard Time", "Europe/Kyiv", "Europe/Kiev", "Europe/Helsinki", "Europe/Riga", "Europe/Tallinn", "Europe/Vilnius", "Europe/Sofia"),
    ("E. Europe Standard Time", "Europe/Chisinau"), ("Ekaterinburg Standard Time", "Asia/Yekaterinburg"), ("Omsk Standard Time", "Asia/Omsk"),
    ("N. Central Asia Standard Time", "Asia/Novosibirsk"), ("Altai Standard Time", "Asia/Barnaul"), ("Tomsk Standard Time", "Asia/Tomsk"),
    ("North Asia Standard Time", "Asia/Krasnoyarsk", "Asia/Novokuznetsk"), ("North Asia East Standard Time", "Asia/Irkutsk"),
    ("Yakutsk Standard Time", "Asia/Yakutsk"), ("Transbaikal Standard Time", "Asia/Chita"), ("Vladivostok Standard Time", "Asia/Vladivostok"),
    ("Magadan Standard Time", "Asia/Magadan"), ("Sakhalin Standard Time", "Asia/Sakhalin"), ("Russia Time Zone 10", "Asia/Srednekolymsk"),
    ("Russia Time Zone 11", "Asia/Kamchatka", "Asia/Anadyr"), ("Central Asia Standard Time", "Asia/Almaty", "Asia/Bishkek"),
    ("Qyzylorda Standard Time", "Asia/Qyzylorda"), ("West Asia Standard Time", "Asia/Tashkent", "Asia/Aqtau", "Asia/Aqtobe", "Asia/Oral"),
    ("Georgian Standard Time", "Asia/Tbilisi"), ("Caucasus Standard Time", "Asia/Yerevan"), ("Azerbaijan Standard Time", "Asia/Baku"),
    ("Arabian Standard Time", "Asia/Dubai"), ("Iran Standard Time", "Asia/Tehran"), ("Afghanistan Standard Time", "Asia/Kabul"),
    ("Pakistan Standard Time", "Asia/Karachi"), ("India Standard Time", "Asia/Kolkata", "Asia/Calcutta"), ("Nepal Standard Time", "Asia/Kathmandu"),
    ("Bangladesh Standard Time", "Asia/Dhaka"), ("SE Asia Standard Time", "Asia/Bangkok", "Asia/Ho_Chi_Minh", "Asia/Jakarta"),
    ("China Standard Time", "Asia/Shanghai", "Asia/Hong_Kong"), ("Singapore Standard Time", "Asia/Singapore", "Asia/Kuala_Lumpur", "Asia/Manila"),
    ("Taipei Standard Time", "Asia/Taipei"), ("Korea Standard Time", "Asia/Seoul"), ("Tokyo Standard Time", "Asia/Tokyo"),
    ("Israel Standard Time", "Asia/Jerusalem"), ("Middle East Standard Time", "Asia/Beirut"), ("Arab Standard Time", "Asia/Riyadh"),
    ("Arabic Standard Time", "Asia/Baghdad"), ("Turkey Standard Time", "Europe/Istanbul", "Asia/Istanbul"),
    ("GMT Standard Time", "Europe/London", "Europe/Dublin", "Europe/Lisbon"), ("Greenwich Standard Time", "Atlantic/Reykjavik"),
    ("W. Europe Standard Time", "Europe/Berlin", "Europe/Amsterdam", "Europe/Rome", "Europe/Vienna", "Europe/Zurich", "Europe/Stockholm", "Europe/Oslo"),
    ("Romance Standard Time", "Europe/Paris", "Europe/Madrid", "Europe/Brussels", "Europe/Copenhagen"), ("Central European Standard Time", "Europe/Warsaw"),
    ("Central Europe Standard Time", "Europe/Budapest", "Europe/Belgrade", "Europe/Prague"), ("GTB Standard Time", "Europe/Bucharest", "Europe/Athens"),
    ("Egypt Standard Time", "Africa/Cairo"), ("South Africa Standard Time", "Africa/Johannesburg"), ("W. Central Africa Standard Time", "Africa/Lagos"),
    ("E. Africa Standard Time", "Africa/Nairobi"), ("Morocco Standard Time", "Africa/Casablanca"), ("Eastern Standard Time", "America/New_York", "America/Toronto"),
    ("Central Standard Time", "America/Chicago"), ("Mountain Standard Time", "America/Denver"), ("US Mountain Standard Time", "America/Phoenix"),
    ("Pacific Standard Time", "America/Los_Angeles", "America/Vancouver"), ("Alaskan Standard Time", "America/Anchorage"), ("Hawaiian Standard Time", "Pacific/Honolulu"),
    ("Atlantic Standard Time", "America/Halifax"), ("Central Standard Time (Mexico)", "America/Mexico_City"), ("E. South America Standard Time", "America/Sao_Paulo"),
    ("Argentina Standard Time", "America/Argentina/Buenos_Aires"), ("SA Pacific Standard Time", "America/Bogota", "America/Lima"),
    ("Pacific SA Standard Time", "America/Santiago"), ("AUS Eastern Standard Time", "Australia/Sydney", "Australia/Melbourne"),
    ("E. Australia Standard Time", "Australia/Brisbane"), ("W. Australia Standard Time", "Australia/Perth"), ("Cen. Australia Standard Time", "Australia/Adelaide"),
    ("New Zealand Standard Time", "Pacific/Auckland"),
]
WIN2IANA = {z[0]: z[1] for z in _ZONES}
IANA2WIN = {n: z[0] for z in _ZONES for n in z[1:]}
_UTCN = ("UTC", "Z", "GMT", "ETC/UTC", "ETC/GMT", "UCT", "UNIVERSAL", "COORDINATED UNIVERSAL TIME")


def zone(name):
    if not name or name.upper() in _UTCN:
        return UTC
    try:
        from zoneinfo import ZoneInfo
        return ZoneInfo(WIN2IANA.get(name, name))
    except Exception:
        raise TzError("нет данных о часовом поясе «%s»: выполните python -m pip install tzdata (или пояс не опознан)" % name)


def iana(name):
    """TZID или имя Windows -> имя IANA ('UTC' для синонимов UTC). Неизвестное - TzError."""
    if not name or name.upper() in _UTCN:
        return "UTC"
    name = WIN2IANA.get(name, name)
    zone(name)
    return name


def win_name(tz):
    """IANA -> имя Windows (для Microsoft Graph) или None."""
    return IANA2WIN.get(tz) if tz != "UTC" else "UTC"


# ---------- время события ----------
T = namedtuple("T", "kind v tz")                  # kind 'D' - дата (v: date), 'T' - время (v: naive datetime в поясе tz, 'UTC' - в UTC)


def tdate(d):
    return T("D", d, None)


def tutc(a):
    return T("T", a.astimezone(UTC).replace(tzinfo=None), "UTC")


def tin(a, tz):
    tz = iana(tz)
    return T("T", a.astimezone(zone(tz)).replace(tzinfo=None), tz)


def inst(t):
    return t.v.replace(tzinfo=zone(t.tz)).astimezone(UTC)


def to_tz(t, tz):
    return t if t.kind == "D" else tin(inst(t), tz)


def key(t):
    return "D" + t.v.isoformat() if t.kind == "D" else "T" + inst(t).strftime("%Y%m%dT%H%M%S")


def same(a, b, with_tz=False):
    if a is None or b is None:
        return a is b
    return a.kind == b.kind and key(a) == key(b) and (not with_tz or a.kind == "D" or a.tz == b.tz)


def fmt_t(t):
    return t.v.strftime("%Y%m%d") if t.kind == "D" else t.v.strftime("%Y%m%dT%H%M%S") + ("Z" if t.tz == "UTC" else "")


def ics_t(name, t):
    if t.kind == "D":
        return "%s;VALUE=DATE:%s" % (name, fmt_t(t))
    return "%s:%s" % (name, fmt_t(t)) if t.tz == "UTC" else "%s;TZID=%s:%s" % (name, t.tz, fmt_t(t))


def parse_rfc3339(s):
    d = dt.datetime.fromisoformat(re.sub(r"(\.\d{6})\d+", r"\1", s.replace("Z", "+00:00")))
    return d


def _params(line):
    raw = D.pvalue(line)
    head = line[:len(line) - len(raw) - 1] if len(line) > len(raw) else line
    out = {}
    for p in head.split(";")[1:]:
        if "=" in p:
            k, v = p.split("=", 1)
            out[k.upper()] = v.strip('"')
    return out


def parse_ts(line):
    """DTSTART/DTEND/RECURRENCE-ID/EXDATE (значение может быть списком) -> [T]. Без пояса - местное время компьютера."""
    pr, out = _params(line), []
    for val in D.pvalue(line).split(","):
        val = val.strip()
        if pr.get("VALUE", "").upper() == "DATE" or re.fullmatch(r"\d{8}", val):
            out.append(tdate(dt.datetime.strptime(val[:8], "%Y%m%d").date()))
            continue
        m = re.fullmatch(r"(\d{8}T\d{6})(Z?)", val)
        if not m:
            continue
        naive = dt.datetime.strptime(m.group(1), "%Y%m%dT%H%M%S")
        if m.group(2):
            out.append(T("T", naive, "UTC"))
        elif pr.get("TZID"):
            out.append(T("T", naive, iana(pr["TZID"])))
        else:
            out.append(tutc(naive.astimezone()))             # плавающее время = местное время этого компьютера
    return out


# ---------- текст ICS ----------
def unesc(s):
    return re.sub(r"\\(.)", lambda m: "\n" if m.group(1) in "nN" else m.group(1), s or "")


def esc(s):
    return (s or "").replace("\\", "\\\\").replace(";", "\\;").replace(",", "\\,").replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\\n")


def norm_text(s):
    return (s or "").replace("\r\n", "\n").replace("\r", "\n").strip()


# ---------- напоминания ----------
_DUR = re.compile(r"^([+-]?)P(?:(\d+)W)?(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?)?$")


def trig_minutes(v):
    """TRIGGER (длительность) -> минут ДО начала (отрицательно - после) или None."""
    m = _DUR.match((v or "").strip().upper())
    if not m:
        return None
    w, d, h, mi, s = (int(x or 0) for x in m.groups()[1:])
    tot = (((w * 7 + d) * 24 + h) * 60 + mi) + s // 60
    return tot if m.group(1) == "-" else (-tot if tot else 0)


def alarm_lines(alarms, summary):
    out = []
    for method, mins in alarms:
        t = "PT0S" if mins == 0 else ("-P%dD" % (mins // 1440) if mins > 0 and mins % 1440 == 0 else "-PT%dH" % (mins // 60) if mins > 0 and mins % 60 == 0
                                       else "-PT%dM" % mins if mins > 0 else "PT%dM" % -mins)
        out += ["BEGIN:VALARM", "ACTION:" + ("EMAIL" if method == "email" else "DISPLAY"), "DESCRIPTION:" + esc(summary or "DKeys"), "TRIGGER:" + t, "END:VALARM"]
        if method == "email":
            out.insert(len(out) - 1, "SUMMARY:" + esc(summary or "DKeys"))
    return out


def parse_alarms(ev):
    out, cur = [], None
    for ln in ev[1:-1]:
        up_ = ln.upper()
        if up_ == "BEGIN:VALARM":
            cur = {}
        elif up_ == "END:VALARM":
            if cur and cur.get("m") is not None:
                out.append((cur.get("a", "popup"), cur["m"]))
            cur = None
        elif cur is not None:
            n = D.pname(ln)
            if n == "ACTION":
                cur["a"] = "email" if D.pvalue(ln).strip().upper() == "EMAIL" else "popup"
            elif n == "TRIGGER" and _params(ln).get("VALUE", "DURATION").upper() == "DURATION" and _params(ln).get("RELATED", "START").upper() == "START":
                cur["m"] = trig_minutes(D.pvalue(ln))
    return sorted(set(out), key=lambda x: (x[1], x[0]))


# ---------- RRULE ----------
RR_ORDER = ["FREQ", "INTERVAL", "COUNT", "UNTIL", "BYMONTH", "BYMONTHDAY", "BYDAY", "BYSETPOS", "WKST"]


def rr_parse(s):
    d = {}
    for part in (s or "").split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            d[k.strip().upper()] = v.strip().upper()
    return d


def rr_fmt(d):
    ks = [k for k in RR_ORDER if k in d] + sorted(k for k in d if k not in RR_ORDER)
    return ";".join("%s=%s" % (k, d[k]) for k in ks)


def rr_norm(s):
    """Каноническая форма для сравнения: без INTERVAL=1 и WKST, UNTIL - только дата, дни по порядку."""
    d = rr_parse(s)
    if d.get("INTERVAL") == "1":
        del d["INTERVAL"]
    d.pop("WKST", None)
    if "UNTIL" in d:
        d["UNTIL"] = d["UNTIL"][:8]
    if "BYDAY" in d:
        d["BYDAY"] = ",".join(sorted(d["BYDAY"].split(",")))
    return rr_fmt(d) if d else ""


# ---------- модель события ----------
def new_model():
    return {"summary": "", "description": "", "location": "", "start": None, "end": None, "rrule": "", "exdates": [], "rlines": [], "recid": None,
            "alarms": [], "transp": "", "cls": "", "status": ""}


def _lines0(ev):
    depth = 0
    for ln in ev[1:-1]:
        u = ln.upper()
        if u.startswith("BEGIN:"):
            depth += 1
        elif u.startswith("END:"):
            depth -= 1
        elif depth == 0:
            yield ln


def read_vevent(ev):
    """VEVENT (список строк) -> модель управляемых полей."""
    m = new_model()
    dur = None
    for ln in _lines0(ev):
        n, v = D.pname(ln), D.pvalue(ln)
        if n == "SUMMARY":
            m["summary"] = unesc(v)
        elif n == "DESCRIPTION":
            m["description"] = unesc(v)
        elif n == "LOCATION":
            m["location"] = unesc(v)
        elif n == "DTSTART":
            ts = parse_ts(ln)
            m["start"] = ts[0] if ts else None
        elif n == "DTEND":
            ts = parse_ts(ln)
            m["end"] = ts[0] if ts else None
        elif n == "DURATION":
            dur = trig_minutes("-" + v.strip().lstrip("+-")) if v.strip() else None
        elif n == "RRULE":
            m["rrule"] = v.strip()
        elif n == "EXDATE":
            m["exdates"] += parse_ts(ln)
        elif n in ("RDATE", "EXRULE"):
            m["rlines"].append(ln)
        elif n == "RECURRENCE-ID":
            ts = parse_ts(ln)
            m["recid"] = ts[0] if ts else None
        elif n == "TRANSP":
            m["transp"] = v.strip().upper()
        elif n == "CLASS":
            m["cls"] = v.strip().upper()
        elif n == "STATUS":
            m["status"] = v.strip().upper()
    m["alarms"] = parse_alarms(ev)
    s = m["start"]
    if s is not None and m["end"] is None:
        if dur:
            m["end"] = tdate(s.v + dt.timedelta(minutes=dur)) if s.kind == "D" else tin(inst(s) + dt.timedelta(minutes=dur), s.tz)
        else:
            m["end"] = tdate(s.v + dt.timedelta(days=1)) if s.kind == "D" else s
    return m


def vevent(m, uid, etag="", updated="", extra=()):
    """Модель -> строки VEVENT. DTSTART и DTEND идут раньше RECURRENCE-ID (так пояс не теряется при разборе)."""
    L = ["BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:" + D.now_ics()]
    if updated:
        L.append("LAST-MODIFIED:" + updated)
    s, e = m["start"], m["end"]
    L.append(ics_t("DTSTART", s))
    if e is not None and (s.kind == "D" or key(e) != key(s)):
        L.append(ics_t("DTEND", e))
    if m["recid"] is not None:
        L.append(ics_t("RECURRENCE-ID", m["recid"]))
    for name, f in (("SUMMARY", "summary"), ("DESCRIPTION", "description"), ("LOCATION", "location")):
        if m[f]:
            L.append("%s:%s" % (name, esc(m[f])))
    if m["rrule"]:
        L.append("RRULE:" + m["rrule"])
    for x in sorted(m["exdates"], key=key):
        L.append(ics_t("EXDATE", x))
    L += m["rlines"]
    for name, f in (("STATUS", "status"), ("TRANSP", "transp"), ("CLASS", "cls")):
        if m[f]:
            L.append("%s:%s" % (name, m[f]))
    L += alarm_lines(m["alarms"], m["summary"])
    L += list(extra)
    if etag:
        L.append("X-DKEYS-ETAG:" + etag)
    L.append("END:VEVENT")
    return L


def diff(inc, srv):
    """Какие управляемые поля входящей версии inc отличаются от серверной srv (для отправки только изменённого)."""
    ch = set()
    for f in ("summary", "description", "location"):
        if norm_text(inc[f]) != norm_text(srv[f]):
            ch.add(f)
    rec = bool(inc["rrule"] or srv["rrule"])
    if not (same(inc["start"], srv["start"], rec) and same(inc["end"], srv["end"], False)):
        ch.add("time")
    if rr_norm(inc["rrule"]) != rr_norm(srv["rrule"]):
        ch.add("rrule")
    if sorted(key(x) for x in inc["exdates"]) != sorted(key(x) for x in srv["exdates"]):
        ch.add("exdates")
    if sorted(inc["rlines"]) != sorted(srv["rlines"]):
        ch.add("rlines")
    if inc["alarms"] != srv["alarms"]:
        ch.add("alarms")
    if (inc["transp"] or "OPAQUE") != (srv["transp"] or "OPAQUE"):
        ch.add("transp")
    if (inc["cls"] if inc["cls"] != "PUBLIC" else "") != (srv["cls"] if srv["cls"] != "PUBLIC" else ""):
        ch.add("cls")
    return ch


def split_group(events):
    """События группы одного UID -> (главное без RECURRENCE-ID | None, [замены])."""
    master, exc = None, []
    for ev in events:
        if D.rec_of(ev) is None and master is None:
            master = ev
        elif D.rec_of(ev) is not None:
            exc.append(ev)
    return master, exc


def series_etag(master_etag, parts):
    """Один etag на серию: etag главного события плюс отпечаток замен (замена изменилась - etag серии другой)."""
    e = (master_etag or "").strip('"')
    if parts:
        e += "." + hashlib.sha1("|".join(sorted(parts)).encode("utf-8")).hexdigest()[:8]
    return e


def stable_id(uid):
    """Детерминированный идентификатор из UID: [0-9a-f] (допустим для Google: base32hex), создание по нему идемпотентно."""
    return hashlib.sha1(uid.encode("utf-8")).hexdigest()


def strip_ops(ev):
    return D.drop(ev, "X-DKEYS-OP", "X-DKEYS-ERROR")


def with_etag(group, etag, uid=None, old_uid=None):
    out = []
    for ev in group:
        e = D.put(strip_ops(ev), "X-DKEYS-ETAG", etag)
        if uid and uid != old_uid:
            e = D.put(D.put(e, "UID", uid), "X-DKEYS-REF", old_uid)
        out.append(e)
    return out


def with_error(group, msg):
    return [D.put(D.drop(e, "X-DKEYS-OP"), "X-DKEYS-ERROR", msg) for e in group]


def as_deleted(group):
    return [D.put(strip_ops(e), "X-DKEYS-DELETED", "1") for e in group]
