# -*- coding: utf-8 -*-
"""Проверка outlook.py без сети и аккаунтов (python outlook.py --selftest): поддельный Microsoft Graph подставляется вместо HTTP.
Не источник (имя на «_»). Что проверяется: повтор Graph <-> RRULE (таблица и круг), вход (PKCE, код устройства), выгрузка (пагинация nextLink,
изменения по changeKey, $batch, серии с заменами и отменами, пояса, целодневные, HTML-описание, неточные повторы только для чтения), отправка
(создание идемпотентно, правка только изменённого, конфликт, отмена вхождения, удаление), повторы при 429/5xx, токены."""
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import threading
import urllib.parse as up
import urllib.request

import dkcal as D
import _model as M
import _oauth as O


def ev_lines(text):
    return D.split_vevents(text)


def lines(ev, name):
    return [ln for ln in ev if D.pname(ln) == name]


def uids(text):
    return sorted(D.uid_of(e) for e in ev_lines(text))


def b64sha(v):
    import base64
    return base64.urlsafe_b64encode(hashlib.sha256(v.encode("ascii")).digest()).rstrip(b"=").decode("ascii")


class Fake(object):
    """Часть Microsoft Graph: календари, события (одиночные, серии, замены, отмены), $batch, instances, токены."""
    def __init__(self):
        self.ev, self.n, self.log, self.page = {}, 0, [], 3
        self.valid, self.at, self.revoked, self.flaky, self.challenge, self.dev_polls = {"AT1"}, 1, False, [], None, 0
        self.batch_flaky = 0

    # --- хранилище ---
    def touch(self, e):
        self.n += 1
        e["changeKey"] = "CK%d" % self.n
        e["lastModifiedDateTime"] = "2026-11-%02dT10:%02d:00.0000000Z" % (10 + self.n // 60, self.n % 60)
        if e.get("seriesMasterId") in self.ev:
            self.touch_master(self.ev[e["seriesMasterId"]])

    def touch_master(self, m):
        self.n += 1
        m["changeKey"] = "CK%d" % self.n

    def add(self, id, tz="Europe/Minsk", start=None, end=None, **kw):
        e = {"id": id, "iCalUId": "uid-" + id, "type": "singleInstance", "subject": "", "body": {"contentType": "text", "content": ""}, "isAllDay": False,
             "location": {"displayName": ""}, "isReminderOn": False, "reminderMinutesBeforeStart": 0, "showAs": "busy", "sensitivity": "normal", "isCancelled": False,
             "attendees": [], "categories": [], "_tz": tz, "_s": start, "_e": end}
        e.update(kw)
        self.touch(e)
        self.ev[id] = e
        return e

    def render(self, e, prefer_tz, select=None, expand=False):
        from zoneinfo import ZoneInfo
        ptz = M.WIN2IANA.get(prefer_tz, prefer_tz) if prefer_tz else "UTC"
        out = {k: v for k, v in e.items() if not k.startswith("_")}
        out["originalStartTimeZone"] = M.IANA2WIN.get(e["_tz"], e["_tz"])
        for k, nm in (("start", "_s"), ("end", "_e")):
            if e["isAllDay"]:
                out[k] = {"dateTime": e[nm].strftime("%Y-%m-%dT00:00:00.0000000"), "timeZone": prefer_tz or "UTC"}
            else:
                loc = e[nm].replace(tzinfo=ZoneInfo(e["_tz"])).astimezone(ZoneInfo(ptz))
                out[k] = {"dateTime": loc.strftime("%Y-%m-%dT%H:%M:%S.0000000"), "timeZone": prefer_tz or "UTC"}
        if e["type"] == "exception":
            out["originalStart"] = e["_orig"].replace(tzinfo=ZoneInfo(e["_tz"])).astimezone(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.0000000Z")
        if select:
            keep = set(select.split(",")) | {"id", "@odata.etag"}
            out = {k: v for k, v in out.items() if k in keep}
        if e["type"] == "seriesMaster":
            if select and "cancelledOccurrences" in select:
                out["cancelledOccurrences"] = list(e.get("_cancelled", []))
            if expand:
                out["exceptionOccurrences"] = [self.render(x, prefer_tz, select) for x in self.ev.values() if x.get("seriesMasterId") == e["id"] and x["type"] == "exception"]
        return out

    def occurrences(self, m):
        step = 7 if m["recurrence"]["pattern"]["type"] == "weekly" else 1
        for k in range(60):
            yield m["_s"] + dt.timedelta(days=k * step), m["_e"] + dt.timedelta(days=k * step)

    # --- HTTP ---
    def http(self, method, url, headers, body, timeout):
        u = up.urlparse(url)
        q = {k: v[-1] for k, v in up.parse_qs(u.query, keep_blank_values=True).items()}
        js = json.loads(body) if body and "json" in headers.get("Content-Type", "") else None
        self.log.append((method, u.path, q, js))
        for f in self.flaky:
            if f[0] == method and f[1] in u.path and f[2]:
                st = f[2].pop(0)
                if st == "net":
                    raise O.NetError("нет связи (тест)")
                return st, {"retry-after": "0"}, json.dumps({"error": {"code": "TooManyRequests", "message": "flaky"}}).encode()
        if u.netloc == "login.microsoftonline.com":
            return self.token(u.path, dict(up.parse_qsl(body.decode())))
        if headers.get("Authorization", "")[7:] not in self.valid:
            return 401, {}, b'{"error":{"code":"InvalidAuthenticationToken","message":"Access token has expired"}}'
        st, d = self.route(method, u.path, q, js, headers, u)
        return st, {}, json.dumps(d, ensure_ascii=False).encode("utf-8") if d is not None else b""

    def token(self, path, f):
        if path.endswith("/devicecode"):
            return 200, {}, json.dumps({"device_code": "DC", "user_code": "ABCD", "verification_uri": "https://microsoft.com/devicelogin", "expires_in": 900, "interval": 1,
                                        "message": "Откройте https://microsoft.com/devicelogin и введите код ABCD"}).encode()
        g = f["grant_type"]
        if g == "authorization_code":
            if f["code"] != "abc" or b64sha(f["code_verifier"]) != self.challenge:
                return 400, {}, b'{"error":"invalid_grant","error_description":"bad code or PKCE"}'
        elif g.endswith("device_code"):
            self.dev_polls += 1
            if self.dev_polls < 3:
                return 400, {}, b'{"error":"authorization_pending"}'
        elif f["refresh_token"] != "RT1" or self.revoked or "Calendars.ReadWrite" not in f.get("scope", ""):
            return 400, {}, b'{"error":"invalid_grant","error_description":"AADSTS700082: The refresh token has expired due to inactivity."}'
        else:
            self.at += 1
            self.valid.add("AT%d" % self.at)
            return 200, {}, json.dumps({"access_token": "AT%d" % self.at, "refresh_token": "RT2", "expires_in": 3600}).encode()
        self.valid = {"AT1"}
        return 200, {}, json.dumps({"access_token": "AT1", "refresh_token": "RT1", "expires_in": 3600}).encode()

    def route(self, method, path, q, body, hd, u=None):
        path = up.unquote(path).replace("/v1.0", "", 1)
        pref = re.search(r'outlook\.timezone="([^"]+)"', hd.get("Prefer", ""))
        ptz = pref.group(1) if pref else None
        if path == "/$batch":
            rs = []
            for r in body["requests"]:
                if self.batch_flaky and r["id"] == "0":
                    self.batch_flaky -= 1
                    rs.append({"id": r["id"], "status": 429, "body": {"error": {"message": "busy"}}})
                    continue
                uu = up.urlparse(r["url"])
                qq = {k: v[-1] for k, v in up.parse_qs(uu.query).items()}
                st, d = self.route(r["method"], uu.path, qq, None, dict(hd, **r.get("headers", {})), uu)
                rs.append({"id": r["id"], "status": st, "body": d})
            return 200, {"responses": rs}
        if path == "/me":
            return 200, {"mail": "me@outlook.com", "displayName": "Me"}
        if path == "/me/calendars":
            return 200, {"value": [{"id": "CAL1", "name": "Календарь", "canEdit": True, "isDefaultCalendar": True}, {"id": "CAL2", "name": "Семья", "canEdit": True},
                                   {"id": "CAL3", "name": "Чужой", "canEdit": False}]}
        m = re.match(r"/me/calendars/[^/]+/events$", path)
        if m and method == "GET":
            return self.list(q, ptz)
        if m and method == "POST":
            return self.create(body, ptz)
        m = re.match(r"/me/events/([^/]+)(/instances)?$", path)
        if not m:
            return 404, {"error": {"message": "no route " + path}}
        eid, inst = m.group(1), m.group(2)
        if inst:
            return self.instances(eid, q, ptz)
        if eid.startswith("OCC:") and method in ("PATCH", "DELETE"):
            mid, day = eid.split(":")[1:]
            mm = self.ev.get(mid)
            d = dt.datetime.strptime(day, "%Y%m%d").date()
            if method == "DELETE":
                mm.setdefault("_cancelled", []).append("OID.%s.%s" % (mid, d.isoformat()))
                self.touch_master(mm)
                return 204, None
            x = self.add("X-" + day, tz=mm["_tz"], start=dt.datetime.combine(d, mm["_s"].time()), end=dt.datetime.combine(d, mm["_e"].time()), type="exception", seriesMasterId=mid,
                         subject=mm["subject"], _orig=dt.datetime.combine(d, mm["_s"].time()), iCalUId="uid-x-" + day)
            eid = x["id"]
        e = self.ev.get(eid)
        if e is None:
            return 404, {"error": {"code": "ErrorItemNotFound", "message": "The specified object was not found in the store."}}
        if method == "GET":
            return 200, self.render(e, ptz, q.get("$select"), "exceptionOccurrences" in q.get("$expand", ""))
        if method == "DELETE":
            for x in [x for x in self.ev.values() if x.get("seriesMasterId") == eid]:
                del self.ev[x["id"]]
            del self.ev[eid]
            return 204, None
        if method == "PATCH":
            self.apply(e, body)
            self.touch(e)
            return 200, self.render(e, ptz)
        return 405, {"error": {"message": "method"}}

    def apply(self, e, body):
        from zoneinfo import ZoneInfo
        for k, v in body.items():
            if k in ("start", "end"):
                tz = M.WIN2IANA.get(v["timeZone"], v["timeZone"])
                d = dt.datetime.fromisoformat(v["dateTime"])
                if body.get("isAllDay", e["isAllDay"]):
                    e["_" + k[0]] = d
                elif tz == e["_tz"] or e["type"] == "seriesMaster":
                    e["_tz"], e["_" + k[0]] = tz, d
                else:
                    e["_" + k[0]] = d.replace(tzinfo=ZoneInfo(tz)).astimezone(ZoneInfo(e["_tz"])).replace(tzinfo=None)
            elif k != "transactionId":
                e[k] = v

    def create(self, body, ptz):
        for x in self.ev.values():
            if body.get("transactionId") and x.get("_tx") == body["transactionId"]:
                return 201, self.render(x, ptz)
        self.cn = getattr(self, "cn", 0) + 1
        tz = M.WIN2IANA.get(body["start"]["timeZone"], body["start"]["timeZone"])
        s, en = dt.datetime.fromisoformat(body["start"]["dateTime"]), dt.datetime.fromisoformat(body["end"]["dateTime"])
        kw = {k: v for k, v in body.items() if k not in ("start", "end", "transactionId")}
        kw["type"] = "seriesMaster" if body.get("recurrence") else "singleInstance"
        e = self.add("NEW%d" % self.cn, tz=tz, start=s, end=en, **kw)
        e["_tx"] = body.get("transactionId")
        return 201, self.render(e, ptz)

    def list(self, q, ptz):
        if "$filter" in q:
            uid = re.search(r"iCalUId eq '(.*)'", q["$filter"]).group(1)
            items = [e for e in self.ev.values() if e["iCalUId"] == uid and e["type"] != "exception"]
        else:
            items = [e for e in self.ev.values() if e["type"] in ("singleInstance", "seriesMaster")]
        off = int(q.get("$skiptoken", 0))
        n = min(int(q.get("$top", 10)), self.page)
        d = {"value": [self.render(e, ptz, q.get("$select")) for e in items[off:off + n]]}
        if off + n < len(items):
            nq = dict(q, **{"$skiptoken": off + n})
            d["@odata.nextLink"] = "https://graph.microsoft.com/v1.0/me/calendars/CAL1/events?" + up.urlencode(nq)
        return 200, d

    def instances(self, mid, q, ptz):
        m = self.ev.get(mid)
        if m is None:
            return 404, {"error": {"message": "Not Found"}}
        lo = dt.datetime.fromisoformat(q["startDateTime"].replace("Z", "+00:00"))
        hi = dt.datetime.fromisoformat(q["endDateTime"].replace("Z", "+00:00"))
        from zoneinfo import ZoneInfo
        items = []
        for s, en in self.occurrences(m):
            su = s.replace(tzinfo=ZoneInfo(m["_tz"])).astimezone(dt.timezone.utc)
            if not (lo <= su < hi):
                continue
            day = s.strftime("%Y%m%d")
            if "OID.%s.%s" % (mid, s.date().isoformat()) in m.get("_cancelled", []):
                continue
            x = next((x for x in self.ev.values() if x.get("seriesMasterId") == mid and x["type"] == "exception" and x["_orig"] == s), None)
            if x:
                items.append(self.render(x, ptz))
                continue
            o = {"id": "OCC:%s:%s" % (mid, day), "type": "occurrence", "subject": m["subject"], "isAllDay": False, "seriesMasterId": mid,
                 "start": self.render(m, ptz)["start"], "end": self.render(m, ptz)["end"]}
            o["start"], o["end"] = [{"dateTime": (v.replace(tzinfo=ZoneInfo(m["_tz"])).astimezone(ZoneInfo(M.WIN2IANA.get(ptz, ptz or "UTC")))).strftime("%Y-%m-%dT%H:%M:%S.0000000"),
                                     "timeZone": ptz or "UTC"} for v in (s, en)]
            items.append(o)
        return 200, {"value": items}


def run(G):
    n = [0]

    def ok(cond, msg):
        n[0] += 1
        if not cond:
            print("FAIL:", msg)
            sys.exit(1)

    tmp = tempfile.mkdtemp(prefix="dkeys_otest_")
    G.INI, G.SBASE = os.path.join(tmp, "outlook.ini"), os.path.join(tmp, "outlook.state")
    fake = Fake()
    O.HTTP, O.SLEEP = fake.http, lambda s: None
    try:
        _run(G, fake, ok, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("selftest OK: %d проверок" % n[0])


def _run(G, fake, ok, tmp):
    import builtins
    import io
    from contextlib import redirect_stderr, redirect_stdout
    HERE = G.HERE
    MIN, NY = "Europe/Minsk", "America/New_York"

    # ---------- повтор Graph <-> RRULE ----------
    S = M.T("T", dt.datetime(2026, 11, 20, 10, 0), MIN)                        # пятница
    rr = lambda t, **kw: G.rec_to_rrule({"pattern": dict(type=t, interval=kw.pop("interval", 1), **kw), "range": kw.pop("range", {"type": "noEnd", "startDate": "2026-11-20"})}, S)  # noqa: E731
    cases = [
        (dict(type="daily", interval=1), {"type": "noEnd"}, "FREQ=DAILY"),
        (dict(type="daily", interval=3), {"type": "numbered", "numberOfOccurrences": 5}, "FREQ=DAILY;INTERVAL=3;COUNT=5"),
        (dict(type="weekly", interval=1, daysOfWeek=["friday"], firstDayOfWeek="sunday"), {"type": "endDate", "endDate": "2026-12-31"}, "FREQ=WEEKLY;UNTIL=20261231;BYDAY=FR"),
        (dict(type="weekly", interval=2, daysOfWeek=["monday", "wednesday"], firstDayOfWeek="monday"), {"type": "noEnd"}, "FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE;WKST=MO"),
        (dict(type="absoluteMonthly", interval=1, dayOfMonth=15), {"type": "noEnd"}, "FREQ=MONTHLY;BYMONTHDAY=15"),
        (dict(type="relativeMonthly", interval=1, daysOfWeek=["tuesday"], index="second"), {"type": "noEnd"}, "FREQ=MONTHLY;BYDAY=2TU"),
        (dict(type="relativeMonthly", interval=2, daysOfWeek=["friday"], index="last"), {"type": "noEnd"}, "FREQ=MONTHLY;INTERVAL=2;BYDAY=-1FR"),
        (dict(type="relativeMonthly", interval=1, daysOfWeek=["monday", "tuesday", "wednesday", "thursday", "friday"], index="first"), {"type": "noEnd"}, "FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=1"),
        (dict(type="absoluteYearly", interval=1, month=3, dayOfMonth=8), {"type": "noEnd"}, "FREQ=YEARLY;BYMONTH=3;BYMONTHDAY=8"),
        (dict(type="relativeYearly", interval=1, month=11, daysOfWeek=["thursday"], index="fourth"), {"type": "noEnd"}, "FREQ=YEARLY;BYMONTH=11;BYDAY=4TH"),
    ]
    for pat, rng, want in cases:
        rec = {"pattern": pat, "range": dict(rng, startDate="2026-11-20")}
        got = G.rec_to_rrule(rec, S)
        ok(M.rr_norm(got) == M.rr_norm(want), "Graph -> RRULE: %s -> %s (ожидалось %s)" % (pat["type"], got, want))
        back = G.rrule_to_rec(got, S, "Belarus Standard Time")
        ok(back is not None and M.rr_norm(G.rec_to_rrule(back, S)) == M.rr_norm(got), "круг RRULE -> Graph -> RRULE: %s" % got)
    ok(G.rec_to_rrule({"pattern": {"type": "weekly", "interval": 1}, "range": {"type": "noEnd"}}, S) == "FREQ=WEEKLY;BYDAY=FR", "weekly без дней - день начала")
    for bad in ("FREQ=WEEKLY;BYWEEKNO=3", "FREQ=DAILY;COUNT=2;UNTIL=20261231", "FREQ=MONTHLY;BYMONTHDAY=1,15", "FREQ=HOURLY", "FREQ=MONTHLY;BYDAY=MO", "FREQ=WEEKLY;BYDAY=1MO", "FREQ=MONTHLY;BYDAY=5MO", "FREQ=YEARLY;BYMONTH=1,2"):
        ok(G.rrule_to_rec(bad, S, "Belarus Standard Time") is None, "не воспроизводится без потерь: %s" % bad)
    r = G.rrule_to_rec("FREQ=WEEKLY;UNTIL=20261231T205959Z;BYDAY=FR", S, "Belarus Standard Time")
    ok(r["range"]["endDate"] == "2026-12-31" and r["range"]["recurrenceTimeZone"] == "Belarus Standard Time" and r["range"]["startDate"] == "2026-11-20", "UNTIL в UTC -> локальная дата конца")
    ok(G.text_of({"contentType": "html", "content": "<html><head><style>p{}</style></head><body><p>Привет &amp; пока</p><br>Строка 2</body></html>"}) == "Привет & пока\n\nСтрока 2", "HTML-описание -> текст")

    # ---------- setup ----------
    def opener(url):
        q = dict(up.parse_qsl(up.urlparse(url).query))
        fake.challenge = q["code_challenge"]
        ok(q["code_challenge_method"] == "S256" and "Calendars.ReadWrite" in q["scope"] and "offline_access" in q["scope"] and q["redirect_uri"].startswith("http://localhost:"), "запрос входа: PKCE, область, loopback")
        go = lambda: urllib.request.urlopen("http://127.0.0.1:%s/?code=abc&state=%s" % (q["redirect_uri"].rsplit(":", 1)[1], q["state"]), timeout=10).read()  # noqa: E731
        threading.Thread(target=go, daemon=True).start()
    O.OPEN = opener
    answers = iter(["my-app-id", "", "1,2"])
    builtins.input = lambda p="": next(answers)
    out = io.StringIO()
    with redirect_stdout(out):
        G.cmd_setup({"profile": "", "since": "", "dir": ""})
    ok("RT1" not in out.getvalue() and "AT1" not in out.getvalue(), "секреты не печатаются")
    cp = O.read_ini(G.INI)
    ok(sorted(cp.sections()) == ["me", "semya"] and cp["me"]["calendar"] == "CAL1" and cp["me"]["account"] == cp["semya"]["account"], "профили: %s" % cp.sections())
    acct = cp["me"]["account"]
    tokp = os.path.join(G.SBASE, acct, "token.json")
    ok(json.load(open(tokp, encoding="utf-8"))["refresh_token"] == "RT1" and json.load(open(os.path.join(G.SBASE, "client.json")))["client_id"] == "my-app-id", "токен и client_id сохранены")
    # вход по коду устройства
    os.remove(G.INI)
    answers = iter(["d", "3"])
    out = io.StringIO()
    with redirect_stdout(out):
        G.cmd_setup({"profile": "", "since": "", "dir": ""})
    cp = O.read_ini(G.INI)
    ok("ABCD" in out.getvalue() and fake.dev_polls >= 3 and cp.sections() == ["chuzhoy"] and cp["chuzhoy"]["readonly"] == "1", "код устройства: pending -> успех; чужой календарь readonly")
    os.remove(G.INI)
    cp = O.read_ini(G.INI)
    for name, cid in (("me", "CAL1"), ("ro", "CAL3")):
        cp.add_section(name)
        cp.set(name, "account", acct)
        cp.set(name, "calendar", cid)
    cp.set("ro", "readonly", "1")
    O.write_ini(cp, G.INI)

    def run_cmd(fn, opts, stdin=""):
        buf, old = io.StringIO(), sys.stdin
        sys.stdin = io.StringIO(stdin)
        try:
            with redirect_stdout(buf):
                getattr(G, fn)(opts)
        finally:
            sys.stdin = old
        return buf.getvalue()

    pull = lambda since="", prof="me": run_cmd("cmd_pull", {"profile": prof, "since": since, "dir": ""})  # noqa: E731
    push = lambda text, prof="me": run_cmd("cmd_push", {"profile": prof, "since": "", "dir": ""}, text)  # noqa: E731

    def cursor(t):
        m = re.search(r"\nX-DKEYS-CURSOR:(\S*)", t)
        return m.group(1) if m else None

    d_ = lambda *a: dt.datetime(*a)  # noqa: E731
    # ---------- данные «на сервере» ----------
    fake.add("a1", subject="Встреча, важная", body={"contentType": "text", "content": "Строка 1\r\nСтрока 2\r\n"}, location={"displayName": "Офис; 5 этаж"}, start=d_(2026, 11, 20, 10), end=d_(2026, 11, 20, 11),
             isReminderOn=True, reminderMinutesBeforeStart=15, showAs="free", sensitivity="private", categories=["Красная"],
             attendees=[{"emailAddress": {"name": "Анна", "address": "a@x.com"}, "type": "required", "status": {"response": "accepted"}},
                        {"emailAddress": {"name": "Боб", "address": "b@x.com"}, "type": "optional", "status": {"response": "none"}}],
             organizer={"emailAddress": {"name": "Босс", "address": "boss@x.com"}}, onlineMeeting={"joinUrl": "https://teams.microsoft.com/l/meetup-join/xyz"})
    fake.add("a2", subject="Праздник", isAllDay=True, start=d_(2026, 11, 21), end=d_(2026, 11, 22))
    fake.add("a3", subject="Звонок NY", tz=NY, start=d_(2026, 11, 23, 9), end=d_(2026, 11, 23, 10), body={"contentType": "html", "content": "<p>Привет</p>"})
    fake.add("a4", subject="UTC", tz="UTC", start=d_(2026, 11, 24, 12), end=d_(2026, 11, 24, 13, 30))
    fake.add("s1", subject="Планёрка", type="seriesMaster", start=d_(2026, 11, 20, 10), end=d_(2026, 11, 20, 10, 30), isReminderOn=True, reminderMinutesBeforeStart=5,
             recurrence={"pattern": {"type": "weekly", "interval": 1, "daysOfWeek": ["friday"], "firstDayOfWeek": "monday"}, "range": {"type": "noEnd", "startDate": "2026-11-20", "recurrenceTimeZone": "Belarus Standard Time"}})
    fake.ev["s1"]["_cancelled"] = ["OID.s1.2026-12-04"]
    fake.add("x1", subject="Планёрка (перенос)", type="exception", seriesMasterId="s1", iCalUId="uid-x1", _orig=d_(2026, 11, 27, 10), start=d_(2026, 11, 27, 11), end=d_(2026, 11, 27, 11, 30))
    fake.add("s2", subject="Раз в месяц", type="seriesMaster", start=d_(2026, 11, 10, 9), end=d_(2026, 11, 10, 10), tz=NY,
             recurrence={"pattern": {"type": "relativeMonthly", "interval": 1, "daysOfWeek": ["tuesday"], "index": "second"}, "range": {"type": "endDate", "startDate": "2026-11-10", "endDate": "2027-06-30", "recurrenceTimeZone": "Eastern Standard Time"}})
    fake.add("s3", subject="Странный повтор", type="seriesMaster", start=d_(2026, 11, 20, 8), end=d_(2026, 11, 20, 9),
             recurrence={"pattern": {"type": "someFutureType", "interval": 1}, "range": {"type": "noEnd", "startDate": "2026-11-20", "recurrenceTimeZone": "Belarus Standard Time"}})

    # ---------- полная выгрузка ----------
    fake.log.clear()
    t = pull("")
    evs = ev_lines(t)
    ok("X-DKEYS-FULL:1" in t and cursor(t) == "g1", "полная выгрузка: признак FULL, курсор g1")
    ok(sum(1 for c in fake.log if c[0] == "GET" and c[1].endswith("/events")) >= 3, "пагинация @odata.nextLink")
    ok(any(c[1].endswith("/$batch") for c in fake.log), "изменённое выбирается через $batch")
    ok(uids(t) == ["uid-a1", "uid-a2", "uid-a3", "uid-a4", "uid-s1", "uid-s1", "uid-s2", "uid-s3"], "события и серия с заменой: %s" % uids(t))
    ok(all(D.get(e, "X-DKEYS-ETAG") for e in evs), "у каждого события есть etag")
    a1 = next(e for e in evs if D.uid_of(e) == "uid-a1")
    ok("DTSTART;TZID=Europe/Minsk:20261120T100000" in a1 and "DTEND;TZID=Europe/Minsk:20261120T110000" in a1, "время в поясе создания")
    ok("DESCRIPTION:Строка 1\\nСтрока 2" in a1 and "LOCATION:Офис\\; 5 этаж" in a1 and "SUMMARY:Встреча\\, важная" in a1, "описание (текст) и экранирование")
    ok(M.parse_alarms(a1) == [("popup", 15)] and "TRANSP:TRANSPARENT" in a1 and "CLASS:PRIVATE" in a1, "напоминание, занятость, приватность")
    ok(len(lines(a1, "ATTENDEE")) == 2 and "PARTSTAT=ACCEPTED" in lines(a1, "ATTENDEE")[0] and "ROLE=OPT-PARTICIPANT" in lines(a1, "ATTENDEE")[1] and lines(a1, "ORGANIZER"), "участники и организатор")
    ok("CATEGORIES:Красная" in a1 and "X-MICROSOFT-SKYPETEAMSMEETINGURL:https://teams.microsoft.com/l/meetup-join/xyz" in a1, "категории и ссылка на онлайн-встречу")
    a2 = next(e for e in evs if D.uid_of(e) == "uid-a2")
    ok("DTSTART;VALUE=DATE:20261121" in a2 and "DTEND;VALUE=DATE:20261122" in a2, "целодневное")
    a3 = next(e for e in evs if D.uid_of(e) == "uid-a3")
    ok("DTSTART;TZID=America/New_York:20261123T090000" in a3 and "DESCRIPTION:Привет" in a3, "пояс New York; HTML-описание")
    a4 = next(e for e in evs if D.uid_of(e) == "uid-a4")
    ok("DTSTART:20261124T120000Z" in a4, "UTC")
    s1 = [e for e in evs if D.uid_of(e) == "uid-s1"]
    sm, sx = next(e for e in s1 if D.rec_of(e) is None), next(e for e in s1 if D.rec_of(e) is not None)
    ok("RRULE:FREQ=WEEKLY;BYDAY=FR" in sm and "DTSTART;TZID=Europe/Minsk:20261120T100000" in sm, "серия: RRULE и локальное время пояса серии")
    ok(lines(sm, "EXDATE") == ["EXDATE;TZID=Europe/Minsk:20261204T100000"], "отменённое вхождение -> EXDATE: %s" % lines(sm, "EXDATE"))
    ok("RECURRENCE-ID;TZID=Europe/Minsk:20261127T100000" in sx and "DTSTART;TZID=Europe/Minsk:20261127T110000" in sx, "замена: RECURRENCE-ID (исходное начало) и новое время")
    ok(D.get(sm, "X-DKEYS-ETAG") == D.get(sx, "X-DKEYS-ETAG") and "." in D.get(sm, "X-DKEYS-ETAG"), "серия: общий etag")
    s_etag1 = D.get(sm, "X-DKEYS-ETAG")
    s2 = next(e for e in evs if D.uid_of(e) == "uid-s2")
    ok("RRULE:FREQ=MONTHLY;BYDAY=2TU;UNTIL=20270630" in s2 or "RRULE:FREQ=MONTHLY;UNTIL=20270630;BYDAY=2TU" in s2, "месячный повтор по дню недели: %s" % lines(s2, "RRULE"))
    s3 = next(e for e in evs if D.uid_of(e) == "uid-s3")
    ok("X-OUTLOOK-READONLY:1" in s3 and not lines(s3, "RRULE"), "неизвестный повтор: серия помечена только для чтения")
    cur1 = cursor(t)

    # ---------- изменения по changeKey ----------
    t2 = pull(cur1)
    c2 = cursor(t2)
    ok(uids(t2) == [] and "X-DKEYS-FULL" not in t2 and c2 == "g2", "без изменений - пусто")
    fake.ev["a1"]["subject"] = "Встреча 2"
    fake.touch(fake.ev["a1"])
    fake.ev["x1"]["subject"] = "Планёрка (перенос 2)"
    fake.touch(fake.ev["x1"])                                           # меняется только замена (главное получает новый changeKey)
    del fake.ev["a2"]
    fake.add("a5", subject="Новое", start=d_(2026, 11, 26, 8), end=d_(2026, 11, 26, 9))
    fake.log.clear()
    t3 = pull(c2)
    ok(uids(t3) == ["uid-a1", "uid-a2", "uid-a5", "uid-s1", "uid-s1"], "изменённое, новое, удалённое, серия целиком: %s" % uids(t3))
    ok(any(D.uid_of(e) == "uid-a2" and D.get(e, "X-DKEYS-DELETED") == "1" for e in ev_lines(t3)), "удалённое - маркер по iCalUId")
    sm = next(e for e in ev_lines(t3) if D.uid_of(e) == "uid-s1" and D.rec_of(e) is None)
    ok(D.get(sm, "X-DKEYS-ETAG") != s_etag1 and "перенос 2" in t3, "серия: замена изменилась - etag другой")
    t3b = pull(c2)                                                    # клиент не применил g4: те же изменения заново
    ok(uids(t3b) == uids(t3), "не применённый ответ отдаётся заново")
    t4 = pull("unknown")
    ok("X-DKEYS-FULL:1" in t4 and "uid-a2" not in uids(t4), "незнакомый курсор: полная выгрузка")
    # много изменённых: постраничный список вместо $batch
    c4 = cursor(t4)
    for i in range(30):
        fake.add("m%d" % i, subject="M%d" % i, start=d_(2026, 12, 1, 8) + dt.timedelta(days=i % 20), end=d_(2026, 12, 1, 9) + dt.timedelta(days=i % 20))
    fake.log.clear()
    t6 = pull(c4)
    ok(len(uids(t6)) == 30 and not any(c[1].endswith("/$batch") and any("/me/events/m" in r["url"] for r in c[3]["requests"]) for c in fake.log if c[3]), "много изменений: список страницами, без $batch для событий")
    # частичный сбой $batch (429 на одном запросе) - повтор
    fake.ev["a4"]["subject"] = "UTC 2"
    fake.touch(fake.ev["a4"])
    fake.batch_flaky = 1
    t7 = pull(cursor(t6))
    ok(uids(t7) == ["uid-a4"] and fake.batch_flaky == 0, "429 внутри $batch: повтор")
    cur = cursor(t7)

    # ---------- push ----------
    def mk(uid, summ, op, etag="", tz=MIN, start="20261130T100000", end="20261130T110000", extra=()):
        z = ";TZID=" + tz if tz != "UTC" else ""
        zz = "Z" if tz == "UTC" else ""
        return (["BEGIN:VEVENT", "UID:" + uid, "SUMMARY:" + summ, "DTSTART%s:%s%s" % (z, start, zz), "DTEND%s:%s%s" % (z, end, zz), "LAST-MODIFIED:20261116T100000Z", "X-DKEYS-OP:" + op]
                + ([("X-DKEYS-ETAG:" + etag)] if etag else []) + list(extra) + ["END:VEVENT"])

    vc = lambda *evs: D.vcalendar(list(evs))  # noqa: E731
    alarm = lambda m: ["BEGIN:VALARM", "ACTION:DISPLAY", "DESCRIPTION:x", "TRIGGER:-PT%dM" % m, "END:VALARM"]  # noqa: E731
    fake.log.clear()
    r = ev_lines(push(vc(mk("local-1", "Новое локальное", "create", extra=alarm(10) + alarm(30)))))
    post = next(c for c in fake.log if c[0] == "POST" and c[1].endswith("/events"))[3]
    ok(len(r) == 1 and D.get(r[0], "X-DKEYS-REF") == "local-1" and D.uid_of(r[0]) == "uid-NEW1", "create: UID сервера, REF на прежний")
    ok(post["start"] == {"dateTime": "2026-11-30T07:00:00", "timeZone": "UTC"} and post["isReminderOn"] and post["reminderMinutesBeforeStart"] == 30 and post["transactionId"], "одиночное: время в UTC, одно напоминание (самое раннее), transactionId")
    ok(D.get(r[0], "X-DKEYS-ETAG") == fake.ev["NEW1"]["changeKey"], "в ответе etag сервера")
    r = ev_lines(push(vc(mk("local-1", "Новое локальное", "create", extra=alarm(10)))))
    ok(sum(1 for e in fake.ev.values() if e.get("subject") == "Новое локальное") == 1, "повторное create (transactionId) без дубля")
    uid, et = D.uid_of(r[0]), D.get(r[0], "X-DKEYS-ETAG")
    # правка: только изменившееся; два напоминания равны одному серверному
    fake.log.clear()
    r = ev_lines(push(vc(mk(uid, "Переименованное", "update", et, extra=alarm(10) + alarm(30)))))
    pt = [c for c in fake.log if c[0] == "PATCH"]
    ok(len(pt) == 1 and set(pt[0][3]) == {"subject"}, "update: PATCH только subject: %s" % [c[3] for c in pt])
    et2 = D.get(r[0], "X-DKEYS-ETAG")
    ok(et2 != et and fake.ev["NEW1"]["subject"] == "Переименованное", "update применён, etag новый")
    fake.log.clear()
    push(vc(mk(uid, "Переименованное", "update", et2, extra=alarm(30))))
    ok(not any(c[0] == "PATCH" for c in fake.log), "без изменений - записи нет")
    fake.log.clear()
    push(vc(mk(uid, "Переименованное", "update", et2, start="20261130T120000", end="20261130T130000", extra=alarm(30))))
    pt = [c for c in fake.log if c[0] == "PATCH"]
    ok(len(pt) == 1 and set(pt[0][3]) == {"isAllDay", "start", "end"} and pt[0][3]["start"] == {"dateTime": "2026-11-30T09:00:00", "timeZone": "UTC"}, "update: время одиночного в UTC")
    # участники и категории не затрагиваются
    fake.log.clear()
    ics = next(e for e in ev_lines(pull("")) if D.uid_of(e) == "uid-a1")
    push(vc(D.put(D.put(ics, "SUMMARY", "Встреча 3"), "X-DKEYS-OP", "update")))
    ok(len(fake.ev["a1"]["attendees"]) == 2 and fake.ev["a1"]["categories"] == ["Красная"] and fake.ev["a1"]["subject"] == "Встреча 3", "участники и категории не затронуты")
    ok(not any(set(c[3]) & {"attendees", "categories", "organizer", "onlineMeeting"} for c in fake.log if c[0] == "PATCH"), "PATCH без чужих полей")
    # конфликты
    fake.log.clear()
    r = ev_lines(push(vc(mk(uid, "Старый", "update", et))))
    ok(D.get(r[0], "X-DKEYS-ERROR") == "conflict" and not any(c[0] == "PATCH" for c in fake.log), "устаревший etag: conflict без записи")
    r = ev_lines(push(vc(mk("uid-nope", "Нет", "update", "x"))))
    ok(D.get(r[0], "X-DKEYS-ERROR") == "not found", "update несуществующего: not found")
    # серия: создание с заменой
    ser = ["RRULE:FREQ=WEEKLY;BYDAY=MO"]
    m1 = mk("local-s", "Еженедельная", "create", start="20261123T100000", end="20261123T110000", extra=ser)
    m2 = mk("local-s", "Перенесённая", "create", start="20261130T150000", end="20261130T160000", extra=["RECURRENCE-ID;TZID=Europe/Minsk:20261130T100000"])
    fake.log.clear()
    r = ev_lines(push(vc(m1, m2)))
    post = next(c for c in fake.log if c[0] == "POST" and c[1].endswith("/events"))[3]
    ok(post["start"]["timeZone"] == "Belarus Standard Time" and post["start"]["dateTime"] == "2026-11-23T10:00:00" and post["recurrence"]["pattern"]["daysOfWeek"] == ["monday"]
       and post["recurrence"]["range"]["recurrenceTimeZone"] == "Belarus Standard Time", "серия: пояс Windows, шаблон и диапазон")
    xs = [e for e in fake.ev.values() if e.get("type") == "exception" and e.get("seriesMasterId") == "NEW2"]
    ok(len(r) == 2 and all(D.get(e, "X-DKEYS-ERROR") is None for e in r) and len(xs) == 1 and xs[0]["subject"] == "Перенесённая" and xs[0]["_s"].hour == 15, "замена применена к вхождению серии")
    new_s = D.uid_of(r[0])
    # удаление вхождения = EXDATE: отмена вхождения
    cur_ics = ev_lines(pull(""))
    sm = next(e for e in cur_ics if D.uid_of(e) == new_s and D.rec_of(e) is None)
    sm2 = D.put(D.drop(sm, "X-DKEYS-OP"), "X-DKEYS-OP", "update")
    sm2 = sm2[:-1] + ["EXDATE;TZID=Europe/Minsk:20261207T100000", sm2[-1]]
    fake.log.clear()
    r = ev_lines(push(vc(sm2, *[D.put(x, "X-DKEYS-OP", "update") for x in cur_ics if D.uid_of(x) == new_s and D.rec_of(x) is not None])))
    ok(D.get(r[0], "X-DKEYS-ERROR") is None and "OID.NEW2.2026-12-07" in fake.ev["NEW2"].get("_cancelled", []) and not any(c[0] == "PATCH" and "NEW2" in c[1] for c in fake.log), "отмена вхождения без правки серии: %s" % r)
    # вернуть отменённое нельзя
    cur_ics = ev_lines(pull(""))
    sm = next(e for e in cur_ics if D.uid_of(e) == new_s and D.rec_of(e) is None)
    r = ev_lines(push(vc(D.put(D.drop(D.drop(sm, "EXDATE"), "X-DKEYS-OP"), "X-DKEYS-OP", "update"))))
    ok("нельзя из DKeys" in (D.get(r[0], "X-DKEYS-ERROR") or ""), "возврат отменённого вхождения: понятная ошибка")
    # правка времени серии: recurrence и пояс серии
    cur_ics = ev_lines(pull(""))
    sm = next(e for e in cur_ics if D.uid_of(e) == new_s and D.rec_of(e) is None)
    sm3 = D.put(sm, "X-DKEYS-OP", "update")
    sm3 = [("DTSTART;TZID=Europe/Minsk:20261123T110000" if ln.startswith("DTSTART") else "DTEND;TZID=Europe/Minsk:20261123T120000" if ln.startswith("DTEND") else ln) for ln in sm3]
    fake.log.clear()
    r = ev_lines(push(vc(sm3)))
    pt = [c for c in fake.log if c[0] == "PATCH" and "NEW2" in c[1]]
    ok(len(pt) == 1 and pt[0][3]["start"] == {"dateTime": "2026-11-23T11:00:00", "timeZone": "Belarus Standard Time"} and "recurrence" in pt[0][3], "серия: новое время в поясе серии вместе с recurrence: %s" % [c[3] for c in pt])
    # серия только для чтения и повтор, который не записать
    s3 = next(e for e in ev_lines(pull("")) if D.uid_of(e) == "uid-s3")
    r = ev_lines(push(vc(D.put(D.put(s3, "SUMMARY", "Правка"), "X-DKEYS-OP", "update"))))
    ok(D.get(r[0], "X-DKEYS-ERROR") is None and fake.ev["s3"]["subject"] == "Правка", "серия только для чтения: правка текста допустима")
    s3u = D.put(D.put(s3, "X-DKEYS-OP", "update"), "X-DKEYS-ETAG", fake.ev["s3"]["changeKey"])
    r = ev_lines(push(vc([("DTSTART;TZID=Europe/Minsk:20261120T090000" if ln.startswith("DTSTART") else ln) for ln in s3u])))
    ok("только для чтения" in (D.get(r[0], "X-DKEYS-ERROR") or ""), "серия только для чтения: правка времени отклонена")
    r = ev_lines(push(vc(mk("local-bad", "Плохой повтор", "create", extra=["RRULE:FREQ=WEEKLY;BYWEEKNO=3"]))))
    ok("без потерь" in (D.get(r[0], "X-DKEYS-ERROR") or "") and not any(e.get("subject") == "Плохой повтор" for e in fake.ev.values()), "создание с неточным RRULE: отказ, ничего не создано")
    # удаление
    r = ev_lines(push(vc(mk(uid, "x", "delete"))))
    ok(D.get(r[0], "X-DKEYS-DELETED") == "1" and "NEW1" not in fake.ev, "delete")
    r = ev_lines(push(vc(mk(uid, "x", "delete"))))
    ok(D.get(r[0], "X-DKEYS-DELETED") == "1" and D.get(r[0], "X-DKEYS-ERROR") is None, "повторное delete - не ошибка")
    ok(D.get(ev_lines(push(vc(mk("never", "x", "delete"))))[0], "X-DKEYS-DELETED") == "1", "delete неизвестного")
    r = ev_lines(push(vc(mk("ro-1", "x", "create")), prof="ro"))
    ok("только для чтения" in (D.get(r[0], "X-DKEYS-ERROR") or ""), "readonly: правки не отправляются")

    # ---------- сбои, токены ----------
    fake.flaky = [("GET", "/events", [429, 503])]
    t = pull("")
    ok("X-DKEYS-FULL:1" in t, "429 и 503: повтор и успех")
    fake.flaky = [("PATCH", "/me/events", [500, 500, 500, 500])]
    fake.ev["a4"]["subject"] = "UTC 2"
    a4 = next(e for e in ev_lines(pull("")) if D.uid_of(e) == "uid-a4")
    r = ev_lines(push(vc(D.put(D.put(a4, "SUMMARY", "Упорный сбой"), "X-DKEYS-OP", "update"))))
    ok("Outlook 500" in (D.get(r[0], "X-DKEYS-ERROR") or ""), "постоянный 5xx: ошибка на событии")
    fake.flaky = [("GET", "/events", ["net"] * 4)]
    try:
        pull("")
        ok(False, "сеть недоступна: pull должен упасть")
    except O.NetError:
        ok(True, "")
    fake.flaky = []
    fake.valid = set()
    t = pull("")
    ok("X-DKEYS-FULL:1" in t and json.load(open(tokp, encoding="utf-8"))["refresh_token"] == "RT2", "401: токен обновлён, новый refresh-токен сохранён")
    O.write_secret(tokp, {"refresh_token": "RT1", "access_token": "x", "expires_at": 0})
    fake.revoked = True
    try:
        pull("")
        ok(False, "отозванный вход")
    except O.AuthError as e:
        ok("Подключить" in str(e) and "invalid_grant" in str(e), "отозванный вход: подсказка про «Подключить…»")
    fake.revoked = False
    os.remove(tokp)
    try:
        with redirect_stderr(io.StringIO()) as se:
            pull("")
        ok(False, "без токена pull должен завершиться")
    except SystemExit:
        ok("Подключить" in se.getvalue(), "нет токена: подсказка про «Подключить…»")
    import subprocess
    r = subprocess.run([sys.executable, os.path.join(HERE, "outlook.py"), "info"], capture_output=True, text=True, encoding="utf-8", env=dict(os.environ, PYTHONIOENCODING="utf-8"), timeout=30)
    j = json.loads(r.stdout.strip().splitlines()[-1])
    ok(r.returncode == 0 and j["contract"] == 1 and j["create"] and j["update"] and j["delete"] and "Outlook" in j["name"], "info: имя и возможности")
