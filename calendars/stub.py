# -*- coding: utf-8 -*-
"""stub - тестовый провайдер календаря DKeys на файловом «сервере» (папка с .ics и счётчиком версий).
Реализует весь договор (README.md): info, setup, pull, push. Нужен для проверки движка без реальных аккаунтов;
как пример - самый короткий рабочий провайдер.

Сервер: папка (--dir, переменная DKEYS_STUB_DIR или stub.state/<профиль>/server): res/<n>.ics - по ресурсу на UID (серия целиком),
meta.json - счётчик версий rev, etag каждого ресурса и надгробия удалённых. Курсор pull: "rev=N".
Режим отладки - файл mode.txt в папке сервера (ставится `stub.py admin mode ...`):
  error | garbage | hang | delay N | newuid (сервер присваивает свои UID, echo X-DKEYS-REF) | failevents (события не применяются) | none
Другой клиент (для проверок): `stub.py admin put` (ICS в stdin), `admin delete UID`, `admin list`."""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dkcal as D  # noqa: E402


def server_dir(opts):
    d = opts["dir"] or os.environ.get("DKEYS_STUB_DIR", "")
    if d and opts["profile"]:
        d = os.path.join(d, opts["profile"])                       # у каждого профиля свой сервер
    if not d:
        d = os.path.join(D.state_dir(__file__, opts["profile"]), "server")
    os.makedirs(os.path.join(d, "res"), exist_ok=True)
    return d


class Server:
    def __init__(self, d):
        self.d = d
        self.meta = D.read_json(os.path.join(d, "meta.json"), {"rev": 0, "items": {}, "tomb": {}})

    def save(self):
        D.write_json(os.path.join(self.d, "meta.json"), self.meta)

    def mode(self):
        try:
            with open(os.path.join(self.d, "mode.txt"), encoding="utf-8") as f:
                return f.read().split()
        except OSError:
            return []

    def bump(self):
        self.meta["rev"] += 1
        return self.meta["rev"]

    def read(self, uid):
        it = self.meta["items"].get(uid)
        if not it:
            return None
        with open(os.path.join(self.d, "res", it["file"]), encoding="utf-8") as f:
            return D.split_vevents(f.read())

    def write(self, uid, events):
        """Сохраняет серию; возвращает новый etag. LAST-MODIFIED клиента сохраняется, иначе ставится текущее."""
        rev = self.bump()
        it = self.meta["items"].get(uid) or {"file": "%d.ics" % rev}
        evs = []
        for ev in events:
            ev = D.drop(ev, "X-DKEYS-OP", "X-DKEYS-ETAG", "X-DKEYS-DELETED", "X-DKEYS-REF", "X-DKEYS-ERROR")
            if D.get(ev, "LAST-MODIFIED") is None:
                ev = D.put(ev, "LAST-MODIFIED", D.now_ics())
            evs.append(ev)
        with open(os.path.join(self.d, "res", it["file"]), "w", encoding="utf-8", newline="") as f:
            f.write(D.vcalendar(evs))
        it.update({"etag": "r%d" % rev, "rev": rev})
        self.meta["items"][uid] = it
        self.meta["tomb"].pop(uid, None)
        return it["etag"], evs

    def remove(self, uid):
        it = self.meta["items"].pop(uid, None)
        if not it:
            return False
        try:
            os.remove(os.path.join(self.d, "res", it["file"]))
        except OSError:
            pass
        self.meta["tomb"][uid] = {"etag": it["etag"], "rev": self.bump()}
        return True


def with_etag(events, etag):
    return [D.put(ev, "X-DKEYS-ETAG", etag) for ev in events]


def apply_mode(srv):
    m = srv.mode()
    if not m:
        return m
    if m[0] == "error":
        D.fail("stub: simulated error", 3)
    if m[0] == "hang":
        time.sleep(3600)
    if m[0] == "delay":
        time.sleep(float(m[1]) if len(m) > 1 else 1)
    return m


def cmd_pull(opts):
    srv = Server(server_dir(opts))
    m = apply_mode(srv)
    if m and m[0] == "garbage":
        D.out("BEGIN:VCALENDAR\r\nthis is not an ics\r\n")
        return
    since = opts["since"].strip()
    full = not since
    base = 0
    if since.startswith("rev="):
        base = int(since[4:] or 0)
    elif since:                                      # ISO-время: отбираем по LAST-MODIFIED
        base = -1
    iso = D.iso_to_ics(since)
    out_ev = []
    for uid, it in sorted(srv.meta["items"].items(), key=lambda kv: kv[1]["rev"]):
        evs = srv.read(uid) or []
        if full or (base >= 0 and it["rev"] > base) or (base < 0 and any((D.get(e, "LAST-MODIFIED") or "") >= iso for e in evs)):
            out_ev.extend(with_etag(evs, it["etag"]))
    if not full:
        for uid, t in sorted(srv.meta["tomb"].items(), key=lambda kv: kv[1]["rev"]):
            if base < 0 or t["rev"] > base:
                out_ev.append(D.deleted_marker(uid, t["etag"]))
    extra = ["X-DKEYS-CURSOR:rev=%d" % srv.meta["rev"]] + (["X-DKEYS-FULL:1"] if full else [])
    D.out(D.vcalendar(out_ev, extra))


def cmd_push(opts):
    srv = Server(server_dir(opts))
    m = apply_mode(srv)
    if m and m[0] == "garbage":
        D.out("garbage\r\n")
        return
    out_ev = []
    for uid, group in D.group_by_uid(D.split_vevents(sys.stdin.read())):
        op = (D.get(group[0], "X-DKEYS-OP") or "create").lower()
        sent = D.get(group[0], "X-DKEYS-ETAG") or ""
        if m and m[0] == "failevents":
            out_ev.extend(D.put(e, "X-DKEYS-ERROR", "stub: событие не применено") for e in group)
            continue
        if op == "delete":
            srv.remove(uid)                                         # удаление идемпотентно, версия не проверяется
            out_ev.extend(D.drop(D.put(e, "X-DKEYS-DELETED", "1"), "X-DKEYS-OP") for e in group)
            continue
        cur = srv.meta["items"].get(uid)
        if op == "update" and not cur:
            out_ev.extend(D.put(e, "X-DKEYS-ERROR", "not found") for e in group)
            continue
        if op == "update" and sent and sent != cur["etag"]:
            out_ev.extend(D.put(e, "X-DKEYS-ERROR", "conflict") for e in group)
            continue
        new_uid, ref = uid, None
        if op == "create" and m and m[0] == "newuid" and not cur:
            new_uid, ref = "srv-" + uid, uid
        evs = [D.put(e, "UID", new_uid) for e in group] if new_uid != uid else group
        etag, stored = srv.write(new_uid, evs)
        for e in with_etag(stored, etag):
            out_ev.append(D.put(e, "X-DKEYS-REF", ref) if ref else e)
    srv.save()
    D.out(D.vcalendar(out_ev))


def cmd_admin(opts, rest):
    srv = Server(server_dir(opts))
    what = rest[0] if rest else ""
    if what == "put":                                               # «другой клиент» изменил или создал событие
        for uid, group in D.group_by_uid(D.split_vevents(sys.stdin.read())):
            srv.write(uid, [D.put(e, "LAST-MODIFIED", D.get(e, "LAST-MODIFIED") or D.now_ics()) for e in group])
    elif what == "delete":
        srv.remove(rest[1])
    elif what == "mode":
        with open(os.path.join(srv.d, "mode.txt"), "w", encoding="utf-8") as f:
            f.write(" ".join(rest[1:]) if rest[1:] and rest[1] != "none" else "")
    elif what == "list":
        for uid, it in sorted(srv.meta["items"].items()):
            D.out("%s %s\n" % (uid, it["etag"]))
        return
    else:
        D.fail("admin put|delete UID|mode MODE|list")
    srv.save()


def main():
    D.io_utf8()
    verb, opts, rest = D.parse_args(sys.argv[1:])
    if verb == "info":
        D.out(D.info_json("Stub (файловый сервер для проверок)") + "\n")
    elif verb == "setup":
        d = server_dir(opts)
        D.out("stub: сервер в %s, входить никуда не нужно\n" % d)
    elif verb == "pull":
        cmd_pull(opts)
    elif verb == "push":
        cmd_push(opts)
    elif verb == "admin":
        cmd_admin(opts, rest)
    else:
        D.fail("usage: stub.py info|setup|pull --since S|push [--profile P] [--dir D]", 2)


if __name__ == "__main__":
    main()
