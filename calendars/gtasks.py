# -*- coding: utf-8 -*-
"""gtasks - источник календаря DKeys: Google Tasks (задачи Google; API tasks/v1 по REST, только стандартная библиотека). Договор команд - README.md рядом.
Каждая задача со сроком - событие на весь день (UID = <id задачи>@gtasks, `X-DKEYS-TASK:1`); задачи без срока не показываются.
Выполненная задача - событие с заголовком «✓ название»: поставить/снять отметку можно в DKeys (меню события «Отметить выполненной»)
или просто дописав/убрав «✓ » в названии; в Google при отправке это становится статусом completed / needsAction.
Вход: тот же ключ приложения, что у google.py (google.state\\client.json), но отдельное разрешение (scope tasks) и свой токен:
gtasks.state\\<account>\\token.json. Профиль = секция gtasks.ini: account, list (id списка задач), name, readonly. Один вход - все списки аккаунта.
Курсор синхронизации - время последнего запроса (updatedMin). Включите «Google Tasks API» в проекте Google Cloud (то же, что для Calendar API)."""
import datetime as dt
import os
import sys
from urllib.parse import quote

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dkcal as D  # noqa: E402
import _oauth as O  # noqa: E402
import _model as M  # noqa: E402

API = "https://tasks.googleapis.com/tasks/v1"
OA_BASE = {"auth_url": "https://accounts.google.com/o/oauth2/v2/auth", "token_url": "https://oauth2.googleapis.com/token",
           "scope": "https://www.googleapis.com/auth/tasks", "redirect_host": "127.0.0.1",
           "auth_params": {"access_type": "offline", "prompt": "consent"}}
INI = os.path.join(HERE, "gtasks.ini")
SBASE = os.path.join(HERE, "gtasks.state")
GCLIENT = os.path.join(HERE, "google.state", "client.json")
DONE = "✓ "                                                           # «✓ » перед названием - задача выполнена
SUF = "@gtasks"


def sdir(name):
    p = os.path.join(SBASE, name)
    os.makedirs(p, exist_ok=True)
    return p


def load_client():
    cid = os.environ.get("DKEYS_GOOGLE_CLIENT_ID")
    if cid:
        return {"client_id": cid, "client_secret": os.environ.get("DKEYS_GOOGLE_CLIENT_SECRET", "")}
    d = D.read_json(GCLIENT, None)
    if isinstance(d, dict):
        d = d.get("installed") or d.get("web") or d
    if isinstance(d, dict) and d.get("client_id"):
        return {"client_id": d["client_id"], "client_secret": d.get("client_secret", "")}
    import _appkey
    if _appkey.CLIENT_ID:                                                    # ключ приложения DKeys (общий для всех пользователей)
        return {"client_id": _appkey.CLIENT_ID, "client_secret": _appkey.CLIENT_SECRET}
    return None


def oauth_cfg(cl):
    return dict(OA_BASE, client_id=cl["client_id"], client_secret=cl.get("client_secret", ""))


def open_profile(profile):
    cp = O.read_ini(INI)
    name = O.pick_profile(cp, profile)
    if not name:
        D.fail("профиль %s не настроен в gtasks.ini: нажмите «Подключить…» в календаре" % (profile or ""), 2)
    sec = cp[name]
    cl = load_client()
    if not cl:
        D.fail("нет ключа приложения Google: сначала подключите источник google («Подключить…» -> google)", 2)
    tokfile = os.path.join(sdir(sec.get("account") or name), "token.json")
    if not os.path.exists(tokfile):
        D.fail(O.NEED_LOGIN, 2)
    sp = os.path.join(sdir(name), "state.json")
    return (O.Client(oauth_cfg(cl), tokfile), (sec.get("list") or "@default").strip(), sp, D.read_json(sp, {}),
            (sec.get("readonly") or "").strip().lower() in ("1", "true", "yes"))


# ---------- задача <-> событие ----------
def turl(lst, tail=""):
    return "%s/lists/%s/tasks%s" % (API, quote(lst, safe=""), tail)


def uid_of_task(t):
    return t["id"] + SUF


def task_id(uid):
    return uid[:-len(SUF)] if uid.endswith(SUF) else ""


def due_date(t):
    s = (t.get("due") or "")[:10]
    try:
        return dt.date.fromisoformat(s)
    except ValueError:
        return None


def task_event(t):
    d = due_date(t)
    if d is None:
        return None
    title = (t.get("title") or "").strip()
    if t.get("status") == "completed":
        title = DONE + title
    upd = D.iso_to_ics((t.get("updated") or "")[:19] + "Z") or D.now_ics()
    ev = ["BEGIN:VEVENT", "UID:" + uid_of_task(t), "DTSTAMP:" + upd, "LAST-MODIFIED:" + upd, "SUMMARY:" + M.esc(title)]
    if t.get("notes"):
        ev.append("DESCRIPTION:" + M.esc(t["notes"]))
    ev += ["DTSTART;VALUE=DATE:" + d.strftime("%Y%m%d"), "DTEND;VALUE=DATE:" + (d + dt.timedelta(days=1)).strftime("%Y%m%d"),
           "X-DKEYS-TASK:1", "X-DKEYS-ETAG:" + (t.get("etag") or t.get("updated") or "0"), "END:VEVENT"]
    return ev


def list_all(api, lst, params):
    items, page = [], None
    while True:
        p = dict(params, maxResults=100)
        if page:
            p["pageToken"] = page
        _, d, _ = api.req("GET", turl(lst), params=p)
        if not isinstance(d, dict):
            raise O.ApiError(502, "неожиданный ответ сервера")
        items += d.get("items", [])
        page = d.get("nextPageToken")
        if not page:
            return items


def now_iso():
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")


def valid_cursor(s):
    try:
        dt.datetime.strptime(s, "%Y-%m-%dT%H:%M:%S.000Z")
        return True
    except ValueError:
        return False


def pull(api, lst, since):
    full = not valid_cursor(since)
    stamp = (dt.datetime.now(dt.timezone.utc) - dt.timedelta(seconds=30)).strftime("%Y-%m-%dT%H:%M:%S.000Z")
    q = {"showCompleted": "true", "showHidden": "true"}
    if not full:
        q.update(updatedMin=since, showDeleted="true")
    out = []
    for t in list_all(api, lst, q):
        ev = None if t.get("deleted") else task_event(t)
        if ev is not None:
            out.append(ev)
        elif not full:
            out.append(D.deleted_marker(uid_of_task(t)))                       # удалена или потеряла срок
    return out, ["X-DKEYS-CURSOR:" + stamp] + (["X-DKEYS-FULL:1"] if full and since else [])


def cmd_pull(opts):
    api, lst, sp, state, _ro = open_profile(opts["profile"])
    evs, extra = pull(api, lst, opts["since"].strip())
    D.out(D.vcalendar(evs, extra))
    D.write_json(sp, state)


# ---------- push ----------
def body_from(ev):
    title = M.unesc(D.get(ev, "SUMMARY") or "").strip()
    done = title.startswith(DONE.strip())
    if done:
        title = title[len(DONE.strip()):].strip()
    b = {"title": title, "notes": M.unesc(D.get(ev, "DESCRIPTION") or ""), "status": "completed" if done else "needsAction"}
    s = (D.get(ev, "DTSTART") or "")[:8]
    if len(s) == 8 and s.isdigit():
        b["due"] = "%s-%s-%sT00:00:00.000Z" % (s[:4], s[4:6], s[6:8])
    if not done:
        b["completed"] = None
    return b


def push_one(api, lst, ev, op, sent):
    uid = D.uid_of(ev)
    tid = task_id(uid)
    if op == "delete":
        if tid:
            api.req("DELETE", turl(lst, "/" + quote(tid, safe="")), ok=(404, 410))
        return M.as_deleted([ev])
    b = body_from(ev)
    if not b["title"]:
        b["title"] = "(без названия)"
    if op == "update":
        st, cur, _ = api.req("GET", turl(lst, "/" + quote(tid, safe="")), ok=(404, 410))
        if st >= 400 or not isinstance(cur, dict) or cur.get("deleted"):
            raise O.ApiError(404, "not found")
        if sent and sent.strip('"') != (cur.get("etag") or cur.get("updated") or "").strip('"'):
            raise O.ApiError(409, "conflict")
        _, t, _ = api.req("PATCH", turl(lst, "/" + quote(tid, safe="")), body=b)
    else:
        _, t, _ = api.req("POST", turl(lst), body=b)
    new_uid = uid_of_task(t)
    g = M.with_etag([ev], t.get("etag") or t.get("updated") or "0", new_uid, uid)
    return [D.put(e, "X-DKEYS-TASK", "1") for e in g]


def push(api, lst, text, readonly=False):
    out = []
    for ev in D.split_vevents(text):
        op = (D.get(ev, "X-DKEYS-OP") or "").lower()
        if readonly or op not in ("create", "update", "delete"):
            out += M.with_error([ev], "источник только для чтения" if readonly else "неизвестная операция")
            continue
        try:
            out += push_one(api, lst, ev, op, D.get(ev, "X-DKEYS-ETAG"))
        except O.ApiError as e:
            if e.status == 409:
                out += M.with_error([ev], "conflict")
            elif e.status == 404:
                out += M.with_error([ev], "not found")
            elif e.status in (400, 422):
                out += M.with_error([ev], str(e))
            else:
                raise
    return out


def cmd_push(opts):
    api, lst, sp, state, ro = open_profile(opts["profile"])
    D.out(D.vcalendar(push(api, lst, sys.stdin.read(), ro)))
    D.write_json(sp, state)


# ---------- setup ----------
def cmd_setup(opts):
    prof = opts["profile"]
    print("Google Tasks: подключение%s" % (" (профиль %s)" % prof if prof else ""))
    cl = load_client()
    if not cl:
        D.fail("нет ключа приложения Google: сначала подключите источник google («Подключить…» -> google), задачи используют тот же ключ", 2)
    print("В проекте Google Cloud должен быть включён «Google Tasks API» (APIs & Services -> Library -> Google Tasks API -> Enable).")
    oa = oauth_cfg(cl)
    try:
        tok = O.login_browser(oa)
    except O.AuthError as e:
        D.fail(str(e), 3)
    cp = O.read_ini(INI)
    if prof and cp.has_section(prof):
        O.write_secret(os.path.join(sdir(cp[prof].get("account") or prof), "token.json"), tok)
        print("Готово: вход выполнен заново. Закройте это окно - DKeys синхронизируется сам.")
        return
    account = "main"
    O.write_secret(os.path.join(sdir(account), "token.json"), tok)
    api = O.Client(oa, os.path.join(sdir(account), "token.json"))
    try:
        _, d, _ = api.req("GET", API + "/users/@me/lists", params={"maxResults": 100})
    except (O.ApiError, O.NetError, O.AuthError) as e:
        D.fail("не удалось получить списки задач (включён ли Google Tasks API?): %s" % e, 3)
    lists = (d or {}).get("items", [])
    if not lists:
        D.fail("в аккаунте нет списков задач", 3)
    for it in O.pick_many(lists, lambda c: c.get("title") or c["id"]):
        if any(cp.has_section(s) and cp[s].get("account") == account and cp[s].get("list") == it["id"] for s in cp.sections()):
            continue
        name = O.unique(O.slug("t_" + (it.get("title") or ""), 15, "tasks"), set(cp.sections()))
        cp.add_section(name)
        cp.set(name, "account", account)
        cp.set(name, "list", it["id"])
        cp.set(name, "name", it.get("title") or it["id"])
        sp = os.path.join(sdir(name), "state.json")
        if os.path.exists(sp):
            os.remove(sp)
        print("  источник gtasks:%s" % name)
    O.write_ini(cp, INI)
    print("Готово. Закройте это окно - DKeys найдёт источники и синхронизируется сам.")


def main():
    D.io_utf8()
    verb, opts, _rest = D.parse_args(sys.argv[1:])
    try:
        if verb == "info":
            D.out(D.info_json("Google Tasks", calendars=True, profiles=True, login="oauth") + "\n")
        elif verb == "setup":
            cmd_setup(opts)
        elif verb == "pull":
            cmd_pull(opts)
        elif verb == "push":
            cmd_push(opts)
        else:
            D.fail("usage: gtasks.py info|setup|pull --since S|push [--profile P]", 2)
    except (O.AuthError, O.NetError) as e:
        D.fail("Google Tasks: %s" % e, 3)
    except O.ApiError as e:
        D.fail("Google Tasks: ошибка сервера: %s" % e, 3)


if __name__ == "__main__":
    main()
