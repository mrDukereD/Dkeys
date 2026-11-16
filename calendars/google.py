# -*- coding: utf-8 -*-
"""google - источник календаря DKeys: Google Calendar (API v3 по REST, только стандартная библиотека). Договор команд - README.md рядом.
Вход: OAuth 2.0 «приложение для ПК» (браузер на 127.0.0.1 + PKCE); ключи приложения (client_id/secret) пользователь создаёт сам (инструкция в README
и в `setup`). Профиль = секция google.ini: account (папка токена), calendar (id календаря), name, readonly; несколько календарей = несколько профилей.
Токен - google.state\\<account>\\token.json, состояние синхронизации - google.state\\<профиль>\\state.json (не в git, доступ только владельцу).
Синхронизация инкрементальная (syncToken; 410 - полная выгрузка). Серия = главное событие + замены вхождений (отдельные события Google с recurringEventId),
etag серии = etag главного + отпечаток замен. В Google уходит только изменившееся (сравнение с серверной версией): участники, цвет, видеозвонок и прочее
не затрагиваются. Проверка без сети и аккаунтов: `python google.py --selftest` (поддельный сервер)."""
import datetime as dt
import glob
import json
import os
import re
import sys
from urllib.parse import quote

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dkcal as D  # noqa: E402
import _oauth as O  # noqa: E402
import _model as M  # noqa: E402

API = "https://www.googleapis.com/calendar/v3"
OA_BASE = {"auth_url": "https://accounts.google.com/o/oauth2/v2/auth", "token_url": "https://oauth2.googleapis.com/token",
           "scope": "https://www.googleapis.com/auth/calendar", "redirect_host": "127.0.0.1",
           "auth_params": {"access_type": "offline", "prompt": "consent"}}
INI = os.path.join(HERE, "google.ini")
SBASE = os.path.join(HERE, "google.state")
HOWTO = """Ключ приложения Google нужен один раз (2-3 минуты, бесплатно):
  1. https://console.cloud.google.com/ -> создайте проект (любое имя).
  2. «APIs & Services» -> «Library» -> «Google Calendar API» -> Enable.
  3. «APIs & Services» -> «OAuth consent screen» (или «Google Auth Platform»): тип External, имя приложения любое, ваш e-mail.
     Затем «Audience» -> Publish app (для личного пользования проверка Google не нужна; в режиме Testing вход слетает через 7 дней).
  4. «Credentials» -> Create credentials -> OAuth client ID -> тип «Desktop app» -> Create.
  5. Скопируйте Client ID и Client secret в ответы ниже (либо скачайте JSON кнопкой Download и положите как calendars\\client_secret.json).
При входе Google скажет «приложение не проверено»: Advanced -> Go to ... (unsafe) - это ваше собственное приложение."""


def sdir(name):
    p = os.path.join(SBASE, name)
    os.makedirs(p, exist_ok=True)
    return p


# ---------- ключи приложения и профили ----------
def load_client():
    cid = os.environ.get("DKEYS_GOOGLE_CLIENT_ID")
    if cid:
        return {"client_id": cid, "client_secret": os.environ.get("DKEYS_GOOGLE_CLIENT_SECRET", "")}
    for p in [os.path.join(SBASE, "client.json"), os.path.join(SBASE, "client_secret.json")] + sorted(glob.glob(os.path.join(HERE, "client_secret*.json"))):
        d = D.read_json(p, None)
        if isinstance(d, dict):
            d = d.get("installed") or d.get("web") or d
            if d.get("client_id"):
                return {"client_id": d["client_id"], "client_secret": d.get("client_secret", "")}
    return None


def oauth_cfg(cl):
    return dict(OA_BASE, client_id=cl["client_id"], client_secret=cl.get("client_secret", ""))


def open_profile(profile):
    cp = O.read_ini(INI)
    name = O.pick_profile(cp, profile)
    if not name:
        D.fail("профиль %s не настроен в google.ini: нажмите «Подключить…» в календаре" % (profile or ""), 2)
    sec = cp[name]
    cl = load_client()
    if not cl:
        D.fail("нет ключа приложения Google (client_id): нажмите «Подключить…» в календаре (setup объяснит, где его взять)", 2)
    tokfile = os.path.join(sdir(sec.get("account") or name), "token.json")
    if not os.path.exists(tokfile):
        D.fail(O.NEED_LOGIN, 2)
    sp = os.path.join(sdir(name), "state.json")
    return (O.Client(oauth_cfg(cl), tokfile), (sec.get("calendar") or "primary").strip(), sp, D.read_json(sp, {}),
            (sec.get("readonly") or "").strip().lower() in ("1", "true", "yes"))


# ---------- события Google <-> модель ----------
def evurl(cal, tail=""):
    return "%s/calendars/%s/events%s" % (API, quote(cal, safe=""), tail)


def list_all(api, cal, params):
    items, page, sync, defrem = [], None, None, None
    while True:
        p = dict(params)
        if page:
            p["pageToken"] = page
        _, d, _ = api.req("GET", evurl(cal), params=p)
        if not isinstance(d, dict):
            raise O.ApiError(502, "неожиданный ответ сервера")
        items += d.get("items", [])
        defrem = d.get("defaultReminders", defrem)
        page = d.get("nextPageToken")
        if not page:
            sync = d.get("nextSyncToken")
            break
    return items, sync, defrem


def g_time(o):
    if "date" in o:
        return M.tdate(dt.date.fromisoformat(o["date"]))
    d, tz = M.parse_rfc3339(o["dateTime"]), o.get("timeZone")
    if d.tzinfo is None:
        return M.T("T", d, M.iana(tz))
    return M.tin(d, tz) if tz else M.tutc(d)


def g_alarms(item, defrem):
    r = item.get("reminders") or {}
    src = (defrem or []) if r.get("useDefault") else r.get("overrides", [])
    return sorted({("email" if x.get("method") == "email" else "popup", int(x.get("minutes", 0))) for x in src}, key=lambda x: (x[1], x[0]))


def g_model(item, defrem):
    m = M.new_model()
    m["summary"], m["description"], m["location"] = item.get("summary", ""), item.get("description", ""), item.get("location", "")
    m["start"], m["end"] = g_time(item["start"]), g_time(item.get("end") or item["start"])
    if m["start"].kind == "T" and m["end"].kind == "T":
        m["end"] = M.to_tz(m["end"], m["start"].tz)
    for ln in item.get("recurrence") or []:
        u = ln.upper()
        if u.startswith("RRULE:"):
            m["rrule"] = ln[6:]
        elif u.startswith("EXDATE"):
            m["exdates"] += [M.to_tz(x, m["start"].tz) if x.kind == "T" else x for x in M.parse_ts(ln)]
        elif u.startswith(("RDATE", "EXRULE")):
            m["rlines"].append(ln)
    m["alarms"] = g_alarms(item, defrem)
    m["transp"] = "TRANSPARENT" if item.get("transparency") == "transparent" else ""
    m["cls"] = {"private": "PRIVATE", "confidential": "CONFIDENTIAL", "public": "PUBLIC"}.get(item.get("visibility"), "")
    m["status"] = "TENTATIVE" if item.get("status") == "tentative" else ""
    return m


def _cn(name):
    return ';CN="%s"' % name.replace('"', "").replace("\\", "") if name else ""


def g_extra(item):
    """Что DKeys не правит: организатор, участники, цвет, видеозвонок, тип события (сохраняется нетронутым)."""
    L = []
    o = item.get("organizer") or {}
    if o.get("email"):
        L.append("ORGANIZER%s:mailto:%s" % (_cn(o.get("displayName")), o["email"]))
    for a in item.get("attendees", []):
        if not a.get("email"):
            continue
        ps = {"accepted": "ACCEPTED", "declined": "DECLINED", "tentative": "TENTATIVE"}.get(a.get("responseStatus"), "NEEDS-ACTION")
        L.append("ATTENDEE%s;CUTYPE=%s;ROLE=%s;PARTSTAT=%s:mailto:%s" % (_cn(a.get("displayName")), "RESOURCE" if a.get("resource") else "INDIVIDUAL",
                                                                   "OPT-PARTICIPANT" if a.get("optional") else "REQ-PARTICIPANT", ps, a["email"]))
    if item.get("colorId"):
        L.append("X-GCAL-COLORID:" + str(item["colorId"]))
    link = item.get("hangoutLink") or next((e.get("uri") for e in (item.get("conferenceData") or {}).get("entryPoints", []) if e.get("entryPointType") == "video"), "")
    if link:
        L.append("X-GOOGLE-CONFERENCE:" + link)
    if item.get("eventType") not in (None, "default"):
        L.append("X-GCAL-TYPE:" + str(item["eventType"]))
    return L


def updated(item):
    try:
        return M.fmt_t(M.tutc(M.parse_rfc3339(item["updated"])))
    except Exception:
        return ""


def g_series(master, excs, defrem):
    """Главное событие и его замены/отмены -> (модель главного, [(модель замены, событие Google)], etag серии)."""
    mm = g_model(master, defrem)
    stz = mm["start"].tz
    xs, parts = [], []
    for x in excs:
        parts.append("%s:%s" % (x["id"], x.get("etag")))
        ot = g_time(x["originalStartTime"])
        if ot.kind == "T":
            ot = M.to_tz(ot, stz or "UTC")
        if x.get("status") == "cancelled":
            mm["exdates"].append(ot)
            continue
        xm = g_model(x, defrem)
        xm["recid"], xm["rrule"], xm["exdates"], xm["rlines"] = ot, "", [], []
        xs.append((xm, x))
    return mm, xs, M.series_etag(master.get("etag"), parts)


def g_group(master, excs, defrem):
    """-> (uid, etag, [строки VEVENT])."""
    mm, xs, etag = g_series(master, excs, defrem)
    uid = iuid(master)
    evs = [M.vevent(mm, uid, etag, updated(master), g_extra(master))]
    evs += [M.vevent(xm, uid, etag, updated(x), g_extra(x)) for xm, x in xs]
    return uid, etag, evs


def iuid(item):
    return item.get("iCalUID") or item["id"] + "@google.com"


def guess_uid(state, gid):
    return (state.get("uids") or {}).get(gid) or gid + "@google.com"


def note_uid(state, item):
    u = item.get("iCalUID")
    if u and u != item["id"] + "@google.com":
        state.setdefault("uids", {})[item["id"]] = u


def ensure_defrem(api, cal, state):
    if state.get("defrem") is None:
        _, d, _ = api.req("GET", "%s/users/me/calendarList/%s" % (API, quote(cal, safe="")))
        state["defrem"] = (d or {}).get("defaultReminders", [])
    return state["defrem"]


def load_group(api, cal, uid, state=None):
    """События с этим iCalUID: (главное | None, [замены и отмены], defaultReminders)."""
    items, _, dr = list_all(api, cal, {"iCalUID": uid, "showDeleted": "true", "singleEvents": "false", "maxResults": 250})
    master = next((i for i in items if not i.get("recurringEventId") and i.get("status") != "cancelled"), None)
    if master is None:
        return None, [], dr
    excs = [i for i in items if i.get("recurringEventId") == master["id"]]
    have = {x["id"] for x in excs}
    for xid in ((state or {}).get("series") or {}).get(master["id"], []):
        if xid not in have:
            st, d, _ = api.req("GET", evurl(cal, "/" + quote(xid, safe="")), ok=(404, 410))
            if st < 400 and isinstance(d, dict) and d.get("recurringEventId") == master["id"]:
                excs.append(d)
    return master, excs, dr


# ---------- pull ----------
FULL_Q = {"showDeleted": "true", "singleEvents": "false", "maxResults": 250}


def pull(api, cal, state, since):
    """-> (события, свойства VCALENDAR). Изменилась серия - приходит целиком."""
    full = not since
    if not full:
        try:
            items, sync, dr = list_all(api, cal, dict(FULL_Q, syncToken=since))
        except O.ApiError as e:
            if e.status != 410:
                raise
            full = True
    if full:
        items, sync, dr = list_all(api, cal, FULL_Q)
        state["series"], state["uids"] = {}, {}
    if dr is not None:
        state["defrem"] = dr
    dr = ensure_defrem(api, cal, state)
    series = state.setdefault("series", {})
    masters, excs_by, singles, deleted = {}, {}, [], []
    for it in items:
        if it.get("recurringEventId"):
            excs_by.setdefault(it["recurringEventId"], []).append(it)
        elif it.get("recurrence") is not None or it["id"] in series:
            masters[it["id"]] = it
        elif it.get("status") == "cancelled":
            deleted.append(it)
        else:
            singles.append(it)
    out = []
    for mid in list(dict.fromkeys(list(masters) + list(excs_by))):
        m = masters.get(mid)
        if full:
            if m is None or m.get("status") == "cancelled":
                continue
            excs = excs_by.get(mid, [])
        else:
            if m is None:
                st, m, _ = api.req("GET", evurl(cal, "/" + quote(mid, safe="")), ok=(404, 410))
                m = m if st < 400 and isinstance(m, dict) else None
            if m is None or m.get("status") == "cancelled":
                uid = (m or {}).get("iCalUID") or guess_uid(state, mid)
                out.append(D.deleted_marker(uid))
                series.pop(mid, None)
                continue
            gm, gx, _ = load_group(api, cal, iuid(m), state)
            seen = {x["id"] for x in excs_by.get(mid, [])}
            excs = list(excs_by.get(mid, [])) + [x for x in gx if x["id"] not in seen and gm is not None and gm["id"] == mid]
        _, _, evs = g_group(m, excs, dr)
        out += evs
        series[mid] = sorted(x["id"] for x in excs if x.get("status") != "cancelled")
        note_uid(state, m)
    for it in singles:
        _, _, evs = g_group(it, [], dr)
        out += evs
        note_uid(state, it)
    if not full:
        for it in deleted:
            out.append(D.deleted_marker(it.get("iCalUID") or guess_uid(state, it["id"])))
            (state.get("uids") or {}).pop(it["id"], None)
    return out, (["X-DKEYS-CURSOR:" + sync] if sync else []) + (["X-DKEYS-FULL:1"] if full else [])


def looks_iso(s):
    return bool(re.match(r"^\d{4}-\d\d-\d\dT", s))


def cmd_pull(opts):
    api, cal, sp, state, _ro = open_profile(opts["profile"])
    since = opts["since"].strip()
    evs, extra = pull(api, cal, state, "" if looks_iso(since) else since)
    D.out(D.vcalendar(evs, extra))
    D.write_json(sp, state)


# ---------- push ----------
def tbody(t):
    if t.kind == "D":
        return {"date": t.v.isoformat()}
    return {"dateTime": t.v.strftime("%Y-%m-%dT%H:%M:%S") + ("Z" if t.tz == "UTC" else ""), "timeZone": t.tz}


def rr_for_google(m):
    """UNTIL у события со временем - в UTC (требование Google), у целодневного - датой."""
    d = M.rr_parse(m["rrule"])
    u, s = d.get("UNTIL"), m["start"]
    if u and s.kind == "T" and not u.endswith("Z"):
        local = dt.datetime.strptime(u[:8] + ("T" + u[9:15] if len(u) >= 15 else "T235959"), "%Y%m%dT%H%M%S")
        d["UNTIL"] = M.inst(M.T("T", local, s.tz)).strftime("%Y%m%dT%H%M%SZ")
    elif u and s.kind == "D":
        d["UNTIL"] = u[:8]
    return M.rr_fmt(d)


def rec_lines(m):
    if not m["rrule"]:
        return None
    return ["RRULE:" + rr_for_google(m)] + [M.ics_t("EXDATE", x) for x in sorted(m["exdates"], key=M.key)] + list(m["rlines"])


def reminders_body(m):
    ov = [{"method": meth, "minutes": mins} for meth, mins in m["alarms"] if mins >= 0][:5]
    return {"useDefault": False, "overrides": ov}


def body_for(m, fields):
    """Тело запроса Google для полей fields (имена как у M.diff) из модели m."""
    b = {}
    for f, g in (("summary", "summary"), ("description", "description"), ("location", "location")):
        if f in fields:
            b[g] = m[f]
    if "time" in fields:
        b["start"], b["end"] = tbody(m["start"]), tbody(m["end"])
    if fields & {"rrule", "exdates", "rlines"}:
        b["recurrence"] = rec_lines(m)
    if "alarms" in fields:
        b["reminders"] = reminders_body(m)
    if "transp" in fields:
        b["transparency"] = "transparent" if m["transp"] == "TRANSPARENT" else "opaque"
    if "cls" in fields:
        b["visibility"] = {"PRIVATE": "private", "CONFIDENTIAL": "confidential", "PUBLIC": "public"}.get(m["cls"], "default")
    return b


ALL = {"summary", "description", "location", "time", "rrule", "exdates", "rlines", "alarms", "transp", "cls"}


def quoted(etag):
    return etag if etag.startswith('"') else '"%s"' % etag


def patch(api, cal, item, body):
    if not body:
        return item
    hd = {"If-Match": quoted(item["etag"])} if item.get("etag") else None
    _, d, _ = api.req("PATCH", evurl(cal, "/" + quote(item["id"], safe="")), body=body, headers=hd)
    return d


def find_instance(api, cal, mid, ot):
    """Вхождение серии mid с исходным началом ot (замена создаётся правкой такого вхождения)."""
    if ot.kind == "D":
        lo = dt.datetime.combine(ot.v, dt.time(0), M.UTC) - dt.timedelta(days=1)
        hi = lo + dt.timedelta(days=3)
    else:
        a = M.inst(ot)
        lo, hi = a - dt.timedelta(minutes=1), a + dt.timedelta(minutes=1)
    f = "%Y-%m-%dT%H:%M:%SZ"
    _, d, _ = api.req("GET", evurl(cal, "/%s/instances" % quote(mid, safe="")), params={"timeMin": lo.strftime(f), "timeMax": hi.strftime(f), "showDeleted": "true", "maxResults": 20})
    for i in (d or {}).get("items", []):
        if M.key(M.to_tz(g_time(i["originalStartTime"]), ot.tz) if ot.kind == "T" else g_time(i["originalStartTime"])) == M.key(ot):
            return i
    return None


def push_exceptions(api, cal, master, srv_xs, xev, dr):
    """Замены вхождений из группы -> события Google (только изменившееся); ошибка по вхождению - исключение."""
    by = {M.key(xm["recid"]): x for xm, x in srv_xs}
    for ev in xev:
        xm = M.read_vevent(ev)
        if xm["recid"] is None:
            continue
        cur = by.get(M.key(xm["recid"])) or find_instance(api, cal, master["id"], xm["recid"])
        if cur is None:
            raise O.ApiError(404, "вхождение серии не найдено: " + M.fmt_t(xm["recid"]))
        patch(api, cal, cur, body_for(xm, M.diff(xm, g_model(cur, dr)) - {"rrule", "exdates", "rlines"}))


def conflict(msg="conflict"):
    return O.ApiError(412, msg)


def push_one(api, cal, state, uid, group, op, sent):
    """Одна группа (серия целиком) -> события ответа. Исключения ApiError/AuthError/NetError разбирает вызывающий."""
    mev, xev = M.split_group(group)
    if op == "delete":
        master, _x, _dr = load_group(api, cal, uid, state)
        for gid in dict.fromkeys([master["id"]] if master else [M.stable_id(uid)]):
            api.req("DELETE", evurl(cal, "/" + quote(gid, safe="")), ok=(404, 410))
        return M.as_deleted(group)
    if mev is None:
        raise O.ApiError(400, "в группе нет главного события")
    inc = M.read_vevent(mev)
    if inc["start"] is None:
        raise O.ApiError(400, "у события нет начала")
    if op == "update":
        master, excs, dr = load_group(api, cal, uid, state)
        if master is None:
            raise O.ApiError(404, "not found")
        mm, xs, etag = g_series(master, excs, dr)
        if sent and sent.strip('"') != etag:
            raise conflict()
        fields = M.diff(inc, mm)
        patch(api, cal, master, body_for(inc, fields))
        push_exceptions(api, cal, master, xs, xev, dr)
        new_uid = uid
    else:                                                                   # create: идемпотентно (id получен из UID)
        body = body_for(inc, ALL)
        body["status"] = "tentative" if inc["status"] == "TENTATIVE" else "confirmed"
        if not inc["rrule"]:
            body.pop("recurrence", None)
        body["id"] = M.stable_id(uid)
        try:
            _, it, _ = api.req("POST", evurl(cal), body=body)
        except O.ApiError as e:
            if e.status != 409:
                raise
            body.pop("id")                                                  # уже создано прошлой попыткой (или было удалено) - перезаписываем
            _, it, _ = api.req("PATCH", evurl(cal, "/" + quote(M.stable_id(uid), safe="")), body=body)
        new_uid = iuid(it)
        if xev:
            master, excs, dr = load_group(api, cal, new_uid, state)
            _mm, xs, _e = g_series(master, excs, dr)
            push_exceptions(api, cal, master, xs, xev, dr)
    master, excs, dr = load_group(api, cal, new_uid, state)
    if master is None:
        raise O.ApiError(502, "событие не найдено после записи")
    _m, _x, etag = g_series(master, excs, dr)
    note_uid(state, master)
    if master.get("recurrence") is not None:
        state.setdefault("series", {})[master["id"]] = sorted(x["id"] for x in excs if x.get("status") != "cancelled")
    return M.with_etag(group, etag, new_uid, uid)


def push(api, cal, state, text, readonly=False):
    out, done = [], 0
    for uid, group in D.group_by_uid(D.split_vevents(text)):
        op = (D.get(group[0], "X-DKEYS-OP") or "create").lower()
        sent = D.get(group[0], "X-DKEYS-ETAG") or ""
        try:
            if readonly:
                raise O.ApiError(403, "календарь только для чтения (readonly в google.ini)")
            out += push_one(api, cal, state, uid, group, op, sent)
            done += 1
        except O.ApiError as e:
            msg = "conflict" if e.status in (409, 412) else "not found" if e.status in (404, 410) else "Google %s: %s" % (e.status, e.message or "ошибка")
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
def calendar_list(api):
    items, page = [], None
    while True:
        _, d, _ = api.req("GET", API + "/users/me/calendarList", params={"minAccessRole": "reader", "maxResults": 250, **({"pageToken": page} if page else {})})
        items += d.get("items", [])
        page = d.get("nextPageToken")
        if not page:
            return items


def cmd_setup(opts):
    import getpass
    prof = opts["profile"]
    print("Google Calendar: подключение%s" % (" (профиль %s)" % prof if prof else ""))
    O.ensure_tzdata()
    cl = load_client()
    if not cl:
        print(HOWTO)
        cid = O.ask("Client ID")
        sec = getpass.getpass("Client secret (ввод не виден): ").strip() if cid else ""
        if not cid:
            D.fail("Client ID не введён: получите ключ по инструкции выше и запустите «Подключить…» снова", 2)
        cl = {"client_id": cid.strip(), "client_secret": sec}
        O.write_secret(os.path.join(SBASE, "client.json"), cl)
    oa = oauth_cfg(cl)
    try:
        tok = O.login_browser(oa)
    except O.AuthError as e:
        D.fail(str(e), 3)
    os.makedirs(SBASE, exist_ok=True)
    tmpf = os.path.join(SBASE, "login.tmp.json")
    api = O.Client(oa, tmpf)
    api.tok = tok
    cp = O.read_ini(INI)
    try:
        if prof and cp.has_section(prof):                                  # повторный вход существующего профиля
            account = cp[prof].get("account") or prof
            O.write_secret(os.path.join(sdir(account), "token.json"), tok)
            print("Готово: вход выполнен заново. Закройте это окно - DKeys синхронизируется сам.")
            return
        cals = calendar_list(api)
    except (O.ApiError, O.NetError, O.AuthError) as e:
        D.fail("не удалось получить список календарей: %s" % e, 3)
    finally:
        if os.path.exists(tmpf):
            os.remove(tmpf)
    if not cals:
        D.fail("в аккаунте нет доступных календарей", 3)
    primary = next((c for c in cals if c.get("primary")), cals[0])
    email = primary.get("id", "")
    account = O.slug(email.replace("@", "_"), 40, "account")
    O.write_secret(os.path.join(sdir(account), "token.json"), tok)
    print("Аккаунт: %s" % email)
    pick = O.pick_many(cals, lambda c: "%s%s [%s]" % (c.get("summaryOverride") or c.get("summary") or c["id"], " (основной)" if c.get("primary") else "", c.get("accessRole", "")))
    for c in pick:
        if any(cp.has_section(s) and cp[s].get("account") == account and cp[s].get("calendar") == c["id"] for s in cp.sections()):
            continue                                                       # этот календарь уже подключён
        base = O.slug(email.split("@")[0] if c.get("primary") else (c.get("summaryOverride") or c.get("summary") or ""), 15, "cal")
        name = O.unique(base, set(cp.sections()))
        cp.add_section(name)
        cp.set(name, "account", account)
        cp.set(name, "calendar", c["id"])
        cp.set(name, "name", c.get("summaryOverride") or c.get("summary") or c["id"])
        if c.get("accessRole") not in ("owner", "writer"):
            cp.set(name, "readonly", "1")
        sp = os.path.join(sdir(name), "state.json")
        if os.path.exists(sp):
            os.remove(sp)
        print("  источник google:%s" % name)
    O.write_ini(cp, INI)
    print("Готово. Закройте это окно - DKeys найдёт источники и синхронизируется сам.")


# ---------- проверка без сети ----------
def selftest():
    import _selftest_google
    _selftest_google.run(sys.modules[__name__])


def main():
    D.io_utf8()
    verb, opts, _rest = D.parse_args(sys.argv[1:])
    try:
        if verb == "--selftest":
            selftest()
        elif verb == "info":
            D.out(D.info_json("Google Calendar", calendars=True, profiles=True, login="oauth", series=True) + "\n")
        elif verb == "setup":
            cmd_setup(opts)
        elif verb == "pull":
            cmd_pull(opts)
        elif verb == "push":
            cmd_push(opts)
        else:
            D.fail("usage: google.py info|setup|pull --since S|push [--profile P] | --selftest", 2)
    except O.AuthError as e:
        D.fail("Google: %s" % e, 3)
    except O.NetError as e:
        D.fail("Google: %s" % e, 3)
    except O.ApiError as e:
        D.fail("Google: ошибка сервера: %s" % e, 3)
    except M.TzError as e:
        D.fail(str(e), 3)


if __name__ == "__main__":
    main()
