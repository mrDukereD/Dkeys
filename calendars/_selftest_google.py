# -*- coding: utf-8 -*-
"""Проверка google.py без сети и аккаунтов (python google.py --selftest): поддельный сервер Google Calendar подставляется вместо HTTP.
Не источник (имя на «_»). Что проверяется: вход (PKCE, ключи, профили), полная и инкрементальная выгрузка (пагинация, syncToken, 410, серии с заменами
и отменами, целодневные, разные пояса, удаления), отправка (создание идемпотентно, правка только изменённого, etag/конфликт, 412, удаление),
повторы при 429/5xx, обновление токена, отказ при отозванном входе, права файла токена."""
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
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


class Fake(object):
    def __init__(self):
        self.ev, self.rev, self.log, self.page = {}, 0, [], 2
        self.valid, self.at, self.revoked, self.flaky, self.race, self.challenge = {"AT1"}, 1, False, [], None, None
        self.defrem = [{"method": "popup", "minutes": 30}]

    # --- хранилище ---
    def touch(self, e):
        self.rev += 1
        e["etag"] = '"%d"' % (self.rev * 1000)
        e["_rev"] = self.rev
        e["updated"] = "2026-11-%02dT10:%02d:00.000Z" % (10 + self.rev // 60, self.rev % 60)

    def norm(self, e):                                            # как Google: время с пояса приходит со смещением
        for k in ("start", "end", "originalStartTime"):
            o = e.get(k)
            if o and "dateTime" in o and o.get("timeZone") and not re.search(r"(Z|[+-]\d\d:\d\d)$", o["dateTime"]):
                from zoneinfo import ZoneInfo
                o["dateTime"] = dt.datetime.fromisoformat(o["dateTime"]).replace(tzinfo=ZoneInfo(o["timeZone"])).isoformat()

    def add(self, **e):
        e.setdefault("status", "confirmed")
        e.setdefault("iCalUID", e["id"] + "@google.com")
        self.norm(e)
        self.touch(e)
        self.ev[e["id"]] = e
        return e

    def occurrences(self, m):
        s, en = dt.datetime.fromisoformat(m["start"]["dateTime"]), dt.datetime.fromisoformat(m["end"]["dateTime"])
        step = 7 if "WEEKLY" in "".join(m.get("recurrence", [])) else 1
        for k in range(60):
            yield s + dt.timedelta(days=k * step), en + dt.timedelta(days=k * step)

    def instance(self, m, eid, window=None):
        for s, en in self.occurrences(m):
            su = s.astimezone(dt.timezone.utc)
            if eid is not None and eid != m["id"] + "_" + su.strftime("%Y%m%dT%H%M%SZ"):
                continue
            if window and not (su < window[1] and en.astimezone(dt.timezone.utc) > window[0]):
                continue
            tz = m["start"].get("timeZone")
            e = {"id": m["id"] + "_" + su.strftime("%Y%m%dT%H%M%SZ"), "recurringEventId": m["id"], "status": "confirmed", "iCalUID": m["iCalUID"], "summary": m.get("summary", ""),
                 "originalStartTime": {"dateTime": s.isoformat(), "timeZone": tz}, "start": {"dateTime": s.isoformat(), "timeZone": tz}, "end": {"dateTime": en.isoformat(), "timeZone": tz},
                 "reminders": m.get("reminders", {"useDefault": True})}
            return e
        return None

    def clean(self, e):
        return {k: v for k, v in e.items() if not k.startswith("_")}

    # --- HTTP ---
    def http(self, method, url, headers, body, timeout):
        u = up.urlparse(url)
        q = {k: v[-1] for k, v in up.parse_qs(u.query).items()}
        self.log.append((method, u.path, q, json.loads(body) if body and "json" in headers.get("Content-Type", "") else None))
        for f in self.flaky:
            if f[0] == method and f[1] in u.path and f[2]:
                st = f[2].pop(0)
                if st == "net":
                    raise O.NetError("нет связи (тест)")
                return st, {"retry-after": "0"}, json.dumps({"error": {"code": st, "message": "flaky"}}).encode()
        if u.netloc == "oauth2.googleapis.com":
            return self.token(dict(up.parse_qsl(body.decode())))
        if headers.get("Authorization", "")[7:] not in self.valid:
            return 401, {}, b'{"error":{"code":401,"message":"Invalid Credentials"}}'
        st, d = self.route(method, u.path, q, json.loads(body) if body else None, headers)
        return st, {}, json.dumps(d, ensure_ascii=False).encode("utf-8") if d is not None else b""

    def token(self, f):
        if f["grant_type"] == "authorization_code":
            ch = base64_sha(f["code_verifier"])
            if f["code"] != "abc" or ch != self.challenge:
                return 400, {}, b'{"error":"invalid_grant","error_description":"bad code or PKCE"}'
            self.valid = {"AT1"}
            return 200, {}, json.dumps({"access_token": "AT1", "refresh_token": "RT1", "expires_in": 3600}).encode()
        if f["refresh_token"] != "RT1" or self.revoked:
            return 400, {}, b'{"error":"invalid_grant","error_description":"Token has been expired or revoked."}'
        self.at += 1
        self.valid.add("AT%d" % self.at)
        return 200, {}, json.dumps({"access_token": "AT%d" % self.at, "expires_in": 3600}).encode()

    def route(self, method, path, q, body, hd):
        m = re.match(r"/calendar/v3/users/me/calendarList(?:/(.+))?$", path)
        if m:
            cals = [{"id": "me@gmail.com", "summary": "me@gmail.com", "primary": True, "accessRole": "owner", "defaultReminders": self.defrem},
                    {"id": "work@group.calendar.google.com", "summary": "Работа", "accessRole": "writer"},
                    {"id": "ru#holiday@group.v.calendar.google.com", "summary": "Праздники", "accessRole": "reader"}]
            return (200, {"items": cals}) if not m.group(1) else (200, cals[0])
        m = re.match(r"/calendar/v3/calendars/[^/]+/events(?:/([^/]+))?(?:/(instances))?$", path)
        if not m:
            return 404, {"error": {"message": "no route " + path}}
        eid, inst = m.group(1), m.group(2)
        if eid and eid.find("%") >= 0:
            eid = up.unquote(eid)
        if method == "GET" and not eid:
            return self.list(q)
        if method == "GET" and inst:
            mm = self.ev.get(eid)
            if mm is None:
                return 404, {"error": {"message": "Not Found"}}
            lo = dt.datetime.fromisoformat(q["timeMin"].replace("Z", "+00:00"))
            hi = dt.datetime.fromisoformat(q["timeMax"].replace("Z", "+00:00"))
            items = []
            for s, en in self.occurrences(mm):
                e = self.instance(mm, mm["id"] + "_" + s.astimezone(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ"), (lo, hi))
                if e:
                    items.append(self.clean(self.ev.get(e["id"], e)))
            return 200, {"items": items}
        if method == "GET":
            e = self.ev.get(eid)
            return (200, self.clean(e)) if e else (404, {"error": {"message": "Not Found"}})
        if method == "POST":
            if body["id"] in self.ev:
                return 409, {"error": {"message": "The requested identifier already exists."}}
            e = dict(body)
            e["start"], e["end"] = body["start"], body["end"]
            return 200, self.clean(self.add(**e))
        if method == "PATCH":
            e = self.ev.get(eid)
            if e is None and "_" in eid and eid.split("_")[0] in self.ev:
                e = self.instance(self.ev[eid.split("_")[0]], eid)
                if e:
                    self.add(**e)
                    e = self.ev[eid]
            if e is None:
                return 404, {"error": {"message": "Not Found"}}
            if self.race:
                self.race()
                self.race = None
            if hd.get("If-Match") and hd["If-Match"] != e["etag"]:
                return 412, {"error": {"message": "Precondition Failed"}}
            e.setdefault("iCalUID", e["id"] + "@google.com")
            for k, v in body.items():
                if v is None:
                    e.pop(k, None)
                else:
                    e[k] = v
            self.norm(e)
            self.touch(e)
            return 200, self.clean(e)
        if method == "DELETE":
            e = self.ev.get(eid)
            if e is None:
                return 404, {"error": {"message": "Not Found"}}
            if e["status"] == "cancelled":
                return 410, {"error": {"message": "Resource has been deleted"}}
            for x in [x for x in self.ev.values() if x.get("recurringEventId") == eid]:
                self.cancel(x)
            self.cancel(e)
            return 204, None
        return 405, {"error": {"message": "method"}}

    def cancel(self, e):
        keep = {k: e[k] for k in ("id", "recurringEventId", "originalStartTime") if k in e}
        e.clear()
        e.update(keep, status="cancelled")
        self.touch(e)

    def list(self, q):
        items = list(self.ev.values())
        if "syncToken" in q:
            if q["syncToken"] == "EXPIRED":
                return 410, {"error": {"message": "Sync token is no longer valid, a full sync is required.", "errors": [{"reason": "fullSyncRequired"}]}}
            items = [e for e in items if e["_rev"] > int(q["syncToken"][1:])]
        else:
            if "iCalUID" in q:
                items = [e for e in items if e.get("iCalUID") == q["iCalUID"]]
            if q.get("showDeleted") != "true":
                items = [e for e in items if e["status"] != "cancelled"]
        off = int(q.get("pageToken", "p0")[1:])
        d = {"items": [self.clean(e) for e in items[off:off + self.page]], "defaultReminders": self.defrem}
        if off + self.page < len(items):
            d["nextPageToken"] = "p%d" % (off + self.page)
        else:
            d["nextSyncToken"] = "S%d" % self.rev
        return 200, d


def base64_sha(v):
    import base64
    return base64.urlsafe_b64encode(hashlib.sha256(v.encode("ascii")).digest()).rstrip(b"=").decode("ascii")


def run(G):
    n = [0]

    def ok(cond, msg):
        n[0] += 1
        if not cond:
            print("FAIL:", msg)
            sys.exit(1)

    tmp = tempfile.mkdtemp(prefix="dkeys_gtest_")
    G.INI, G.SBASE = os.path.join(tmp, "google.ini"), os.path.join(tmp, "google.state")
    fake = Fake()
    O.HTTP, O.SLEEP = fake.http, lambda s: None
    try:
        _run(G, fake, ok, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("selftest OK: %d проверок" % n[0])


def _run(G, fake, ok, tmp):
    import builtins
    import getpass
    import io
    from contextlib import redirect_stderr, redirect_stdout
    HERE = G.HERE

    # ---------- setup: ключи, вход (PKCE), список календарей, профили ----------
    def opener(url):
        q = dict(up.parse_qsl(up.urlparse(url).query))
        fake.challenge = q["code_challenge"]
        ok(q["code_challenge_method"] == "S256" and q["access_type"] == "offline" and q["scope"].endswith("/calendar"), "в запросе входа PKCE, offline и область календаря")
        go = lambda: urllib.request.urlopen(q["redirect_uri"] + "/?code=abc&state=" + q["state"], timeout=10).read()  # noqa: E731
        threading.Thread(target=go, daemon=True).start()                 # «браузер» приходит на 127.0.0.1, пока основной поток слушает
    O.OPEN = opener
    answers = iter(["my-client-id", "1,2"])
    builtins.input = lambda p="": next(answers)
    getpass.getpass = lambda p="": "my-secret"
    out = io.StringIO()
    with redirect_stdout(out):
        G.cmd_setup({"profile": "", "since": "", "dir": ""})
    shown = out.getvalue()
    ok("RT1" not in shown and "AT1" not in shown and "my-secret" not in shown, "секреты не печатаются")
    cp = O.read_ini(G.INI)
    ok(sorted(cp.sections()) == ["me", "rabota"], "профили: основной по e-mail, второй по названию (транслитерация): %s" % cp.sections())
    ok(cp["me"]["calendar"] == "me@gmail.com" and cp["rabota"]["calendar"] == "work@group.calendar.google.com" and cp["me"]["account"] == cp["rabota"]["account"], "ini: календари и общий аккаунт")
    acct = cp["me"]["account"]
    tokp = os.path.join(G.SBASE, acct, "token.json")
    tok = json.load(open(tokp, encoding="utf-8"))
    ok(tok["refresh_token"] == "RT1", "токен сохранён в папке аккаунта")
    cj = json.load(open(os.path.join(G.SBASE, "client.json"), encoding="utf-8"))
    ok(cj["client_id"] == "my-client-id" and cj["client_secret"] == "my-secret", "ключи приложения сохранены")
    if os.name == "nt":
        r = subprocess.run(["icacls", tokp], capture_output=True, text=True)
        ok(r.returncode != 0 or ("Everyone" not in r.stdout and "BUILTIN\\Users" not in r.stdout), "файл токена недоступен другим пользователям")
    else:
        ok(os.stat(tokp).st_mode & 0o077 == 0, "файл токена 600")
    # повторный запуск: те же календари не дублируются
    answers = iter(["1,2"])
    with redirect_stdout(io.StringIO()):
        G.cmd_setup({"profile": "", "since": "", "dir": ""})
    ok(sorted(O.read_ini(G.INI).sections()) == ["me", "rabota"], "повторный setup не дублирует профили")
    # повторный вход существующего профиля обновляет токен и не трогает ini
    os.remove(tokp)
    with redirect_stdout(io.StringIO()):
        G.cmd_setup({"profile": "rabota", "since": "", "dir": ""})
    ok(os.path.exists(tokp) and sorted(O.read_ini(G.INI).sections()) == ["me", "rabota"], "setup --profile: повторный вход")
    cp = O.read_ini(G.INI)
    cp.add_section("ro")
    cp.set("ro", "account", acct)
    cp.set("ro", "calendar", "ru#holiday@group.v.calendar.google.com")
    cp.set("ro", "readonly", "1")
    O.write_ini(cp, G.INI)

    pull = lambda since="", prof="me": run_cmd(G, "cmd_pull", {"profile": prof, "since": since, "dir": ""})  # noqa: E731

    def run_cmd(G, fn, opts, stdin=""):
        buf, old = io.StringIO(), sys.stdin
        sys.stdin = io.StringIO(stdin)
        try:
            with redirect_stdout(buf):
                getattr(G, fn)(opts)
        finally:
            sys.stdin = old
        return buf.getvalue()

    def push(text, prof="me"):
        return run_cmd(G, "cmd_push", {"profile": prof, "since": "", "dir": ""}, text)

    def cursor(t):
        m = re.search(r"\nX-DKEYS-CURSOR:(\S*)", t)
        return m.group(1) if m else None

    # ---------- данные «на сервере» ----------
    MIN = "Europe/Minsk"
    fake.add(id="e1", summary="Обед, с коллегой", description="Строка 1\nСтрока 2", location="Кафе; центр", start={"dateTime": "2026-11-20T10:00:00", "timeZone": MIN},
             end={"dateTime": "2026-11-20T11:00:00", "timeZone": MIN}, reminders={"useDefault": False, "overrides": [{"method": "popup", "minutes": 10}, {"method": "email", "minutes": 60}]},
             attendees=[{"email": "a@x.com", "displayName": "Анна", "responseStatus": "accepted"}, {"email": "b@x.com", "optional": True}], organizer={"email": "boss@x.com", "displayName": "Босс"},
             colorId="5", hangoutLink="https://meet.google.com/abc-defg-hij", transparency="transparent", visibility="private")
    fake.add(id="e2", summary="Праздник", start={"date": "2026-11-21"}, end={"date": "2026-11-22"}, reminders={"useDefault": True})
    fake.add(id="e3", summary="Удалённое", start={"date": "2026-11-25"}, end={"date": "2026-11-26"})
    fake.ev["e3"].update(status="cancelled")
    fake.add(id="e4", iCalUID="ext-1@example.com", summary="Звонок NY", start={"dateTime": "2026-11-23T09:00:00", "timeZone": "America/New_York"}, end={"dateTime": "2026-11-23T10:00:00", "timeZone": "America/New_York"},
             reminders={"useDefault": True})
    fake.add(id="e5", summary="UTC событие", start={"dateTime": "2026-11-24T12:00:00Z"}, end={"dateTime": "2026-11-24T13:30:00Z"})
    fake.add(id="s1", summary="Планёрка", start={"dateTime": "2026-11-20T10:00:00", "timeZone": MIN}, end={"dateTime": "2026-11-20T10:30:00", "timeZone": MIN},
             recurrence=["RRULE:FREQ=WEEKLY;BYDAY=FR", "EXDATE;TZID=Europe/Minsk:20261211T100000"], reminders={"useDefault": False, "overrides": [{"method": "popup", "minutes": 5}]})
    fake.add(id="s1_20261127T070000Z", recurringEventId="s1", iCalUID="s1@google.com", summary="Планёрка (перенос)", originalStartTime={"dateTime": "2026-11-27T10:00:00", "timeZone": MIN},
             start={"dateTime": "2026-11-27T11:00:00", "timeZone": MIN}, end={"dateTime": "2026-11-27T11:30:00", "timeZone": MIN}, reminders={"useDefault": False, "overrides": [{"method": "popup", "minutes": 5}]})
    fake.add(id="s1_20261204T070000Z", recurringEventId="s1", iCalUID="s1@google.com", originalStartTime={"dateTime": "2026-12-04T10:00:00", "timeZone": MIN},
             start={"dateTime": "2026-12-04T10:00:00", "timeZone": MIN}, end={"dateTime": "2026-12-04T10:30:00", "timeZone": MIN})
    fake.ev["s1_20261204T070000Z"].update(status="cancelled")

    # ---------- полная выгрузка ----------
    fake.log.clear()
    t = pull("")
    evs = ev_lines(t)
    ok("X-DKEYS-FULL:1" in t and cursor(t) == "S%d" % fake.rev, "полная выгрузка: признак FULL и курсор syncToken")
    ok(sum(1 for c in fake.log if c[0] == "GET" and c[1].endswith("/events")) >= 3, "пагинация nextPageToken: несколько страниц")
    ok(uids(t) == ["e1@google.com", "e2@google.com", "e5@google.com", "ext-1@example.com", "s1@google.com", "s1@google.com"], "события: отменённое одиночное не приходит, серия = главное + замена: %s" % uids(t))
    ok(all(D.get(e, "X-DKEYS-ETAG") for e in evs), "у каждого события есть etag")
    e1 = next(e for e in evs if D.uid_of(e) == "e1@google.com")
    ok("DTSTART;TZID=Europe/Minsk:20261120T100000" in e1 and "DTEND;TZID=Europe/Minsk:20261120T110000" in e1, "время с поясом")
    ok("SUMMARY:Обед\\, с коллегой" in e1 and "DESCRIPTION:Строка 1\\nСтрока 2" in e1 and "LOCATION:Кафе\\; центр" in e1, "текст экранируется по ICS")
    ok(len(lines(e1, "ATTENDEE")) == 2 and "ROLE=OPT-PARTICIPANT" in lines(e1, "ATTENDEE")[1] and "PARTSTAT=ACCEPTED" in lines(e1, "ATTENDEE")[0] and lines(e1, "ORGANIZER"), "участники и организатор сохранены как ATTENDEE/ORGANIZER")
    ok("X-GCAL-COLORID:5" in e1 and "X-GOOGLE-CONFERENCE:https://meet.google.com/abc-defg-hij" in e1 and "TRANSP:TRANSPARENT" in e1 and "CLASS:PRIVATE" in e1, "цвет, видеозвонок, занятость, приватность")
    ok(M.parse_alarms(e1) == [("popup", 10), ("email", 60)], "напоминания (popup и email)")
    e2 = next(e for e in evs if D.uid_of(e) == "e2@google.com")
    ok("DTSTART;VALUE=DATE:20261121" in e2 and "DTEND;VALUE=DATE:20261122" in e2 and M.parse_alarms(e2) == [("popup", 30)], "целодневное и напоминания по умолчанию календаря")
    e4 = next(e for e in evs if D.uid_of(e) == "ext-1@example.com")
    ok("DTSTART;TZID=America/New_York:20261123T090000" in e4, "другой пояс сохраняется")
    e5 = next(e for e in evs if D.uid_of(e) == "e5@google.com")
    ok("DTSTART:20261124T120000Z" in e5 and "DTEND:20261124T133000Z" in e5, "UTC")
    s1 = [e for e in evs if D.uid_of(e) == "s1@google.com"]
    sm = next(e for e in s1 if D.rec_of(e) is None)
    sx = next(e for e in s1 if D.rec_of(e) is not None)
    ok("RRULE:FREQ=WEEKLY;BYDAY=FR" in sm, "RRULE главного события")
    ok(sorted(lines(sm, "EXDATE")) == ["EXDATE;TZID=Europe/Minsk:20261204T100000", "EXDATE;TZID=Europe/Minsk:20261211T100000"], "EXDATE: из recurrence и отменённое вхождение: %s" % lines(sm, "EXDATE"))
    ok(sx.index("RECURRENCE-ID;TZID=Europe/Minsk:20261127T100000") > sx.index("DTSTART;TZID=Europe/Minsk:20261127T110000"), "замена: RECURRENCE-ID после DTSTART")
    ok(D.get(sm, "X-DKEYS-ETAG") == D.get(sx, "X-DKEYS-ETAG") and "." in D.get(sm, "X-DKEYS-ETAG"), "серия: один etag на главное и замену")
    s_etag1 = D.get(sm, "X-DKEYS-ETAG")
    cur1 = cursor(t)

    # ---------- инкрементально ----------
    t = pull(cur1)
    ok(uids(t) == [] and cursor(t) == cur1 and "X-DKEYS-FULL" not in t, "без изменений: пусто")
    fake.ev["e1"]["summary"] = "Обед 2"
    fake.touch(fake.ev["e1"])
    fake.ev["s1_20261127T070000Z"]["summary"] = "Планёрка (перенос 2)"
    fake.touch(fake.ev["s1_20261127T070000Z"])                          # изменилась только замена
    fake.cancel(fake.ev["e2"])
    fake.cancel(fake.ev["e4"])                                          # со своим iCalUID
    fake.add(id="e6", summary="Новое", start={"dateTime": "2026-11-26T08:00:00", "timeZone": MIN}, end={"dateTime": "2026-11-26T09:00:00", "timeZone": MIN})
    t = pull(cur1)
    ok(uids(t) == ["e1@google.com", "e2@google.com", "e6@google.com", "ext-1@example.com", "s1@google.com", "s1@google.com"], "изменения: изменённое, новое, серия целиком, удалённые: %s" % uids(t))
    dl = {D.uid_of(e): D.get(e, "X-DKEYS-DELETED") for e in ev_lines(t)}
    ok(dl["e2@google.com"] == "1" and dl["ext-1@example.com"] == "1" and not dl["e1@google.com"], "маркеры удаления (в т.ч. с чужим iCalUID из состояния)")
    sm = next(e for e in ev_lines(t) if D.uid_of(e) == "s1@google.com" and D.rec_of(e) is None)
    ok(D.get(sm, "X-DKEYS-ETAG") != s_etag1 and "SUMMARY:Планёрка (перенос 2)" in t, "изменилась замена - etag серии другой, серия пришла целиком")
    cur2 = cursor(t)
    # удалена серия целиком
    fake.race = None
    run_cmd(G, "cmd_push", {"profile": "me", "since": "", "dir": ""}, "BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n")
    fake.cancel(fake.ev["s1_20261127T070000Z"])
    fake.cancel(fake.ev["s1"])
    t = pull(cur2)
    ok(any(D.uid_of(e) == "s1@google.com" and D.get(e, "X-DKEYS-DELETED") == "1" for e in ev_lines(t)), "удалённая серия - маркер удаления")
    # 410 и ISO-время вместо курсора - полная выгрузка
    t = pull("EXPIRED")
    ok("X-DKEYS-FULL:1" in t and "e1@google.com" in uids(t), "410 GONE: полная выгрузка")
    t = pull("2026-11-16T10:00:00Z")
    ok("X-DKEYS-FULL:1" in t, "ISO-время вместо курсора: полная выгрузка")
    # состояние переживает перезапуск процесса
    st = json.load(open(os.path.join(G.SBASE, "me", "state.json"), encoding="utf-8"))
    ok("defrem" in st and "series" in st, "состояние синхронизации сохранено")

    # ---------- push ----------
    def mk(uid, summ, op, etag="", extra=()):
        ev = ["BEGIN:VEVENT", "UID:" + uid, "SUMMARY:" + summ, "DTSTART;TZID=Europe/Minsk:20261130T100000", "DTEND;TZID=Europe/Minsk:20261130T110000", "LAST-MODIFIED:20261116T100000Z",
              "X-DKEYS-OP:" + op] + ([("X-DKEYS-ETAG:" + etag)] if etag else []) + list(extra) + ["END:VEVENT"]
        return ev

    def vc(*evs):
        return D.vcalendar(list(evs))

    # создание: идемпотентно, UID меняется на iCalUID сервера (REF)
    alarm = ["BEGIN:VALARM", "ACTION:DISPLAY", "DESCRIPTION:x", "TRIGGER:-PT15M", "END:VALARM"]
    fake.log.clear()
    r = ev_lines(push(vc(mk("local-1", "Новое локальное", "create", extra=alarm + ["RRULE:FREQ=DAILY;COUNT=3;UNTIL=20261231"]))))
    ok(len(r) == 1 and D.get(r[0], "X-DKEYS-ERROR") is None and D.get(r[0], "X-DKEYS-REF") == "local-1", "create: REF на прежний UID")
    new_uid = D.uid_of(r[0])
    gid = M.stable_id("local-1")
    post = next(c for c in fake.log if c[0] == "POST")
    ok(post[3]["id"] == gid and post[3]["reminders"] == {"useDefault": False, "overrides": [{"method": "popup", "minutes": 15}]}, "id из UID; напоминания overrides")
    ok(new_uid == gid + "@google.com" and gid in fake.ev, "событие создано, UID = iCalUID сервера")
    ok("UNTIL=20261231T205959Z" in post[3]["recurrence"][0], "UNTIL (дата) -> UTC конец дня в поясе события: %s" % post[3]["recurrence"])
    e1_etag = D.get(r[0], "X-DKEYS-ETAG")
    ok(e1_etag and e1_etag == fake.ev[gid]["etag"].strip('"'), "в ответе etag сервера")
    r = ev_lines(push(vc(mk("local-1", "Новое локальное", "create"))))
    ok(sum(1 for e in fake.ev.values() if e.get("summary") == "Новое локальное") == 1 and D.get(r[0], "X-DKEYS-ERROR") is None, "повторное create (409) - без дубля")
    fake.cancel(fake.ev[gid])                                           # удалённое на сервере; create восстанавливает
    r = ev_lines(push(vc(mk("local-1", "Восстановленное", "create"))))
    ok(fake.ev[gid]["status"] == "confirmed" and fake.ev[gid]["summary"] == "Восстановленное", "create поверх удалённого: восстановление")
    uid = D.uid_of(r[0])
    et = D.get(r[0], "X-DKEYS-ETAG")
    # правка: уходит только изменившееся, прочее не трогается
    fake.log.clear()
    r = ev_lines(push(vc(mk(uid, "Переименованное", "update", et))))
    pt = [c for c in fake.log if c[0] == "PATCH"]
    ok(len(pt) == 1 and set(pt[0][3]) == {"summary"}, "update: PATCH только summary: %s %s" % ([c[3] for c in pt], r))
    et2 = D.get(r[0], "X-DKEYS-ETAG")
    ok(et2 != et and fake.ev[gid]["summary"] == "Переименованное", "update применён, etag новый")
    fake.log.clear()
    ok(D.get(ev_lines(push(vc(mk(uid, "Переименованное", "update", et2))))[0], "X-DKEYS-ETAG") == et2 and not any(c[0] == "PATCH" for c in fake.log), "без изменений - запроса записи нет")
    fake.log.clear()
    ev = mk(uid, "Переименованное", "update", et2, extra=alarm)
    ev[3], ev[4] = "DTSTART;TZID=Europe/Minsk:20261130T120000", "DTEND;TZID=Europe/Minsk:20261130T130000"
    push(vc(ev))
    pt = [c for c in fake.log if c[0] == "PATCH"]
    ok(len(pt) == 1 and set(pt[0][3]) == {"start", "end", "reminders"} and pt[0][3]["start"] == {"dateTime": "2026-11-30T12:00:00", "timeZone": MIN}, "update: время и напоминания")
    # участники и цвет не теряются при правке
    fake.log.clear()
    ev1 = ev_lines(pull(""))
    ev1 = next(e for e in ev1 if D.uid_of(e) == "e1@google.com")
    ev1 = D.put(D.put(ev1, "SUMMARY", "Обед 3"), "X-DKEYS-OP", "update")
    push(vc(ev1))
    ok(len(fake.ev["e1"]["attendees"]) == 2 and fake.ev["e1"]["colorId"] == "5" and fake.ev["e1"]["summary"] == "Обед 3", "участники, цвет не затронуты правкой")
    ok(not any(set(c[3]) & {"attendees", "colorId", "hangoutLink"} for c in fake.log if c[0] == "PATCH"), "PATCH не содержит чужих полей")
    # конфликты
    fake.log.clear()
    r = ev_lines(push(vc(mk(uid, "Старый etag", "update", et))))
    ok(D.get(r[0], "X-DKEYS-ERROR") == "conflict" and not any(c[0] == "PATCH" for c in fake.log), "устаревший etag: conflict без записи")
    cur = D.get(ev_lines(push(vc(mk(uid, "Переименованное", "update", "0"))))[0], "X-DKEYS-ERROR")
    ok(cur == "conflict", "чужой etag: conflict")
    et3 = fake.ev[gid]["etag"].strip('"')
    fake.race = lambda: fake.touch(fake.ev[gid])                        # кто-то правит между чтением и записью
    r = ev_lines(push(vc(mk(uid, "Гонка", "update", et3))))
    ok(D.get(r[0], "X-DKEYS-ERROR") == "conflict" and fake.ev[gid]["summary"] != "Гонка", "412 от сервера (If-Match): conflict")
    r = ev_lines(push(vc(mk("nope@google.com", "Нет", "update", "1"))))
    ok(D.get(r[0], "X-DKEYS-ERROR") == "not found", "update несуществующего: not found")
    # серия с заменой вхождения
    ser = ["RRULE:FREQ=WEEKLY;BYDAY=MO"]
    m1 = mk("local-s", "Еженедельная", "create", extra=ser)
    m1[3], m1[4] = "DTSTART;TZID=Europe/Minsk:20261123T100000", "DTEND;TZID=Europe/Minsk:20261123T110000"
    m2 = mk("local-s", "Перенесённая", "create", extra=["RECURRENCE-ID;TZID=Europe/Minsk:20261130T100000"])
    m2[3], m2[4] = "DTSTART;TZID=Europe/Minsk:20261130T150000", "DTEND;TZID=Europe/Minsk:20261130T160000"
    r = ev_lines(push(vc(m1, m2)))
    sid = M.stable_id("local-s")
    ok(len(r) == 2 and all(D.get(e, "X-DKEYS-ERROR") is None for e in r) and D.get(r[0], "X-DKEYS-ETAG") == D.get(r[1], "X-DKEYS-ETAG"), "серия с заменой создана одним ответом")
    xe = [e for e in fake.ev.values() if e.get("recurringEventId") == sid]
    ok(len(xe) == 1 and xe[0]["summary"] == "Перенесённая" and "T15:00:00+03:00" in xe[0]["start"]["dateTime"], "замена применена к вхождению серии: %s" % xe)
    # удаление вхождения в DKeys = EXDATE в главном
    new_s = D.uid_of(r[0])
    cur = ev_lines(pull(""))
    sm = next(e for e in cur if D.uid_of(e) == new_s and D.rec_of(e) is None)
    sm2 = D.put(D.drop(sm, "X-DKEYS-OP"), "X-DKEYS-OP", "update")
    sm2 = sm2[:-1] + ["EXDATE;TZID=Europe/Minsk:20261207T100000", sm2[-1]]
    fake.log.clear()
    r = ev_lines(push(vc(sm2, *[D.put(x, "X-DKEYS-OP", "update") for x in cur if D.uid_of(x) == new_s and D.rec_of(x) is not None])))
    pt = [c for c in fake.log if c[0] == "PATCH"]
    ok(D.get(r[0], "X-DKEYS-ERROR") is None and len(pt) == 1 and pt[0][3]["recurrence"][1:] == ["EXDATE;TZID=Europe/Minsk:20261207T100000"], "удалённое вхождение уходит как EXDATE: %s" % [c[3] for c in pt])
    # удаление: идемпотентно, в т.ч. неизвестного
    r = ev_lines(push(vc(mk(uid, "x", "delete", et3))))
    ok(D.get(r[0], "X-DKEYS-DELETED") == "1" and fake.ev[gid]["status"] == "cancelled", "delete")
    r = ev_lines(push(vc(mk(uid, "x", "delete"))))
    ok(D.get(r[0], "X-DKEYS-DELETED") == "1" and D.get(r[0], "X-DKEYS-ERROR") is None, "повторное delete - не ошибка")
    r = ev_lines(push(vc(mk("never-existed", "x", "delete"))))
    ok(D.get(r[0], "X-DKEYS-DELETED") == "1", "delete неизвестного")
    # профиль только для чтения
    r = ev_lines(push(vc(mk("ro-1", "x", "create")), prof="ro"))
    ok("только для чтения" in (D.get(r[0], "X-DKEYS-ERROR") or ""), "readonly: правки не отправляются")

    # ---------- повторы и сбои ----------
    fake.flaky = [("GET", "/events", [429, 503])]
    fake.log.clear()
    t = pull("")
    ok("X-DKEYS-FULL:1" in t and sum(1 for c in fake.log if c[1].endswith("/events") and c[2].get("pageToken") is None) == 3, "429 и 503: повтор и успех")
    e1_ics = next(e for e in ev_lines(pull("")) if D.uid_of(e) == "e1@google.com")
    upd = lambda summ: vc(D.put(D.put(e1_ics, "SUMMARY", summ), "X-DKEYS-OP", "update"))  # noqa: E731
    fake.flaky = [("PATCH", "/events", [500])]
    r = ev_lines(push(upd("Повтор 500")))
    ok(D.get(r[0], "X-DKEYS-ERROR") is None and fake.ev["e1"]["summary"] == "Повтор 500", "5xx при записи: повтор и успех")
    e1_ics = next(e for e in ev_lines(pull("")) if D.uid_of(e) == "e1@google.com")
    fake.flaky = [("PATCH", "/events", [500, 500, 500, 500])]
    r = ev_lines(push(upd("Упорный сбой")))
    ok("Google 500" in (D.get(r[0], "X-DKEYS-ERROR") or "") and fake.ev["e1"]["summary"] == "Повтор 500", "постоянный 5xx: ошибка на событии, не падение")
    fake.flaky = [("GET", "/events", ["net", "net", "net", "net"])]
    try:
        pull("")
        ok(False, "сеть недоступна: pull должен упасть")
    except O.NetError:
        ok(True, "")
    fake.flaky = []
    # токен: истёк -> обновление; отозван -> понятный текст
    fake.valid = set()
    t = pull("")
    ok("X-DKEYS-FULL:1" in t and json.load(open(tokp, encoding="utf-8"))["access_token"] != "AT1", "401: токен доступа обновлён по refresh-токену")
    os.remove(tokp)
    try:
        with redirect_stderr(io.StringIO()) as se:
            pull("")
        ok(False, "без токена pull должен завершиться")
    except SystemExit:
        ok("Подключить" in se.getvalue(), "нет токена: подсказка про «Подключить…»")
    O.write_secret(tokp, {"refresh_token": "RT1", "access_token": "x", "expires_at": 0})
    fake.revoked = True
    try:
        pull("")
        ok(False, "отозванный вход")
    except O.AuthError as e:
        ok("Подключить" in str(e) and "invalid_grant" in str(e), "отозванный вход: подсказка про «Подключить…»: %s" % e)
    fake.revoked = False
    # приложение: профиль не настроен / нет ключей
    try:
        with redirect_stderr(io.StringIO()) as se:
            pull("", prof="ghost")
        ok(False, "профиль не настроен")
    except SystemExit:
        ok("не настроен" in se.getvalue(), "неизвестный профиль: понятная ошибка")
    # контракт на уровне процесса: info
    r = subprocess.run([sys.executable, os.path.join(HERE, "google.py"), "info"], capture_output=True, text=True, encoding="utf-8", env=dict(os.environ, PYTHONIOENCODING="utf-8"), timeout=30)
    j = json.loads(r.stdout.strip().splitlines()[-1])
    ok(r.returncode == 0 and j["contract"] == 1 and j["create"] and j["update"] and j["delete"] and "Google" in j["name"], "info: имя и возможности")
