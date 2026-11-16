# -*- coding: utf-8 -*-
"""outlook - источник календаря DKeys: Outlook / Microsoft 365 / Outlook.com через Microsoft Graph (REST, только стандартная библиотека).
Договор команд - README.md рядом. Вход: OAuth 2.0 публичный клиент (приложение для ПК, без секрета) - браузер на localhost + PKCE или код устройства;
client_id приложения пользователь регистрирует в Azure сам (инструкция в README и в `setup`). Профиль = секция outlook.ini: account, calendar, name, readonly.
Токен - outlook.state\\<account>\\token.json, состояние - outlook.state\\<профиль>\\state.json (не в git, доступ только владельцу).
Графу delta нужен только для calendarView (развёрнутые вхождения, без серий), поэтому изменения ищутся по списку id + changeKey (лёгкий запрос) и
выбираются только изменённые события; серия = главное событие + замены (exceptionOccurrences) + отменённые вхождения (cancelledOccurrences).
Повтор Graph (pattern/range) <-> RRULE для обычных случаев; что не записывается без потерь - серия только для чтения (X-DKEYS-ERROR при правке).
Outlook хранит одно напоминание на событие: из нескольких берётся самое раннее. Проверка без сети и аккаунтов: `python outlook.py --selftest`."""
import datetime as dt
import html
import os
import re
import sys
from urllib.parse import quote

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dkcal as D  # noqa: E402
import _oauth as O  # noqa: E402
import _model as M  # noqa: E402

GRAPH = "https://graph.microsoft.com/v1.0"
LOGIN = "https://login.microsoftonline.com/%s/oauth2/v2.0/"
SCOPE = "offline_access Calendars.ReadWrite User.Read"
INI = os.path.join(HERE, "outlook.ini")
SBASE = os.path.join(HERE, "outlook.state")
SELECT = ("id,iCalUId,changeKey,type,subject,body,start,end,isAllDay,isCancelled,location,recurrence,reminderMinutesBeforeStart,isReminderOn,showAs,sensitivity,"
          "categories,attendees,organizer,seriesMasterId,originalStart,originalStartTimeZone,lastModifiedDateTime,onlineMeeting")
PREF = {"Prefer": 'outlook.timezone="UTC", outlook.body-content-type="text"'}
HOWTO = """Приложение Microsoft нужно зарегистрировать один раз (3-5 минут, бесплатно, подойдёт и личная учётная запись Outlook.com):
  1. https://entra.microsoft.com (или portal.azure.com) -> «Identity» / «Microsoft Entra ID» -> «App registrations» -> New registration.
  2. Имя любое. «Supported account types»: Accounts in any organizational directory and personal Microsoft accounts.
     «Redirect URI»: платформа «Public client/native (mobile & desktop)», адрес http://localhost
  3. Откройте «Authentication»: «Allow public client flows» = Yes (нужно для входа по коду устройства; браузерному входу не мешает).
  4. «API permissions» -> Add -> Microsoft Graph -> Delegated: Calendars.ReadWrite, User.Read, offline_access.
  5. На странице «Overview» скопируйте «Application (client) ID» и вставьте ниже (секрет не нужен).
Рабочие аккаунты организации могут требовать согласия администратора на доступ приложения к календарю."""

DOW = {"monday": "MO", "tuesday": "TU", "wednesday": "WE", "thursday": "TH", "friday": "FR", "saturday": "SA", "sunday": "SU"}
DOW_R = {v: k for k, v in DOW.items()}
IDX = {"first": 1, "second": 2, "third": 3, "fourth": 4, "last": -1}
IDX_R = {v: k for k, v in IDX.items()}
WD = ["MO", "TU", "WE", "TH", "FR", "SA", "SU"]


def sdir(name):
    p = os.path.join(SBASE, name)
    os.makedirs(p, exist_ok=True)
    return p


# ---------- ключи приложения и профили ----------
def load_client():
    cid = os.environ.get("DKEYS_OUTLOOK_CLIENT_ID")
    if cid:
        return {"client_id": cid, "tenant": os.environ.get("DKEYS_OUTLOOK_TENANT", "common")}
    d = D.read_json(os.path.join(SBASE, "client.json"), None)
    return {"client_id": d["client_id"], "tenant": d.get("tenant") or "common"} if isinstance(d, dict) and d.get("client_id") else None


def oauth_cfg(cl):
    base = LOGIN % cl.get("tenant", "common")
    return {"auth_url": base + "authorize", "token_url": base + "token", "device_url": base + "devicecode", "scope": SCOPE, "scope_on_refresh": True,
            "redirect_host": "localhost", "client_id": cl["client_id"], "auth_params": {"prompt": "select_account"}}


def open_profile(profile):
    cp = O.read_ini(INI)
    name = O.pick_profile(cp, profile)
    if not name:
        D.fail("профиль %s не настроен в outlook.ini: нажмите «Подключить…» в календаре" % (profile or ""), 2)
    sec = cp[name]
    cl = load_client()
    if not cl:
        D.fail("нет client_id приложения Microsoft: нажмите «Подключить…» в календаре (setup объяснит, где его взять)", 2)
    tokfile = os.path.join(sdir(sec.get("account") or name), "token.json")
    if not os.path.exists(tokfile):
        D.fail(O.NEED_LOGIN, 2)
    sp = os.path.join(sdir(name), "state.json")
    return (O.Client(oauth_cfg(cl), tokfile), (sec.get("calendar") or "").strip(), sp, D.read_json(sp, {}),
            (sec.get("readonly") or "").strip().lower() in ("1", "true", "yes"))


def calurl(cal, tail=""):
    return "%s/me/calendars/%s%s" % (GRAPH, quote(cal, safe=""), tail) if cal else "%s/me/calendar%s" % (GRAPH, tail)


def evurl(eid, tail=""):
    return "%s/me/events/%s%s" % (GRAPH, quote(eid, safe=""), tail)


# ---------- повтор Graph <-> RRULE ----------
def _date(s):
    return dt.date.fromisoformat(s[:10])


def rec_to_rrule(rec, start):
    """recurrence Graph -> строка RRULE или None (не воспроизводится)."""
    try:
        pat, rng = rec["pattern"], rec["range"]
        t, iv = pat.get("type"), int(pat.get("interval") or 1)
        days = [DOW[x.lower()] for x in pat.get("daysOfWeek") or []]
        d = {}
        if t == "daily":
            d["FREQ"] = "DAILY"
        elif t == "weekly":
            d["FREQ"] = "WEEKLY"
            d["BYDAY"] = ",".join(days or [WD[start.v.weekday()]])
            if iv > 1 and pat.get("firstDayOfWeek"):
                d["WKST"] = DOW[pat["firstDayOfWeek"].lower()]
        elif t == "absoluteMonthly":
            d["FREQ"], d["BYMONTHDAY"] = "MONTHLY", str(int(pat["dayOfMonth"]))
        elif t in ("relativeMonthly", "relativeYearly"):
            d["FREQ"] = "MONTHLY" if t == "relativeMonthly" else "YEARLY"
            if t == "relativeYearly":
                d["BYMONTH"] = str(int(pat["month"]))
            idx = IDX[pat.get("index", "first").lower()]
            if len(days) == 1:
                d["BYDAY"] = "%d%s" % (idx, days[0])
            else:
                d["BYDAY"], d["BYSETPOS"] = ",".join(days), str(idx)
        elif t == "absoluteYearly":
            d["FREQ"], d["BYMONTH"], d["BYMONTHDAY"] = "YEARLY", str(int(pat["month"])), str(int(pat["dayOfMonth"]))
        else:
            return None
        if iv > 1:
            d["INTERVAL"] = str(iv)
        if rng.get("type") == "numbered":
            d["COUNT"] = str(int(rng["numberOfOccurrences"]))
        elif rng.get("type") == "endDate":
            d["UNTIL"] = _date(rng["endDate"]).strftime("%Y%m%d")
        return M.rr_fmt(d)
    except (KeyError, ValueError, TypeError):
        return None


_RR_KEYS = {"FREQ", "INTERVAL", "COUNT", "UNTIL", "BYDAY", "BYMONTHDAY", "BYMONTH", "BYSETPOS", "WKST"}


def rrule_to_rec(rr, start, wtz):
    """RRULE -> recurrence Graph или None, если повтор Graph выразить без потерь нельзя."""
    d = M.rr_parse(rr)
    if not d or set(d) - _RR_KEYS or ("COUNT" in d and "UNTIL" in d):
        return None
    try:
        iv = int(d.get("INTERVAL", 1))
        s = start.v if start.kind == "D" else start.v.date()
        byday = [x for x in d.get("BYDAY", "").split(",") if x]
        plain = all(re.fullmatch(r"[A-Z]{2}", x) and x in WD for x in byday)
        pat = {"interval": iv}
        f = d["FREQ"]
        if f == "DAILY" and not byday and "BYMONTHDAY" not in d:
            pat["type"] = "daily"
        elif f == "WEEKLY" and plain and not {"BYMONTHDAY", "BYSETPOS", "BYMONTH"} & set(d):
            pat.update(type="weekly", daysOfWeek=[DOW_R[x] for x in (byday or [WD[s.weekday()]])], firstDayOfWeek=DOW_R[d.get("WKST", "MO")])
        elif f in ("MONTHLY", "YEARLY") and byday and "BYMONTHDAY" not in d:
            if plain:
                if "," in d.get("BYSETPOS", ",") or "BYSETPOS" not in d:
                    return None
                days, idx = byday, int(d["BYSETPOS"])
            else:
                if len(byday) != 1 or "BYSETPOS" in d:
                    return None
                m = re.fullmatch(r"([+-]?\d)([A-Z]{2})", byday[0])
                if not m or m.group(2) not in WD:
                    return None
                days, idx = [m.group(2)], int(m.group(1))
            if idx not in IDX_R:
                return None
            pat.update(type="relativeMonthly" if f == "MONTHLY" else "relativeYearly", daysOfWeek=[DOW_R[x] for x in days], index=IDX_R[idx], firstDayOfWeek="sunday")
            if f == "YEARLY":
                pat["month"] = int(d.get("BYMONTH", s.month))
        elif f == "MONTHLY" and not byday and "BYSETPOS" not in d and "BYMONTH" not in d:
            if "," in d.get("BYMONTHDAY", ""):
                return None
            pat.update(type="absoluteMonthly", dayOfMonth=int(d.get("BYMONTHDAY", s.day)))
            if pat["dayOfMonth"] < 1:
                return None
        elif f == "YEARLY" and not byday and "BYSETPOS" not in d and "," not in d.get("BYMONTH", "") and "," not in d.get("BYMONTHDAY", ""):
            pat.update(type="absoluteYearly", month=int(d.get("BYMONTH", s.month)), dayOfMonth=int(d.get("BYMONTHDAY", s.day)))
        else:
            return None
        rng = {"startDate": s.isoformat(), "recurrenceTimeZone": wtz}
        if "COUNT" in d:
            rng.update(type="numbered", numberOfOccurrences=int(d["COUNT"]), endDate="0001-01-01")
        elif "UNTIL" in d:
            u = d["UNTIL"]
            day = dt.datetime.strptime(u[:8], "%Y%m%d").date()
            if u.endswith("Z") and len(u) >= 15 and start.kind == "T":
                day = dt.datetime.strptime(u[:15], "%Y%m%dT%H%M%S").replace(tzinfo=M.UTC).astimezone(M.zone(start.tz)).date()
            rng.update(type="endDate", endDate=day.isoformat())
        else:
            rng.update(type="noEnd", endDate="0001-01-01")
        return {"pattern": pat, "range": rng}
    except (KeyError, ValueError, TypeError):
        return None


# ---------- события Graph <-> модель ----------
def o_dt(s):
    return dt.datetime.fromisoformat(re.sub(r"(\.\d{6})\d+", r"\1", s.rstrip("Z")))


def o_time(o, disp=None):
    d, tzn = o_dt(o["dateTime"]), M.iana(o.get("timeZone") or "UTC")
    return M.T("T", d, tzn) if disp is None or disp == tzn else M.tin(d.replace(tzinfo=M.zone(tzn)), disp)


def orig_tz(ev):
    try:
        return M.iana(ev.get("originalStartTimeZone") or "UTC")
    except M.TzError:
        return "UTC"


def text_of(body):
    c = (body or {}).get("content") or ""
    if (body or {}).get("contentType", "text").lower() == "html":
        c = html.unescape(re.sub(r"<[^>]+>", "", re.sub(r"(?i)<br\s*/?>|</p>", "\n", re.sub(r"(?is)<(head|style)\b.*?</\1>", "", c))))
    return M.norm_text(c)


def o_model(ev, disp=None):
    m = M.new_model()
    m["summary"], m["description"] = ev.get("subject") or "", text_of(ev.get("body"))
    m["location"] = (ev.get("location") or {}).get("displayName") or ""
    if ev.get("isAllDay"):
        m["start"], m["end"] = M.tdate(_date(ev["start"]["dateTime"])), M.tdate(_date(ev["end"]["dateTime"]))
        if m["end"].v <= m["start"].v:
            m["end"] = M.tdate(m["start"].v + dt.timedelta(days=1))
    else:
        m["start"] = o_time(ev["start"], disp)
        m["end"] = o_time(ev["end"], m["start"].tz)
    if ev.get("isReminderOn") and ev.get("reminderMinutesBeforeStart") is not None:
        m["alarms"] = [("popup", int(ev["reminderMinutesBeforeStart"]))]
    show = ev.get("showAs")
    m["transp"] = "TRANSPARENT" if show == "free" else ""
    m["status"] = "CANCELLED" if ev.get("isCancelled") else "TENTATIVE" if show == "tentative" else ""
    m["cls"] = {"private": "PRIVATE", "personal": "PRIVATE", "confidential": "CONFIDENTIAL"}.get(ev.get("sensitivity"), "")
    return m


def _cn(name):
    return ';CN="%s"' % name.replace('"', "").replace("\\", "") if name else ""


def o_extra(ev):
    L = []
    o = (ev.get("organizer") or {}).get("emailAddress") or {}
    if o.get("address"):
        L.append("ORGANIZER%s:mailto:%s" % (_cn(o.get("name")), o["address"]))
    for a in ev.get("attendees", []):
        e = a.get("emailAddress") or {}
        if not e.get("address"):
            continue
        ps = {"accepted": "ACCEPTED", "declined": "DECLINED", "tentativelyAccepted": "TENTATIVE"}.get((a.get("status") or {}).get("response"), "NEEDS-ACTION")
        L.append("ATTENDEE%s;CUTYPE=%s;ROLE=%s;PARTSTAT=%s:mailto:%s" % (_cn(e.get("name")), "RESOURCE" if a.get("type") == "resource" else "INDIVIDUAL",
                                                                   "OPT-PARTICIPANT" if a.get("type") == "optional" else "REQ-PARTICIPANT", ps, e["address"]))
    if ev.get("categories"):
        L.append("CATEGORIES:" + ",".join(M.esc(c) for c in ev["categories"]))
    url = (ev.get("onlineMeeting") or {}).get("joinUrl")
    if url:
        L.append("X-MICROSOFT-SKYPETEAMSMEETINGURL:" + url)
    return L


def updated(ev):
    try:
        return M.fmt_t(M.tutc(o_dt(ev["lastModifiedDateTime"]).replace(tzinfo=M.UTC)))
    except Exception:
        return ""


def series_tz(a):
    """(пояс серии IANA, имя Windows для Prefer, точно ли): по range.recurrenceTimeZone, иначе по поясу создания."""
    name = ((a.get("recurrence") or {}).get("range") or {}).get("recurrenceTimeZone") or a.get("originalStartTimeZone") or "UTC"
    try:
        tz = M.iana(name)
        w = M.win_name(tz)
        return (tz, w, True) if w else ("UTC", "UTC", False)
    except M.TzError:
        return "UTC", "UTC", False


class Cur(object):
    """Серверная версия события-группы: модель главного, замены, etag, признак серии/только чтения."""
    series = False
    ro = False
    stz = None
    wtz = None
    xs = ()


def build(a, b=None):
    """a - событие (поясе UTC); b - главное событие серии в поясе серии с exceptionOccurrences/cancelledOccurrences -> Cur."""
    c = Cur()
    c.a, c.id = a, a["id"]
    c.uid = a.get("iCalUId") or a["id"]
    if a.get("type") != "seriesMaster":
        c.mm, c.etag, c.xs, c.updated = o_model(a, orig_tz(a)), a.get("changeKey", ""), [], updated(a)
        return c
    c.series = True
    c.stz, c.wtz, exact = series_tz(a)
    src = b or a
    c.mm = o_model(src, c.stz)
    rec = a.get("recurrence") or src.get("recurrence")
    rr = rec_to_rrule(rec, c.mm["start"]) if rec else None
    c.ro = not exact or rr is None
    c.mm["rrule"] = rr or ""
    parts = []
    for oc in (b or {}).get("cancelledOccurrences") or []:
        parts.append(oc)
        m = re.search(r"(\d{4}-\d\d-\d\d)$", oc)
        if m:
            d = dt.date.fromisoformat(m.group(1))
            c.mm["exdates"].append(M.tdate(d) if c.mm["start"].kind == "D" else M.T("T", dt.datetime.combine(d, c.mm["start"].v.time()), c.mm["start"].tz))
    xs = []
    for x in (b or {}).get("exceptionOccurrences") or []:
        parts.append("%s:%s" % (x.get("id"), x.get("changeKey")))
        if x.get("isCancelled"):
            continue
        xm = o_model(x, c.stz)
        xm["recid"] = M.tin(o_dt(x["originalStart"]).replace(tzinfo=M.UTC), c.stz) if x.get("originalStart") else xm["start"]
        xs.append((xm, x))
    c.xs = xs
    c.etag = M.series_etag(a.get("changeKey"), parts)
    c.updated = updated(a)
    return c


def cur_events(c):
    """Cur -> строки VEVENT (главное, затем замены). У замены свой UID не нужен: UID серии общий."""
    ex = o_extra(c.a)
    if c.ro:
        ex = ex + ["X-OUTLOOK-READONLY:1"]
    evs = [M.vevent(c.mm, c.uid, c.etag, c.updated, ex)]
    for xm, x in c.xs:
        xm["rrule"] = ""
        evs.append(M.vevent(xm, c.uid, c.etag, updated(x), o_extra(x)))
    return evs


# ---------- запросы ----------
def pages(api, url, params=None, headers=None):
    items = []
    while url:
        _, d, _ = api.req("GET", url, params=params, headers=headers)
        if not isinstance(d, dict):
            raise O.ApiError(502, "неожиданный ответ сервера")
        items += d.get("value", [])
        url, params = d.get("@odata.nextLink"), None
    return items


def list_light(api, cal):
    items = pages(api, calurl(cal, "/events"), {"$select": "id,iCalUId,changeKey,type", "$top": 100}, PREF)
    return {e["id"]: (e.get("changeKey", ""), e.get("iCalUId") or e["id"]) for e in items}


def batch(api, reqs):
    """GET-запросы пачками по 20 через $batch: [(относительный url, заголовки)] -> [(статус, тело)] в том же порядке."""
    out, todo = [None] * len(reqs), list(range(len(reqs)))
    for attempt in range(3):
        nxt = []
        for k in range(0, len(todo), 20):
            chunk = todo[k:k + 20]
            _, d, _ = api.req("POST", GRAPH + "/$batch", body={"requests": [{"id": str(i), "method": "GET", "url": reqs[i][0], "headers": reqs[i][1]} for i in chunk]})
            for r in (d or {}).get("responses", []):
                i, st = int(r["id"]), int(r.get("status", 0))
                if st in (429, 502, 503, 504) and attempt < 2:
                    nxt.append(i)
                else:
                    out[i] = (st, r.get("body"))
        todo = nxt
        if not todo:
            break
        O.SLEEP(2)
    return [o or (0, None) for o in out]


def fetch_a(api, cal, ids):
    """События (в UTC) по списку id: много - постраничным списком, мало - через $batch."""
    if len(ids) > 25:
        want = set(ids)
        return [e for e in pages(api, calurl(cal, "/events"), {"$select": SELECT, "$top": 100}, PREF) if e["id"] in want]
    res = batch(api, [("/me/events/%s?$select=%s" % (quote(i, safe=""), SELECT), PREF) for i in ids])
    return [b for st, b in res if st == 200 and isinstance(b, dict)]


def series_headers(a):
    return {"Prefer": 'outlook.timezone="%s", outlook.body-content-type="text"' % series_tz(a)[1]}


def fetch_b(api, masters):
    """Главные события серий в поясе серии, с заменами и отменами: {id: json}. Без $expand (сервер не принял) - только главное."""
    reqs = [("/me/events/%s?$select=%s&$expand=exceptionOccurrences" % (quote(a["id"], safe=""), SELECT + ",cancelledOccurrences"), series_headers(a)) for a in masters]
    out = {}
    for a, (st, b) in zip(masters, batch(api, reqs) if reqs else []):
        if st == 200 and isinstance(b, dict):
            out[a["id"]] = b
        elif st == 400:
            _, b, _ = api.req("GET", evurl(a["id"]), params={"$select": SELECT}, headers=series_headers(a))
            out[a["id"]] = b
    return out


def get_cur(api, eid):
    """Событие по id (серия - вместе с заменами) -> Cur или None, если его нет."""
    st, a, _ = api.req("GET", evurl(eid), params={"$select": SELECT}, headers=PREF, ok=(404,))
    if st == 404 or not isinstance(a, dict):
        return None
    if a.get("type") != "seriesMaster":
        return build(a)
    return build(a, fetch_b(api, [a]).get(a["id"]))


# ---------- pull ----------
def new_state(state):
    state.setdefault("gen", 0)
    state.setdefault("committed", {"cursor": "", "etags": {}, "uids": {}})
    state.setdefault("pending", None)
    return state


def pull(api, cal, state, since):
    s = new_state(state)
    cm = s["committed"]
    if s["pending"] and since and since == s["pending"]["cursor"]:           # клиент применил прошлый ответ
        cm = s["committed"] = s["pending"]
        s["pending"] = None
    full = not since or since != cm["cursor"]
    light = list_light(api, cal)
    be, bu = ({}, {}) if full else (cm["etags"], cm["uids"])
    changed = [i for i, v in light.items() if be.get(i) != v[0]]
    gone = [i for i in be if i not in light]
    got = fetch_a(api, cal, changed) if changed else []
    masters = [a for a in got if a.get("type") == "seriesMaster"]
    bs = fetch_b(api, masters) if masters else {}
    out = []
    etags, uids = {i: v[0] for i, v in light.items() if i not in changed}, {i: v[1] for i, v in light.items() if i not in changed}
    for a in got:
        if a.get("type") not in ("seriesMaster", "singleInstance", None):
            continue
        c = build(a, bs.get(a["id"]))
        out += cur_events(c)
        etags[a["id"]], uids[a["id"]] = light[a["id"]][0], c.uid
    for i in gone:
        out.append(D.deleted_marker(bu.get(i) or i, be[i]))
    s["gen"] += 1
    cur = "g%d" % s["gen"]
    s["pending"] = {"cursor": cur, "etags": etags, "uids": uids}
    return out, ["X-DKEYS-CURSOR:" + cur] + (["X-DKEYS-FULL:1"] if full else [])


def cmd_pull(opts):
    api, cal, sp, state, _ro = open_profile(opts["profile"])
    evs, extra = pull(api, cal, state, opts["since"].strip())
    D.out(D.vcalendar(evs, extra))
    D.write_json(sp, state)


# ---------- push ----------
def record(state, eid, uid, etag):
    new_state(state)
    for sn in (state["pending"], state["committed"]):
        if sn:
            sn["etags"][eid], sn["uids"][eid] = etag, uid


def forget(state, eid):
    for sn in (state.get("pending"), state.get("committed")):
        if sn:
            sn["etags"].pop(eid, None)
            sn["uids"].pop(eid, None)


def lookup(api, cal, state, uid):
    for sn in (state.get("pending"), state.get("committed")):
        for i, u in ((sn or {}).get("uids") or {}).items():
            if u == uid:
                return i
    items = pages(api, calurl(cal, "/events"), {"$filter": "iCalUId eq '%s'" % uid.replace("'", "''"), "$select": "id,type", "$top": 5}, PREF)
    return next((e["id"] for e in items if e.get("type") in (None, "singleInstance", "seriesMaster")), None)


def utc_iso(t):
    return {"dateTime": (t.v.strftime("%Y-%m-%dT00:00:00") if t.kind == "D" else M.inst(t).strftime("%Y-%m-%dT%H:%M:%S")), "timeZone": "UTC"}


def o_body(m, fields, wtz=None, stz=None):
    """Тело запроса Graph для полей fields из модели m; wtz/stz - пояс серии (имя Windows / IANA), иначе время уходит в UTC."""
    b = {}
    if "summary" in fields:
        b["subject"] = m["summary"]
    if "description" in fields:
        b["body"] = {"contentType": "text", "content": m["description"]}
    if "location" in fields:
        b["location"] = {"displayName": m["location"]}
    if "time" in fields:
        b["isAllDay"] = m["start"].kind == "D"
        if wtz and m["start"].kind == "T":
            b["start"], b["end"] = [{"dateTime": M.to_tz(t, stz).v.strftime("%Y-%m-%dT%H:%M:%S"), "timeZone": wtz} for t in (m["start"], m["end"])]
        else:
            b["start"], b["end"] = utc_iso(m["start"]), utc_iso(m["end"])
    if "alarms" in fields:
        al = [x[1] for x in m["alarms"] if x[1] >= 0]
        b["isReminderOn"] = bool(al)
        if al:
            b["reminderMinutesBeforeStart"] = max(al)
    if "transp" in fields:
        b["showAs"] = "free" if m["transp"] == "TRANSPARENT" else "busy"
    if "cls" in fields:
        b["sensitivity"] = {"PRIVATE": "private", "CONFIDENTIAL": "confidential"}.get(m["cls"], "normal")
    return b


def collapse_alarms(m):
    """Outlook хранит одно напоминание: из нескольких берётся самое раннее (для сравнения с сервером)."""
    al = [x[1] for x in m["alarms"] if x[1] >= 0]
    m["alarms"] = [("popup", max(al))] if al else []
    return m


def rec_for(inc, c=None):
    """recurrence для записи: (тело, пояс Windows, IANA); ApiError, если повтор или пояс нельзя записать без потерь."""
    s = inc["start"]
    tz = s.tz if s.kind == "T" else "UTC"
    w = M.win_name(tz)
    if not w:
        raise O.ApiError(400, "часовой пояс %s нельзя записать в Outlook без потерь: правьте серию в Outlook" % tz)
    rec = rrule_to_rec(inc["rrule"], s, w)
    if rec is None:
        raise O.ApiError(400, "повтор «%s» нельзя записать в Outlook без потерь: серия только для чтения, правьте её в Outlook" % inc["rrule"])
    return rec, w, tz


def instance_for(api, mid, ot):
    """Вхождение серии mid с исходным началом ot (замена создаётся правкой такого вхождения)."""
    a = M.inst(ot) if ot.kind == "T" else dt.datetime.combine(ot.v, dt.time(0), M.UTC)
    lo, hi = a - dt.timedelta(days=1), a + dt.timedelta(days=2)
    f = "%Y-%m-%dT%H:%M:%SZ"
    items = pages(api, evurl(mid, "/instances"), {"startDateTime": lo.strftime(f), "endDateTime": hi.strftime(f), "$select": SELECT, "$top": 50}, series_headers({"originalStartTimeZone": ot.tz or "UTC"}))
    for i in items:
        if i.get("isAllDay") and ot.kind == "D":
            if _date(i["start"]["dateTime"]) == ot.v:
                return i
        elif ot.kind == "T" and not i.get("isAllDay") and M.key(M.T("T", o_dt(i["start"]["dateTime"]), M.iana(i["start"].get("timeZone") or "UTC"))) == M.key(ot):
            return i
    return None


def push_exceptions(api, c, xev):
    by = {M.key(xm["recid"]): x for xm, x in c.xs}
    for ev in xev:
        xm = M.read_vevent(ev)
        if xm["recid"] is None:
            continue
        cur = by.get(M.key(xm["recid"])) or instance_for(api, c.id, xm["recid"])
        if cur is None:
            raise O.ApiError(404, "вхождение серии не найдено: " + M.fmt_t(xm["recid"]))
        xs = o_model(cur, c.stz)
        body = o_body(collapse_alarms(xm), M.diff(collapse_alarms(xm), xs) - {"rrule", "exdates", "rlines"}, c.wtz, c.stz)
        if body:
            api.req("PATCH", evurl(cur["id"]), body=body, headers=PREF)


def push_one(api, cal, state, uid, group, op, sent):
    mev, xev = M.split_group(group)
    if op == "delete":
        eid = lookup(api, cal, state, uid)
        if eid:
            api.req("DELETE", evurl(eid), ok=(404, 410))
            forget(state, eid)
        return M.as_deleted(group)
    if mev is None:
        raise O.ApiError(400, "в группе нет главного события")
    inc = collapse_alarms(M.read_vevent(mev))
    if inc["start"] is None:
        raise O.ApiError(400, "у события нет начала")
    if op == "update":
        eid = lookup(api, cal, state, uid)
        c = get_cur(api, eid) if eid else None
        if c is None:
            raise O.ApiError(404, "not found")
        if sent and sent != c.etag:
            raise O.ApiError(412, "conflict")
        fields = M.diff(inc, c.mm) - {"exdates", "rlines"}
        have = {M.key(x) for x in c.mm["exdates"]}
        new_ex = [x for x in inc["exdates"] if M.key(x) not in have]
        if have - {M.key(x) for x in inc["exdates"]}:
            raise O.ApiError(400, "вернуть удалённое вхождение серии нельзя из DKeys: правьте серию в Outlook")
        if c.series and c.mm["rrule"] and not inc["rrule"]:
            raise O.ApiError(400, "превратить серию Outlook в одиночное событие нельзя из DKeys: правьте её в Outlook")
        if c.series and c.ro and (fields & {"time", "rrule"} or new_ex):
            raise O.ApiError(400, "серия Outlook с повтором, который нельзя записать без потерь, только для чтения: правьте её в Outlook")
        wtz, stz, rec = c.wtz, c.stz, None
        if inc["rrule"] and (not c.series or fields & {"time", "rrule"}):
            rec, wtz, stz = rec_for(inc)
            fields |= {"time"}
        body = o_body(inc, fields - {"rrule"}, wtz, stz)
        if rec is not None:
            body["recurrence"] = rec
        if body:
            api.req("PATCH", evurl(eid), body=body, headers=PREF)
        for x in new_ex:
            i = instance_for(api, eid, x)
            if i is not None:
                api.req("DELETE", evurl(i["id"]), ok=(404, 410))
        if xev and c.series:
            push_exceptions(api, c, xev)
        new_uid = c.uid
    else:                                                                       # create: transactionId делает повтор идемпотентным
        fields = {"summary", "description", "location", "time", "alarms", "transp", "cls"}
        wtz = stz = None
        rec = None
        if inc["rrule"]:
            rec, wtz, stz = rec_for(inc)
        body = o_body(inc, fields, wtz, stz)
        if rec is not None:
            body["recurrence"] = rec
        body["transactionId"] = M.stable_id(uid)[:32]
        _, it, _ = api.req("POST", calurl(cal, "/events"), body=body, headers=PREF)
        eid, new_uid = it["id"], it.get("iCalUId") or it["id"]
        if xev and inc["rrule"]:
            c = get_cur(api, eid)
            push_exceptions(api, c, xev)
    c = get_cur(api, eid)
    if c is None:
        raise O.ApiError(502, "событие не найдено после записи")
    record(state, eid, c.uid, c.a.get("changeKey", ""))
    return M.with_etag(group, c.etag, c.uid, uid)


def push(api, cal, state, text, readonly=False):
    out, done = [], 0
    for uid, group in D.group_by_uid(D.split_vevents(text)):
        op = (D.get(group[0], "X-DKEYS-OP") or "create").lower()
        sent = D.get(group[0], "X-DKEYS-ETAG") or ""
        try:
            if readonly:
                raise O.ApiError(403, "календарь только для чтения (readonly в outlook.ini)")
            out += push_one(api, cal, state, uid, group, op, sent)
            done += 1
        except O.ApiError as e:
            msg = "conflict" if e.status in (409, 412) else "not found" if e.status in (404, 410) else "Outlook %s: %s" % (e.status, e.message or "ошибка")
            out += M.with_error(group, msg)
        except (O.NetError, O.AuthError) as e:
            if not done:
                raise
            out += M.with_error(group, str(e))
        except M.TzError as e:
            out += M.with_error(group, str(e))
    return out


def cmd_push(opts):
    api, cal, sp, state, ro = open_profile(opts["profile"])
    D.out(D.vcalendar(push(api, cal, state, sys.stdin.read(), ro)))
    D.write_json(sp, state)


# ---------- setup ----------
def cmd_setup(opts):
    prof = opts["profile"]
    print("Outlook (Microsoft Graph): подключение%s" % (" (профиль %s)" % prof if prof else ""))
    O.ensure_tzdata()
    cl = load_client()
    if not cl:
        print(HOWTO)
        cid = O.ask("Application (client) ID")
        if not cid:
            D.fail("client_id не введён: зарегистрируйте приложение по инструкции выше и запустите «Подключить…» снова", 2)
        cl = {"client_id": cid.strip(), "tenant": "common"}
        O.write_secret(os.path.join(SBASE, "client.json"), cl)
    oa = oauth_cfg(cl)
    how = O.ask("Вход: Enter - в браузере на этом компьютере, d - по коду (на другом устройстве)", "").lower()
    try:
        tok = O.login_device(oa) if how.startswith("d") else O.login_browser(oa)
    except O.AuthError as e:
        D.fail(str(e), 3)
    os.makedirs(SBASE, exist_ok=True)
    tmpf = os.path.join(SBASE, "login.tmp.json")
    api = O.Client(oa, tmpf)
    api.tok = tok
    cp = O.read_ini(INI)
    try:
        if prof and cp.has_section(prof):
            O.write_secret(os.path.join(sdir(cp[prof].get("account") or prof), "token.json"), tok)
            print("Готово: вход выполнен заново. Закройте это окно - DKeys синхронизируется сам.")
            return
        _, me, _ = api.req("GET", GRAPH + "/me", params={"$select": "mail,userPrincipalName,displayName"})
        cals = pages(api, GRAPH + "/me/calendars", {"$select": "id,name,canEdit,isDefaultCalendar", "$top": 100})
    except (O.ApiError, O.NetError, O.AuthError) as e:
        D.fail("не удалось получить список календарей: %s" % e, 3)
    finally:
        if os.path.exists(tmpf):
            os.remove(tmpf)
    if not cals:
        D.fail("в аккаунте нет календарей", 3)
    email = me.get("mail") or me.get("userPrincipalName") or "account"
    account = O.slug(email.replace("@", "_"), 40, "account")
    O.write_secret(os.path.join(sdir(account), "token.json"), tok)
    print("Аккаунт: %s" % email)
    pick = O.pick_many(cals, lambda c: "%s%s%s" % (c.get("name") or c["id"], " (основной)" if c.get("isDefaultCalendar") else "", "" if c.get("canEdit", True) else " [только чтение]"))
    for c in pick:
        if any(cp.has_section(s) and cp[s].get("account") == account and cp[s].get("calendar") == c["id"] for s in cp.sections()):
            continue
        base = O.slug(email.split("@")[0] if c.get("isDefaultCalendar") else (c.get("name") or ""), 15, "cal")
        name = O.unique(base, set(cp.sections()))
        cp.add_section(name)
        cp.set(name, "account", account)
        cp.set(name, "calendar", c["id"])
        cp.set(name, "name", c.get("name") or c["id"])
        if c.get("canEdit") is False:
            cp.set(name, "readonly", "1")
        sp = os.path.join(sdir(name), "state.json")
        if os.path.exists(sp):
            os.remove(sp)
        print("  источник outlook:%s" % name)
    O.write_ini(cp, INI)
    print("Готово. Закройте это окно - DKeys найдёт источники и синхронизируется сам.")


def selftest():
    import _selftest_outlook
    _selftest_outlook.run(sys.modules[__name__])


def main():
    D.io_utf8()
    verb, opts, _rest = D.parse_args(sys.argv[1:])
    try:
        if verb == "--selftest":
            selftest()
        elif verb == "info":
            D.out(D.info_json("Outlook (Microsoft 365, Outlook.com)", calendars=True, profiles=True, login="oauth", series=True) + "\n")
        elif verb == "setup":
            cmd_setup(opts)
        elif verb == "pull":
            cmd_pull(opts)
        elif verb == "push":
            cmd_push(opts)
        else:
            D.fail("usage: outlook.py info|setup|pull --since S|push [--profile P] | --selftest", 2)
    except O.AuthError as e:
        D.fail("Outlook: %s" % e, 3)
    except O.NetError as e:
        D.fail("Outlook: %s" % e, 3)
    except O.ApiError as e:
        D.fail("Outlook: ошибка сервера: %s" % e, 3)
    except M.TzError as e:
        D.fail(str(e), 3)


if __name__ == "__main__":
    main()
