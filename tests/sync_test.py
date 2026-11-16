# -*- coding: utf-8 -*-
"""Проверки договора провайдеров календаря (calendars\\README.md) на файловом stub.py и самопроверки caldav.py, google.py, outlook.py (--selftest, info, ненастроенный профиль).
Движок слияния (конфликты, удаления, hidden, перенос между источниками, серии, сбои, повторная синхронизация) проверяет caltest.cpp
вместе с настоящим stub.py; здесь - сами скрипты как внешние процессы: команды, коды выхода, формат ответов, режимы сбоев.
Запуск: python tests\\sync_test.py (из tests\\run.cmd, если найден python)."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
CAL = os.path.normpath(os.path.join(HERE, "..", "calendars"))
STUB = os.path.join(CAL, "stub.py")


def vevent(uid, summ, lm="20261101T100000Z", extra=""):
    return "BEGIN:VEVENT\r\nUID:%s\r\nSUMMARY:%s\r\nDTSTART:20261120T100000Z\r\nDTEND:20261120T110000Z\r\nLAST-MODIFIED:%s\r\n%sEND:VEVENT\r\n" % (uid, summ, lm, extra)


def cal(*events, head=""):
    return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n%s%sEND:VCALENDAR\r\n" % (head, "".join(events))


def prop(text, name):
    for ln in text.replace("\r\n", "\n").split("\n"):
        if ln.startswith(name + ":"):
            return ln[len(name) + 1:]
    return None


def props(text, name):
    return [ln[len(name) + 1:] for ln in text.replace("\r\n", "\n").split("\n") if ln.startswith(name + ":")]


class StubTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="dkeys_synctest_")
        self.env = dict(os.environ, DKEYS_STUB_DIR=self.tmp, PYTHONIOENCODING="utf-8")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_stub(self, *args, inp=None, profile="p", timeout=30):
        a = [sys.executable, STUB] + list(args) + (["--profile", profile] if profile else [])
        return subprocess.run(a, input=inp, capture_output=True, text=True, encoding="utf-8", env=self.env, timeout=timeout)

    def pull(self, since="", profile="p"):
        r = self.run_stub("pull", "--since", since, profile=profile)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def push(self, text, profile="p"):
        r = self.run_stub("push", inp=text, profile=profile)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def op(self, op, uid, summ, etag="", extra=""):
        return vevent(uid, summ, extra="X-DKEYS-OP:%s\r\n%s%s" % (op, ("X-DKEYS-ETAG:%s\r\n" % etag) if etag else "", extra))

    def test_info(self):
        r = self.run_stub("info", profile="")
        self.assertEqual(r.returncode, 0)
        j = json.loads(r.stdout.strip().splitlines()[-1])
        self.assertTrue(j["name"] and j["create"] and j["update"] and j["delete"])

    def test_full_pull_has_cursor_and_flag(self):
        t = self.pull("")
        self.assertIn("X-DKEYS-FULL:1", t)
        self.assertTrue(prop(t, "X-DKEYS-CURSOR"))

    def test_create_pull_update_delete(self):
        out = self.push(cal(self.op("create", "u1", "One")))
        et1 = prop(out, "X-DKEYS-ETAG")
        self.assertTrue(et1)
        self.assertEqual(prop(out, "UID"), "u1")
        self.assertIsNone(prop(out, "X-DKEYS-OP"))
        full = self.pull("")
        self.assertIn("SUMMARY:One", full)
        cur = prop(full, "X-DKEYS-CURSOR")
        self.assertNotIn("SUMMARY:One", self.pull(cur))                                 # после курсора изменений нет
        out = self.push(cal(self.op("update", "u1", "One2", et1)))
        et2 = prop(out, "X-DKEYS-ETAG")
        self.assertTrue(et2 and et2 != et1)
        inc = self.pull(cur)
        self.assertIn("SUMMARY:One2", inc)
        self.assertNotIn("X-DKEYS-FULL", inc)
        out = self.push(cal(self.op("delete", "u1", "One2", et2)))
        self.assertEqual(prop(out, "X-DKEYS-DELETED"), "1")
        gone = self.pull(prop(inc, "X-DKEYS-CURSOR"))
        self.assertEqual(prop(gone, "X-DKEYS-DELETED"), "1")                            # маркер удаления для другого клиента
        self.assertEqual(prop(gone, "UID"), "u1")
        again = self.push(cal(self.op("delete", "u1", "One2", et2)))                    # удаление идемпотентно
        self.assertEqual(prop(again, "X-DKEYS-DELETED"), "1")
        self.assertIsNone(prop(again, "X-DKEYS-ERROR"))

    def test_stale_etag_and_missing(self):
        et1 = prop(self.push(cal(self.op("create", "u1", "A"))), "X-DKEYS-ETAG")
        self.push(cal(self.op("update", "u1", "B", et1)))
        out = self.push(cal(self.op("update", "u1", "C", et1)))                         # etag устарел
        self.assertEqual(prop(out, "X-DKEYS-ERROR"), "conflict")
        out = self.push(cal(self.op("update", "ghost", "G", "x")))
        self.assertEqual(prop(out, "X-DKEYS-ERROR"), "not found")
        self.assertIn("SUMMARY:B", self.pull(""))
        self.assertNotIn("SUMMARY:C", self.pull(""))

    def test_create_is_idempotent(self):
        self.push(cal(self.op("create", "u1", "A")))
        out = self.push(cal(self.op("create", "u1", "A2")))
        self.assertIsNone(prop(out, "X-DKEYS-ERROR"))
        self.assertEqual(self.pull("").count("BEGIN:VEVENT"), 1)

    def test_series_stays_together(self):
        s = cal(self.op("create", "s", "Weekly", extra="RRULE:FREQ=WEEKLY\r\n"), self.op("create", "s", "Moved", extra="RECURRENCE-ID:20261127T100000Z\r\n"))
        out = self.push(s)
        self.assertEqual(out.count("BEGIN:VEVENT"), 2)
        self.assertEqual(len(set(props(out, "X-DKEYS-ETAG"))), 1)                       # один etag на серию
        t = self.pull("")
        self.assertEqual(t.count("BEGIN:VEVENT"), 2)
        self.assertIn("RRULE:FREQ=WEEKLY", t)
        self.assertEqual(len(props(t, "RECURRENCE-ID")), 1)

    def test_profiles_are_isolated(self):
        self.push(cal(self.op("create", "u1", "OnlyA")), profile="a")
        self.assertIn("OnlyA", self.pull("", profile="a"))
        self.assertNotIn("OnlyA", self.pull("", profile="b"))

    def test_other_client_changes_arrive_and_since_iso(self):
        self.run_stub("admin", "put", inp=cal(vevent("x1", "Ext")))
        t = self.pull("")
        self.assertIn("SUMMARY:Ext", t)
        self.assertTrue(prop(t, "X-DKEYS-ETAG"))
        self.assertIn("SUMMARY:Ext", self.pull("2000-01-01T00:00:00Z"))                 # ISO-время вместо курсора
        self.assertNotIn("SUMMARY:Ext", self.pull("2099-01-01T00:00:00Z"))

    def test_failure_modes(self):
        self.run_stub("admin", "mode", "error")
        r = self.run_stub("pull", "--since", "")
        self.assertNotEqual(r.returncode, 0)
        self.assertTrue(r.stderr.strip())
        self.run_stub("admin", "mode", "garbage")
        r = self.run_stub("pull", "--since", "")
        self.assertEqual(r.returncode, 0)
        self.assertNotIn("END:VCALENDAR", r.stdout)                                     # мусор: движок отвергнет ответ
        self.run_stub("admin", "mode", "failevents")
        out = self.push(cal(self.op("create", "u1", "A")))
        self.assertTrue(prop(out, "X-DKEYS-ERROR"))                                     # ошибка по событию при коде 0
        self.run_stub("admin", "mode", "newuid")
        out = self.push(cal(self.op("create", "u2", "B")))
        self.assertEqual(prop(out, "X-DKEYS-REF"), "u2")
        self.assertNotEqual(prop(out, "UID"), "u2")                                     # сервер присвоил свой UID
        self.run_stub("admin", "mode", "none")
        self.assertEqual(self.run_stub("pull", "--since", "").returncode, 0)

    def test_hang_is_killed_by_caller_timeout(self):
        self.run_stub("admin", "mode", "hang")
        t0 = time.time()
        with self.assertRaises(subprocess.TimeoutExpired):
            self.run_stub("pull", "--since", "", timeout=2)
        self.assertLess(time.time() - t0, 10)


class ProviderBase:
    """Общие проверки скрипта-провайдера как внешнего процесса (наследник задаёт SCRIPT)."""
    SCRIPT = ""

    def run_script(self, *args, timeout=30):
        return subprocess.run([sys.executable, os.path.join(CAL, self.SCRIPT)] + list(args), capture_output=True, text=True, encoding="utf-8",
                              env=dict(os.environ, PYTHONIOENCODING="utf-8"), timeout=timeout)

    def test_selftest_without_network(self):
        r = self.run_script("--selftest", timeout=180)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("selftest OK", r.stdout)

    def test_info(self):
        r = self.run_script("info")
        self.assertEqual(r.returncode, 0)
        self.assertTrue(json.loads(r.stdout.strip().splitlines()[-1])["calendars"])

    def test_unconfigured_profile_fails_clearly(self):
        r = self.run_script("pull", "--since", "", "--profile", "no_such_profile_xyz")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("Подключить", r.stderr)


class CaldavTest(ProviderBase, unittest.TestCase):
    SCRIPT = "caldav.py"


class GoogleTest(ProviderBase, unittest.TestCase):
    SCRIPT = "google.py"

    def test_info_name_and_capabilities(self):
        i = json.loads(self.run_script("info").stdout.strip().splitlines()[-1])
        self.assertIn("Google", i["name"])
        self.assertTrue(i["create"] and i["update"] and i["delete"] and i["series"])


class OutlookTest(ProviderBase, unittest.TestCase):
    SCRIPT = "outlook.py"

    def test_info_name_and_capabilities(self):
        i = json.loads(self.run_script("info").stdout.strip().splitlines()[-1])
        self.assertIn("Outlook", i["name"])
        self.assertTrue(i["create"] and i["update"] and i["delete"] and i["series"])


if __name__ == "__main__":
    unittest.main(verbosity=1)
