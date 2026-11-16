# -*- coding: utf-8 -*-
"""caldav - универсальный источник календаря DKeys по CalDAV: Apple iCloud, Яндекс, Nextcloud, Fastmail и любой сервер CalDAV.
Договор команд (info, setup, pull, push) - README.md рядом. Настройки - caldav.ini (профиль = секция, см. caldav.ini.example), пароль и состояние -
caldav.state\\<профиль>\\ (не в git). Библиотеки: caldav (соединение, поиск календарей) и icalendar (часовые пояса); при `setup` ставятся сами.
Обмен с сервером - обычные запросы PROPFIND/REPORT/PUT/DELETE через соединение библиотеки caldav, поэтому от версии библиотеки зависит мало.
Синхронизация экономная: если метка календаря (ctag) не изменилась - сервер больше не опрашивается; иначе один запрос со списком etag и выборка только изменённого.
Проверка без сети и аккаунтов: `python caldav.py --selftest` (сервер в памяти)."""
import configparser
import getpass
import os
import re
import subprocess
import sys
import urllib.parse as up
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dkcal as D  # noqa: E402

NS_D, NS_C, NS_CS = "DAV:", "urn:ietf:params:xml:ns:caldav", "http://calendarserver.org/ns/"
PRESETS = {
    "icloud": {"url": "https://caldav.icloud.com/", "hint": "пароль приложения: appleid.apple.com -> Вход и безопасность -> Пароли приложений"},
    "yandex": {"url": "https://caldav.yandex.ru/", "hint": "пароль приложения: id.yandex.ru -> Безопасность -> Пароли приложений -> Календарь"},
    "fastmail": {"url": "https://caldav.fastmail.com/dav/", "hint": "пароль приложения Fastmail (Settings -> Privacy & Security -> App passwords, доступ CalDAV)"},
    "nextcloud": {"url": "", "suffix": "remote.php/dav/", "hint": "адрес сервера вида https://cloud.example.com; лучше пароль приложения (Настройки -> Безопасность)"},
    "custom": {"url": "", "hint": "полный адрес CalDAV сервера"},
}
STRIP = ("X-DKEYS-OP", "X-DKEYS-ETAG", "X-DKEYS-DELETED", "X-DKEYS-REF", "X-DKEYS-ERROR", "X-DKEYS-HIDDEN", "X-DKEYS-SYNCED")


class HttpError(Exception):
    def __init__(self, status, text=""):
        Exception.__init__(self, ("HTTP %s %s" % (status, text)).strip())
        self.status = status


# ---------- разбор ответов сервера (XML multistatus) ----------
def parse_multi(raw):
    """207 Multi-Status -> [(href, {тег свойства: элемент})]; берутся только свойства со статусом 200."""
    if isinstance(raw, str):
        raw = raw.encode("utf-8")
    root = ET.fromstring(raw)
    out = []
    for resp in root.iter("{%s}response" % NS_D):
        href = (resp.findtext("{%s}href" % NS_D) or "").strip()
        props = {}
        for ps in resp.findall("{%s}propstat" % NS_D):
            if " 200" not in (ps.findtext("{%s}status" % NS_D) or ""):
                continue
            pr = ps.find("{%s}prop" % NS_D)
            for p in (list(pr) if pr is not None else []):
                props[p.tag] = p
        out.append((href, props))
    return out


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


BODY_CTAG = ('<?xml version="1.0" encoding="utf-8"?><D:propfind xmlns:D="DAV:" xmlns:CS="%s"><D:prop><CS:getctag/><D:sync-token/></D:prop></D:propfind>' % NS_CS)
BODY_ETAGS = '<?xml version="1.0" encoding="utf-8"?><D:propfind xmlns:D="DAV:"><D:prop><D:getetag/><D:resourcetype/></D:prop></D:propfind>'


def body_multiget(hrefs):
    return ('<?xml version="1.0" encoding="utf-8"?><C:calendar-multiget xmlns:D="DAV:" xmlns:C="%s"><D:prop><D:getetag/><C:calendar-data/></D:prop>%s</C:calendar-multiget>'
            % (NS_C, "".join("<D:href>%s</D:href>" % esc(h) for h in hrefs)))


def body_uid(uid):
    return ('<?xml version="1.0" encoding="utf-8"?><C:calendar-query xmlns:D="DAV:" xmlns:C="%s"><D:prop><D:getetag/></D:prop><C:filter><C:comp-filter name="VCALENDAR">'
            '<C:comp-filter name="VEVENT"><C:prop-filter name="UID"><C:text-match collation="i;octet">%s</C:text-match></C:prop-filter></C:comp-filter></C:comp-filter></C:filter></C:calendar-query>'
            % (NS_C, esc(uid)))


# ---------- транспорт: настоящий (библиотека caldav) и сервер в памяти для --selftest ----------
class Real(object):
    def __init__(self, url, user, password):
        import caldav
        self.client = caldav.DAVClient(url=url, username=user, password=password)

    def _req(self, url, method, body="", headers=None, ok=(200, 201, 204, 207)):
        h = {"Content-Type": "application/xml; charset=utf-8"}
        h.update(headers or {})
        r = self.client.request(url, method, body, h)
        st = int(getattr(r, "status", 0) or 0)
        if st not in ok:
            raise HttpError(st, method)
        return r

    @staticmethod
    def _raw(r):
        raw = getattr(r, "raw", b"")
        return raw if raw else b""

    def calendars(self):
        out = []
        for c in self.client.principal().calendars():
            try:
                comps = c.get_supported_components()
                if comps and "VEVENT" not in [str(x).upper() for x in comps]:
                    continue                                       # списки дел и напоминаний - не события
            except Exception:
                pass
            out.append((str(c.name or ""), str(c.url)))
        return out

    def ctag(self, cal):
        try:
            r = self._req(cal, "PROPFIND", BODY_CTAG, {"Depth": "0"})
            for _h, props in parse_multi(self._raw(r)):
                for tag in ("{%s}getctag" % NS_CS, "{%s}sync-token" % NS_D):
                    if tag in props and (props[tag].text or "").strip():
                        return props[tag].text.strip()
        except Exception:
            pass
        return None

    def etags(self, cal):
        r = self._req(cal, "PROPFIND", BODY_ETAGS, {"Depth": "1"})
        out = {}
        for href, props in parse_multi(self._raw(r)):
            rt = props.get("{%s}resourcetype" % NS_D)
            if rt is not None and rt.find("{%s}collection" % NS_D) is not None:
                continue
            et = props.get("{%s}getetag" % NS_D)
            if href and et is not None and (et.text or "").strip():
                out[href] = et.text.strip()
        return out

    def multiget(self, cal, hrefs):
        out, hrefs = {}, list(hrefs)
        for i in range(0, len(hrefs), 40):
            r = self._req(cal, "REPORT", body_multiget(hrefs[i:i + 40]), {"Depth": "1"})
            for href, props in parse_multi(self._raw(r)):
                et, data = props.get("{%s}getetag" % NS_D), props.get("{%s}calendar-data" % NS_C)
                if et is not None and data is not None and data.text:
                    out[href] = (et.text.strip(), data.text)
        return out

    def new_href(self, cal, name):
        return up.urlparse(cal).path.rstrip("/") + "/" + up.quote(name)

    def put(self, cal, href, text, if_match=None, if_none_match=False):
        h = {"Content-Type": "text/calendar; charset=utf-8"}
        if if_match:
            h["If-Match"] = if_match
        elif if_none_match:
            h["If-None-Match"] = "*"
        r = self._req(up.urljoin(cal, href), "PUT", text.encode("utf-8"), h)
        hd = getattr(r, "headers", {}) or {}
        return hd.get("ETag") or hd.get("etag")

    def delete(self, cal, href):
        try:
            self._req(up.urljoin(cal, href), "DELETE")
        except HttpError as e:
            if e.status not in (404, 410):
                raise

    def find_uid(self, cal, uid):
        r = self._req(cal, "REPORT", body_uid(uid), {"Depth": "1"})
        for href, props in parse_multi(self._raw(r)):
            et = props.get("{%s}getetag" % NS_D)
            if href and et is not None:
                return href, et.text.strip()
        return None


class Fake(object):
    """Сервер CalDAV в памяти (тот же интерфейс, что у Real): для --selftest без сети и аккаунтов."""
    def __init__(self):
        self.res, self.n, self.cnt, self.calls = {}, 0, 0, {}

    def _c(self, name):
        self.calls[name] = self.calls.get(name, 0) + 1

    def calendars(self):
        return [("Home", "/cal/")]

    def ctag(self, cal):
        self._c("ctag")
        return "c%d" % self.cnt

    def etags(self, cal):
        self._c("etags")
        return {h: v[0] for h, v in self.res.items()}

    def multiget(self, cal, hrefs):
        self._c("multiget")
        return {h: self.res[h] for h in hrefs if h in self.res}

    def new_href(self, cal, name):
        return cal.rstrip("/") + "/" + name

    def put(self, cal, href, text, if_match=None, if_none_match=False):
        self._c("put")
        cur = self.res.get(href)
        if if_match is not None and (cur is None or cur[0] != if_match):
            raise HttpError(412)
        if if_none_match and cur is not None:
            raise HttpError(412)
        self.n += 1
        self.cnt += 1
        et = '"%d"' % self.n
        self.res[href] = (et, text)
        return et

    def delete(self, cal, href):
        self._c("delete")
        if self.res.pop(href, None) is not None:
            self.cnt += 1

    def find_uid(self, cal, uid):
        self._c("find_uid")
        for h, (et, text) in self.res.items():
            if any(D.uid_of(e) == uid for e in D.split_vevents(text)):
                return h, et
        return None

    def admin_put(self, href, text):                    # «другой клиент» изменил или создал событие
        self.n += 1
        self.cnt += 1
        self.res[href] = ('"%d"' % self.n, text)

    def admin_del(self, href):
        self.res.pop(href, None)
        self.cnt += 1


# ---------- часовые пояса ----------
def vtimezone(tzid):
    """Строки VTIMEZONE для tzid или None. icalendar (если есть и умеет), иначе - фиксированное смещение из zoneinfo."""
    try:
        import icalendar
        tz = icalendar.Timezone.from_tzid(tzid)
        return [x for x in tz.to_ical().decode("utf-8").replace("\r\n", "\n").split("\n") if x]
    except Exception:
        pass
    try:
        import datetime
        from zoneinfo import ZoneInfo
        off = ZoneInfo(tzid).utcoffset(datetime.datetime.now())
        m = int(off.total_seconds() // 60)
        s = "%s%02d%02d" % ("-" if m < 0 else "+", abs(m) // 60, abs(m) % 60)
        return ["BEGIN:VTIMEZONE", "TZID:" + tzid, "BEGIN:STANDARD", "DTSTART:19700101T000000", "TZOFFSETFROM:" + s, "TZOFFSETTO:" + s, "END:STANDARD", "END:VTIMEZONE"]
    except Exception:
        return None


def ics_for_server(group):
    """VCALENDAR для отправки: события группы без служебных X-DKEYS-*, плюс VTIMEZONE для упомянутых TZID."""
    evs = [D.drop(e, *STRIP) for e in group]
    tzids = []
    for e in evs:
        for ln in e:
            for m in re.finditer(r"TZID=([^;:]+)", ln.split(":", 1)[0] if ":" in ln else ln):
                t = m.group(1).strip('"')
                if t.upper() != "UTC" and t not in tzids:
                    tzids.append(t)
    extra = []
    for t in tzids:
        extra.extend(vtimezone(t) or [])
    return D.vcalendar(evs, extra)


# ---------- движок синхронизации ----------
def safe_name(uid):
    return re.sub(r"[^A-Za-z0-9._@-]", "_", uid)[:120] + ".ics"


def snap(cursor, etags, uids, ctag):
    return {"cursor": cursor, "etags": dict(etags), "uids": dict(uids), "ctag": ctag}


class Engine(object):
    """state: {'cal': url, 'gen': n, 'committed': снимок, 'pending': снимок|None}. Снимок - то, что сервер отдал клиенту под курсором g<N>.
    Курсор клиент запоминает, только если применил ответ: пришёл pull с курсором pending - значит, применил, pending становится committed;
    пришёл с прежним committed - не применил, отдаём изменения заново; незнакомый курсор или пусто - полная выгрузка."""
    def __init__(self, t, cal, state):
        self.t, self.cal, self.s = t, cal, state
        self.s.setdefault("gen", 0)
        self.s.setdefault("committed", snap("", {}, {}, None))
        self.s.setdefault("pending", None)

    def pull(self, since):
        s, cm = self.s, self.s["committed"]
        if s["pending"] and since and since == s["pending"]["cursor"]:
            cm = s["committed"] = s["pending"]
            s["pending"] = None
        full = not since or since != cm["cursor"]
        ctag = self.t.ctag(self.cal)
        if not full and s["pending"] is None and ctag is not None and ctag == cm["ctag"]:
            return D.vcalendar([], ["X-DKEYS-CURSOR:" + since])                       # метка календаря та же - сервер больше не опрашиваем
        etags = self.t.etags(self.cal)
        base_e = {} if full else cm["etags"]
        base_u = {} if full else cm["uids"]
        changed = [h for h, e in etags.items() if base_e.get(h) != e]
        gone = [h for h in base_e if h not in etags]
        got = self.t.multiget(self.cal, changed) if changed else {}
        events, uids, new_e = [], {h: u for h, u in base_u.items() if h in etags}, {}
        for h, e in etags.items():
            if h not in changed:
                new_e[h] = e
        for h in changed:
            if h not in got:
                continue                                                               # исчез между запросами
            et, text = got[h]
            evs = D.split_vevents(text)
            if not evs or not D.uid_of(evs[0]):
                continue
            new_e[h] = et
            uids[h] = D.uid_of(evs[0])
            events.extend(D.put(e, "X-DKEYS-ETAG", et) for e in evs)
        for h in gone:
            events.append(D.deleted_marker(base_u.get(h) or os.path.basename(h)[:-4], base_e[h]))
        s["gen"] += 1
        cur = "g%d" % s["gen"]
        s["pending"] = snap(cur, new_e, {h: u for h, u in uids.items() if h in new_e}, ctag)
        return D.vcalendar(events, ["X-DKEYS-CURSOR:" + cur] + (["X-DKEYS-FULL:1"] if full else []))

    def _href_of(self, uid):
        for sn in (self.s["pending"], self.s["committed"]):
            for h, u in (sn["uids"].items() if sn else []):
                if u == uid:
                    return h
        r = self.t.find_uid(self.cal, uid)
        return r[0] if r else None

    def _record(self, href, uid, etag):
        for sn in (self.s["pending"], self.s["committed"]):
            if sn:
                sn["etags"][href] = etag
                sn["uids"][href] = uid

    def _forget(self, href):
        for sn in (self.s["pending"], self.s["committed"]):
            if sn:
                sn["etags"].pop(href, None)
                sn["uids"].pop(href, None)

    def _push1(self, uid, group, op, sent):
        href = self._href_of(uid)
        if op == "delete":
            if href:
                self.t.delete(self.cal, href)
                self._forget(href)
            return [D.drop(D.put(e, "X-DKEYS-DELETED", "1"), "X-DKEYS-OP") for e in group]
        body = ics_for_server(group)
        if op == "update":
            if not href:
                raise HttpError(404)
            try:
                et = self.t.put(self.cal, href, body, if_match=sent or None)
            except HttpError as e:
                if e.status in (404, 410, 412):
                    if href in self.t.etags(self.cal):
                        raise HttpError(412)
                    raise HttpError(404)
                raise
        else:
            href = href or self.t.new_href(self.cal, safe_name(uid))
            try:
                et = self.t.put(self.cal, href, body, if_none_match=True)
            except HttpError as e:
                if e.status != 412:
                    raise
                et = self.t.put(self.cal, href, body)                                   # create - идемпотентная запись: уже есть - перезаписываем
        et = et or self.t.etags(self.cal).get(href, "")
        self._record(href, uid, et)
        return [D.put(D.drop(e, *STRIP), "X-DKEYS-ETAG", et) for e in group]

    def push(self, text):
        out = []
        for uid, group in D.group_by_uid(D.split_vevents(text)):
            op = (D.get(group[0], "X-DKEYS-OP") or "create").lower()
            sent = D.get(group[0], "X-DKEYS-ETAG") or ""
            try:
                out.extend(self._push1(uid, group, op, sent))
            except HttpError as e:
                msg = "conflict" if e.status == 412 else "not found" if e.status in (404, 410) else str(e)
                out.extend(D.put(D.drop(e2, "X-DKEYS-OP"), "X-DKEYS-ERROR", msg) for e2 in group)
            except Exception as e:                                                       # сеть, разбор и т.п.: событие не отправлено, остальные - да
                out.extend(D.put(D.drop(e2, "X-DKEYS-OP"), "X-DKEYS-ERROR", "%s: %s" % (type(e).__name__, e)) for e2 in group)
        return D.vcalendar(out)


# ---------- настройки профиля ----------
def ini_path():
    return os.path.join(HERE, "caldav.ini")


def read_ini():
    cp = configparser.ConfigParser(interpolation=None)
    cp.read(ini_path(), encoding="utf-8")
    return cp


def pick_profile(cp, profile):
    if profile:
        return profile if cp.has_section(profile) else ""
    if cp.has_section("default"):
        return "default"
    return cp.sections()[0] if len(cp.sections()) == 1 else ""


def secret_path(profile):
    return os.path.join(D.state_dir(__file__, profile or "default"), "secret.txt")


def server_url(sec):
    pre = PRESETS.get((sec.get("provider") or "custom").lower(), PRESETS["custom"])
    url = (sec.get("url") or pre["url"] or "").strip()
    if pre.get("suffix") and url and "/dav" not in url:
        url = url.rstrip("/") + "/" + pre["suffix"]
    return url


def need_caldav(install):
    try:
        import caldav  # noqa: F401
        return True
    except ImportError:
        if not install:
            D.fail("не установлена библиотека caldav: нажмите «Подключить…» в календаре (setup поставит её) или выполните: python -m pip install caldav icalendar", 4)
        print("Устанавливаю библиотеки caldav и icalendar...")
        rc = subprocess.call([sys.executable, "-m", "pip", "install", "--user", "caldav", "icalendar"])
        if rc != 0:
            D.fail("не удалось установить caldav (pip вернул %d): выполните вручную: python -m pip install caldav icalendar" % rc, 4)
        import importlib
        importlib.invalidate_caches()
        import caldav  # noqa: F401,F811
        return True


def open_profile(profile):
    """(транспорт, url календаря, путь к файлу состояния, состояние) для профиля."""
    cp = read_ini()
    name = pick_profile(cp, profile)
    if not name:
        D.fail("профиль %s не настроен в caldav.ini: нажмите «Подключить…» в календаре" % (profile or ""), 2)
    sec = cp[name]
    user = (sec.get("user") or "").strip()
    password = os.environ.get("DKEYS_CALDAV_PASSWORD", "")
    if not password:
        try:
            with open(secret_path(name), encoding="utf-8") as f:
                password = f.read().strip()
        except OSError:
            D.fail("нет пароля для профиля %s: нажмите «Подключить…» в календаре" % name, 2)
    need_caldav(False)
    t = Real(server_url(sec), user, password)
    sd = D.state_dir(__file__, name)
    sp = os.path.join(sd, "state.json")
    state = D.read_json(sp, {})
    want = (sec.get("calendar") or "").strip().casefold()
    cal = state.get("cal") if state.get("want") == want else None
    if not cal:
        cals = t.calendars()
        if not cals:
            D.fail("на сервере нет календарей с событиями", 3)
        pick = [c for c in cals if c[0].casefold() == want] if want else []
        cal = (pick or cals)[0][1]
        state = {"cal": cal, "want": want}
    return t, cal, sp, state


def cmd_pull_push(verb, opts):
    t, cal, sp, state = open_profile(opts["profile"])
    eng = Engine(t, cal, state)
    try:
        if verb == "pull":
            D.out(eng.pull(opts["since"].strip()))
        else:
            D.out(eng.push(sys.stdin.read()))
    except HttpError as e:
        if e.status in (401, 403):
            D.fail("сервер отказал в доступе (%s): проверьте пароль приложения, «Подключить…» задаст заново" % e.status, 3)
        D.fail("ошибка сервера: %s" % e, 3)
    D.write_json(sp, eng.s)


def cmd_setup(opts):
    print("CalDAV: подключение календаря (профиль: %s)" % (opts["profile"] or "default"))
    need_caldav(True)
    prof = opts["profile"] or "default"
    cp = read_ini()
    sec = dict(cp[prof]) if cp.has_section(prof) else {}
    prov = (sec.get("provider") or "").lower()
    while prov not in PRESETS:
        prov = input("Сервер (icloud / yandex / nextcloud / fastmail / custom): ").strip().lower()
    pre = PRESETS[prov]
    print("Подсказка:", pre["hint"])
    user = input("Логин [%s]: " % sec.get("user", "")).strip() or sec.get("user", "")
    url = sec.get("url", "")
    if prov in ("nextcloud", "custom"):
        url = input("Адрес сервера [%s]: " % url).strip() or url
    password = getpass.getpass("Пароль приложения (ввод не виден): ").strip()
    if not password:
        D.fail("пароль не введён", 2)
    sec.update({"provider": prov, "user": user})
    if url:
        sec["url"] = url
    t = Real(server_url(sec), user, password)
    try:
        cals = t.calendars()
    except Exception as e:
        D.fail("не удалось подключиться: %s" % e, 3)
    if not cals:
        D.fail("на сервере нет календарей с событиями", 3)
    for i, (n, _u) in enumerate(cals, 1):
        print("  %d. %s" % (i, n or "(без названия)"))
    k = input("Какой календарь синхронизировать [1]: ").strip()
    pick = cals[int(k) - 1] if k.isdigit() and 1 <= int(k) <= len(cals) else cals[0]
    sec["calendar"] = pick[0]
    if not cp.has_section(prof):
        cp.add_section(prof)
    for key, val in sec.items():
        cp.set(prof, key, val)
    with open(ini_path(), "w", encoding="utf-8") as f:
        cp.write(f)
    with open(secret_path(prof), "w", encoding="utf-8") as f:
        f.write(password)
    sp = os.path.join(D.state_dir(__file__, prof), "state.json")
    if os.path.exists(sp):
        os.remove(sp)                                                                  # новый календарь - новая выгрузка
    print("Готово: календарь «%s» подключён. Закройте это окно - DKeys синхронизируется сам." % pick[0])


# ---------- проверка без сети ----------
def selftest():
    n = [0]

    def ok(cond, msg):
        n[0] += 1
        if not cond:
            print("FAIL:", msg)
            sys.exit(1)

    def ev(uid, summ, extra=(), lm="20261101T100000Z"):
        return ["BEGIN:VEVENT", "UID:" + uid, "SUMMARY:" + summ, "DTSTART:20261120T100000Z", "DTEND:20261120T110000Z", "LAST-MODIFIED:" + lm] + list(extra) + ["END:VEVENT"]

    def res(*evs):
        return D.vcalendar(list(evs))

    def evs_of(text):
        return D.split_vevents(text)

    def uids(text):
        return sorted(D.uid_of(e) for e in evs_of(text))

    def cursor(text):
        m = re.search(r"\nX-DKEYS-CURSOR:(\S*)", text)
        return m.group(1) if m else None

    # разбор ответов
    multi = ('<?xml version="1.0"?><d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav" xmlns:cs="http://calendarserver.org/ns/">'
             '<d:response><d:href>/c/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype><cs:getctag>77</cs:getctag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>'
             '<d:response><d:href>/c/a.ics</d:href><d:propstat><d:prop><d:getetag>"e1"</d:getetag><c:calendar-data>BEGIN:VCALENDAR</c:calendar-data></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>'
             '<d:propstat><d:prop><d:getlastmodified/></d:prop><d:status>HTTP/1.1 404 Not Found</d:status></d:propstat></d:response></d:multistatus>')
    pm = parse_multi(multi)
    ok(len(pm) == 2 and pm[0][0] == "/c/" and "{DAV:}resourcetype" in pm[0][1] and pm[1][1]["{DAV:}getetag"].text == '"e1"' and "{DAV:}getlastmodified" not in pm[1][1], "parse_multi")
    ok("<D:href>/a&amp;b</D:href>" in body_multiget(["/a&b"]) and "x&lt;y" in body_uid("x<y"), "тела запросов экранируются")

    srv, state = Fake(), {}
    cal = "/cal/"
    eng = lambda: Engine(srv, cal, state)  # noqa: E731
    # пустой сервер: полная выгрузка, курсор
    o = eng().pull("")
    ok(uids(o) == [] and cursor(o) == "g1" and "X-DKEYS-FULL:1" in o, "пустая полная выгрузка")
    # на сервере: обычное событие и серия (главное + замена вхождения в одном ресурсе)
    srv.admin_put("/cal/a.ics", res(ev("a", "Alpha")))
    srv.admin_put("/cal/s.ics", res(ev("s", "Series", ["RRULE:FREQ=WEEKLY"]), ev("s", "Moved", ["RECURRENCE-ID:20261127T100000Z"])))
    o = eng().pull("g1")
    ok(uids(o) == ["a", "s", "s"] and cursor(o) == "g2" and "X-DKEYS-FULL" not in o, "изменения после курсора: новое событие и серия целиком")
    ok(all(D.get(e, "X-DKEYS-ETAG") for e in evs_of(o)), "у каждого события есть etag")
    # клиент не применил ответ (прислал прежний курсор g1): те же изменения заново
    o2 = eng().pull("g1")
    ok(uids(o2) == ["a", "s", "s"] and cursor(o2) == "g3", "не применённый ответ отдаётся заново")
    # клиент применил g3: изменений нет, метка календаря та же - сервер больше не опрашивается, курсор прежний
    srv.calls.clear()
    o3 = eng().pull("g3")
    ok(uids(o3) == [] and cursor(o3) == "g3" and srv.calls.get("etags", 0) == 0 and srv.calls.get("multiget", 0) == 0, "метка не изменилась - без запросов списка")
    # другой клиент: изменил a, удалил s, создал b
    srv.admin_put("/cal/a.ics", res(ev("a", "Alpha2")))
    srv.admin_del("/cal/s.ics")
    srv.admin_put("/cal/b.ics", res(ev("b", "Beta")))
    srv.calls.clear()
    o = eng().pull("g3")
    ok(uids(o) == ["a", "b", "s"] and sum(1 for e in evs_of(o) if D.get(e, "X-DKEYS-DELETED") == "1" and D.uid_of(e) == "s") == 1, "изменено, создано, удалено (маркер удаления серии)")
    ok(srv.calls.get("multiget") == 1 and "Alpha2" in o and "Beta" in o, "изменённое выбирается одним запросом")
    ok("SUMMARY:Series" not in o, "удалённое не присылается целиком")
    # незнакомый курсор - полная выгрузка
    o = eng().pull("g999")
    ok("X-DKEYS-FULL:1" in o and uids(o) == ["a", "b"], "незнакомый курсор: полная выгрузка")
    cur = cursor(o)
    eng().pull(cur)                                                  # применён
    # push: создание нового события; сервер хранит без служебных свойств; повторный pull не возвращает его же
    p = D.vcalendar([D.put(D.put(ev("n1", "New", ["DTSTART;TZID=Europe/Minsk:20261121T100000"]), "X-DKEYS-OP", "create"), "X-DKEYS-HIDDEN", "1")])
    o = eng().push(p)
    et_n1 = D.get(evs_of(o)[0], "X-DKEYS-ETAG")
    ok(uids(o) == ["n1"] and et_n1 and not D.get(evs_of(o)[0], "X-DKEYS-OP"), "create: ответ с etag")
    stored = [v for h, v in srv.res.items() if "n1" in h]
    ok(len(stored) == 1 and "X-DKEYS" not in stored[0][1] and stored[0][1].count("BEGIN:VEVENT") == 1, "на сервере без служебных свойств")
    try:
        import zoneinfo  # noqa: F401
        zoneinfo.ZoneInfo("Europe/Minsk")
        ok("BEGIN:VTIMEZONE" in stored[0][1] and "TZID:Europe/Minsk" in stored[0][1], "VTIMEZONE добавлен для TZID")
    except Exception:
        pass
    ok(uids(eng().pull(cursor(eng().pull(cur)) or "")) == [], "собственная отправка не возвращается эхом")
    # update с верным etag; с устаревшим - conflict; несуществующего - not found
    upd = lambda uid, summ, et, op="update": D.vcalendar([D.put(D.put(ev(uid, summ), "X-DKEYS-OP", op), "X-DKEYS-ETAG", et)])  # noqa: E731
    o = eng().push(upd("n1", "New2", et_n1))
    et2 = D.get(evs_of(o)[0], "X-DKEYS-ETAG")
    ok(et2 and et2 != et_n1 and "New2" in [v for h, v in srv.res.items() if "n1" in h][0][1], "update с верным etag")
    o = eng().push(upd("n1", "Stale", et_n1))
    ok(D.get(evs_of(o)[0], "X-DKEYS-ERROR") == "conflict" and "Stale" not in [v for h, v in srv.res.items() if "n1" in h][0][1], "устаревший etag: conflict")
    o = eng().push(upd("ghost", "Ghost", '"1"'))
    ok(D.get(evs_of(o)[0], "X-DKEYS-ERROR") == "not found", "update несуществующего: not found")
    # create существующего - идемпотентная запись, одна копия
    o = eng().push(upd("n1", "Again", "", "create"))
    ok(D.get(evs_of(o)[0], "X-DKEYS-ERROR") is None and sum(1 for h in srv.res if "n1" in h) == 1 and "Again" in [v for h, v in srv.res.items() if "n1" in h][0][1], "create существующего: перезапись без дубля")
    # серия: главное и замена одним запросом, один ресурс
    ser = D.vcalendar([D.put(ev("s2", "Weekly", ["RRULE:FREQ=WEEKLY"]), "X-DKEYS-OP", "create"), D.put(ev("s2", "Moved", ["RECURRENCE-ID:20261127T100000Z"]), "X-DKEYS-OP", "create")])
    o = eng().push(ser)
    ok(len(evs_of(o)) == 2 and sum(1 for h in srv.res if "s2" in h) == 1 and [v for h, v in srv.res.items() if "s2" in h][0][1].count("BEGIN:VEVENT") == 2, "серия - один ресурс")
    # delete: идемпотентно
    d = D.vcalendar([D.put(ev("n1", "x"), "X-DKEYS-OP", "delete")])
    o = eng().push(d)
    ok(D.get(evs_of(o)[0], "X-DKEYS-DELETED") == "1" and not any("n1" in h for h in srv.res), "delete")
    o = eng().push(d)
    ok(D.get(evs_of(o)[0], "X-DKEYS-DELETED") == "1" and D.get(evs_of(o)[0], "X-DKEYS-ERROR") is None, "повторный delete не ошибка")
    # состояние переживает перезапуск процесса (json) и без него курсор неизвестен -> полная выгрузка
    import json
    st2 = json.loads(json.dumps(state))
    o = Engine(srv, cal, st2).pull(cursor(eng().pull("")) or "")
    ok(cursor(o) is not None, "состояние сериализуется в json")
    # ошибка сети в одном событии не мешает остальным
    class Boom(Fake):
        def put(self, cal, href, text, if_match=None, if_none_match=False):
            if "bad" in href:
                raise OSError("network down")
            return Fake.put(self, cal, href, text, if_match, if_none_match)
    b = Boom()
    o = Engine(b, cal, {}).push(D.vcalendar([D.put(ev("bad", "B"), "X-DKEYS-OP", "create"), D.put(ev("good", "G"), "X-DKEYS-OP", "create")]))
    errs = {D.uid_of(e): D.get(e, "X-DKEYS-ERROR") for e in evs_of(o)}
    ok("OSError" in (errs["bad"] or "") and errs["good"] is None, "сбой одного события не мешает остальным")
    print("selftest OK: %d проверок" % n[0])


def main():
    D.io_utf8()
    verb, opts, _rest = D.parse_args(sys.argv[1:])
    if verb == "--selftest":
        selftest()
    elif verb == "info":
        D.out(D.info_json("CalDAV (iCloud, Яндекс, Nextcloud, Fastmail, любой сервер)", calendars=True, profiles=True) + "\n")
    elif verb == "setup":
        cmd_setup(opts)
    elif verb in ("pull", "push"):
        cmd_pull_push(verb, opts)
    else:
        D.fail("usage: caldav.py info|setup|pull --since S|push [--profile P] | --selftest", 2)


if __name__ == "__main__":
    main()
